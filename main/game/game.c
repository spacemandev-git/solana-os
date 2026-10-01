// Shared plumbing for the HH GtS services, see game.h.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "bsp.h"
#include "core/os.h"
#include "core/os_cloud.h"
#include "core/os_data.h"
#include "core/ui.h"
#include "game.h"

static const char *TAG = "game";

static esp_timer_handle_t s_tick_timer, s_led_timer;
static volatile bool s_tick_queued;
static char s_id[OS_MESH_ID_LEN + 1];
static bool s_claimed;

// What the mesh task lets through (see on_mesh), under s_filter_mux: the mesh
// task reads it, the OS task writes it.
#define HOPS_MAX    4               // messages per type waiting for the OS task
static portMUX_TYPE s_filter_mux = portMUX_INITIALIZER_UNLOCKED;
typedef struct {
    char id[OS_MESH_ID_LEN + 1];
    char tid[17];
    char beacon[OS_MESH_ID_LEN + 1];
    int rid;
} filter_t;
static filter_t s_filter;
static int s_hops[OS_MESH_TYPE_MAX];

int64_t game_now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

// ---------------------------------------------------------------------------
// Tick and mesh hop: both land on the OS task.

static void tick(void *arg)
{
    s_tick_queued = false;
    os_cloud_info_t *in = malloc(sizeof(*in));
    if (in) {
        os_cloud_get_info(in);
        strlcpy(s_id, in->id, sizeof(s_id));
        s_claimed = in->claimed;
        portENTER_CRITICAL(&s_filter_mux);
        strlcpy(s_filter.id, s_id, sizeof(s_filter.id));
        portEXIT_CRITICAL(&s_filter_mux);
        free(in);
    }
    int64_t now = game_now_ms();
    xdex_tick(now);
    trade_tick(now);
    quests_tick(now);
}

static void tick_timer(void *arg)
{
    if (!s_tick_queued) {
        s_tick_queued = true;
        if (!os_post(tick, NULL)) {
            s_tick_queued = false;      // OS not started yet, or its queue is full
        }
    }
}

typedef struct {
    os_mesh_type_t type;
    os_mesh_peer_t from;
    char json[];
} hop_t;

void game_expect_trade(const char *tid)
{
    portENTER_CRITICAL(&s_filter_mux);
    strlcpy(s_filter.tid, tid ? tid : "", sizeof(s_filter.tid));
    portEXIT_CRITICAL(&s_filter_mux);
}

void game_expect_relay(const char *beacon, int rid)
{
    portENTER_CRITICAL(&s_filter_mux);
    strlcpy(s_filter.beacon, beacon ? beacon : "", sizeof(s_filter.beacon));
    s_filter.rid = rid;
    portEXIT_CRITICAL(&s_filter_mux);
}

static void deliver(void *arg)
{
    hop_t *h = arg;
    portENTER_CRITICAL(&s_filter_mux);
    s_hops[h->type]--;
    portEXIT_CRITICAL(&s_filter_mux);
    cJSON *msg = cJSON_Parse(h->json);
    if (cJSON_IsObject(msg)) {
        switch (h->type) {
        case OS_MESH_XDEX:       xdex_on_mesh(&h->from, msg); break;
        case OS_MESH_TRADE:      trade_on_mesh(&h->from, msg); break;
        case OS_MESH_RELAY_RESP: quests_on_mesh(&h->from, msg); break;
        default: break;
        }
    } else {
        ESP_LOGW(TAG, "bad message from %s", h->from.id);
    }
    cJSON_Delete(msg);
    free(h);
}

// The string value of "key" in a flat JSON object equals want (n chars).
// Escaped quotes inside other strings can't fake the pattern, so a miss is
// a real miss; the OS task parses and checks everything again anyway.
static bool json_has(const char *json, const char *key, const char *want)
{
    char pat[12] = "\"";
    strlcat(pat, key, sizeof(pat));
    strlcat(pat, "\":\"", sizeof(pat));
    const char *p = strstr(json, pat);
    size_t n = strlen(want), at = strlen(pat);
    return p && n && !strncmp(p + at, want, n) && p[at + n] == '"';
}

