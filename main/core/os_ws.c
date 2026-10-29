// os_ws on the badge: RFC 6455 over ESP-IDF's TLS transport. The client
// side of the protocol is small (an upgrade request, masked frames out,
// unmasked frames in), so it's written here rather than with ESP-IDF's
// WebSocket transport, which waits on the socket for a frame it may already
// have buffered with the upgrade answer (a host's greeting would stall).
//
// RAM is the constraint, as for os_http: the handshake to a host we never
// spoke to is a full one (~33 KB at its peak), so it needs the same room as
// os_http's first connection. Once open, the connection keeps record buffers
// the size of its records (dynamic mbedTLS buffers), and a message costs its
// own size.
//
// The badge server checked the host's addresses when it signed the URL; we
// check them again from here, where a rebinding answer or the network's own
// DNS could point a store app at the network the badge is on.

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_transport.h"
#include "esp_transport_ssl.h"
#include "lwip/netdb.h"
#include "mbedtls/base64.h"
#include "mbedtls/sha1.h"
#include "os_ws.h"

static const char *TAG = "os_ws";

#define TIMEOUT_MS      10000
#define HOST_MAX        64              // validate.ts: host names are at most 64 characters
// os_http.c's room for a full handshake: ~33 KB at the peak, ~24 KB left over.
#define TLS_MIN_FREE    (56 * 1024)
#define TLS_MIN_BLOCK   (8 * 1024)
#define ANSWER_MAX      2048            // the upgrade answer's headers
#define CONTROL_MAX     125             // RFC 6455: control frames carry at most this
#define USER_AGENT      "SolanaOS-Badge/1"
#define WS_GUID         "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

enum { OP_CONT = 0x0, OP_TEXT = 0x1, OP_BINARY = 0x2, OP_CLOSE = 0x8, OP_PING = 0x9, OP_PONG = 0xA };

static esp_transport_handle_t s_tls;
static char *s_left;                    // what came after the upgrade answer, read first
static size_t s_left_len, s_left_off;

static void drop(void)
{
    if (s_tls) {
        esp_transport_close(s_tls);
        esp_transport_destroy(s_tls);
        s_tls = NULL;
    }
    free(s_left);
    s_left = NULL;
}

// Up to n bytes: > 0 read, 0 nothing within timeout_ms, < 0 the connection is gone.
static int rd(char *buf, size_t n, int timeout_ms)
{
    if (s_left) {
        size_t k = s_left_len - s_left_off < n ? s_left_len - s_left_off : n;
        memcpy(buf, s_left + s_left_off, k);
        s_left_off += k;
        if (s_left_off == s_left_len) {
            free(s_left);
            s_left = NULL;
        }
        return (int)k;
    }
    return esp_transport_read(s_tls, buf, (int)n, timeout_ms);
}

// Exactly n bytes (the rest of something already started), or false.
static bool rd_all(char *buf, size_t n)
{
    while (n) {
        int r = rd(buf, n, TIMEOUT_MS);
        if (r <= 0) {
            return false;
        }
        buf += r;
        n -= r;
    }
    return true;
}

static bool wr_all(const char *buf, size_t n)
{
    while (n) {
        int r = esp_transport_write(s_tls, buf, (int)n, TIMEOUT_MS);
        if (r <= 0) {
            return false;
        }
        buf += r;
        n -= r;
    }
    return true;
}

// One frame, masked as a client must (in place, restored after).
static bool send_frame(int op, char *data, size_t len)
{
    uint8_t h[8];
    size_t hl = 0;
    h[hl++] = 0x80 | op;                    // FIN: we never fragment
    if (len <= 125) {
        h[hl++] = 0x80 | len;
    } else {
        h[hl++] = 0x80 | 126;               // our messages stay under 64 KB
        h[hl++] = len >> 8;
        h[hl++] = len & 0xFF;
    }
    uint8_t *mask = h + hl;
    esp_fill_random(mask, 4);
    hl += 4;
    for (size_t i = 0; i < len; i++) {
        data[i] ^= mask[i % 4];
    }
    bool ok = wr_all((const char *)h, hl) && (!len || wr_all(data, len));
    for (size_t i = 0; i < len; i++) {
        data[i] ^= mask[i % 4];
    }
    return ok;
}

// The ranges server/src/apphttp.ts publicAddress() refuses, for IPv4 (the
// badge connects over IPv4 only).
static bool public_v4(uint32_t ip)
{
    uint8_t a = ip >> 24, b = ip >> 16, c = ip >> 8;
    return !(a == 0 || a == 10 || a == 127 || a >= 224 ||
             (a == 100 && b >= 64 && b < 128) || (a == 169 && b == 254) ||
             (a == 172 && b >= 16 && b < 32) || (a == 192 && b == 168) ||
             (a == 192 && b == 0 && (c == 0 || c == 2)) || (a == 198 && (b == 18 || b == 19)) ||
             (a == 198 && b == 51 && c == 100) || (a == 203 && b == 0 && c == 113));
}

