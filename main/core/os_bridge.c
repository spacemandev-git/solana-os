// Bridges (docs/MESH.md "Bridges"), see os_bridge.h.
//
// Everything but the send queue lives on the OS task: the bridges from the
// last sync, the code panel, the sync schedule and a scan waiting for Wi-Fi. A
// 250 ms timer posts a tick there. When a sync is due it goes out through
// os_api (the cloud task makes the call) and its answer comes back to the OS
// task: the open bridges go to os_mesh as peers, the messages to
// os_mesh_bridge_rx(), which hands them to the mesh task like radio input.
//
// Messages for bridged badges come from any task (os_mesh_send) into a short
// locked queue. Each keeps its number until a sync's answer says the server
// has it: a retried sync sends the same numbers, which the server drops. Like
// the air, the queue is lossy: what no sync took in 20 s is dropped.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "cJSON.h"
#include "bsp.h"
#include "os.h"
#include "os_api.h"
#include "os_bridge.h"
#include "os_cloud.h"
#include "os_cmd.h"
#include "os_wifi.h"
#include "ui.h"

static const char *TAG = "os_bridge";

#define TICK_MS         250
#define FAST_MS         1000            // messages flowed in the last ACTIVE_MS
#define SLOW_MS         3000
#define SHOW_MS         2000            // the code shows: a new bridge appears within 2 s
#define RETRY_MS        5000            // after a failed sync
#define ACTIVE_MS       20000
#define CODE_LIVE_MS    (3 * 60000 + 30000) // MESH.md: a code counts 3 minutes, so keep looking that long
#define SCAN_WAIT_MS    (2 * 60000)     // a scan waits this long for Wi-Fi
#define OUT_MAX         16              // queued messages (API.md: 16 per sync)
#define OUT_BYTES       6144            // ...and their JSON together
#define OUT_TTL_MS      20000
#define BODY_MAX        4000            // os_api: request bodies are <= 4 KB
#define WINDOW_S        30
#define QR_SIZE         132
#define PLATE           (QR_SIZE + 8)
#define BOX_W           304
#define BOX_H           218
#define COL_X           (8 + PLATE + 8)
#define COL_W           (BOX_W - COL_X - 8)

typedef struct out {
    struct out *next;
    uint32_t n;
    int64_t at;
    uint8_t type;
    bool sent;                          // carried by the sync in flight
    char to[OS_MESH_ID_LEN + 1];        // "" = every bridge
    char json[];
} out_t;

static SemaphoreHandle_t s_lock;        // s_list, s_count, s_out*
static os_bridge_t s_list[OS_BRIDGE_MAX];
static int s_count;
static out_t *s_out;
static int s_out_n, s_out_bytes;
static uint32_t s_out_next;

// OS task.
static esp_timer_handle_t s_timer;
static volatile bool s_tick_queued, s_kick;
static void *s_token;
static bool s_busy, s_failing;          // a sync in flight; the last one failed
// The bridge list changed here (a scan, an end) since the sync in flight
// started: its answer lists bridges as they were before, so it must not
// replace s_list. Bumped on every local change; a sync notes it at the start.
static uint32_t s_local_gen, s_sync_gen;
static int64_t s_next_sync, s_traffic_at, s_list_at, s_code_until;
static uint64_t s_ack;                  // highest message id handled
static char s_end[OS_BRIDGE_MAX][OS_MESH_ID_LEN + 1];
static int s_nend, s_nend_sent;         // ends to tell the server; how many the sync in flight carries
static char s_scan[OS_BRIDGE_CODE_LEN]; // a scan to make, "" = none
static int64_t s_scan_until, s_scan_at;
static bool s_scanning, s_kept;

static struct {
    lv_obj_t *root, *plate, *qr, *code, *status, *hint;
    long long window;
    int ticks;
} s_ui;

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static void kick(void);

// Printable ASCII only (the fonts have no other glyphs).
static void ascii(char *out, size_t n, const char *in)
{
    size_t j = 0;
    for (const char *p = in ? in : ""; *p && j + 1 < n; p++) {
        if (*p >= 0x20 && *p < 0x7F) {
            out[j++] = *p;
        }
    }
    out[j] = 0;
}

