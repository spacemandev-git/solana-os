// os_http on the badge: one blocking esp_http_client request per call, no
// keep-alive. HTTPS verifies against the IDF certificate bundle. RAM is the
// constraint here, not speed: small client buffers, and a TLS request is
// refused up front when the heap could not hold a handshake and still leave
// ~24 KB for Wi-Fi and apps (it would fail half-way or starve the rest).

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "os_http.h"

static const char *TAG = "os_http";

#define TIMEOUT_MS      10000
#define TLS_MIN_FREE    (56 * 1024)     // a handshake costs ~26-32 KB (dynamic mbedTLS buffers)
#define TLS_MIN_BLOCK   (20 * 1024)     // the 16 KB incoming record buffer must fit in one piece
#define CHUNK           1024            // download copy buffer

typedef struct {
    const char *name;                   // response header to capture, or NULL
    char *out;
    size_t cap;
} header_ctx_t;

static esp_err_t on_event(esp_http_client_event_t *e)
{
    header_ctx_t *h = e->user_data;
    if (e->event_id == HTTP_EVENT_ON_HEADER && h && h->name && !strcasecmp(e->header_key, h->name)) {
        strlcpy(h->out, e->header_value, h->cap);
    }
    return ESP_OK;
}

static esp_http_client_method_t method_of(const char *m)
{
    return !strcmp(m, "POST") ? HTTP_METHOD_POST : !strcmp(m, "PATCH") ? HTTP_METHOD_PATCH
         : !strcmp(m, "DELETE") ? HTTP_METHOD_DELETE : HTTP_METHOD_GET;
}

// Open the request (TLS heap check, headers, body) and read the response
// headers. *c is set when a client was created (clean it up either way).
static esp_err_t open_request(esp_http_client_handle_t *c, const char *method, const char *url, const char *bearer,
                              const char *body, header_ctx_t *hdr, int *status)
{
    *c = NULL;
    *status = 0;
    bool tls = !strncmp(url, "https://", 8);
    size_t free0 = heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
    if (tls && (free0 < TLS_MIN_FREE || heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT) < TLS_MIN_BLOCK)) {
        ESP_LOGW(TAG, "not enough heap for TLS (%u free)", (unsigned)free0);
        return ESP_ERR_NO_MEM;
    }
    esp_http_client_config_t cfg = {
        .url = url,
        .method = method_of(method),
        .timeout_ms = TIMEOUT_MS,
        .buffer_size = 1024,
        .buffer_size_tx = 768,
        .crt_bundle_attach = tls ? esp_crt_bundle_attach : NULL,
        .event_handler = hdr ? on_event : NULL,
        .user_data = hdr,
    };
    *c = esp_http_client_init(&cfg);
    if (!*c) {
        return ESP_ERR_NO_MEM;
    }
    int len = body ? strlen(body) : 0;
    if (body) {
        esp_http_client_set_header(*c, "Content-Type", "application/json");
    }
    if (bearer && bearer[0]) {
        char auth[160];
        snprintf(auth, sizeof(auth), "Bearer %s", bearer);
        esp_http_client_set_header(*c, "Authorization", auth);
    }
    esp_err_t err = esp_http_client_open(*c, len);
    for (int off = 0; err == ESP_OK && off < len;) {
        int n = esp_http_client_write(*c, body + off, len - off);
        if (n <= 0) {
            err = ESP_FAIL;
        }
        off += n;
    }
    if (err == ESP_OK && esp_http_client_fetch_headers(*c) < 0) {
        err = ESP_FAIL;
    }
    if (err == ESP_OK) {
        *status = esp_http_client_get_status_code(*c);
    }
    return err;
}

static void close_request(esp_http_client_handle_t c)
{
    if (c) {
        esp_http_client_close(c);
        esp_http_client_cleanup(c);
    }
}

esp_err_t os_http_request(const char *method, const char *url, const char *bearer, const char *body,
                          char *resp, size_t cap, int *status)
{
    if (cap) {
        resp[0] = 0;
    }
    size_t free0 = heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
    heap_caps_monitor_local_minimum_free_size_start();
    esp_http_client_handle_t c;
    esp_err_t err = open_request(&c, method, url, bearer, body, NULL, status);
    size_t got = 0;
    if (err == ESP_OK) {
        while (got + 1 < cap) {
            int n = esp_http_client_read(c, resp + got, cap - 1 - got);
            if (n <= 0) {
                break;
            }
            got += n;
        }
        if (cap) {
            resp[got] = 0;
        }
    }
    close_request(c);
    // What this request cost: the heap low-water mark while it ran.
    size_t low = heap_caps_get_minimum_free_size(MALLOC_CAP_DEFAULT);
    heap_caps_monitor_local_minimum_free_size_stop();
    ESP_LOGD(TAG, "%s %s -> %d (%u B), heap %u free before, %u at the low point (-%u)", method, url, *status,
             (unsigned)got, (unsigned)free0, (unsigned)low, (unsigned)(free0 - low));
    if (err != ESP_OK && err != ESP_ERR_NO_MEM) {
        ESP_LOGW(TAG, "%s %s failed: %s", method, url, esp_err_to_name(err));
    }
    return err;
}

esp_err_t os_http_download(const char *url, const char *bearer, const char *path, size_t max,
                           const char *header, char *hdr_out, size_t hdr_cap, int *status)
{
    header_ctx_t hdr = { .name = header, .out = hdr_out, .cap = hdr_cap };
    if (hdr_out && hdr_cap) {
        hdr_out[0] = 0;
    }
    esp_http_client_handle_t c;
    esp_err_t err = open_request(&c, "GET", url, bearer, NULL, header ? &hdr : NULL, status);
    if (err == ESP_OK && *status / 100 == 2) {
        char *buf = malloc(CHUNK);
        FILE *f = buf ? fopen(path, "wb") : NULL;
        size_t total = 0;
        if (!f) {
            err = buf ? ESP_FAIL : ESP_ERR_NO_MEM;
        }
        while (err == ESP_OK) {
            int n = esp_http_client_read(c, buf, CHUNK);
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
        free(buf);
        if (err == ESP_OK && !esp_http_client_is_complete_data_received(c)) {
            err = ESP_FAIL;
        }
        ESP_LOGD(TAG, "GET %s -> %s: %u B, %s", url, path, (unsigned)total, esp_err_to_name(err));
    }
    close_request(c);
    if (err != ESP_OK && err != ESP_ERR_NO_MEM) {
        ESP_LOGW(TAG, "download %s failed: %s", url, esp_err_to_name(err));
    }
    return err;
}
