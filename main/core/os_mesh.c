// Mesh service (docs/MESH.md), see os_mesh.h.
//
// One "mesh" task owns the protocol: it drains the frames the radio hands
// over (os_radio's rx callback only copies them into a queue), reassembles
// fragments, drops duplicates, keeps the peer table from HELLOs, raises the
// background toasts and calls subscribers. It also broadcasts our HELLO every
// 2 s +- 0.5 s while the radio is on the mesh channel. Senders fragment on
// their own task under a tx lock.
//
// RAM is fixed: two reassembly slots, a small peer table and a dedup ring,
// all static. Portable: the emulator compiles this file unchanged over its
// BroadcastChannel radio, so SHA-256/HMAC are implemented here too.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "cJSON.h"
#include "os.h"
#include "os_cloud.h"
#include "os_cmd.h"
#include "os_mesh.h"
#include "os_radio.h"
#include "os_wifi.h"

static const char *TAG = "os_mesh";

#define HDR_LEN         16
#define FRAG_DATA       232             // HDR_LEN + FRAG_DATA <= OS_RADIO_FRAME_MAX
#define FRAG_MAX        8
#define REASM_SLOTS     2
#define REASM_MS        2000            // incomplete messages are dropped after 2 s
#define DEDUP_N         32
#define DEDUP_MS        10000
#define PEER_MAX        20
#define SUB_MAX         12
#define RXQ_LEN         6
#define HELLO_MS        1500            // + 0..1000 ms jitter
#define MAINTAIN_MS     250
#define SYNC_MS         1000            // identity and channel from os_cloud
#define TX_RETRIES      25              // ~50 ms of "driver queue full"
#define BEACON_TOAST_MS (10 * 60 * 1000)    // per character
#define BEACON_GAP_MS   30000               // between any two beacon toasts
#define ASK_TOAST_MS    15000               // per peer, swap/trade requests
#define ASK_GAP_MS      8000                // between any two swap/trade toasts
#define NEST_MAX        8                   // deeper JSON is dropped before anyone parses it
#define MAC_CHANGE_N    3                   // HELLOs in a row from a new MAC before an id moves

typedef struct {
    uint8_t mac[6];
    int8_t rssi;
    uint8_t len;
    uint8_t data[OS_RADIO_FRAME_MAX];
} rx_t;

typedef struct {
    uint8_t mac[6];
    uint16_t msg;
    uint8_t type, cnt;
    uint8_t got;                // bitmask of received fragments
    uint16_t len;
    int64_t t0;                 // 0 = free
    char src[OS_MESH_ID_LEN + 1];
    char buf[OS_MESH_MSG_MAX + 1];
} reasm_t;

typedef struct {
    uint8_t mac[6];
    uint16_t msg;
    int64_t t;
} seen_t;

typedef struct {
    os_mesh_type_t type;
    os_mesh_handler_t cb;
    void *ctx;
} sub_t;

static QueueHandle_t s_rxq;
static SemaphoreHandle_t s_lock;        // peers, state, counters
static SemaphoreHandle_t s_tx_lock;     // message counter + fragment order
static SemaphoreHandle_t s_sub_lock;    // recursive: held while handlers run
static TaskHandle_t s_task;

static os_mesh_peer_t s_peers[PEER_MAX];
// Per entry of s_peers (same index): pinning and the MAC-change vote.
static struct {
    bool pinned;
    uint8_t next_mac[6];                // a different MAC claiming this id...
    uint8_t next_n;                     // ...in this many HELLOs in a row
} s_meta[PEER_MAX];
static reasm_t s_reasm[REASM_SLOTS];
static seen_t s_seen[DEDUP_N];
static int s_seen_next;
static sub_t s_subs[SUB_MAX];
static char s_one[FRAG_DATA + 1];       // single-fragment messages, mesh task only
static uint8_t s_mac[6];
static uint16_t s_msg;

// What the mesh task last learned from os_cloud.
static char s_id[OS_MESH_ID_LEN + 1];
static char s_label[25];
static int s_level, s_channel;
static bool s_has_key;
static int s_server_ch, s_override;     // s_lock: the server's channel, and the user's choice (0 = none)

static int s_acquired;
static bool s_took_wifi;                // our acquire suspended Wi-Fi
static bool s_log;                      // console "mesh log on"

static struct {
    uint32_t tx_msgs, tx_frames, tx_err, hello_tx;
    uint32_t rx_frames, rx_bad, rx_dup, rx_drop, rx_msgs, rx_multi, reasm_lost, rx_spoof, rx_deep;
} s_n;

static struct {
    char who[17];
    int64_t t;
} s_toasted[8];
static int64_t s_last_beacon_toast, s_last_ask_toast;

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

// ---------------------------------------------------------------------------
// SHA-256 and HMAC-SHA256 (FIPS 180-4, RFC 2104)

typedef struct {
    uint32_t h[8];
    uint64_t bytes;
    uint8_t buf[64];
    size_t n;
} sha256_t;

static const uint32_t K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha256_block(sha256_t *s, const uint8_t *p)
{
    uint32_t w[64], a, b, c, d, e, f, g, h;
    for (int i = 0; i < 16; i++) {
        w[i] = (uint32_t)p[i * 4] << 24 | (uint32_t)p[i * 4 + 1] << 16 | (uint32_t)p[i * 4 + 2] << 8 | p[i * 4 + 3];
    }
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3], e = s->h[4], f = s->h[5], g = s->h[6], h = s->h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = h + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + K256[i] + w[i];
        uint32_t t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g, g = f, f = e, e = d + t1, d = c, c = b, b = a, a = t1 + t2;
    }
    s->h[0] += a, s->h[1] += b, s->h[2] += c, s->h[3] += d;
    s->h[4] += e, s->h[5] += f, s->h[6] += g, s->h[7] += h;
}

