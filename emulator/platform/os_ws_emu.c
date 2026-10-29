// Emulator counterpart of core/os_ws.c: the browser's WebSocket. Its events
// land in a JS queue; the calling task polls it in vTaskDelay, like
// os_http_emu.c, so the rest of the emulated badge keeps running. A browser
// opens a WebSocket to any host (no CORS), but says nothing about why one
// failed, and closes only with 1000 or 3000-4999 from our side. Memory is
// never short here.

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <emscripten.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "core/os_ws.h"

static const char *TAG = "os_ws";

#define TIMEOUT_MS  10000
#define POLL_MS     20

static int s_id;

EM_JS(int, ws_js_open, (const char *url, const char *protocols), {
    const id = (Module.wsNext = (Module.wsNext || 0) + 1);
    const s = { q: [], state: 0, code: 1006 };   // state: 0 connecting, 1 open, 2 closed
    (Module.ws = Module.ws || new Map()).set(id, s);
    try {
        const p = protocols ? UTF8ToString(protocols).split(",").map((x) => x.trim()).filter(Boolean) : [];
        const w = new WebSocket(UTF8ToString(url), p);
        w.binaryType = "arraybuffer";
        w.onopen = () => { s.state = 1; };
        w.onmessage = (e) => {
            s.q.push(typeof e.data === "string" ? new TextEncoder().encode(e.data) : new Uint8Array(e.data));
        };
        w.onclose = (e) => { s.state = 2; s.code = e.code; };
        s.w = w;
    } catch (e) {
        s.state = 2;
    }
    return id;
});

EM_JS(int, ws_js_state, (int id), {
    const s = Module.ws && Module.ws.get(id);
    return s ? s.state : 2;
});

// The next message's length + 1, or 0 if none is waiting.
EM_JS(int, ws_js_peek, (int id), {
    const s = Module.ws && Module.ws.get(id);
    return s && s.q.length ? s.q[0].length + 1 : 0;
});

EM_JS(void, ws_js_take, (int id, uint8_t *out), {
    const m = Module.ws.get(id).q.shift();
    HEAPU8.set(m, out);
});

EM_JS(int, ws_js_code, (int id), {
    const s = Module.ws && Module.ws.get(id);
    return s ? s.code : 1006;
});

EM_JS(int, ws_js_send, (int id, const uint8_t *data, int len), {
    const s = Module.ws && Module.ws.get(id);
    if (!s || s.state !== 1) return -1;
    s.w.send(new TextDecoder().decode(HEAPU8.slice(data, data + len)));
    return 0;
});

EM_JS(void, ws_js_close, (int id, int code), {
    const s = Module.ws && Module.ws.get(id);
    if (!s) return;
    Module.ws.delete(id);
    try {
        if (s.w) s.w.close(code === 1000 || (code >= 3000 && code <= 4999) ? code : 1000);
    } catch (e) {}
});

esp_err_t os_ws_connect(const char *url, const char *protocols)
{
    os_ws_close(0);
    if (!url || strncmp(url, "wss://", 6)) {
        return ESP_ERR_INVALID_ARG;
    }
    s_id = ws_js_open(url, protocols && protocols[0] ? protocols : NULL);
    for (int ms = 0; ws_js_state(s_id) == 0 && ms < TIMEOUT_MS; ms += POLL_MS) {
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
    if (ws_js_state(s_id) != 1) {
        ESP_LOGW(TAG, "no connection to %s", url);
        os_ws_close(0);
        return ESP_FAIL;
    }
    return ESP_OK;
}

int os_ws_recv(char **out, size_t *len, size_t max, int timeout_ms, int *code)
{
    *out = NULL;
    *len = 0;
    for (int ms = 0;; ms += POLL_MS) {
        int n = s_id ? ws_js_peek(s_id) : 0;
        if (n) {
            size_t bytes = n - 1;
            if (bytes > max) {
                return OS_WS_TOO_BIG;
            }
            char *m = malloc(bytes + 1);
            if (!m) {
                return OS_WS_LOST;
            }
            ws_js_take(s_id, (uint8_t *)m);
            m[bytes] = 0;
            *out = m;
            *len = bytes;
            return OS_WS_MSG;
        }
        // Messages that came before the close are read first, as on the badge.
        if (!s_id || ws_js_state(s_id) == 2) {
            *code = s_id ? ws_js_code(s_id) : 1006;
            return *code == 1006 ? OS_WS_LOST : OS_WS_CLOSED;
        }
        if (ms >= timeout_ms) {
            return OS_WS_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
}

esp_err_t os_ws_send(char *text, size_t len)
{
    return s_id && ws_js_send(s_id, (const uint8_t *)text, (int)len) == 0 ? ESP_OK : ESP_FAIL;
}

void os_ws_close(int code)
{
    if (s_id) {
        ws_js_close(s_id, code);
        s_id = 0;
    }
}