// As os_mesh checks a frame's src: 6 Crockford chars, an emulator's first one a lowercase 'e'.
static bool valid_id(const char *id)
{
    if (!id || strlen(id) != OS_MESH_ID_LEN) {
        return false;
    }
    for (int i = 0; i < OS_MESH_ID_LEN; i++) {
        char c = id[i];
        if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (i == 0 && c == 'e'))) {
            return false;
        }
    }
    return true;
}

static const char *label(const os_bridge_t *b)
{
    return b->name[0] ? b->name : b->id;
}

// ---------------------------------------------------------------------------
// The code

static bool make_code(char *out, size_t n, long long *window)
{
    os_cloud_info_t *in = malloc(sizeof(*in));
    if (!in) {
        return false;
    }
    os_cloud_get_info(in);
    char id[OS_MESH_ID_LEN + 1];
    bool have = valid_id(in->id) && in->has_key;
    strlcpy(id, in->id, sizeof(id));
    free(in);
    int64_t t = os_cloud_time_ms();
    if (!have || t <= 0) {
        return false;
    }
    long long w = t / 1000 / WINDOW_S;
    char msg[48], tag[OS_MESH_TAG_LEN + 1];
    snprintf(msg, sizeof(msg), "bridge|%s|%lld", id, w);
    if (!os_mesh_sign(msg, tag)) {
        return false;
    }
    snprintf(out, n, "%s.%lld.%s", id, w, tag);
    if (window) {
        *window = w;
    }
    return true;
}

bool os_bridge_code(char *code, size_t n)
{
    return make_code(code, n, NULL);
}

// The site's address (hello `site`, else the server's), without a trailing slash.
static bool site_of(char *out, size_t n)
{
    os_cloud_info_t *in = malloc(sizeof(*in));
    if (!in) {
        return false;
    }
    os_cloud_get_info(in);
    strlcpy(out, in->site[0] ? in->site : in->server, n);
    free(in);
    size_t l = strlen(out);
    while (l && out[l - 1] == '/') {
        out[--l] = 0;
    }
    return l > 0;
}

bool os_bridge_url(char *url, size_t n)
{
    char code[OS_BRIDGE_CODE_LEN], site[96];
    if (!make_code(code, sizeof(code), NULL) || !site_of(site, sizeof(site))) {
        return false;
    }
    snprintf(url, n, "%s/emulator?bridge=%s", site, code);
    return true;
}

// ---------------------------------------------------------------------------
// Open bridges

int os_bridge_count(void)
{
    if (!s_lock) {
        return 0;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int n = s_count;
    xSemaphoreGive(s_lock);
    return n;
}

int os_bridge_list(os_bridge_t *out, int max)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int n = s_count < max ? s_count : max;
    memcpy(out, s_list, n * sizeof(*out));
    xSemaphoreGive(s_lock);
    return n;
}

static bool in_list(const os_bridge_t *list, int n, const char *id)
{
    for (int i = 0; i < n; i++) {
        if (!strcmp(list[i].id, id)) {
            return true;
        }
    }
    return false;
}

// Wi-Fi stays joined while bridges are open, the code shows or a scan waits.
static void update_keep(void)
{
    bool want = s_count > 0 || s_ui.root || s_scan[0] || s_scanning;
    if (want != s_kept) {
        s_kept = want;
        os_mesh_keep_wifi(want);
    }
}

static void panel_status(void);