static void sha256_init(sha256_t *s)
{
    static const uint32_t iv[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                   0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    memcpy(s->h, iv, sizeof(iv));
    s->bytes = 0;
    s->n = 0;
}

static void sha256_update(sha256_t *s, const void *data, size_t len)
{
    const uint8_t *p = data;
    s->bytes += len;
    while (len) {
        size_t take = 64 - s->n < len ? 64 - s->n : len;
        memcpy(s->buf + s->n, p, take);
        s->n += take;
        p += take;
        len -= take;
        if (s->n == 64) {
            sha256_block(s, s->buf);
            s->n = 0;
        }
    }
}

static void sha256_final(sha256_t *s, uint8_t out[32])
{
    uint64_t bits = s->bytes * 8;
    uint8_t pad = 0x80;
    sha256_update(s, &pad, 1);
    pad = 0;
    while (s->n != 56) {
        sha256_update(s, &pad, 1);
    }
    uint8_t len[8];
    for (int i = 0; i < 8; i++) {
        len[i] = bits >> (56 - i * 8);
    }
    sha256_update(s, len, 8);
    for (int i = 0; i < 8; i++) {
        out[i * 4] = s->h[i] >> 24;
        out[i * 4 + 1] = s->h[i] >> 16;
        out[i * 4 + 2] = s->h[i] >> 8;
        out[i * 4 + 3] = s->h[i];
    }
}

static void hmac_sha256(const uint8_t *key, size_t klen, const void *msg, size_t mlen, uint8_t out[32])
{
    uint8_t k[64] = {0}, pad[64];
    sha256_t s;
    if (klen > 64) {
        sha256_init(&s);
        sha256_update(&s, key, klen);
        sha256_final(&s, k);
    } else {
        memcpy(k, key, klen);
    }
    for (int i = 0; i < 64; i++) {
        pad[i] = k[i] ^ 0x36;
    }
    sha256_init(&s);
    sha256_update(&s, pad, 64);
    sha256_update(&s, msg, mlen);
    sha256_final(&s, out);
    for (int i = 0; i < 64; i++) {
        pad[i] = k[i] ^ 0x5c;
    }
    sha256_init(&s);
    sha256_update(&s, pad, 64);
    sha256_update(&s, out, 32);
    sha256_final(&s, out);
}

static void to_hex(const uint8_t *b, int n, char *out)
{
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < n; i++) {
        out[i * 2] = hex[b[i] >> 4];
        out[i * 2 + 1] = hex[b[i] & 15];
    }
    out[n * 2] = 0;
}

static void tag_with(const uint8_t key[16], const char *msg, char out[OS_MESH_TAG_LEN + 1])
{
    uint8_t mac[32];
    hmac_sha256(key, 16, msg, strlen(msg), mac);
    to_hex(mac, OS_MESH_TAG_LEN / 2, out);
}

bool os_mesh_sign(const char *msg, char out[OS_MESH_TAG_LEN + 1])
{
    uint8_t key[16];
    if (!msg || !os_cloud_mesh_key(key)) {
        out[0] = 0;
        return false;
    }
    tag_with(key, msg, out);
    memset(key, 0, sizeof(key));
    return true;
}

void os_mesh_nonce(char out[17])
{
    uint8_t b[8];
    esp_fill_random(b, sizeof(b));
    to_hex(b, 8, out);
}

// ---------------------------------------------------------------------------
// Peers

// Printable ASCII only (the fonts have no other glyphs), at most n - 1 chars.
static void clean_copy(char *out, size_t n, const char *in)
{
    size_t j = 0;
    for (const char *p = in ? in : ""; *p && j + 1 < n; p++) {
        if (*p >= 0x20 && *p < 0x7F) {
            out[j++] = *p;
        }
    }
    out[j] = 0;
}

static os_mesh_peer_t *find_peer_locked(const char *id)
{
    for (int i = 0; i < PEER_MAX; i++) {
        if (s_peers[i].id[0] && !strcmp(s_peers[i].id, id)) {
            return &s_peers[i];
        }
    }
    return NULL;
}

static bool live(const os_mesh_peer_t *p, int64_t now)
{
    return p->id[0] && p->last_ms && now - p->last_ms < OS_MESH_PEER_TTL_MS;
}

// The entry for id, creating it if needed. A full table (a crowded hall)
// gives up a silent entry first, else its weakest peer if the newcomer is
// stronger: "nearby" means close. NULL when the newcomer loses.
static os_mesh_peer_t *peer_slot_locked(const char *id, int rssi, bool *created)
{
    os_mesh_peer_t *p = find_peer_locked(id);
    *created = false;
    if (p) {
        return p;
    }
    int64_t now = now_ms();
    os_mesh_peer_t *silent = NULL, *weak = NULL;
    for (int i = 0; i < PEER_MAX && !p; i++) {
        os_mesh_peer_t *q = &s_peers[i];
        if (!q->id[0]) {
            p = q;
        } else if (s_meta[i].pinned) {
            continue;                   // an app's partner: never evicted
        } else if (!live(q, now)) {
            silent = !silent || q->last_ms < silent->last_ms ? q : silent;
        } else if (!weak || q->rssi < weak->rssi) {
            weak = q;
        }
    }
    p = p ? p : silent ? silent : weak && weak->rssi < rssi ? weak : NULL;
    if (p) {
        memset(p, 0, sizeof(*p));
        memset(&s_meta[p - s_peers], 0, sizeof(s_meta[0]));
        strlcpy(p->id, id, sizeof(p->id));
        *created = true;
    }
    return p;
}