// Mesh task: drop what isn't for us, copy the rest and hop over to the OS
// task. A flood of messages for other badges (or of fakes) must not fill
// the OS queue that buttons and API answers share, so at most HOPS_MAX per
// type wait there at a time.
static void on_mesh(os_mesh_type_t type, const os_mesh_peer_t *from, const char *json, void *ctx)
{
    filter_t f;
    portENTER_CRITICAL(&s_filter_mux);
    f = s_filter;
    portEXIT_CRITICAL(&s_filter_mux);
    bool want;
    switch (type) {
    case OS_MESH_XDEX:
        want = json_has(json, "to", f.id);
        break;
    case OS_MESH_TRADE:
        // invite / join / decline name us; the rest name the open trade.
        want = strstr(json, "\"to\":\"") ? json_has(json, "to", f.id) : json_has(json, "tid", f.tid);
        break;
    case OS_MESH_RELAY_RESP: {
        const char *rid = strstr(json, "\"rid\":");
        want = rid && f.beacon[0] && !strcmp(from->id, f.beacon) && atoi(rid + 6) == f.rid;
        break;
    }
    default:
        want = false;
        break;
    }
    portENTER_CRITICAL(&s_filter_mux);
    want = want && s_hops[type] < HOPS_MAX;
    if (want) {
        s_hops[type]++;
    }
    portEXIT_CRITICAL(&s_filter_mux);
    if (!want) {
        return;
    }
    size_t len = strlen(json);
    hop_t *h = malloc(sizeof(hop_t) + len + 1);
    if (h) {
        h->type = type;
        h->from = *from;
        memcpy(h->json, json, len + 1);
    }
    if (!h || !os_post(deliver, h)) {
        free(h);
        portENTER_CRITICAL(&s_filter_mux);
        s_hops[type]--;
        portEXIT_CRITICAL(&s_filter_mux);
    }
}

void game_init(void)
{
    xdex_init();
    trade_init();
    quests_init();
    os_mesh_subscribe(OS_MESH_XDEX, on_mesh, NULL);
    os_mesh_subscribe(OS_MESH_TRADE, on_mesh, NULL);
    os_mesh_subscribe(OS_MESH_RELAY_RESP, on_mesh, NULL);
    const esp_timer_create_args_t args = { .callback = tick_timer, .name = "game" };
    ESP_ERROR_CHECK(esp_timer_create(&args, &s_tick_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(s_tick_timer, GAME_TICK_MS * 1000));
}

// ---------------------------------------------------------------------------
// Identity and cards

const char *game_badge_id(void)
{
    return s_id;
}

bool game_claimed(void)
{
    return s_claimed;
}

bool game_can_sign(void)
{
    return s_id[0] && os_mesh_ready();
}

void game_add_card(cJSON *obj)
{
    os_cloud_info_t *in = malloc(sizeof(*in));
    char n[GAME_CARD_NAME] = "";
    int lv = 0;
    if (in) {
        os_cloud_get_info(in);
        if (in->claimed) {
            game_ascii(n, sizeof(n), in->owner);
        }
        lv = in->level;
        free(in);
    }
    cJSON_AddStringToObject(obj, "n", n);
    // os_cloud labels an owner with a linked X account "@username".
    cJSON_AddStringToObject(obj, "x", n[0] == '@' ? n + 1 : "");
    cJSON_AddNumberToObject(obj, "lv", lv);
}

// ---------------------------------------------------------------------------
// Small helpers

const char *game_jstr(const cJSON *obj, const char *key)
{
    const char *s = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(obj, key));
    return s ? s : "";
}

int game_jint(const cJSON *obj, const char *key, int def)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsNumber(v) ? (int)v->valuedouble : def;
}

void game_ascii(char *out, size_t n, const char *in)
{
    size_t j = 0;
    for (const char *p = in ? in : ""; *p && j + 1 < n; p++) {
        if (*p >= 0x20 && *p < 0x7F) {
            out[j++] = *p;
        }
    }
    out[j] = 0;
}

void game_peer_label(const char *name, const char *id, char *out, size_t n)
{
    if (name && name[0]) {
        strlcpy(out, name, n);
    } else {
        snprintf(out, n, "badge %s", id);
    }
}

const char *game_signal(int rssi)
{
    return rssi >= -55 ? "||||" : rssi >= -67 ? "|||." : rssi >= -78 ? "||.." : "|...";
}

uint32_t game_parse_hex(const char *hex, uint32_t def)
{
    if (!hex || strlen(hex) != 6) {
        return def;
    }
    char *end;
    unsigned long v = strtoul(hex, &end, 16);
    return *end ? def : (uint32_t)v;
}

// ---------------------------------------------------------------------------
// Cache: {"at":<unix ms>,"body":<server answer>}

#define CACHE_MAX   (8 * 1024)