// OS task: the new set of open bridges, to os_mesh as peers. `toast` names
// the ones that came and went (not the ones this badge opened or ended).
static void set_list(const os_bridge_t *list, int n, bool toast)
{
    if (toast) {
        for (int i = 0; i < n; i++) {
            if (!in_list(s_list, s_count, list[i].id)) {
                os_toast("Bridged with %s", label(&list[i]));
            }
        }
        for (int i = 0; i < s_count; i++) {
            if (!in_list(list, n, s_list[i].id)) {
                os_toast("Bridge with %s closed", label(&s_list[i]));
            }
        }
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memcpy(s_list, list, n * sizeof(*list));
    s_count = n;
    xSemaphoreGive(s_lock);
    s_list_at = now_ms();
    if (!n) {
        s_ack = 0;                          // nothing left to ack; a fresh server starts its ids over
    }
    os_mesh_bridged_t peers[OS_BRIDGE_MAX];
    for (int i = 0; i < n; i++) {
        strlcpy(peers[i].id, list[i].id, sizeof(peers[i].id));
        strlcpy(peers[i].name, list[i].name, sizeof(peers[i].name));
        peers[i].level = list[i].level;
    }
    os_mesh_set_bridged(peers, n);
    update_keep();
    if (s_ui.root) {
        panel_status();
    }
}

static bool parse_bridge(const cJSON *o, os_bridge_t *out)
{
    const char *id = cJSON_GetStringValue(cJSON_GetObjectItem(o, "id"));
    const cJSON *lv = cJSON_GetObjectItem(o, "lv"), *left = cJSON_GetObjectItem(o, "left");
    if (!valid_id(id)) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    strlcpy(out->id, id, sizeof(out->id));
    ascii(out->name, sizeof(out->name), cJSON_GetStringValue(cJSON_GetObjectItem(o, "n")));
    out->level = cJSON_IsNumber(lv) ? lv->valueint : 0;
    out->left_s = cJSON_IsNumber(left) ? left->valueint : 0;
    return true;
}

// Offline, bridges still run out: count down from the last sync.
static void expire_local(int64_t now)
{
    os_bridge_t keep[OS_BRIDGE_MAX];
    int n = 0;
    for (int i = 0; i < s_count; i++) {
        if (s_list_at + (int64_t)s_list[i].left_s * 1000 > now) {
            keep[n++] = s_list[i];
        }
    }
    if (n != s_count) {
        set_list(keep, n, true);
    }
}

// ---------------------------------------------------------------------------
// Sending

esp_err_t os_bridge_send(os_mesh_type_t type, const char *to, const char *json)
{
    if (type != OS_MESH_XDEX && type != OS_MESH_TRADE && type != OS_MESH_APP) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t len = json ? strlen(json) : 0;
    if (!len || len > OS_MESH_MSG_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }
    out_t *o = malloc(sizeof(*o) + len + 1);
    if (!o) {
        return ESP_ERR_NO_MEM;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = s_count && (!to || in_list(s_list, s_count, to)) ? ESP_OK : ESP_ERR_NOT_FOUND;
    if (err == ESP_OK && (s_out_n >= OUT_MAX || s_out_bytes + (int)len > OUT_BYTES)) {
        err = ESP_ERR_NO_MEM;
    }
    if (err == ESP_OK) {
        *o = (out_t){ .n = s_out_next++, .at = now_ms(), .type = type };
        strlcpy(o->to, to ? to : "", sizeof(o->to));
        memcpy(o->json, json, len + 1);
        out_t **tail = &s_out;
        while (*tail) {
            tail = &(*tail)->next;
        }
        *tail = o;
        s_out_n++;
        s_out_bytes += len;
    }
    xSemaphoreGive(s_lock);
    if (err != ESP_OK) {
        free(o);
        return err;
    }
    kick();
    return ESP_OK;
}

// Under s_lock. keep(o) false removes the entry.
static void out_filter_locked(bool (*keep)(out_t *o, int64_t now), int64_t now)
{
    for (out_t **pp = &s_out; *pp;) {
        out_t *o = *pp;
        if (keep(o, now)) {
            pp = &o->next;
            continue;
        }
        *pp = o->next;
        s_out_n--;
        s_out_bytes -= strlen(o->json);
        free(o);
    }
}

static bool keep_fresh(out_t *o, int64_t now)
{
    return o->sent || now - o->at < OUT_TTL_MS;
}

static bool keep_unsent(out_t *o, int64_t now)
{
    return !o->sent;
}

static bool unmark(out_t *o, int64_t now)
{
    o->sent = false;
    return true;
}

static bool has_unsent(void)
{
    bool any = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (out_t *o = s_out; o && !any; o = o->next) {
        any = !o->sent;
    }
    xSemaphoreGive(s_lock);
    return any;
}

static size_t esc_len(const char *s)
{
    size_t n = 0;
    for (; *s; s++) {
        unsigned char c = *s;
        n += c == '"' || c == '\\' ? 2 : c < 0x20 ? 6 : 1;
    }
    return n;
}

static char *put_esc(char *p, const char *s)
{
    for (; *s; s++) {
        unsigned char c = *s;
        if (c == '"' || c == '\\') {
            *p++ = '\\';
            *p++ = c;
        } else if (c < 0x20) {
            p += sprintf(p, "\\u%04x", c);
        } else {
            *p++ = c;
        }
    }
    return p;
}

// {"ack":..,"end":[..],"send":[{"n","to","t","m"},..]}, as much of the queue as fits 4 KB.
static char *sync_body(int64_t now)
{
    char *body = malloc(BODY_MAX + 1);
    if (!body) {
        return NULL;
    }
    char *p = body, *end = body + BODY_MAX;
    p += sprintf(p, "{\"ack\":%llu", (unsigned long long)s_ack);
    s_nend_sent = s_nend;
    if (s_nend) {
        p += sprintf(p, ",\"end\":[");
        for (int i = 0; i < s_nend; i++) {
            p += sprintf(p, "%s\"%s\"", i ? "," : "", s_end[i]);
        }
        *p++ = ']';
    }
    p += sprintf(p, ",\"send\":[");
    int k = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    out_filter_locked(keep_fresh, now);
    for (out_t *o = s_out; o && k < OUT_MAX; o = o->next) {
        if (p + esc_len(o->json) + 64 > end) {
            break;
        }
        p += sprintf(p, "%s{\"n\":%lu,\"to\":\"%s\",\"t\":%d,\"m\":\"", k++ ? "," : "", (unsigned long)o->n,
                     o->to[0] ? o->to : "*", o->type);
        p = put_esc(p, o->json);
        p += sprintf(p, "\"}");
        o->sent = true;
    }
    xSemaphoreGive(s_lock);
    strcpy(p, "]}");
    return body;
}

// ---------------------------------------------------------------------------
// Syncing (OS task)

// The sync that carried the first s_nend_sent ends got its answer.
static void drop_ends_sent(void)
{
    memmove(s_end, s_end[s_nend_sent], (s_nend - s_nend_sent) * sizeof(s_end[0]));
    s_nend -= s_nend_sent;
    s_nend_sent = 0;
}

static int64_t interval(int64_t now)
{
    if (has_unsent()) {
        return 0;
    }
    if (now - s_traffic_at < ACTIVE_MS) {
        return FAST_MS;
    }
    return s_ui.root ? SHOW_MS : SLOW_MS;
}

static bool wanted(int64_t now)
{
    return s_count > 0 || s_ui.root || s_nend > 0 || has_unsent() || now < s_code_until;
}

static void on_sync(int status, const char *body, void *ctx)
{
    int64_t now = now_ms();
    s_busy = false;
    cJSON *j = status == 200 && body ? cJSON_Parse(body) : NULL;
    const cJSON *bridges = cJSON_GetObjectItem(j, "bridges");
    if (!cJSON_IsArray(bridges)) {
        // Try again with the same messages, unless the server refused them.
        bool refused = status >= 400 && status < 500 && status != 401 && status != 408 && status != 429;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        out_filter_locked(refused ? keep_unsent : unmark, now);
        xSemaphoreGive(s_lock);
        if (refused) {
            drop_ends_sent();
        }
        s_nend_sent = 0;
        cJSON_Delete(j);
        ESP_LOGW(TAG, "sync failed: %d", status);
        s_failing = true;
        s_next_sync = now + RETRY_MS;
        if (s_ui.root) {
            panel_status();
        }
        return;
    }
    s_failing = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int before = s_out_n;
    out_filter_locked(keep_unsent, now);    // the server has them
    bool sent = s_out_n < before;
    xSemaphoreGive(s_lock);
    drop_ends_sent();                       // the server closed them

    bool stale = s_sync_gen != s_local_gen;
    if (!stale) {
        os_bridge_t list[OS_BRIDGE_MAX];
        int n = 0;
        const cJSON *b;
        cJSON_ArrayForEach(b, bridges) {
            if (n < OS_BRIDGE_MAX && parse_bridge(b, &list[n])) {
                n++;
            }
        }
        set_list(list, n, true);
    }

    bool got = false;
    const cJSON *m;
    cJSON_ArrayForEach(m, cJSON_GetObjectItem(j, "recv")) {
        const cJSON *id = cJSON_GetObjectItem(m, "id"), *t = cJSON_GetObjectItem(m, "t");
        const char *from = cJSON_GetStringValue(cJSON_GetObjectItem(m, "from"));
        const char *text = cJSON_GetStringValue(cJSON_GetObjectItem(m, "m"));
        if (cJSON_IsNumber(id) && id->valuedouble > (double)s_ack) {
            s_ack = (uint64_t)id->valuedouble;
        }
        if (from && text && cJSON_IsNumber(t)) {
            os_mesh_bridge_rx((os_mesh_type_t)t->valueint, from, text);
            got = true;
        }
    }
    if (sent || got) {
        s_traffic_at = now;
    }
    bool more = cJSON_IsTrue(cJSON_GetObjectItem(j, "more"));
    cJSON_Delete(j);
    s_next_sync = more || stale ? now : now + interval(now);  // stale: ask again for the list as it is now
}

static void sync_start(int64_t now)
{
    char *body = sync_body(now);
    esp_err_t err = body ? os_api_call("POST", "/api/device/bridge/sync", body, 0, on_sync, s_token) : ESP_ERR_NO_MEM;
    free(body);
    if (err != ESP_OK) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        out_filter_locked(unmark, now);
        xSemaphoreGive(s_lock);
        s_nend_sent = 0;
        s_next_sync = now + RETRY_MS;
        return;
    }
    s_busy = true;
    s_sync_gen = s_local_gen;
}

// ---------------------------------------------------------------------------
// Scanning (OS task)

static void on_scan(int status, const char *body, void *ctx)
{
    int64_t now = now_ms();
    s_scanning = false;
    if (status == 0) {
        s_scan_at = now + 2000;             // no answer: try again while the code counts
        update_keep();
        return;
    }
    s_scan[0] = 0;
    cJSON *j = body ? cJSON_Parse(body) : NULL;
    os_bridge_t got;
    if (status == 200 && parse_bridge(cJSON_GetObjectItem(j, "bridge"), &got)) {
        os_bridge_t list[OS_BRIDGE_MAX];
        int n = 0;
        for (int i = 0; i < s_count && n < OS_BRIDGE_MAX - 1; i++) {
            if (strcmp(s_list[i].id, got.id)) {
                list[n++] = s_list[i];
            }
        }
        list[n++] = got;
        set_list(list, n, false);
        s_local_gen++;
        os_toast("Bridged with %s", label(&got));
        s_traffic_at = now;
        s_next_sync = now;
    } else {
        char why[64];
        ascii(why, sizeof(why), cJSON_GetStringValue(cJSON_GetObjectItem(j, "error")));
        if (why[0]) {
            os_toast("%s", why);
        } else {
            os_toast("Bridge failed (%d)", status);
        }
    }
    cJSON_Delete(j);
    update_keep();
}

static void scan_tick(int64_t now)
{
    if (!s_scan[0] || s_scanning || now < s_scan_at) {
        return;
    }
    if (now > s_scan_until) {
        s_scan[0] = 0;
        os_toast("Bridge: no Wi-Fi. Turn it on and scan again");
        update_keep();
        return;
    }
    if (!os_api_online()) {
        return;
    }
    char body[OS_BRIDGE_CODE_LEN + 16];
    snprintf(body, sizeof(body), "{\"code\":\"%s\"}", s_scan);
    if (os_api_call("POST", "/api/device/bridge", body, 0, on_scan, s_token) == ESP_OK) {
        s_scanning = true;
    } else {
        s_scan_at = now + 2000;
    }
}

// The code from what was scanned or pasted: the code, or a link with bridge=<code>.
static bool code_from(const char *in, char *out, size_t n)
{
    const char *p = in ? strstr(in, "bridge=") : NULL;
    p = p ? p + 7 : in;
    if (!p) {
        return false;
    }
    while (*p == ' ') {
        p++;
    }
    size_t len = strcspn(p, "&# \r\n");
    if (len < OS_MESH_ID_LEN + 3 + OS_MESH_TAG_LEN || len >= n) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        char c = p[i];
        if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '.')) {
            return false;
        }
    }
    memcpy(out, p, len);
    out[len] = 0;
    return true;
}