static int by_rssi(const void *a, const void *b)
{
    return ((const os_mesh_peer_t *)b)->rssi - ((const os_mesh_peer_t *)a)->rssi;
}

int os_mesh_peers(os_mesh_peer_t *out, int max)
{
    int n = 0;
    int64_t now = now_ms();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < PEER_MAX && n < max; i++) {
        if (live(&s_peers[i], now)) {
            out[n++] = s_peers[i];
        }
    }
    xSemaphoreGive(s_lock);
    qsort(out, n, sizeof(*out), by_rssi);
    return n;
}

bool os_mesh_pin(const char *id, bool pin)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    os_mesh_peer_t *p = id ? find_peer_locked(id) : NULL;
    if (p) {
        s_meta[p - s_peers].pinned = pin;
    }
    xSemaphoreGive(s_lock);
    return p != NULL;
}

bool os_mesh_peer(const char *id, os_mesh_peer_t *out)
{
    bool ok = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    os_mesh_peer_t *p = id ? find_peer_locked(id) : NULL;
    if (p && live(p, now_ms())) {
        *out = *p;
        ok = true;
    }
    xSemaphoreGive(s_lock);
    return ok;
}

// ---------------------------------------------------------------------------
// State

int os_mesh_channel(void)
{
    return s_channel;
}

void os_mesh_set_channel(int channel)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_override = channel > 0 ? channel : 0;
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "mesh channel %s %d", channel > 0 ? "set to" : "back to the server's,", channel > 0 ? channel : s_server_ch);
}

int os_mesh_server_channel(void)
{
    return s_server_ch;
}

bool os_mesh_channel_overridden(void)
{
    return s_override != 0;
}

// The ESP-NOW scanner borrowed the radio (Wi-Fi suspended, not by us).
static bool paused(void)
{
    return os_wifi_state() == OS_WIFI_SUSPENDED && !s_took_wifi;
}

static bool on_channel(void)
{
    return !paused() && s_channel && os_radio_channel() == s_channel;
}

bool os_mesh_ready(void)
{
    return on_channel() && s_id[0] && s_has_key;
}

// Pick up identity and channel changes from os_cloud (mesh task).
static void sync_cloud(void)
{
    os_cloud_info_t *in = malloc(sizeof(*in));
    if (!in) {
        return;
    }
    os_cloud_get_info(in);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(s_id, strlen(in->id) == OS_MESH_ID_LEN ? in->id : "", sizeof(s_id));
    clean_copy(s_label, sizeof(s_label), in->claimed ? in->owner : "");
    s_level = in->level;
    s_has_key = in->has_key;
    int ch = in->mesh_channel;
    if (ch != s_server_ch) {
        // The server moved the mesh: a channel the user picked gives way to it.
        if (s_override && s_server_ch && ch) {
            ESP_LOGI(TAG, "the server announced channel %d: dropping the chosen channel %d", ch, s_override);
            s_override = 0;
        }
        s_server_ch = ch;
    }
    if (s_override) {
        ch = s_override;
    }
    xSemaphoreGive(s_lock);
    free(in);
    if (ch != s_channel) {
        ESP_LOGI(TAG, "mesh channel %d", ch);
        s_channel = ch;
        os_wifi_set_park_channel(ch);
    }
}

// While our acquire holds the radio, os_wifi does not park it: we do.
static void hold_channel(void)
{
    if (s_took_wifi && s_channel && os_radio_channel() != s_channel) {
        os_radio_set_channel(s_channel);    // fails until the disconnect settles; retried
    }
}

void os_mesh_acquire(void)
{
    bool suspend = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_acquired++ == 0) {
        os_wifi_info_t w;
        os_wifi_get_info(&w);
        bool joined = w.state == OS_WIFI_CONNECTED || w.state == OS_WIFI_CONNECTING;
        suspend = joined && !(w.state == OS_WIFI_CONNECTED && w.channel == s_channel);
        s_took_wifi = suspend;
    }
    bool first = s_acquired == 1;
    xSemaphoreGive(s_lock);
    if (first) {
        os_radio_stay_awake(true);
    }
    if (suspend) {
        ESP_LOGI(TAG, "Wi-Fi is off the mesh channel: suspending it while a mesh app runs");
        os_wifi_suspend();
        xTaskNotifyGive(s_task);
    }
}

void os_mesh_release(void)
{
    bool resume = false, last = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_acquired > 0 && --s_acquired == 0) {
        last = true;
        resume = s_took_wifi;
        s_took_wifi = false;
    }
    xSemaphoreGive(s_lock);
    if (last) {
        os_radio_stay_awake(false);
    }
    if (resume) {
        os_wifi_resume();
    }
}

// ---------------------------------------------------------------------------
// Sending