// ESP_OK when every address host resolves to is public.
static esp_err_t check_host(const char *host)
{
    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM }, *res = NULL;
    if (getaddrinfo(host, NULL, &hints, &res) != 0 || !res) {
        return ESP_FAIL;
    }
    esp_err_t err = ESP_OK;
    for (struct addrinfo *a = res; a; a = a->ai_next) {
        const struct sockaddr_in *in = (const struct sockaddr_in *)a->ai_addr;
        if (a->ai_family != AF_INET || !public_v4(ntohl(in->sin_addr.s_addr))) {
            err = ESP_ERR_INVALID_ARG;
        }
    }
    freeaddrinfo(res);
    return err;
}

// The value of header `name` in an HTTP answer, trimmed, or NULL.
static const char *header(char *answer, const char *name, size_t *len)
{
    size_t n = strlen(name);
    for (char *p = strstr(answer, "\r\n"); p; p = strstr(p + 2, "\r\n")) {
        if (!strncasecmp(p + 2, name, n) && p[2 + n] == ':') {
            const char *v = p + 3 + n;
            while (*v == ' ' || *v == '\t') {
                v++;
            }
            size_t l = strcspn(v, "\r");
            while (l && (v[l - 1] == ' ' || v[l - 1] == '\t')) {
                l--;
            }
            *len = l;
            return v;
        }
    }
    return NULL;
}

// The upgrade (RFC 6455 4.1): our request, then the answer's headers, keeping
// whatever came after them for the frame reader.
static esp_err_t upgrade(const char *host, const char *path, const char *protocols)
{
    unsigned char raw[16], key[32], want[32], digest[20];
    size_t kl = 0, wl = 0;
    esp_fill_random(raw, sizeof(raw));
    mbedtls_base64_encode(key, sizeof(key), &kl, raw, sizeof(raw));
    key[kl] = 0;

    size_t cap = strlen(path) + strlen(host) + (protocols ? strlen(protocols) : 0) + 256;
    char *req = malloc(cap > ANSWER_MAX ? cap : ANSWER_MAX);
    if (!req) {
        return ESP_ERR_NO_MEM;
    }
    int n = snprintf(req, cap,
                     "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: " USER_AGENT "\r\nUpgrade: websocket\r\n"
                     "Connection: Upgrade\r\nSec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n%s%s%s\r\n",
                     path, host, (const char *)key, protocols ? "Sec-WebSocket-Protocol: " : "",
                     protocols ? protocols : "", protocols ? "\r\n" : "");
    if (n <= 0 || (size_t)n >= cap || !wr_all(req, n)) {
        free(req);
        return ESP_FAIL;
    }

    // The answer, a piece at a time, until its blank line.
    char *ans = req;
    size_t got = 0;
    char *end = NULL;
    while (!end) {
        int r = got < ANSWER_MAX - 1 ? esp_transport_read(s_tls, ans + got, ANSWER_MAX - 1 - got, TIMEOUT_MS) : -1;
        if (r <= 0) {
            ESP_LOGW(TAG, "%s: no upgrade answer", host);
            free(ans);
            return ESP_FAIL;
        }
        got += r;
        ans[got] = 0;
        end = strstr(ans, "\r\n\r\n");
    }
    size_t head = end + 4 - ans;
    if (got > head) {
        s_left = malloc(got - head);
        if (!s_left) {
            free(ans);
            return ESP_ERR_NO_MEM;
        }
        memcpy(s_left, ans + head, got - head);
        s_left_len = got - head;
        s_left_off = 0;
    }
    end[2] = 0;                             // keep the last header's \r\n for header()

    int status = 0;
    sscanf(ans, "HTTP/1.%*d %d", &status);
    size_t al = 0;
    const char *accept = header(ans, "Sec-WebSocket-Accept", &al);
    esp_err_t err = ESP_FAIL;
    char *joined = malloc(kl + sizeof(WS_GUID));
    if (joined) {
        memcpy(joined, key, kl);
        memcpy(joined + kl, WS_GUID, sizeof(WS_GUID));
        mbedtls_sha1((const unsigned char *)joined, kl + sizeof(WS_GUID) - 1, digest);
        mbedtls_base64_encode(want, sizeof(want), &wl, digest, sizeof(digest));
        free(joined);
        err = status == 101 && accept && al == wl && !memcmp(accept, want, wl) ? ESP_OK : ESP_FAIL;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "%s refused the upgrade: HTTP %d", host, status);
    }
    free(ans);
    return err;
}