bool game_cache_put(const char *name, const char *body)
{
    if (!os_data_ready() || !body) {
        return false;
    }
    char rel[64];
    snprintf(rel, sizeof(rel), "cache/%s.json", name);
    size_t len = strlen(body);
    char *buf = malloc(len + 48);
    if (!buf) {
        return false;
    }
    int n = snprintf(buf, len + 48, "{\"at\":%lld,\"body\":%s}", (long long)os_cloud_time_ms(), body);
    bool ok = os_data_write(rel, buf, n);
    free(buf);
    return ok;
}

cJSON *game_cache_get(const char *name, int64_t *at_ms)
{
    char rel[64];
    snprintf(rel, sizeof(rel), "cache/%s.json", name);
    if (at_ms) {
        *at_ms = 0;
    }
    if (!os_data_ready()) {
        return NULL;
    }
    char *buf = os_data_read(rel, CACHE_MAX, NULL);
    if (!buf) {
        return NULL;
    }
    cJSON *j = cJSON_Parse(buf);
    free(buf);
    cJSON *body = cJSON_DetachItemFromObjectCaseSensitive(j, "body");
    const cJSON *at = cJSON_GetObjectItemCaseSensitive(j, "at");
    if (at_ms && cJSON_IsNumber(at)) {
        *at_ms = (int64_t)at->valuedouble;
    }
    cJSON_Delete(j);
    return body;
}

void game_synced(int64_t at_ms, char *out, size_t n)
{
    int64_t now = os_cloud_time_ms();
    if (!at_ms || !now || now < at_ms) {
        strlcpy(out, "saved copy", n);
        return;
    }
    char ago[24];
    snprintf(out, n, "synced %s", ui_fmt_ago(ago, sizeof(ago), now - at_ms));
}

// ---------------------------------------------------------------------------
// LED effects: a one-shot esp_timer steps through frames (bsp_rgb is
// thread-safe), so they run the same from any app.

#define LED_FRAME_MS    50
#define FLASH_FRAMES    12
#define SWEEP_FRAMES    (BOARD_RGB_COUNT + 14)

static uint32_t s_led_rgb;
static int s_led_frame, s_led_frames;
static bool s_led_sweep;

static uint8_t scale(uint32_t c, int shift, int num, int den)
{
    return ((c >> shift) & 0xFF) * OS_LED_MAX / 255 * num / den;
}

static uint32_t mix(uint32_t a, uint32_t b, int t, int tmax)
{
    uint32_t out = 0;
    for (int s = 0; s <= 16; s += 8) {
        int ca = (a >> s) & 0xFF, cb = (b >> s) & 0xFF;
        out |= (uint32_t)(ca + (cb - ca) * t / tmax) << s;
    }
    return out;
}

static void led_frame(void *arg)
{
    int f = s_led_frame++;
    if (f >= s_led_frames) {
        bsp_rgb_fill(0, 0, 0);
        bsp_rgb_show();
        return;
    }
    if (s_led_sweep) {
        // Light up left to right in the gradient, hold, then fade out.
        int fade = s_led_frames - f < 6 ? s_led_frames - f : 6;
        for (int i = 0; i < BOARD_RGB_COUNT; i++) {
            uint32_t c = i <= f ? mix(COL_PURPLE, COL_GREEN, i, BOARD_RGB_COUNT - 1) : 0;
            bsp_rgb_set(i, scale(c, 16, fade, 6), scale(c, 8, fade, 6), scale(c, 0, fade, 6));
        }
    } else {
        // Up fast, down slow.
        int level = f < 3 ? (f + 1) * 3 : (s_led_frames - f) * 9 / (s_led_frames - 3);
        bsp_rgb_fill(scale(s_led_rgb, 16, level, 9), scale(s_led_rgb, 8, level, 9), scale(s_led_rgb, 0, level, 9));
    }
    bsp_rgb_show();
    esp_timer_start_once(s_led_timer, LED_FRAME_MS * 1000);
}

static void led_start(bool sweep, uint32_t rgb, int frames)
{
    if (!s_led_timer) {
        const esp_timer_create_args_t args = { .callback = led_frame, .name = "game_led" };
        if (esp_timer_create(&args, &s_led_timer) != ESP_OK) {
            return;
        }
    }
    esp_timer_stop(s_led_timer);
    s_led_sweep = sweep;
    s_led_rgb = rgb;
    s_led_frame = 0;
    s_led_frames = frames;
    esp_timer_start_once(s_led_timer, 1000);
}

void game_led_flash(uint32_t rgb)
{
    led_start(false, rgb, FLASH_FRAMES);
}

void game_led_sweep(void)
{
    led_start(true, 0, SWEEP_FRAMES);
}