static esp_err_t send_to(const uint8_t *mac, os_mesh_type_t type, const char *json)
{
    size_t len = strlen(json);
    int cnt = (len + FRAG_DATA - 1) / FRAG_DATA;
    if (!len || len > OS_MESH_MSG_MAX || cnt > FRAG_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }
    char id[OS_MESH_ID_LEN + 1];
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(id, s_id, sizeof(id));
    xSemaphoreGive(s_lock);
    if (!id[0]) {
        return ESP_ERR_INVALID_STATE;       // MESH.md: no server-assigned id, no transmit
    }

    uint8_t f[HDR_LEN + FRAG_DATA];
    esp_err_t err = ESP_OK;
    xSemaphoreTake(s_tx_lock, portMAX_DELAY);
    uint16_t msg = s_msg++;
    for (int idx = 0; idx < cnt && err == ESP_OK; idx++) {
        int n = idx < cnt - 1 ? FRAG_DATA : (int)(len - FRAG_DATA * (cnt - 1));
        f[0] = 'H';
        f[1] = 'G';
        f[2] = 1;
        f[3] = type;
        memcpy(f + 4, id, OS_MESH_ID_LEN);
        f[10] = msg & 0xFF;
        f[11] = msg >> 8;
        f[12] = idx;
        f[13] = cnt;
        f[14] = len & 0xFF;
        f[15] = len >> 8;
        memcpy(f + HDR_LEN, json + idx * FRAG_DATA, n);
        for (int tries = 0;; tries++) {
            err = os_radio_send(mac, f, HDR_LEN + n);
            if (err != ESP_ERR_NO_MEM || tries == TX_RETRIES) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(2));
        }
        if (err == ESP_OK) {
            s_n.tx_frames++;
        }
    }
    xSemaphoreGive(s_tx_lock);
    if (err == ESP_OK) {
        s_n.tx_msgs++;
    } else {
        s_n.tx_err++;
        ESP_LOGW(TAG, "send type %d: %s", type, esp_err_to_name(err));
    }
    return err;
}

esp_err_t os_mesh_send(os_mesh_type_t type, const char *to_id, const char *json)
{
    if (type <= 0 || type >= OS_MESH_TYPE_MAX || !json) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!on_channel()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!to_id || !strcmp(to_id, "*")) {
        return send_to(NULL, type, json);
    }
    uint8_t mac[6];
    xSemaphoreTake(s_lock, portMAX_DELAY);
    os_mesh_peer_t *p = find_peer_locked(to_id);
    if (p) {
        memcpy(mac, p->mac, 6);
    }
    xSemaphoreGive(s_lock);
    return p ? send_to(mac, type, json) : ESP_ERR_NOT_FOUND;
}

static void send_hello(void)
{
    char json[64];
    xSemaphoreTake(s_lock, portMAX_DELAY);
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "k", "b");
    cJSON_AddStringToObject(j, "n", s_label);
    cJSON_AddNumberToObject(j, "lv", s_level);
    xSemaphoreGive(s_lock);
    bool ok = j && cJSON_PrintPreallocated(j, json, sizeof(json), false);
    cJSON_Delete(j);
    if (ok && send_to(NULL, OS_MESH_HELLO, json) == ESP_OK) {
        s_n.hello_tx++;
    }
}

// ---------------------------------------------------------------------------
// Subscribers

int os_mesh_subscribe(os_mesh_type_t type, os_mesh_handler_t cb, void *ctx)
{
    int h = -1;
    xSemaphoreTakeRecursive(s_sub_lock, portMAX_DELAY);
    for (int i = 0; i < SUB_MAX; i++) {
        if (!s_subs[i].cb) {
            s_subs[i] = (sub_t){ .type = type, .cb = cb, .ctx = ctx };
            h = i;
            break;
        }
    }
    xSemaphoreGiveRecursive(s_sub_lock);
    if (h < 0) {
        ESP_LOGE(TAG, "no room for another subscriber");
    }
    return h;
}

// Waits for a handler that is running right now, so after this returns the
// callback is never called again (its ctx may be freed).
void os_mesh_unsubscribe(int handle)
{
    if (handle < 0 || handle >= SUB_MAX) {
        return;
    }
    xSemaphoreTakeRecursive(s_sub_lock, portMAX_DELAY);
    s_subs[handle].cb = NULL;
    xSemaphoreGiveRecursive(s_sub_lock);
}

static void dispatch(os_mesh_type_t type, const os_mesh_peer_t *from, const char *json)
{
    xSemaphoreTakeRecursive(s_sub_lock, portMAX_DELAY);
    for (int i = 0; i < SUB_MAX; i++) {
        if (s_subs[i].cb && s_subs[i].type == type) {
            s_subs[i].cb(type, from, json, s_subs[i].ctx);
        }
    }
    xSemaphoreGiveRecursive(s_sub_lock);
}

// ---------------------------------------------------------------------------
// Receiving (mesh task)

static bool seen(const uint8_t *mac, uint16_t msg, int64_t now)
{
    for (int i = 0; i < DEDUP_N; i++) {
        if (s_seen[i].t && now - s_seen[i].t < DEDUP_MS && s_seen[i].msg == msg && !memcmp(s_seen[i].mac, mac, 6)) {
            return true;
        }
    }
    return false;
}

static void mark_seen(const uint8_t *mac, uint16_t msg, int64_t now)
{
    seen_t *s = &s_seen[s_seen_next];
    s_seen_next = (s_seen_next + 1) % DEDUP_N;
    memcpy(s->mac, mac, 6);
    s->msg = msg;
    s->t = now;
}

// Toast once per `who` within `quiet_ms`.
static bool toast_due(const char *who, int64_t quiet_ms, int64_t now)
{
    int slot = 0;
    for (int i = 0; i < 8; i++) {
        if (!strcmp(s_toasted[i].who, who)) {
            if (now - s_toasted[i].t < quiet_ms) {
                return false;
            }
            slot = i;
            break;
        }
        if (s_toasted[i].t < s_toasted[slot].t) {
            slot = i;
        }
    }
    strlcpy(s_toasted[slot].who, who, sizeof(s_toasted[slot].who));
    s_toasted[slot].t = now;
    return true;
}

static bool app_open(const char *id)
{
    const os_app_t *app = os_current();
    return app && app->id && !strcmp(app->id, id);
}

