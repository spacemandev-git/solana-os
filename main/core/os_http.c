// os_http on the badge: blocking esp_http_client requests over ONE kept-alive
// connection to the server origin. RAM is the constraint here, not speed: a
// TLS handshake needs ~35 KB, much of it in one piece (the 16 KB incoming
// record buffer), and leaves the heap fragmented. Once the connection is up,
// a request only needs record buffers the size of its records (dynamic
// mbedTLS buffers; device answers are <= 4 KB). So the badge shakes hands
// once, reuses the connection for hello, the device API and downloads, and
// only refuses a NEW handshake when the heap could not hold it and still
// leave ~24 KB for Wi-Fi and apps. The connection stays up while the badge is
// online (hello every 300 s keeps it busy; idle for 5.5 min it closes); a
// session ticket makes the handshake after a close a short resumption.
//
// Only the cloud task calls in here (os_http.h), so one connection is enough.

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "os_http.h"

static const char *TAG = "os_http";

#define TIMEOUT_MS      10000
// Heap a new connection may start from. A full handshake peaks at ~33 KB, a
// resumption with the saved session ticket at ~13 KB; either should leave
// ~24 KB for Wi-Fi and apps. Dynamic mbedTLS buffers are sized per record,
// and handshake records are a few KB, so no 16 KB block is needed up front.
#define TLS_MIN_FREE    (56 * 1024)
#define TLS_RESUME_FREE (40 * 1024)
#define TLS_MIN_BLOCK   (8 * 1024)
// Each reconnect is a handshake that fragments the heap (a resumption left a
// 15 KB largest block on the badge). The front end keeps idle connections
// for minutes, so stay connected across the 300 s hello of a claimed badge.
#define IDLE_CLOSE_MS   330000
#define CHUNK           1024            // download copy / drain buffer

typedef struct {
    const char *name;                   // response header to capture, or NULL
    char *out;
    size_t cap;
} header_ctx_t;

static SemaphoreHandle_t s_lock;
static esp_http_client_handle_t s_client;
static char s_origin[128];
static bool s_open;                     // a kept-alive connection is (probably) still up
static bool s_ticket;                   // a TLS connection to s_origin succeeded: we hold its session ticket
static int64_t s_last_ms;
static header_ctx_t s_hdr;              // what the event handler captures for this request
static struct {
    uint32_t requests, connects;
} s_n;

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static esp_err_t on_event(esp_http_client_event_t *e)
{
    if (e->event_id == HTTP_EVENT_ON_HEADER && s_hdr.name && !strcasecmp(e->header_key, s_hdr.name)) {
        strlcpy(s_hdr.out, e->header_value, s_hdr.cap);
    }
    return ESP_OK;
}

static esp_http_client_method_t method_of(const char *m)
{
    return !strcmp(m, "POST") ? HTTP_METHOD_POST : !strcmp(m, "PATCH") ? HTTP_METHOD_PATCH
         : !strcmp(m, "DELETE") ? HTTP_METHOD_DELETE : HTTP_METHOD_GET;
}

// "https://host:port/path" -> "https://host:port"
static void origin_of(const char *url, char *out, size_t n)
{
    const char *p = strstr(url, "://");
    p = p ? p + 3 : url;
    size_t len = (p - url) + strcspn(p, "/?#");
    snprintf(out, n, "%.*s", (int)(len < n ? len : n - 1), url);
}

static void drop_client(void)
{
    if (s_client) {
        esp_http_client_cleanup(s_client);
        s_client = NULL;
    }
    s_open = s_ticket = false;
    s_origin[0] = 0;
}

static void close_conn(void)
{
    if (s_client && s_open) {
        esp_http_client_close(s_client);
    }
    s_open = false;
}

