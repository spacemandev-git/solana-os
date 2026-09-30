// os_http on the badge: one blocking esp_http_client request per call, no
// keep-alive. HTTPS verifies against the IDF certificate bundle. RAM is the
// constraint here, not speed: small client buffers, and a TLS request is
// refused up front when the heap could not hold a handshake and still leave
// ~24 KB for Wi-Fi and apps (it would fail half-way or starve the rest).

#include <string.h>
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "os_http.h"

static const char *TAG = "os_http";

#define TIMEOUT_MS      10000
#define TLS_MIN_FREE    (56 * 1024)     // a handshake costs ~26-32 KB (dynamic mbedTLS buffers)
#define TLS_MIN_BLOCK   (20 * 1024)     // the 16 KB incoming record buffer must fit in one piece

esp_err_t os_http_request(const char *method, const char *url, const char *bearer, const char *body,
                          char *resp, size_t cap, int *status)
{
    *status = 0;
    if (cap) {
        resp[0] = 0;
    }
    bool tls = !strncmp(url, "https://", 8);
    size_t free0 = heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
    if (tls && (free0 < TLS_MIN_FREE || heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT) < TLS_MIN_BLOCK)) {
        ESP_LOGW(TAG, "not enough heap for TLS (%u free)", (unsigned)free0);
        return ESP_ERR_NO_MEM;
    }

    heap_caps_monitor_local_minimum_free_size_start();
    esp_http_client_config_t cfg = {
        .url = url,
        .method = strcmp(method, "POST") ? HTTP_METHOD_GET : HTTP_METHOD_POST,
        .timeout_ms = TIMEOUT_MS,
        .buffer_size = 1024,
        .buffer_size_tx = 768,
        .crt_bundle_attach = tls ? esp_crt_bundle_attach : NULL,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) {
        heap_caps_monitor_local_minimum_free_size_stop();
        return ESP_ERR_NO_MEM;
    }
    int len = body ? strlen(body) : 0;
    if (body) {
        esp_http_client_set_header(c, "Content-Type", "application/json");
    }
    if (bearer && bearer[0]) {
        char auth[160];
        snprintf(auth, sizeof(auth), "Bearer %s", bearer);
        esp_http_client_set_header(c, "Authorization", auth);
    }

    esp_err_t err = esp_http_client_open(c, len);
    for (int off = 0; err == ESP_OK && off < len;) {
        int n = esp_http_client_write(c, body + off, len - off);
        if (n <= 0) {
            err = ESP_FAIL;
        }
        off += n;
    }
    if (err == ESP_OK && esp_http_client_fetch_headers(c) < 0) {
        err = ESP_FAIL;
    }
    size_t got = 0;
    if (err == ESP_OK) {
        *status = esp_http_client_get_status_code(c);
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
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    // What this request cost: the heap low-water mark while it ran.
    size_t low = heap_caps_get_minimum_free_size(MALLOC_CAP_DEFAULT);
    heap_caps_monitor_local_minimum_free_size_stop();
    ESP_LOGD(TAG, "%s %s -> %d (%u B), heap %u free before, %u at the low point (-%u)", method, url, *status,
             (unsigned)got, (unsigned)free0, (unsigned)low, (unsigned)(free0 - low));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "%s %s failed: %s", method, url, esp_err_to_name(err));
    }
    return err;
}