// Is this HELLO from the MAC we know for its id? A live id moves to another
// MAC only after MAC_CHANGE_N HELLOs in a row from it (one forged frame can't
// hijack a peer), and a live beacon id never turns into a badge.
static bool hello_mac_ok_locked(os_mesh_peer_t *p, bool created, const uint8_t *mac, bool beacon, int64_t now)
{
    int i = p - s_peers;
    if (created || !live(p, now) || !memcmp(p->mac, mac, 6)) {
        s_meta[i].next_n = 0;
        return true;
    }
    if (p->kind == OS_PEER_BEACON && !beacon) {
        return false;
    }
    if (memcmp(s_meta[i].next_mac, mac, 6)) {
        memcpy(s_meta[i].next_mac, mac, 6);
        s_meta[i].next_n = 0;
    }
    if (++s_meta[i].next_n < MAC_CHANGE_N) {
        return false;
    }
    s_meta[i].next_n = 0;
    return true;
}

// Only HELLOs create peers and set their MAC, kind and name. Returns false
// when the HELLO is not accepted (then nobody hears about it).
static bool on_hello(const os_mesh_peer_t *heard, const char *json, int64_t now)
{
    cJSON *j = cJSON_Parse(json);
    if (!cJSON_IsObject(j)) {
        cJSON_Delete(j);
        s_n.rx_bad++;
        return false;
    }
    const char *k = cJSON_GetStringValue(cJSON_GetObjectItem(j, "k"));
    const char *n = cJSON_GetStringValue(cJSON_GetObjectItem(j, "n"));
    bool beacon = k && !strcmp(k, "c");
    char toast[48] = "";

    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool created;
    os_mesh_peer_t *p = peer_slot_locked(heard->id, heard->rssi, &created);
    if (!p || !hello_mac_ok_locked(p, created, heard->mac, beacon, now)) {
        if (p && !created) {
            s_n.rx_spoof++;
        }
        xSemaphoreGive(s_lock);
        cJSON_Delete(j);
        return false;                       // table full of closer peers, or not the MAC we know
    }
    bool fresh = created || !live(p, now);
    memcpy(p->mac, heard->mac, 6);
    p->rssi = heard->rssi;
    p->last_ms = now;
    p->kind = beacon ? OS_PEER_BEACON : OS_PEER_BADGE;
    clean_copy(p->name, sizeof(p->name), n);
    if (beacon) {
        const char *col = cJSON_GetStringValue(cJSON_GetObjectItem(j, "col"));
        clean_copy(p->title, sizeof(p->title), cJSON_GetStringValue(cJSON_GetObjectItem(j, "t")));
        clean_copy(p->character, sizeof(p->character), cJSON_GetStringValue(cJSON_GetObjectItem(j, "ch")));
        clean_copy(p->nonce, sizeof(p->nonce), cJSON_GetStringValue(cJSON_GetObjectItem(j, "nonce")));
        p->color = col ? (uint32_t)strtoul(col, NULL, 16) & 0xFFFFFF : COL_TEXT;
        p->level = 0;
        if (fresh && p->name[0] && now - s_last_beacon_toast > BEACON_GAP_MS && !app_open("quests") &&
                toast_due(p->character[0] ? p->character : p->id, BEACON_TOAST_MS, now)) {
            s_last_beacon_toast = now;
            snprintf(toast, sizeof(toast), "%s is nearby", p->name);
        }
    } else {
        const cJSON *lv = cJSON_GetObjectItem(j, "lv");
        p->level = cJSON_IsNumber(lv) ? (int)lv->valuedouble : 0;
        p->title[0] = p->character[0] = p->nonce[0] = 0;
        p->color = 0;
    }
    xSemaphoreGive(s_lock);
    cJSON_Delete(j);
    if (toast[0]) {
        os_toast("%s", toast);
    }
    return true;
}

// "@bob wants to swap cards" / "@bob wants to trade", from any app but the one
// that handles it.
static void maybe_ask_toast(os_mesh_type_t type, const os_mesh_peer_t *from, const char *json, int64_t now)
{
    if (!strstr(json, "\"req\"") && !strstr(json, "\"invite\"")) {
        return;                             // cheap filter before parsing
    }
    cJSON *j = cJSON_Parse(json);
    const char *op = cJSON_GetStringValue(cJSON_GetObjectItem(j, "op"));
    const char *to = cJSON_GetStringValue(cJSON_GetObjectItem(j, "to"));
    bool xdex = type == OS_MESH_XDEX && op && !strcmp(op, "req");
    bool trade = type == OS_MESH_TRADE && op && !strcmp(op, "invite");
    char name[25];
    clean_copy(name, sizeof(name), from->name);
    if (!name[0] && xdex) {
        const cJSON *card = cJSON_GetObjectItem(j, "card");
        clean_copy(name, sizeof(name), cJSON_GetStringValue(cJSON_GetObjectItem(card, "n")));
    }
    bool mine = to && !strcmp(to, s_id);
    cJSON_Delete(j);
    if (!mine || (!xdex && !trade) || app_open(xdex ? "xdex" : "trade")) {
        return;
    }
    char who[17];
    snprintf(who, sizeof(who), "%s%s", xdex ? "x" : "t", from->id);
    if (now - s_last_ask_toast > ASK_GAP_MS && toast_due(who, ASK_TOAST_MS, now)) {
        s_last_ask_toast = now;
        os_toast("%s wants to %s", name[0] ? name : from->id, xdex ? "swap cards" : "trade");
    }
}