// A client for url's origin. The handle survives idle closes, so its session
// ticket does too.
static esp_err_t prepare(const char *url)
{
    char origin[sizeof(s_origin)];
    origin_of(url, origin, sizeof(origin));
    if (s_client && strcmp(origin, s_origin)) {
        drop_client();                  // the server was changed
    }
    if (s_open && now_ms() - s_last_ms > IDLE_CLOSE_MS) {
        close_conn();
    }
    bool tls = !strncmp(url, "https://", 8);
    if (tls && !s_open) {
        // A new handshake: only when the heap can take it.
        size_t free0 = heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
        size_t block = heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);
        if (free0 < (s_ticket ? TLS_RESUME_FREE : TLS_MIN_FREE) || block < TLS_MIN_BLOCK) {
            ESP_LOGW(TAG, "not enough heap for a TLS handshake (%u free, %u largest)", (unsigned)free0,
                     (unsigned)block);
            return ESP_ERR_NO_MEM;
        }
    }
    if (!s_client) {
        esp_http_client_config_t cfg = {
            .url = url,
            .timeout_ms = TIMEOUT_MS,
            .buffer_size = 1024,
            .buffer_size_tx = 768,
            .crt_bundle_attach = tls ? esp_crt_bundle_attach : NULL,
            .event_handler = on_event,
            .keep_alive_enable = true,
#if CONFIG_ESP_TLS_CLIENT_SESSION_TICKETS
            .save_client_session = tls,
#endif
        };
        s_client = esp_http_client_init(&cfg);
        if (!s_client) {
            return ESP_ERR_NO_MEM;
        }
        strlcpy(s_origin, origin, sizeof(s_origin));
        s_open = false;
        return ESP_OK;
    }
    return esp_http_client_set_url(s_client, url);
}

// Send one request on the current connection (opening it if needed) and read
// the status and headers.
static esp_err_t send_request(const char *method, const char *bearer, const char *body)
{
    esp_http_client_set_method(s_client, method_of(method));
    if (body) {
        esp_http_client_set_header(s_client, "Content-Type", "application/json");
    } else {
        esp_http_client_delete_header(s_client, "Content-Type");
    }
    if (bearer && bearer[0]) {
        char auth[160];
        snprintf(auth, sizeof(auth), "Bearer %s", bearer);
        esp_http_client_set_header(s_client, "Authorization", auth);
    } else {
        esp_http_client_delete_header(s_client, "Authorization");
    }
    int len = body ? strlen(body) : 0;
    if (!s_open) {
        s_n.connects++;
    }
    esp_err_t err = esp_http_client_open(s_client, len);
    for (int off = 0; err == ESP_OK && off < len;) {
        int n = esp_http_client_write(s_client, body + off, len - off);
        if (n <= 0) {
            err = ESP_ERR_HTTP_WRITE_DATA;
        }
        off += n;
    }
    if (err == ESP_OK) {
        int64_t r = esp_http_client_fetch_headers(s_client);
        err = r == -ESP_ERR_HTTP_EAGAIN ? ESP_ERR_HTTP_READ_TIMEOUT : r < 0 ? ESP_ERR_HTTP_FETCH_HEADER : ESP_OK;
    }
    return err;
}

// After a response: keep the connection only if the whole body was read and
// the server agrees.
static void finish(void)
{
    s_last_ms = now_ms();
    s_open = esp_http_client_is_complete_data_received(s_client) && esp_http_client_is_persistent_connection(s_client);
    if (!s_open) {
        esp_http_client_close(s_client);
    }
}

// Open the request, retrying once on a fresh connection when a kept-alive one
// turns out to be closed by the server (it fails before the request is
// processed). Read timeouts are not retried: the server may be busy.
static esp_err_t start(const char *method, const char *url, const char *bearer, const char *body, int *status)
{
    *status = 0;
    for (int attempt = 0; attempt < 2; attempt++) {
        esp_err_t err = prepare(url);
        if (err != ESP_OK) {
            return err;
        }
        bool reused = s_open;
        err = send_request(method, bearer, body);
        if (err == ESP_OK) {
            *status = esp_http_client_get_status_code(s_client);
            s_open = true;              // until finish() decides
#if CONFIG_ESP_TLS_CLIENT_SESSION_TICKETS
            s_ticket = !strncmp(url, "https://", 8);
#endif
            return ESP_OK;
        }
        close_conn();
        if (!reused || err == ESP_ERR_HTTP_READ_TIMEOUT) {
            ESP_LOGW(TAG, "%s %s failed: %s", method, url, esp_err_to_name(err));
            return err;
        }
        ESP_LOGD(TAG, "kept-alive connection was closed; reconnecting");
    }
    return ESP_FAIL;
}

// Only the cloud task makes requests; the lock guards against a future second caller.
static void lock(void)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
}

