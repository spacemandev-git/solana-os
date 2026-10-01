// Emulator counterpart of core/os_http.c: os_http_request() and
// os_http_download() over the browser's fetch(). The request runs in JS while the calling task polls in
// vTaskDelay, so the rest of the emulated badge keeps running (a blocking
// call on the badge blocks only its own task, too). Same-origin requests
// only need no CORS setup: the emulator talks to the server that serves it.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <emscripten.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "core/os_http.h"

static const char *TAG = "os_http";

// Requests give up after 10 s, like the badge's esp_http_client timeout.

EM_JS(int, http_js_start, (const char *method, const char *url, const char *bearer, const char *body), {
    const id = (Module.httpNext = (Module.httpNext || 0) + 1);
    const req = { done: false, status: 0, text: "", failed: false };
    (Module.http = Module.http || new Map()).set(id, req);
    const headers = { 'Content-Type': 'application/json' };
    if (bearer) headers['Authorization'] = 'Bearer ' + UTF8ToString(bearer);
    const opts = { method: UTF8ToString(method), headers, credentials: 'omit', cache: 'no-store' };
    if (body) opts.body = UTF8ToString(body);
    if (typeof AbortSignal !== 'undefined' && AbortSignal.timeout) opts.signal = AbortSignal.timeout(10000);
    fetch(new URL(UTF8ToString(url), location.href), opts)
        .then((r) => r.text().then((t) => { req.status = r.status; req.text = t; req.done = true; }))
        .catch(() => { req.failed = true; req.done = true; });
    return id;
});

// 0 = pending, 1 = response in resp/status, -1 = failed.
EM_JS(int, http_js_poll, (int id, char *resp, int cap, int *status), {
    const req = Module.http.get(id);
    if (!req || !req.done) return req ? 0 : -1;
    Module.http.delete(id);
    if (req.failed) return -1;
    stringToUTF8(req.text, resp, cap);
    HEAP32[status >> 2] = req.status;
    return 1;
});

esp_err_t os_http_request(const char *method, const char *url, const char *bearer, const char *body,
                          char *resp, size_t cap, int *status)
{
    if (!resp || !cap || !status) {
        return ESP_ERR_INVALID_ARG;
    }
    resp[0] = 0;
    *status = 0;
    int id = http_js_start(method, url, bearer, body);
    int rc;
    while ((rc = http_js_poll(id, resp, (int)cap, status)) == 0) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (rc < 0) {
        ESP_LOGW(TAG, "%s %s failed", method, url);
        return ESP_FAIL;
    }
    return ESP_OK;
}

// Downloads keep the body as bytes plus one response header.
EM_JS(int, http_js_download, (const char *url, const char *bearer, const char *header), {
    const id = (Module.httpNext = (Module.httpNext || 0) + 1);
    const req = { done: false, status: 0, bytes: null, hdr: "", failed: false };
    (Module.http = Module.http || new Map()).set(id, req);
    const headers = {};
    if (bearer) headers["Authorization"] = "Bearer " + UTF8ToString(bearer);
    const name = header ? UTF8ToString(header) : "";
    const opts = { method: "GET", headers, credentials: "omit", cache: "no-store" };
    if (typeof AbortSignal !== "undefined" && AbortSignal.timeout) opts.signal = AbortSignal.timeout(10000);
    fetch(new URL(UTF8ToString(url), location.href), opts)
        .then((r) => r.arrayBuffer().then((b) => {
            req.status = r.status;
            req.bytes = new Uint8Array(b);
            req.hdr = (name && r.headers.get(name)) || "";
            req.done = true;
        }))
        .catch(() => { req.failed = true; req.done = true; });
    return id;
});

// 0 = pending, -1 = failed, else 1 + body length (status and header filled in).
EM_JS(int, http_js_download_poll, (int id, int *status, char *hdr, int hdr_cap), {
    const req = Module.http.get(id);
    if (!req) return -1;
    if (!req.done) return 0;
    if (req.failed) { Module.http.delete(id); return -1; }
    HEAP32[status >> 2] = req.status;
    if (hdr && hdr_cap) stringToUTF8(req.hdr, hdr, hdr_cap);
    return 1 + req.bytes.length;
});

EM_JS(void, http_js_download_take, (int id, uint8_t *out, int len), {
    const req = Module.http.get(id);
    Module.http.delete(id);
    if (req && out) HEAPU8.set(req.bytes.subarray(0, len), out);
});

esp_err_t os_http_download(const char *url, const char *bearer, const char *path, size_t max,
                           const char *header, char *hdr_out, size_t hdr_cap, int *status)
{
    *status = 0;
    if (hdr_out && hdr_cap) {
        hdr_out[0] = 0;
    }
    int id = http_js_download(url, bearer, header);
    int rc;
    while ((rc = http_js_download_poll(id, status, hdr_out, hdr_out ? (int)hdr_cap : 0)) == 0) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (rc < 0) {
        ESP_LOGW(TAG, "GET %s failed", url);
        return ESP_FAIL;
    }
    size_t len = rc - 1;
    if (*status / 100 != 2) {
        http_js_download_take(id, NULL, 0);
        return ESP_OK;
    }
    if (len > max) {
        http_js_download_take(id, NULL, 0);
        return ESP_ERR_INVALID_SIZE;
    }
    uint8_t *buf = malloc(len ? len : 1);
    if (!buf) {
        http_js_download_take(id, NULL, 0);
        return ESP_ERR_NO_MEM;
    }
    http_js_download_take(id, buf, len);
    FILE *f = fopen(path, "wb");
    esp_err_t err = f && fwrite(buf, 1, len, f) == len ? ESP_OK : ESP_FAIL;
    if (f && fclose(f) != 0) {
        err = ESP_FAIL;
    }
    free(buf);
    return err;
}