static void scan_posted(void *arg)
{
    char *code = arg;
    int64_t now = now_ms();
    strlcpy(s_scan, code, sizeof(s_scan));
    free(code);
    s_scan_until = now + SCAN_WAIT_MS;
    s_scan_at = now;
    if (!os_api_online()) {
        char ssid[33];
        if (os_wifi_state() != OS_WIFI_CONNECTED && os_wifi_saved_ssid(ssid, sizeof(ssid))) {
            os_wifi_set_enabled(true);
            os_toast("Turning on Wi-Fi to bridge...");
        } else if (os_wifi_state() != OS_WIFI_CONNECTED) {
            os_toast("Join a Wi-Fi network to bridge");
        }
    }
    update_keep();
    scan_tick(now);
}

esp_err_t os_bridge_scan(const char *text)
{
    char code[OS_BRIDGE_CODE_LEN];
    if (!code_from(text, code, sizeof(code))) {
        return ESP_ERR_INVALID_ARG;
    }
    char *copy = strdup(code);
    if (!copy || !os_post(scan_posted, copy)) {
        free(copy);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static void end_posted(void *arg)
{
    int n = s_count;
    for (int i = 0; i < s_count; i++) {
        bool queued = false;
        for (int k = 0; k < s_nend && !queued; k++) {
            queued = !strcmp(s_end[k], s_list[i].id);
        }
        if (!queued && s_nend < OS_BRIDGE_MAX) {
            strlcpy(s_end[s_nend++], s_list[i].id, sizeof(s_end[0]));
        }
    }
    if (n) {
        os_toast(n == 1 ? "Bridge closed" : "Bridges closed");
    }
    set_list(NULL, 0, false);
    s_local_gen++;
    s_next_sync = now_ms();
}

void os_bridge_end_all(void)
{
    os_post(end_posted, NULL);
}

// ---------------------------------------------------------------------------
// The code panel (OS task, LVGL locked)

static void panel_status(void)
{
    char line[96];
    uint32_t col = COL_DIM;
    if (!s_ui.window) {
        strlcpy(line, "Turn on Wi-Fi once: the badge needs its id and the time", sizeof(line));
        col = COL_WARN;
    } else if (!os_api_online()) {
        strlcpy(line, "Wi-Fi is off. Bridges need it on both badges", sizeof(line));
        col = COL_WARN;
    } else if (s_failing) {
        strlcpy(line, "Can't reach the server. Trying again...", sizeof(line));
        col = COL_WARN;
    } else if (!s_count) {
        strlcpy(line, "Waiting for a scan...", sizeof(line));
    } else {
        strlcpy(line, "Bridged: ", sizeof(line));
        for (int i = 0; i < s_count; i++) {
            if (i) {
                strlcat(line, ", ", sizeof(line));
            }
            strlcat(line, label(&s_list[i]), sizeof(line));
        }
        col = COL_GREEN;
    }
    lv_label_set_text(s_ui.status, line);
    lv_obj_set_style_text_color(s_ui.status, ui_col(col), 0);
    lv_label_set_text(s_ui.hint, s_count ? "CANCEL close   " LV_SYMBOL_LEFT " end bridges" : "CANCEL close");
}

// Every 30 s a new code: the QR code and the text under it.
static void panel_code(void)
{
    char code[OS_BRIDGE_CODE_LEN], site[96], url[160];
    long long w = 0;
    bool ok = make_code(code, sizeof(code), &w) && site_of(site, sizeof(site));
    if (ok && w == s_ui.window) {
        return;
    }
    s_ui.window = ok ? w : 0;
    lv_obj_set_hidden(s_ui.plate, !ok);
    lv_label_set_text(s_ui.code, ok ? code : "");
    if (ok) {
        snprintf(url, sizeof(url), "%s/emulator?bridge=%s", site, code);
        lv_qrcode_update(s_ui.qr, url, strlen(url));
    }
}

// "https://solana.com/breakpoint/badge" -> the emulator's address in lines
// that fit the column, broken after a '/' ('.' would break it mid-name).
static void host_lines(char *out, size_t n)
{
    enum { LINE = 19 };                     // sol_mono_12 characters in COL_W
    char site[96], addr[112];
    if (!site_of(site, sizeof(site))) {
        strlcpy(out, "/emulator", n);
        return;
    }
    const char *s = strstr(site, "://");
    snprintf(addr, sizeof(addr), "%s/emulator", s ? s + 3 : site);
    out[0] = 0;
    size_t line = 0;
    for (const char *seg = addr; *seg;) {
        size_t len = strcspn(seg, "/");
        len += seg[len] == '/';             // keep the '/' at the end of its segment
        if (line && line + len > LINE) {
            strlcat(out, "\n", n);
            line = 0;
        }
        size_t at = strlen(out);
        if (at + len >= n) {
            break;
        }
        memcpy(out + at, seg, len);
        out[at + len] = 0;
        line += len;
        seg += len;
    }
}

static void panel_close(void)
{
    if (s_ui.root) {
        lv_obj_delete(s_ui.root);
    }
    memset(&s_ui, 0, sizeof(s_ui));
    update_keep();
}

static bool panel_key(os_key_t key, bool repeat)
{
    if (key == OS_KEY_BACK || key == OS_KEY_SELECT) {
        return repeat;                      // a press closes it
    }
    if (key == OS_KEY_LEFT && !repeat && s_count) {
        end_posted(NULL);
    }
    return true;
}

static void panel_tick(void)
{
    if (!s_ui.root || ++s_ui.ticks % (1000 / OS_TICK_MS)) {
        return;
    }
    panel_code();
    panel_status();
    s_code_until = now_ms() + CODE_LIVE_MS;
}

static const os_overlay_t s_panel = { .on_key = panel_key, .on_tick = panel_tick, .on_close = panel_close };

static void panel_build(void)
{
    bool light = ui_light();
    s_ui.root = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_ui.root);
    lv_obj_set_size(s_ui.root, BOARD_LCD_H_RES, BOARD_LCD_V_RES);
    lv_obj_set_style_bg_opa(s_ui.root, LV_OPA_70, 0);
    lv_obj_set_style_bg_color(s_ui.root, lv_color_black(), 0);
    lv_obj_set_scrollable(s_ui.root, false);

    lv_obj_t *box = ui_panel(s_ui.root, 0, 0, BOX_W, BOX_H);
    lv_obj_set_style_bg_color(box, ui_col(COL_PANEL_HI), 0);
    lv_obj_set_style_border_color(box, ui_col(COL_PURPLE), 0);
    lv_obj_set_style_pad_all(box, 0, 0);
    lv_obj_set_scrollable(box, false);
    lv_obj_center(box);

    // White modules on black in Dark, black on white in Light, as on My Badge.
    lv_color_t fg = light ? lv_color_black() : lv_color_white();
    lv_color_t bg = light ? lv_color_white() : lv_color_black();
    s_ui.plate = lv_obj_create(box);
    lv_obj_remove_style_all(s_ui.plate);
    lv_obj_set_size(s_ui.plate, PLATE, PLATE);
    lv_obj_set_pos(s_ui.plate, 8, 8);
    lv_obj_set_style_bg_opa(s_ui.plate, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_ui.plate, bg, 0);
    lv_obj_set_style_radius(s_ui.plate, 6, 0);
    lv_obj_set_scrollable(s_ui.plate, false);
    s_ui.qr = lv_qrcode_create(s_ui.plate);
    lv_qrcode_set_size(s_ui.qr, QR_SIZE);
    lv_qrcode_set_dark_color(s_ui.qr, fg);
    lv_qrcode_set_light_color(s_ui.qr, bg);
    lv_obj_center(s_ui.qr);

    lv_obj_t *col = lv_obj_create(box);
    lv_obj_remove_style_all(col);
    lv_obj_set_pos(col, COL_X, 10);
    lv_obj_set_size(col, COL_W, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(col, 6, 0);
    lv_obj_set_scrollable(col, false);
    ui_label(col, &sol_font_20, COL_TEXT, "Bridge code");
    lv_obj_t *t = ui_label(col, &sol_font_14, COL_DIM, "No radio between you? Scan it in the emulator at");
    lv_obj_set_width(t, COL_W);
    lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_WRAP);
    char host[128];
    host_lines(host, sizeof(host));
    ui_label(col, &sol_mono_12, COL_TEXT, host);

    s_ui.code = ui_label(box, &sol_mono_12, COL_TEXT, "");
    lv_obj_set_width(s_ui.code, BOX_W);
    lv_obj_set_style_text_align(s_ui.code, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s_ui.code, 0, 8 + PLATE + 6);
    s_ui.status = ui_label(box, &sol_font_14, COL_DIM, "");
    lv_obj_set_width(s_ui.status, BOX_W - 16);
    lv_label_set_long_mode(s_ui.status, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_pos(s_ui.status, 8, 8 + PLATE + 24);
    s_ui.hint = ui_label(box, &sol_mono_12, COL_DIM, "");
    lv_obj_set_pos(s_ui.hint, 8, BOX_H - 20);

    panel_code();
    panel_status();
}

static void show_posted(void *arg)
{
    if (!s_ui.root) {
        panel_build();
        os_overlay_open(&s_panel);
    }
    int64_t now = now_ms();
    s_code_until = now + CODE_LIVE_MS;
    s_next_sync = now;                      // see who bridged right away
    update_keep();
}

static void hide_posted(void *arg)
{
    if (os_overlay() == &s_panel) {
        os_overlay_close();
    }
}

void os_bridge_show(void)
{
    os_post(show_posted, NULL);
}

void os_bridge_hide(void)
{
    os_post(hide_posted, NULL);
}

bool os_bridge_showing(void)
{
    return s_ui.root != NULL;
}

// ---------------------------------------------------------------------------
// Tick (OS task)

static void tick(void *arg)
{
    s_tick_queued = false;
    int64_t now = now_ms();
    if (s_kick) {
        s_kick = false;
        if (s_next_sync > now) {
            s_next_sync = now;
        }
    }
    expire_local(now);
    scan_tick(now);
    update_keep();
    if (s_busy || now < s_next_sync || !wanted(now)) {
        return;
    }
    if (!os_api_online()) {
        s_next_sync = now + 1000;
        return;
    }
    sync_start(now);
}

static void tick_timer(void *arg)
{
    if (!s_tick_queued) {
        s_tick_queued = true;
        if (!os_post(tick, NULL)) {
            s_tick_queued = false;          // OS not started yet, or its queue is full
        }
    }
}

static void kick(void)
{
    s_kick = true;
    tick_timer(NULL);
}

// ---------------------------------------------------------------------------
// Console

static void code_shown(void *arg)
{
    s_code_until = now_ms() + CODE_LIVE_MS; // someone may scan it: keep looking
}

static int cmd_bridge(int argc, char **argv)
{
    const char *sub = argc > 1 ? argv[1] : "status";
    if (!strcmp(sub, "status")) {
        os_bridge_t list[OS_BRIDGE_MAX];
        int n = os_bridge_list(list, OS_BRIDGE_MAX);
        int64_t now = now_ms();
        printf("bridge: %d open%s%s%s%s, %d queued, ack %llu\n", n, os_api_online() ? "" : ", offline",
               s_ui.root ? ", code showing" : "",
               s_scan[0] ? (os_api_online() ? ", scanning" : ", scan waiting for Wi-Fi") : "",
               s_failing ? ", last sync failed" : "", s_out_n, (unsigned long long)s_ack);
        for (int i = 0; i < n; i++) {
            long left = list[i].left_s - (long)((now - s_list_at) / 1000);
            printf("  %-6s %-24s Lv %-3d %ld s left\n", list[i].id, list[i].name[0] ? list[i].name : "(unclaimed)",
                   list[i].level, left > 0 ? left : 0);
        }
    } else if (!strcmp(sub, "show")) {
        os_bridge_show();
    } else if (!strcmp(sub, "hide")) {
        os_bridge_hide();
    } else if (!strcmp(sub, "end")) {
        os_bridge_end_all();
    } else if (!strcmp(sub, "code")) {
        char code[OS_BRIDGE_CODE_LEN], url[160];
        if (!os_bridge_code(code, sizeof(code)) || !os_bridge_url(url, sizeof(url))) {
            printf("bridge: no code yet (the badge needs its id and the server's clock: turn Wi-Fi on once)\n");
            return 1;
        }
        printf("bridge code: %s\nbridge url: %s\n", code, url);
        os_post(code_shown, NULL);
    } else {
        const char *text = !strcmp(sub, "scan") && argc > 2 ? argv[2] : sub;
        esp_err_t err = os_bridge_scan(text);
        if (err == ESP_ERR_INVALID_ARG) {
            printf("usage: bridge [status|show|hide|code|end|<code or link>]\n");
            return 1;
        }
        printf("bridge: %s\n", err == ESP_OK ? "scanning" : esp_err_to_name(err));
        return err == ESP_OK ? 0 : 1;
    }
    return 0;
}

// ---------------------------------------------------------------------------

void os_bridge_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_token = os_api_token();
    s_out_next = esp_random();              // a reboot must not reuse numbers the server still holds
    os_cmd_register("bridge", "bridge [status|show|hide|code|end|<code or link>]", cmd_bridge);
    const esp_timer_create_args_t args = { .callback = tick_timer, .name = "bridge" };
    ESP_ERROR_CHECK(esp_timer_create(&args, &s_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(s_timer, TICK_MS * 1000));
}