esp_err_t os_http_request(const char *method, const char *url, const char *bearer, const char *body,
                          char *resp, size_t cap, int *status)
{
    if (cap) {
        resp[0] = 0;
    }
    lock();
    size_t free0 = heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
    heap_caps_monitor_local_minimum_free_size_start();
    bool fresh = !s_open;
    s_hdr = (header_ctx_t){0};
    s_n.requests++;
    esp_err_t err = start(method, url, bearer, body, status);
    size_t got = 0;
    if (err == ESP_OK) {
        while (got + 1 < cap) {
            int n = esp_http_client_read(s_client, resp + got, cap - 1 - got);
            if (n <= 0) {
                break;
            }
            got += n;
        }
        if (cap) {
            resp[got] = 0;
        }
        finish();
    }
    // What this request cost: the heap low-water mark while it ran.
    size_t low = heap_caps_get_minimum_free_size(MALLOC_CAP_DEFAULT);
    heap_caps_monitor_local_minimum_free_size_stop();
    if (fresh && err == ESP_OK) {
        // Once per connection: what the handshake cost.
        ESP_LOGI(TAG, "connected to %s: heap %u before, %u at the low point (-%u)", s_origin, (unsigned)free0,
                 (unsigned)low, (unsigned)(free0 - low));
    }
    ESP_LOGD(TAG, "%s %s -> %d (%u B, %s)", method, url, *status, (unsigned)got, fresh ? "new connection" : "reused");
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t os_http_download(const char *url, const char *bearer, const char *path, size_t max,
                           const char *header, char *hdr_out, size_t hdr_cap, int *status)
{
    if (hdr_out && hdr_cap) {
        hdr_out[0] = 0;
    }
    lock();
    s_hdr = (header_ctx_t){ .name = header, .out = hdr_out, .cap = hdr_cap };
    s_n.requests++;
    esp_err_t err = start("GET", url, bearer, NULL, status);
    char *buf = err == ESP_OK ? malloc(CHUNK) : NULL;
    if (err == ESP_OK && !buf) {
        err = ESP_ERR_NO_MEM;
    }
    if (err == ESP_OK && *status / 100 == 2) {
        FILE *f = fopen(path, "wb");
        size_t total = 0;
        if (!f) {
            err = ESP_FAIL;
        }
        while (err == ESP_OK) {
            int n = esp_http_client_read(s_client, buf, CHUNK);
            if (n < 0) {
                err = ESP_FAIL;
            } else if (n == 0) {
                break;
            } else if (total + n > max) {
                err = ESP_ERR_INVALID_SIZE;
            } else if (fwrite(buf, 1, n, f) != (size_t)n) {
                err = ESP_FAIL;
            }
            total += n > 0 ? n : 0;
        }
        if (f && fclose(f) != 0 && err == ESP_OK) {
            err = ESP_FAIL;
        }
        if (err == ESP_OK && !esp_http_client_is_complete_data_received(s_client)) {
            err = ESP_FAIL;
        }
        ESP_LOGD(TAG, "GET %s -> %s: %u B, %s", url, path, (unsigned)total, esp_err_to_name(err));
    } else if (err == ESP_OK) {
        // An error answer is small: read it so the connection stays usable.
        while (esp_http_client_read(s_client, buf, CHUNK) > 0) {
        }
    }
    if (err == ESP_OK) {
        finish();
    } else if (s_client) {
        close_conn();
    }
    free(buf);
    s_hdr = (header_ctx_t){0};
    xSemaphoreGive(s_lock);
    if (err != ESP_OK && err != ESP_ERR_NO_MEM) {
        ESP_LOGW(TAG, "download %s failed: %s", url, esp_err_to_name(err));
    }
    return err;
}

void os_http_tick(void)
{
    lock();
    if (s_open && now_ms() - s_last_ms > IDLE_CLOSE_MS) {
        ESP_LOGD(TAG, "closing the idle connection");
        close_conn();
    }
    xSemaphoreGive(s_lock);
}

bool os_http_connected(void)
{
    return s_open;
}

void os_http_close(void)
{
    lock();
    drop_client();
    xSemaphoreGive(s_lock);
}

void os_http_stats(uint32_t *requests, uint32_t *connects)
{
    *requests = s_n.requests;
    *connects = s_n.connects;
}
