// ESP-IDF system services for the emulator: log, errors, esp_timer, identity
// (MAC, app description, chip), heap figures, random, base64 and NVS.
//
// Values describe the emulator honestly where firmware prints them: the IDF
// version reads "browser emulator", the MAC is a random locally administered
// address, heap figures are the wasm heap.

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <unistd.h>
#include <emscripten.h>
#include <emscripten/heap.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_mac.h"
#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_idf_version.h"
#include "esp_system.h"
#include "esp_random.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "mbedtls/base64.h"
#include "emu.h"

// ---------------------------------------------------------------------------
// Time

int64_t emu_now_us(void)
{
    static double t0 = -1;
    static int64_t last;
    double now = emscripten_get_now();
    if (t0 < 0) {
        t0 = now;
    }
    int64_t us = (int64_t)((now - t0) * 1000.0);
    // Browsers coarsen performance.now(); never return the same value twice
    // so firmware that divides by elapsed time stays safe.
    if (us <= last) {
        us = last + 1;
    }
    last = us;
    return us;
}

int64_t esp_timer_get_time(void)
{
    return emu_now_us();
}

// ---------------------------------------------------------------------------
// Log

#define LOG_TAGS 16
static struct { char tag[16]; esp_log_level_t level; } s_levels[LOG_TAGS];
static int s_nlevels;
static esp_log_level_t s_default_level = ESP_LOG_INFO;

void esp_log_level_set(const char *tag, esp_log_level_t level)
{
    if (!strcmp(tag, "*")) {
        s_default_level = level;
        s_nlevels = 0;
        return;
    }
    for (int i = 0; i < s_nlevels; i++) {
        if (!strcmp(s_levels[i].tag, tag)) {
            s_levels[i].level = level;
            return;
        }
    }
    if (s_nlevels < LOG_TAGS) {
        strncpy(s_levels[s_nlevels].tag, tag, sizeof(s_levels[0].tag) - 1);
        s_levels[s_nlevels++].level = level;
    }
}

uint32_t esp_log_timestamp(void)
{
    return (uint32_t)(emu_now_us() / 1000);
}