// cJSON parses recursively, ~80 B of stack per level: one frame of nested
// brackets could overflow the mesh task (or any task that parses it later)
// and reboot every badge in range. Nothing deeper than NEST_MAX gets past
// here. Brackets inside strings don't count.
static bool nesting_ok(const char *s, int max)
{
    int depth = 0;
    bool str = false, esc = false;
    for (; *s; s++) {
        if (str) {
            if (esc) {
                esc = false;
            } else if (*s == '\\') {
                esc = true;
            } else if (*s == '"') {
                str = false;
            }
        } else if (*s == '"') {
            str = true;
        } else if (*s == '[' || *s == '{') {
            if (++depth > max) {
                return false;
            }
        } else if (*s == ']' || *s == '}') {
            depth--;
        }
    }
    return true;
}

static void deliver(uint8_t type, const uint8_t *mac, const char *src, int rssi, const char *json, int len, int frags)
{
    int64_t now = now_ms();
    os_mesh_peer_t from = {0};
    if (!nesting_ok(json, NEST_MAX)) {
        s_n.rx_deep++;
        return;
    }
    s_n.rx_msgs++;
    if (frags > 1) {
        s_n.rx_multi++;
    }
    if (s_log) {
        printf("mesh rx type %d from %s (%d B, %d frag%s, %d dBm): %.80s\n", type, src, len, frags,
               frags == 1 ? "" : "s", rssi, json);
    }

    if (type == OS_MESH_HELLO) {
        strlcpy(from.id, src, sizeof(from.id));
        memcpy(from.mac, mac, 6);
        from.rssi = rssi;
        if (!on_hello(&from, json, now)) {
            return;
        }
    }
    // Everything else must come from the MAC a HELLO taught us for that id.
    xSemaphoreTake(s_lock, portMAX_DELAY);
    os_mesh_peer_t *p = find_peer_locked(src);
    bool known = p && !memcmp(p->mac, mac, 6);
    if (known) {
        p->rssi = rssi;
        from = *p;
    }
    xSemaphoreGive(s_lock);
    if (!known) {
        s_n.rx_spoof++;
        return;
    }
    if (type == OS_MESH_XDEX || type == OS_MESH_TRADE) {
        maybe_ask_toast(type, &from, json, now);
    }
    dispatch(type, &from, json);
}

static void on_frame(const rx_t *rx)
{
    const uint8_t *f = rx->data;
    int64_t now = now_ms();
    s_n.rx_frames++;
    if (rx->len < HDR_LEN + 1 || f[0] != 'H' || f[1] != 'G') {
        s_n.rx_bad++;
        return;
    }
    uint8_t type = f[3], idx = f[12], cnt = f[13];
    uint16_t msg = f[10] | f[11] << 8, len = f[14] | f[15] << 8;
    int n = rx->len - HDR_LEN;
    if (f[2] != 1 || type < OS_MESH_HELLO || type >= OS_MESH_TYPE_MAX) {
        return;                             // MESH.md: unknown versions and types are ignored
    }
    char src[OS_MESH_ID_LEN + 1];
    for (int i = 0; i < OS_MESH_ID_LEN; i++) {
        char c = f[4 + i];                  // an emulator's id starts with a lowercase 'e'
        if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (i == 0 && c == 'e'))) {
            s_n.rx_bad++;
            return;
        }
        src[i] = c;
    }
    src[OS_MESH_ID_LEN] = 0;
    int want = idx < cnt - 1 ? FRAG_DATA : (int)len - FRAG_DATA * (cnt - 1);
    if (cnt < 1 || cnt > FRAG_MAX || idx >= cnt || len < 1 || len > OS_MESH_MSG_MAX ||
            (len + FRAG_DATA - 1) / FRAG_DATA != cnt || n != want) {
        s_n.rx_bad++;
        return;
    }
    if (seen(rx->mac, msg, now)) {
        s_n.rx_dup++;
        return;
    }
    if (cnt == 1) {
        mark_seen(rx->mac, msg, now);
        memcpy(s_one, f + HDR_LEN, n);
        s_one[n] = 0;
        deliver(type, rx->mac, src, rx->rssi, s_one, n, 1);
        return;
    }

    reasm_t *r = NULL, *oldest = &s_reasm[0];
    for (int i = 0; i < REASM_SLOTS; i++) {
        reasm_t *s = &s_reasm[i];
        if (s->t0 && s->msg == msg && !memcmp(s->mac, rx->mac, 6)) {
            r = s;
            break;
        }
        if (s->t0 < oldest->t0) {
            oldest = s;
        }
    }
    if (!r) {
        r = oldest;
        if (r->t0) {
            s_n.reasm_lost++;               // every slot busy: the oldest message loses
        }
        memset(r, 0, offsetof(reasm_t, buf));
        memcpy(r->mac, rx->mac, 6);
        memcpy(r->src, src, sizeof(r->src));
        r->msg = msg;
        r->type = type;
        r->cnt = cnt;
        r->len = len;
        r->t0 = now;
    }
    if (r->type != type || r->cnt != cnt || r->len != len) {
        s_n.rx_bad++;
        return;
    }
    if (r->got & (1u << idx)) {
        s_n.rx_dup++;
        return;
    }
    memcpy(r->buf + idx * FRAG_DATA, f + HDR_LEN, n);
    r->got |= 1u << idx;
    if (r->got == (1u << cnt) - 1) {
        mark_seen(r->mac, r->msg, now);
        r->buf[r->len] = 0;
        r->t0 = 0;
        deliver(r->type, r->mac, r->src, rx->rssi, r->buf, r->len, r->cnt);
    }
}

static void expire_reassembly(int64_t now)
{
    for (int i = 0; i < REASM_SLOTS; i++) {
        if (s_reasm[i].t0 && now - s_reasm[i].t0 > REASM_MS) {
            s_reasm[i].t0 = 0;
            s_n.reasm_lost++;
        }
    }
}