esp_err_t os_ws_connect(const char *url, const char *protocols)
{
    drop();
    if (!url || strncmp(url, "wss://", 6)) {
        return ESP_ERR_INVALID_ARG;
    }
    // The server sent us the URL as it parsed it: a lowercase name, no port.
    const char *h = url + 6;
    size_t n = strcspn(h, "/?#");
    char host[HOST_MAX + 1];
    if (!n || n > HOST_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t i = 0; i < n; i++) {
        char c = h[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.')) {
            return ESP_ERR_INVALID_ARG;
        }
    }
    memcpy(host, h, n);
    host[n] = 0;
    // "wss://host?x" asks for "/?x"; a #fragment is never sent.
    const char *rest = h + n;
    char *path = malloc(strlen(rest) + 2);
    if (!path) {
        return ESP_ERR_NO_MEM;
    }
    path[0] = '/';
    strcpy(path + (*rest != '/'), rest);
    path[strcspn(path, "#")] = 0;

    size_t free0 = heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
    size_t block = heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);
    esp_err_t err = ESP_OK;
    if (free0 < TLS_MIN_FREE || block < TLS_MIN_BLOCK) {
        ESP_LOGW(TAG, "not enough heap for a TLS handshake (%u free, %u largest)", (unsigned)free0, (unsigned)block);
        err = ESP_ERR_NO_MEM;
    } else if ((err = check_host(host)) != ESP_OK) {
        ESP_LOGW(TAG, "%s: %s", host, err == ESP_ERR_INVALID_ARG ? "a private address" : "doesn't resolve");
    } else if (!(s_tls = esp_transport_ssl_init())) {
        err = ESP_ERR_NO_MEM;
    } else {
        esp_transport_ssl_crt_bundle_attach(s_tls, esp_crt_bundle_attach);
        esp_transport_ssl_set_addr_family(s_tls, ESP_TLS_AF_INET);
        if (esp_transport_connect(s_tls, host, 443, TIMEOUT_MS) != 0) {
            ESP_LOGW(TAG, "%s: no TLS connection", host);
            err = ESP_FAIL;
        } else {
            err = upgrade(host, path, protocols && protocols[0] ? protocols : NULL);
        }
    }
    free(path);
    if (err != ESP_OK) {
        drop();
    } else {
        ESP_LOGI(TAG, "connected to %s, %u bytes free", host, (unsigned)heap_caps_get_free_size(MALLOC_CAP_DEFAULT));
    }
    return err;
}

int os_ws_recv(char **out, size_t *len, size_t max, int timeout_ms, int *code)
{
    *out = NULL;
    *len = 0;
    if (!s_tls) {
        return OS_WS_LOST;
    }
    char *msg = NULL;
    size_t n = 0;
    bool joining = false;                   // fragments of a message so far
    int ret = OS_WS_LOST;
    for (;;) {
        uint8_t h[8];
        int r = rd((char *)h, 1, joining ? TIMEOUT_MS : timeout_ms);
        if (r == 0 && !joining) {
            return OS_WS_TIMEOUT;
        }
        if (r <= 0 || !rd_all((char *)h + 1, 1)) {
            break;
        }
        bool fin = h[0] & 0x80;
        int op = h[0] & 0x0F;
        uint64_t flen = h[1] & 0x7F;
        if ((h[0] & 0x70) || (h[1] & 0x80)) {
            break;                          // extension bits, or a masked frame from a server
        }
        if (flen == 126 || flen == 127) {
            int ext = flen == 126 ? 2 : 8;
            if (!rd_all((char *)h, ext)) {
                break;
            }
            flen = 0;
            for (int i = 0; i < ext; i++) {
                flen = flen << 8 | h[i];
            }
        }

        if (op & 0x08) {
            // Control frames come between a message's fragments too.
            char ctl[CONTROL_MAX];
            if (flen > CONTROL_MAX || !fin || !rd_all(ctl, flen)) {
                break;
            }
            if (op == OP_PING) {
                if (!send_frame(OP_PONG, ctl, flen)) {
                    break;
                }
            } else if (op == OP_CLOSE) {
                *code = flen >= 2 ? ((uint8_t)ctl[0] << 8) | (uint8_t)ctl[1] : 1005;
                send_frame(OP_CLOSE, ctl, flen >= 2 ? 2 : 0);   // the same code back
                ret = OS_WS_CLOSED;
                break;
            } else if (op != OP_PONG) {
                break;
            }
            continue;
        }

        // A message starts with text or binary and goes on with continuations.
        if (op > OP_BINARY || (op == OP_CONT) != joining) {
            break;
        }
        if (n + flen > max) {
            ret = OS_WS_TOO_BIG;
            break;
        }
        char *grown = realloc(msg, n + flen + 1);
        if (!grown) {
            break;
        }
        msg = grown;
        if (!rd_all(msg + n, flen)) {
            break;
        }
        n += flen;
        if (fin) {
            msg[n] = 0;
            *out = msg;
            *len = n;
            return OS_WS_MSG;
        }
        joining = true;
    }
    free(msg);
    return ret;
}

esp_err_t os_ws_send(char *text, size_t len)
{
    return s_tls && send_frame(OP_TEXT, text, len) ? ESP_OK : ESP_FAIL;
}

void os_ws_close(int code)
{
    if (s_tls && code) {
        char c[2] = { (char)(code >> 8), (char)(code & 0xFF) };
        send_frame(OP_CLOSE, c, 2);
    }
    drop();
}