void esp_log_write(esp_log_level_t level, const char *tag, const char *fmt, ...)
{
    esp_log_level_t max = s_default_level;
    for (int i = 0; i < s_nlevels; i++) {
        if (!strcmp(s_levels[i].tag, tag)) {
            max = s_levels[i].level;
        }
    }
    if (level > max) {
        return;
    }
    static const char letters[] = "NEWIDV";
    char msg[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    printf("%c (%lu) %s: %s\n", letters[level], (unsigned long)esp_log_timestamp(), tag, msg);
}

const char *esp_err_to_name(esp_err_t code)
{
    switch (code) {
    case ESP_OK:                    return "ESP_OK";
    case ESP_FAIL:                  return "ESP_FAIL";
    case ESP_ERR_NO_MEM:            return "ESP_ERR_NO_MEM";
    case ESP_ERR_INVALID_ARG:       return "ESP_ERR_INVALID_ARG";
    case ESP_ERR_INVALID_STATE:     return "ESP_ERR_INVALID_STATE";
    case ESP_ERR_INVALID_SIZE:      return "ESP_ERR_INVALID_SIZE";
    case ESP_ERR_NOT_FOUND:         return "ESP_ERR_NOT_FOUND";
    case ESP_ERR_NOT_SUPPORTED:     return "ESP_ERR_NOT_SUPPORTED";
    case ESP_ERR_TIMEOUT:           return "ESP_ERR_TIMEOUT";
    case ESP_ERR_INVALID_RESPONSE:  return "ESP_ERR_INVALID_RESPONSE";
    case ESP_ERR_NOT_ALLOWED:       return "ESP_ERR_NOT_ALLOWED";
    case ESP_ERR_NVS_NOT_FOUND:     return "ESP_ERR_NVS_NOT_FOUND";
    case ESP_ERR_NVS_INVALID_LENGTH: return "ESP_ERR_NVS_INVALID_LENGTH";
    case ESP_ERR_NVS_TYPE_MISMATCH: return "ESP_ERR_NVS_TYPE_MISMATCH";
    case ESP_ERR_NVS_INVALID_HANDLE: return "ESP_ERR_NVS_INVALID_HANDLE";
    case ESP_ERR_NVS_READ_ONLY:     return "ESP_ERR_NVS_READ_ONLY";
    default:                        return "ERROR";
    }
}

// ---------------------------------------------------------------------------
// Identity: one random 48-bit value in localStorage.

EM_JS(void, emu_js_identity, (uint8_t *out), {
    const KEY = 'solana-os-emu:id';
    let id = null;
    try { id = localStorage.getItem(KEY); } catch (e) {}
    if (!id || !/^[0-9a-f]{12}$/.test(id)) {
        const b = new Uint8Array(6);
        crypto.getRandomValues(b);
        id = Array.from(b, (x) => x.toString(16).padStart(2, '0')).join("");
        try { localStorage.setItem(KEY, id); } catch (e) {}
    }
    for (let i = 0; i < 6; i++) HEAPU8[out + i] = parseInt(id.substr(i * 2, 2), 16);
});

// os_cloud.c sends "emu-" + this MAC as the hardware ID (OS_CLOUD_HW_PREFIX).
static uint8_t s_mac[6];
static bool s_have_mac;

const uint8_t *emu_mac(void)
{
    if (!s_have_mac) {
        emu_js_identity(s_mac);
        s_mac[0] = (s_mac[0] & 0xFC) | 0x02;    // unicast, locally administered: never a real vendor MAC
        s_have_mac = true;
    }
    return s_mac;
}

esp_err_t esp_efuse_mac_get_default(uint8_t *mac)
{
    memcpy(mac, emu_mac(), 6);
    return ESP_OK;
}

esp_err_t esp_read_mac(uint8_t *mac, esp_mac_type_t type)
{
    memcpy(mac, emu_mac(), 6);
    if (type == ESP_MAC_BT) {
        mac[5] += 2;                        // same offsets as the badge's eFuse scheme
    } else if (type == ESP_MAC_WIFI_SOFTAP) {
        mac[5] += 1;
    }
    return ESP_OK;
}

const esp_app_desc_t *esp_app_get_description(void)
{
    static esp_app_desc_t d;
    if (!d.version[0]) {
        strncpy(d.version, CONFIG_APP_PROJECT_VER, sizeof(d.version) - 1);
        strncpy(d.project_name, "solana_os", sizeof(d.project_name) - 1);
        strncpy(d.date, __DATE__, sizeof(d.date) - 1);
        strncpy(d.time, __TIME__, sizeof(d.time) - 1);
        strncpy(d.idf_ver, esp_get_idf_version(), sizeof(d.idf_ver) - 1);
    }
    return &d;
}

const char *esp_get_idf_version(void)
{
    return "browser emulator";
}

void esp_chip_info(esp_chip_info_t *out)
{
    // The chip being emulated (see board README): ESP32-C5 rev 1.2, one core.
    out->model = CHIP_ESP32C5;
    out->features = CHIP_FEATURE_WIFI_BGN | CHIP_FEATURE_BLE | CHIP_FEATURE_IEEE802154;
    out->revision = 102;
    out->cores = 1;
}

esp_flash_t *esp_flash_default_chip;

esp_err_t esp_flash_get_size(esp_flash_t *chip, uint32_t *out_size)
{
    (void)chip;
    *out_size = 16 * 1024 * 1024;
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// Heap: real wasm figures.

static size_t s_min_free = SIZE_MAX;

size_t heap_caps_get_free_size(uint32_t caps)
{
    (void)caps;
    struct mallinfo mi = mallinfo();
    size_t used_top = (size_t)sbrk(0);
    size_t total = emscripten_get_heap_size();
    size_t free_b = mi.fordblks + (total > used_top ? total - used_top : 0);
    if (free_b < s_min_free) {
        s_min_free = free_b;
    }
    return free_b;
}

size_t heap_caps_get_minimum_free_size(uint32_t caps)
{
    heap_caps_get_free_size(caps);
    return s_min_free;
}

size_t heap_caps_get_largest_free_block(uint32_t caps)
{
    return heap_caps_get_free_size(caps);
}

size_t heap_caps_get_total_size(uint32_t caps)
{
    (void)caps;
    return emscripten_get_heap_size();
}

// ---------------------------------------------------------------------------
// System

EM_JS(int, emu_js_reset_was_sw, (void), { return Module.resetReason === 'sw' ? 1 : 0; });
EM_JS(void, emu_js_restart, (void), { if (Module.onRestart) setTimeout(() => Module.onRestart(), 0); });

esp_reset_reason_t esp_reset_reason(void)
{
    return emu_js_reset_was_sw() ? ESP_RST_SW : ESP_RST_POWERON;
}

void esp_restart(void)
{
    ESP_LOGI("system", "restart requested");
    emu_js_restart();       // the page tears this instance down and boots a new one
    for (;;) {
        vTaskDelay(portMAX_DELAY);
    }
}

EM_JS(uint32_t, emu_js_random, (void), {
    const b = new Uint32Array(1);
    crypto.getRandomValues(b);
    return b[0];
});

uint32_t esp_random(void)
{
    return emu_js_random();
}

void esp_fill_random(void *buf, size_t len)
{
    uint8_t *p = buf;
    while (len) {
        uint32_t r = esp_random();
        size_t n = len < 4 ? len : 4;
        memcpy(p, &r, n);
        p += n;
        len -= n;
    }
}

// ---------------------------------------------------------------------------
// esp_timer: callbacks run on an "esp_timer" task, as on the badge.

#define TIMER_MAX 16
struct emu_timer {
    esp_timer_create_args_t args;
    int64_t due, period;
    bool active, used;
};
static struct emu_timer s_timers[TIMER_MAX];
static TaskHandle_t s_timer_task;

static void timer_task(void *arg)
{
    for (;;) {
        int64_t now = emu_now_us(), next = INT64_MAX;
        for (int i = 0; i < TIMER_MAX; i++) {
            struct emu_timer *t = &s_timers[i];
            if (!t->used || !t->active) {
                continue;
            }
            if (now >= t->due) {
                if (t->period) {
                    t->due += t->period;
                    if (t->due < now) {
                        t->due = now + t->period;
                    }
                } else {
                    t->active = false;
                }
                t->args.callback(t->args.arg);
                now = emu_now_us();
            }
            if (t->active && t->due < next) {
                next = t->due;
            }
        }
        int64_t wait_ms = next == INT64_MAX ? 1000 : (next - now + 999) / 1000;
        ulTaskNotifyTake(pdTRUE, wait_ms > 0 ? pdMS_TO_TICKS(wait_ms) + 1 : 1);
    }
}

esp_err_t esp_timer_create(const esp_timer_create_args_t *args, esp_timer_handle_t *out)
{
    for (int i = 0; i < TIMER_MAX; i++) {
        if (!s_timers[i].used) {
            s_timers[i] = (struct emu_timer){ .args = *args, .used = true };
            *out = &s_timers[i];
            if (!s_timer_task) {
                xTaskCreate(timer_task, "esp_timer", 4096, NULL, 22, &s_timer_task);
            }
            return ESP_OK;
        }
    }
    return ESP_ERR_NO_MEM;
}

static esp_err_t timer_start(esp_timer_handle_t t, uint64_t us, bool periodic)
{
    if (!t || t->active) {
        return ESP_ERR_INVALID_STATE;
    }
    t->due = emu_now_us() + us;
    t->period = periodic ? us : 0;
    t->active = true;
    xTaskNotifyGive(s_timer_task);
    return ESP_OK;
}

esp_err_t esp_timer_start_once(esp_timer_handle_t t, uint64_t us)
{
    return timer_start(t, us, false);
}

esp_err_t esp_timer_start_periodic(esp_timer_handle_t t, uint64_t us)
{
    return timer_start(t, us, true);
}

esp_err_t esp_timer_stop(esp_timer_handle_t t)
{
    if (!t || !t->active) {
        return ESP_ERR_INVALID_STATE;
    }
    t->active = false;
    return ESP_OK;
}

esp_err_t esp_timer_delete(esp_timer_handle_t t)
{
    if (!t) {
        return ESP_ERR_INVALID_ARG;
    }
    t->used = t->active = false;
    return ESP_OK;
}

bool esp_timer_is_active(esp_timer_handle_t t)
{
    return t && t->active;
}

// ---------------------------------------------------------------------------
// base64 (mbedtls API)

static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int mbedtls_base64_encode(unsigned char *dst, size_t dlen, size_t *olen, const unsigned char *src, size_t slen)
{
    size_t need = (slen + 2) / 3 * 4;
    if (!dst || dlen < need + 1) {
        *olen = need + 1;
        return MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL;
    }
    size_t o = 0;
    for (size_t i = 0; i < slen; i += 3) {
        uint32_t v = src[i] << 16;
        if (i + 1 < slen) {
            v |= src[i + 1] << 8;
        }
        if (i + 2 < slen) {
            v |= src[i + 2];
        }
        dst[o++] = b64[(v >> 18) & 63];
        dst[o++] = b64[(v >> 12) & 63];
        dst[o++] = i + 1 < slen ? b64[(v >> 6) & 63] : '=';
        dst[o++] = i + 2 < slen ? b64[v & 63] : '=';
    }
    dst[o] = 0;
    *olen = o;
    return 0;
}

int mbedtls_base64_decode(unsigned char *dst, size_t dlen, size_t *olen, const unsigned char *src, size_t slen)
{
    uint32_t acc = 0;
    int bits = 0;
    size_t o = 0;
    for (size_t i = 0; i < slen; i++) {
        unsigned char c = src[i];
        if (c == '=' || c == '\r' || c == '\n' || c == ' ') {
            continue;
        }
        const char *p = strchr(b64, c);
        if (!p || !c) {
            return MBEDTLS_ERR_BASE64_INVALID_CHARACTER;
        }
        acc = (acc << 6) | (uint32_t)(p - b64);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (dst && o < dlen) {
                dst[o] = (acc >> bits) & 0xFF;
            }
            o++;
        }
    }
    *olen = o;
    return !dst || o > dlen ? MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL : 0;
}

// ---------------------------------------------------------------------------
// NVS: namespaces of typed values in localStorage ("solana-os-emu:nvs").

EM_JS(void, nvs_js_load, (void), {
    if (Module.nvs) return;
    Module.nvs = {};
    try {
        const raw = localStorage.getItem('solana-os-emu:nvs');
        if (raw) Module.nvs = JSON.parse(raw) || {};
    } catch (e) {}
    Module.nvsSave = () => {
        try { localStorage.setItem('solana-os-emu:nvs', JSON.stringify(Module.nvs)); } catch (e) {}
    };
});

EM_JS(int, nvs_js_has_ns, (const char *ns), { return Module.nvs[UTF8ToString(ns)] ? 1 : 0; });

// Returns the value length, -1 if missing, -2 on a type mismatch. Copies up to cap bytes.
EM_JS(int, nvs_js_get, (const char *ns, const char *key, int type, uint8_t *out, int cap), {
    const n = Module.nvs[UTF8ToString(ns)];
    const v = n && n[UTF8ToString(key)];
    if (!v) return -1;
    if (v.t !== type) return -2;
    const len = v.h.length / 2;
    for (let i = 0; out && i < len && i < cap; i++) HEAPU8[out + i] = parseInt(v.h.substr(i * 2, 2), 16);
    return len;
});

EM_JS(void, nvs_js_set, (const char *ns, const char *key, int type, const uint8_t *val, int len), {
    const name = UTF8ToString(ns);
    const n = (Module.nvs[name] = Module.nvs[name] || {});
    let h = "";
    for (let i = 0; i < len; i++) h += HEAPU8[val + i].toString(16).padStart(2, '0');
    n[UTF8ToString(key)] = { t: type, h };
    Module.nvsSave();
});

EM_JS(int, nvs_js_erase, (const char *ns, const char *key), {
    const name = UTF8ToString(ns);
    const n = Module.nvs[name];
    if (!n) return -1;
    if (key) {
        const k = UTF8ToString(key);
        if (!(k in n)) return -1;
        delete n[k];
    } else {
        delete Module.nvs[name];
    }
    Module.nvsSave();
    return 0;
});

EM_JS(void, nvs_js_erase_everything, (void), {
    Module.nvs = {};
    Module.nvsSave();
});

#define NVS_HANDLES 16
enum { T_STR = 1, T_BLOB, T_I8, T_U8, T_I16, T_U16, T_I32, T_U32, T_I64, T_U64 };
static struct { char ns[16]; bool used, rw; } s_handles[NVS_HANDLES];
static bool s_nvs_ready;

esp_err_t nvs_flash_init(void)
{
    nvs_js_load();
    s_nvs_ready = true;
    return ESP_OK;
}

esp_err_t nvs_flash_erase(void)
{
    nvs_js_load();
    nvs_js_erase_everything();
    return ESP_OK;
}

esp_err_t nvs_open(const char *ns, nvs_open_mode_t mode, nvs_handle_t *out)
{
    if (!s_nvs_ready) {
        return ESP_ERR_NVS_NOT_INITIALIZED;
    }
    if (!ns || strlen(ns) > 15) {
        return ESP_ERR_NVS_INVALID_NAME;
    }
    if (mode == NVS_READONLY && !nvs_js_has_ns(ns)) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    for (int i = 0; i < NVS_HANDLES; i++) {
        if (!s_handles[i].used) {
            s_handles[i].used = true;
            s_handles[i].rw = mode == NVS_READWRITE;
            strcpy(s_handles[i].ns, ns);
            *out = i + 1;
            return ESP_OK;
        }
    }
    return ESP_ERR_NO_MEM;
}

static const char *handle_ns(nvs_handle_t h, bool write, esp_err_t *err)
{
    if (h < 1 || h > NVS_HANDLES || !s_handles[h - 1].used) {
        *err = ESP_ERR_NVS_INVALID_HANDLE;
        return NULL;
    }
    if (write && !s_handles[h - 1].rw) {
        *err = ESP_ERR_NVS_READ_ONLY;
        return NULL;
    }
    return s_handles[h - 1].ns;
}

void nvs_close(nvs_handle_t h)
{
    if (h >= 1 && h <= NVS_HANDLES) {
        s_handles[h - 1].used = false;
    }
}

esp_err_t nvs_commit(nvs_handle_t h)
{
    esp_err_t err = ESP_OK;
    return handle_ns(h, false, &err) ? ESP_OK : err;
}

esp_err_t nvs_erase_key(nvs_handle_t h, const char *key)
{
    esp_err_t err = ESP_OK;
    const char *ns = handle_ns(h, true, &err);
    if (!ns) {
        return err;
    }
    return nvs_js_erase(ns, key) == 0 ? ESP_OK : ESP_ERR_NVS_NOT_FOUND;
}

esp_err_t nvs_erase_all(nvs_handle_t h)
{
    esp_err_t err = ESP_OK;
    const char *ns = handle_ns(h, true, &err);
    if (!ns) {
        return err;
    }
    nvs_js_erase(ns, NULL);
    return ESP_OK;
}

static esp_err_t set_raw(nvs_handle_t h, const char *key, int type, const void *v, size_t len)
{
    esp_err_t err = ESP_OK;
    const char *ns = handle_ns(h, true, &err);
    if (!ns) {
        return err;
    }
    if (!key || strlen(key) > 15) {
        return ESP_ERR_NVS_INVALID_NAME;
    }
    nvs_js_set(ns, key, type, v, (int)len);
    return ESP_OK;
}

static esp_err_t get_raw(nvs_handle_t h, const char *key, int type, void *out, size_t *len)
{
    esp_err_t err = ESP_OK;
    const char *ns = handle_ns(h, false, &err);
    if (!ns) {
        return err;
    }
    int n = nvs_js_get(ns, key, type, NULL, 0);
    if (n == -1) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    if (n == -2) {
        return ESP_ERR_NVS_TYPE_MISMATCH;
    }
    if (!out) {
        *len = n;
        return ESP_OK;
    }
    if (*len < (size_t)n) {
        return ESP_ERR_NVS_INVALID_LENGTH;
    }
    nvs_js_get(ns, key, type, out, n);
    *len = n;
    return ESP_OK;
}

esp_err_t nvs_set_str(nvs_handle_t h, const char *key, const char *value)
{
    return set_raw(h, key, T_STR, value, strlen(value) + 1);
}

esp_err_t nvs_get_str(nvs_handle_t h, const char *key, char *out, size_t *len)
{
    return get_raw(h, key, T_STR, out, len);
}

esp_err_t nvs_set_blob(nvs_handle_t h, const char *key, const void *value, size_t len)
{
    return set_raw(h, key, T_BLOB, value, len);
}

esp_err_t nvs_get_blob(nvs_handle_t h, const char *key, void *out, size_t *len)
{
    return get_raw(h, key, T_BLOB, out, len);
}

#define NVS_INT(suffix, ctype, tcode)                                               \
    esp_err_t nvs_set_##suffix(nvs_handle_t h, const char *key, ctype v)            \
    {                                                                               \
        return set_raw(h, key, tcode, &v, sizeof(v));                               \
    }                                                                               \
    esp_err_t nvs_get_##suffix(nvs_handle_t h, const char *key, ctype *v)           \
    {                                                                               \
        size_t len = sizeof(*v);                                                    \
        return get_raw(h, key, tcode, v, &len);                                     \
    }

NVS_INT(i8, int8_t, T_I8)
NVS_INT(u8, uint8_t, T_U8)
NVS_INT(i16, int16_t, T_I16)
NVS_INT(u16, uint16_t, T_U16)
NVS_INT(i32, int32_t, T_I32)
NVS_INT(u32, uint32_t, T_U32)
NVS_INT(i64, int64_t, T_I64)
NVS_INT(u64, uint64_t, T_U64)