// Wi-Fi task (badge) / radio task (emulator): copy and hand over.
static void on_rx(const uint8_t mac[6], const uint8_t *data, int len, int rssi)
{
    if (len <= 0 || len > OS_RADIO_FRAME_MAX || !memcmp(mac, s_mac, 6)) {
        return;
    }
    rx_t rx;
    memcpy(rx.mac, mac, 6);
    rx.rssi = rssi;
    rx.len = len;
    memcpy(rx.data, data, len);
    if (xQueueSend(s_rxq, &rx, 0) != pdTRUE) {
        s_n.rx_drop++;
    }
}

static void mesh_task(void *arg)
{
    static rx_t rx;                     // 260 B: keep it off the task stack
    int64_t next_hello = now_ms() + HELLO_MS, next_maintain = 0, next_sync = 0;
    for (;;) {
        int64_t now = now_ms();
        int64_t wait = (next_hello < next_maintain ? next_hello : next_maintain) - now;
        if (wait < 0) {
            wait = 0;
        }
        if (xQueueReceive(s_rxq, &rx, pdMS_TO_TICKS(wait))) {
            if (!paused()) {
                on_frame(&rx);
            }
        }
        ulTaskNotifyTake(pdTRUE, 0);    // acquire nudges us to take the channel now
        now = now_ms();
        if (now >= next_maintain) {
            next_maintain = now + MAINTAIN_MS;
            if (now >= next_sync) {
                next_sync = now + SYNC_MS;
                sync_cloud();
            }
            hold_channel();
            expire_reassembly(now);
        }
        if (now >= next_hello) {
            next_hello = now + HELLO_MS + esp_random() % 1000;
            if (on_channel() && s_id[0]) {
                send_hello();
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Console

static const char *const type_names[OS_MESH_TYPE_MAX] = {
    [OS_MESH_HELLO] = "hello", [OS_MESH_XDEX] = "xdex", [OS_MESH_TRADE] = "trade",
    [OS_MESH_RELAY] = "relay", [OS_MESH_RELAY_RESP] = "relay_resp", [OS_MESH_APP] = "app",
};

static int selftest(void)
{
    static const struct {
        const char *s, *tag;
    } v[] = {
        {"xdex|7K2Q9M|9XQ3TT|0011223344556677", "bc6e8523fe5d601c"},
        {"trade|7K2Q9M|3f2a9c1e00b14d77|7K2Q9M|9XQ3TT|50|it_8f2k,it_k2m1||0||p007,p013", "cfa9b760dfdf0de4"},
        {"relay|7K2Q9M|BCN001|a1b2c3d4e5f60718|17|{\"op\":\"open\"}", "81402ef7e089ca97"},
    };
    uint8_t key[16], digest[32];
    char hex[65];
    int fails = 0;
    for (int i = 0; i < 16; i++) {
        key[i] = i;
    }
    sha256_t s;
    sha256_init(&s);
    sha256_update(&s, "abc", 3);
    sha256_final(&s, digest);
    to_hex(digest, 32, hex);
    bool ok = !strcmp(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    fails += !ok;
    printf("%s sha256(\"abc\")\n", ok ? "PASS" : "FAIL");
    for (int i = 0; i < 3; i++) {
        char tag[OS_MESH_TAG_LEN + 1];
        tag_with(key, v[i].s, tag);
        ok = !strcmp(tag, v[i].tag);
        fails += !ok;
        printf("%s tag %s  (%.40s%s)\n", ok ? "PASS" : "FAIL", tag, v[i].s, strlen(v[i].s) > 40 ? "..." : "");
    }
    // A frame of nested brackets must not reach cJSON, and cJSON itself must
    // refuse deep nesting (CJSON_NESTING_LIMIT in both builds).
    char *deep = malloc(FRAG_DATA + 1);
    if (deep) {
        memset(deep, '[', FRAG_DATA);
        deep[FRAG_DATA] = 0;
        ok = !nesting_ok(deep, NEST_MAX) && nesting_ok("{\"k\":\"b\",\"n\":\"[[[[[[[[[[\",\"d\":[[[1]]]}", NEST_MAX);
        fails += !ok;
        printf("%s nesting scan drops %d nested brackets, passes brackets in strings\n", ok ? "PASS" : "FAIL", FRAG_DATA);
        memset(deep, 0, FRAG_DATA + 1);
        for (int i = 0; i < 40; i++) {
            deep[i] = '[';
            deep[79 - i] = ']';
        }
        cJSON *j = cJSON_Parse(deep);
        ok = j == NULL;
        cJSON_Delete(j);
        fails += !ok;
        printf("%s cJSON refuses 40 nested arrays\n", ok ? "PASS" : "FAIL");
        free(deep);
    }
    printf("mesh selftest: %s\n", fails ? "FAILED" : "all passed");
    return fails ? 1 : 0;
}

static void print_status(void)
{
    os_wifi_info_t w;
    os_wifi_get_info(&w);
    printf("mesh: id %s  channel %d (radio on %d)  %s%s  key %s  acquired %d%s\n", s_id[0] ? s_id : "-", s_channel,
           os_radio_channel(), os_mesh_ready() ? "ready" : on_channel() ? "on channel" : "off channel",
           paused() ? " (paused: radio lent out)" : "", s_has_key ? "yes" : "no", s_acquired,
           s_took_wifi ? " (Wi-Fi suspended by mesh)" : "");
    if (s_override) {
        printf("  channel %d chosen on the badge; the server announced %d (\"mesh channel auto\" goes back)\n", s_override,
               s_server_ch);
    }
    printf("  hello: '%s' Lv %d, %lu sent\n", s_label, s_level, (unsigned long)s_n.hello_tx);
    printf("  tx: %lu msgs, %lu frames, %lu errors\n", (unsigned long)s_n.tx_msgs, (unsigned long)s_n.tx_frames,
           (unsigned long)s_n.tx_err);
    printf("  rx: %lu frames, %lu msgs (%lu multi-fragment), %lu dup, %lu bad, %lu dropped, %lu lost in reassembly\n",
           (unsigned long)s_n.rx_frames, (unsigned long)s_n.rx_msgs, (unsigned long)s_n.rx_multi,
           (unsigned long)s_n.rx_dup, (unsigned long)s_n.rx_bad, (unsigned long)s_n.rx_drop,
           (unsigned long)s_n.reasm_lost);
    printf("  refused: %lu from an unknown or changed MAC, %lu nested too deep\n", (unsigned long)s_n.rx_spoof,
           (unsigned long)s_n.rx_deep);
    printf("  mesh task stack left %u B\n", (unsigned)uxTaskGetStackHighWaterMark(s_task));
}

static void print_peers(void)
{
    os_mesh_peer_t *list = malloc(PEER_MAX * sizeof(*list));
    int n = list ? os_mesh_peers(list, PEER_MAX) : 0;
    int64_t now = now_ms();
    for (int i = 0; i < n; i++) {
        const os_mesh_peer_t *p = &list[i];
        printf("%s %-6s %02x:%02x:%02x:%02x:%02x:%02x %4d dBm %3llds  %s", p->kind == OS_PEER_BEACON ? "beacon" : "badge ",
               p->id, p->mac[0], p->mac[1], p->mac[2], p->mac[3], p->mac[4], p->mac[5], p->rssi,
               (long long)((now - p->last_ms) / 1000), p->name[0] ? p->name : "(unclaimed)");
        if (p->kind == OS_PEER_BEACON) {
            printf(" / %s [%s] #%06lX nonce %s\n", p->title, p->character, (unsigned long)p->color, p->nonce);
        } else {
            printf(" Lv %d\n", p->level);
        }
    }
    printf("%d peer%s\n", n, n == 1 ? "" : "s");
    free(list);
}

// mesh send <type> <id|*> <json, ' for "> | big <bytes>
static int cmd_send(int argc, char **argv)
{
    int type = 0;
    for (int i = 1; i < OS_MESH_TYPE_MAX; i++) {
        if (!strcmp(argv[2], type_names[i]) || atoi(argv[2]) == i) {
            type = i;
        }
    }
    if (!type) {
        printf("unknown type '%s'\n", argv[2]);
        return 1;
    }
    char *json = calloc(1, OS_MESH_MSG_MAX + 1);
    if (!json) {
        return 1;
    }
    if (!strcmp(argv[4], "big")) {
        int n = argc > 5 ? atoi(argv[5]) : 600;
        n = n < 40 ? 40 : n > OS_MESH_MSG_MAX ? OS_MESH_MSG_MAX : n;
        int head = snprintf(json, OS_MESH_MSG_MAX, "{\"app\":\"test\",\"d\":{\"pad\":\"");
        for (int i = head; i < n - 4; i++) {
            json[i] = 'a' + (i % 26);
        }
        strcpy(json + n - 4, "\"}}");
    } else {
        for (int i = 4; i < argc; i++) {
            strlcat(json, argv[i], OS_MESH_MSG_MAX + 1);
            if (i + 1 < argc) {
                strlcat(json, " ", OS_MESH_MSG_MAX + 1);
            }
        }
        for (char *c = json; *c; c++) {
            if (*c == '\'') {
                *c = '"';                   // the badge console eats double quotes
            }
        }
    }
    esp_err_t err = os_mesh_send(type, argv[3], json);
    printf("sent %s (%u B): %s\n", type_names[type], (unsigned)strlen(json), esp_err_to_name(err));
    free(json);
    return err == ESP_OK ? 0 : 1;
}

static int cmd_mesh(int argc, char **argv)
{
    const char *sub = argc > 1 ? argv[1] : "status";
    if (!strcmp(sub, "status")) {
        print_status();
    } else if (!strcmp(sub, "peers")) {
        print_peers();
    } else if (!strcmp(sub, "selftest")) {
        return selftest();
    } else if (!strcmp(sub, "log") && argc == 3) {
        s_log = !strcmp(argv[2], "on");
    } else if (!strcmp(sub, "send") && argc >= 5) {
        return cmd_send(argc, argv);
    } else if (!strcmp(sub, "channel") && argc == 3) {
        os_mesh_set_channel(strcmp(argv[2], "auto") ? atoi(argv[2]) : 0);
    } else if (!strcmp(sub, "acquire")) {
        os_mesh_acquire();
    } else if (!strcmp(sub, "release")) {
        os_mesh_release();
    } else {
        printf("usage: mesh [status|peers|selftest|log on|off|channel <n|auto>|acquire|release|send <type> <id|*> <json>|big <bytes>]\n");
        return 1;
    }
    return 0;
}

// ---------------------------------------------------------------------------

void os_mesh_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_tx_lock = xSemaphoreCreateMutex();
    s_sub_lock = xSemaphoreCreateRecursiveMutex();
    s_rxq = xQueueCreate(RXQ_LEN, sizeof(rx_t));
    s_msg = esp_random();                       // a quick reboot must not look like duplicates
    os_radio_mac(s_mac);
    os_cmd_register("mesh", "mesh [status|peers|selftest|log on|off|channel <n|auto>|acquire|release|send <type> <id|*> <json>]",
                    cmd_mesh);
    xTaskCreate(mesh_task, "mesh", 4096, NULL, 3, &s_task);
    esp_err_t err = os_radio_init(on_rx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "radio unavailable: %s", esp_err_to_name(err));
    }
}
