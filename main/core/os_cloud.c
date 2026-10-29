// Badge <-> server link, see os_cloud.h and server/API.md.
//
// One low-priority task owns the conversation: it waits for Wi-Fi and a
// configured server, says hello, and then repeats every `poll` seconds (or
// with backoff after errors). Between hellos it runs the device API queue and
// the outbox (os_api.c, through os_net.h), so the badge never has two HTTP
// requests in flight, and a Lua app's socket (os_socket.c) in place of all
// that until it closes. Everything it learns goes into s_info under a mutex;
// apps and the console only read snapshots of it. The owner's figures, the
// mesh key and channel live in NVS, because the badge must work offline
// (Wi-Fi is often off).
//
// Portable: only os_http, os_wifi_state(), NVS, the MAC, the app version,
// FreeRTOS tasks and cJSON. The emulator compiles this file unchanged and can
// override the identity below with -D flags.

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "cJSON.h"
#include "os.h"
#include "os_cloud.h"
#include "os_cmd.h"
#include "os_data.h"
#include "os_http.h"
#include "os_net.h"
#include "os_wifi.h"

static const char *TAG = "os_cloud";

#ifndef CONFIG_SOLANA_OS_SERVER_URL
#define CONFIG_SOLANA_OS_SERVER_URL ""
#endif
#ifndef CONFIG_SOLANA_OS_MESH_CHANNEL
#define CONFIG_SOLANA_OS_MESH_CHANNEL 1
#endif
#ifndef OS_CLOUD_MODEL
#define OS_CLOUD_MODEL      "bp26"
#endif
#ifndef OS_CLOUD_HW_PREFIX
#define OS_CLOUD_HW_PREFIX  ""          // the emulator uses "emu-"
#endif

#define NVS_NS          "cloud"
#define RESP_CAP        1024            // API.md: responses stay under 1 KB
#define POLL_DEFAULT_S  60
#define BACKOFF_MIN_S   15              // API.md: never faster than 15 s on errors
#define BACKOFF_MAX_S   120             // ...and at most the last `poll` once we have one
#define ERRORS_TO_SHOW  3               // transient failures keep the last good screen
#define WIFI_CHECK_MS   1000
#define DEFER_S         10              // retry delay when the heap can't hold a request
#define HOME_WAIT_MS    3000            // owner change: wait this long for the launcher
#define LINKED_POLL_S   60              // 409 "linked": ask again until the owner releases it
#define OWNER_FILE      "owner"         // OS_DATA_DIR/owner: the owner tag, beside the owner's data

// Task notification bits.
#define BIT_REFRESH     (1u << 0)       // hello now
#define BIT_WORK        (1u << 1)       // API request or outbox entry queued
#define BIT_RESET       (1u << 2)       // factory reset

extern const os_app_t app_wifi, app_badge;

static SemaphoreHandle_t s_lock;
static TaskHandle_t s_task;
static os_cloud_info_t s_info;
static char s_secret[96];
static uint8_t s_key[16];
static char s_owner_tag[9];             // last owner's tag, kept while unclaimed
static int64_t s_next_ms = -1;          // next hello, -1 = not scheduled
static int64_t s_time_off;              // server unix ms - esp_timer ms, 0 = never synced
static int s_poll_s;                    // last `poll` from the server, 0 = none yet

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static void lock(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
}

static void unlock(void)
{
    xSemaphoreGive(s_lock);
}

// ---------------------------------------------------------------------------
// NVS "cloud": server, id, secret, key, mesh channel and the owner's figures

static void nvs_load(const char *key, char *out, size_t n)
{
    nvs_handle_t h;
    out[0] = 0;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        size_t len = n;
        if (nvs_get_str(h, key, out, &len) != ESP_OK) {
            out[0] = 0;
        }
        nvs_close(h);
    }
}

static int nvs_load_int(const char *key, int def)
{
    nvs_handle_t h;
    int32_t v = def;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_i32(h, key, &v);
        nvs_close(h);
    }
    return v;
}

static void nvs_save(const char *key, const char *val)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGE(TAG, "nvs open failed");
        return;
    }
    if (val && val[0]) {
        nvs_set_str(h, key, val);
    } else {
        nvs_erase_key(h, key);
    }
    nvs_commit(h);
    nvs_close(h);
}

static void nvs_save_int(const char *key, int val)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_i32(h, key, val);
        nvs_commit(h);
        nvs_close(h);
    }
}

static bool hex_decode(const char *hex, uint8_t *out, size_t n)
{
    if (!hex || strlen(hex) != n * 2) {
        return false;
    }
    for (size_t i = 0; i < n * 2; i++) {
        char c = hex[i];
        int v = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (v < 0) {
            return false;
        }
        out[i / 2] = i % 2 ? (out[i / 2] | v) : v << 4;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Hello

typedef struct {
    int status;                     // HTTP status, 0 = no response
    bool ok, claimed;
    bool linked;                    // 409: claimed on the server, and we have no valid secret
    bool needs_x;                   // "welcome":"needs_x"
    bool deferred;                  // not sent: too little heap right now (e.g. a big app is open)
    char id[16], secret[96], claim_url[160], owner[64];
    char key[33], user[9];
    char message[160];              // linked: the server's text
    char update[32], update_sha[65];    // "update": a Solana OS build on offer, "" = none
    char site[96];                      // "site", printable ASCII, "" = none
    int update_size;
    int poll, mesh, lamps, level, packs;
    int64_t time;
    const char *error;
} hello_t;

// Owner label: "@username", else the display name (ASCII only: the fonts
// have no other glyphs), else a shortened wallet.
static void owner_label(const cJSON *o, char *out, size_t n)
{
    const char *user = cJSON_GetStringValue(cJSON_GetObjectItem(o, "username"));
    const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(o, "name"));
    const char *wallet = cJSON_GetStringValue(cJSON_GetObjectItem(o, "wallet"));
    out[0] = 0;
    if (user && user[0]) {
        snprintf(out, n, "@%s", user);
        return;
    }
    if (name) {
        size_t j = 0;
        for (const char *p = name; *p && j + 1 < n; p++) {
            if (*p >= 0x20 && *p < 0x7F) {
                out[j++] = *p;
            }
        }
        while (j && out[j - 1] == ' ') {
            j--;
        }
        out[j] = 0;
        if (out[0] && out[0] != ' ') {
            return;
        }
    }
    if (wallet && strlen(wallet) > 10) {
        snprintf(out, n, "%.4s..%s", wallet, wallet + strlen(wallet) - 4);
    } else {
        strlcpy(out, wallet ? wallet : "someone", n);
    }
}

static int num_or(const cJSON *j, const char *key, int def)
{
    const cJSON *v = cJSON_GetObjectItem(j, key);
    return cJSON_IsNumber(v) ? (int)v->valuedouble : def;
}

static void parse(const char *body, hello_t *r)
{
    cJSON *j = cJSON_Parse(body);
    if (!j) {
        r->error = "Bad response from the server";
        return;
    }
    if (r->status != 200) {
        switch (r->status / 100) {
        case 4:  r->error = "The server refused the badge"; break;
        case 5:  r->error = "The server had an error"; break;
        default: r->error = "Unexpected server response"; break;
        }
        const char *msg = cJSON_GetStringValue(cJSON_GetObjectItem(j, "error"));
        ESP_LOGW(TAG, "hello: HTTP %d %s", r->status, msg ? msg : "");
        if (r->status == 409 && cJSON_IsTrue(cJSON_GetObjectItem(j, "linked"))) {
            r->linked = true;
            r->error = "Linked to an account";
            size_t k = 0;               // printable ASCII only: the fonts have nothing else
            for (const char *p = msg ? msg : ""; *p && k + 1 < sizeof(r->message); p++) {
                if (*p >= 0x20 && *p < 0x7F) {
                    r->message[k++] = *p;
                }
            }
            r->message[k] = 0;
        }
        cJSON_Delete(j);
        return;
    }
    const char *id = cJSON_GetStringValue(cJSON_GetObjectItem(j, "id"));
    const char *secret = cJSON_GetStringValue(cJSON_GetObjectItem(j, "secret"));
    const char *key = cJSON_GetStringValue(cJSON_GetObjectItem(j, "key"));
    const char *user = cJSON_GetStringValue(cJSON_GetObjectItem(j, "user"));
    const char *url = cJSON_GetStringValue(cJSON_GetObjectItem(j, "claimUrl"));
    const cJSON *owner = cJSON_GetObjectItem(j, "owner");
    const cJSON *time = cJSON_GetObjectItem(j, "time");
    r->claimed = cJSON_IsTrue(cJSON_GetObjectItem(j, "claimed"));
    const char *welcome = cJSON_GetStringValue(cJSON_GetObjectItem(j, "welcome"));
    r->needs_x = welcome && !strcmp(welcome, "needs_x");
    r->poll = num_or(j, "poll", POLL_DEFAULT_S);
    r->mesh = num_or(j, "mesh", 0);
    r->lamps = num_or(j, "lamps", 0);
    r->level = num_or(j, "level", 0);
    r->packs = num_or(j, "packs", 0);
    r->time = cJSON_IsNumber(time) ? (int64_t)time->valuedouble : 0;
    const cJSON *up = cJSON_GetObjectItem(j, "update");
    const char *uv = cJSON_GetStringValue(cJSON_GetObjectItem(up, "version"));
    const char *us = cJSON_GetStringValue(cJSON_GetObjectItem(up, "sha256"));
    int usize = num_or(up, "size", 0);
    bool uok = uv && uv[0] && strlen(uv) < sizeof(r->update) && us && strlen(us) == 64 && usize > 0;
    for (const char *p = uv; uok && *p; p++) {
        uok = *p > 0x20 && *p < 0x7F;   // shown on screen and sent back in a URL
    }
    const char *site = cJSON_GetStringValue(cJSON_GetObjectItem(j, "site"));
    bool sok = site && strlen(site) < sizeof(r->site);
    for (const char *p = site; sok && *p; p++) {
        sok = *p > 0x20 && *p < 0x7F;   // shown on screen
    }
    if (sok) {
        strlcpy(r->site, site, sizeof(r->site));
    }
    if (uok) {
        strlcpy(r->update, uv, sizeof(r->update));
        strlcpy(r->update_sha, us, sizeof(r->update_sha));
        r->update_size = usize;
    }
    if (!id || !id[0] || strlen(id) >= sizeof(r->id)) {
        r->error = "Bad response from the server";
    } else if (!r->claimed && (!url || strlen(url) >= sizeof(r->claim_url))) {
        r->error = "No claim link from the server";
    } else {
        strlcpy(r->id, id, sizeof(r->id));
        if (secret && strlen(secret) < sizeof(r->secret)) {
            strlcpy(r->secret, secret, sizeof(r->secret));
        }
        uint8_t k[16];
        if (key && hex_decode(key, k, sizeof(k))) {
            strlcpy(r->key, key, sizeof(r->key));
        }
        if (url && !r->claimed) {
            strlcpy(r->claim_url, url, sizeof(r->claim_url));
        }
        if (r->claimed) {
            owner_label(cJSON_IsObject(owner) ? owner : NULL, r->owner, sizeof(r->owner));
            if (user && strlen(user) < sizeof(r->user)) {
                strlcpy(r->user, user, sizeof(r->user));
            }
        }
        r->ok = true;
    }
    cJSON_Delete(j);
}

static void hello(const char *server, const char *secret, bool need_key, hello_t *r)
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char url[128], body[160];
    snprintf(url, sizeof(url), "%s/api/badge/hello", server);
    snprintf(body, sizeof(body),
             "{\"hw\":\"%s%02x%02x%02x%02x%02x%02x\",\"fw\":\"%s\",\"model\":\"%s\",\"needKey\":%s}",
             OS_CLOUD_HW_PREFIX, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
             esp_app_get_description()->version, OS_CLOUD_MODEL, need_key ? "true" : "false");

    char *resp = malloc(RESP_CAP);
    if (!resp) {
        r->deferred = true;
        return;
    }
    int64_t t0 = now_ms();
    esp_err_t err = os_http_request("POST", url, secret[0] ? secret : NULL, body, resp, RESP_CAP, &r->status);
    if (err == ESP_ERR_NO_MEM) {
        r->deferred = true;
    } else if (err != ESP_OK) {
        r->error = "Can't reach the server";
    } else {
        parse(resp, r);
        if (r->ok && r->time) {
            // The server stamped `time` somewhere in the round trip: assume the middle.
            int64_t t1 = now_ms();
            r->time -= (t0 + t1) / 2;
        }
    }
    free(resp);
}

// A different owner claimed this badge: /sdcard/hh belongs to the previous
// one (docs/HHGTS.md). Go home first so no app has its files open, then
// wipe, and only then remember the new owner, so an interrupted wipe is
// redone at the next hello.
static void owner_changed(const char *user)
{
    ESP_LOGW(TAG, "new owner %s (was %s): wiping %s", user, s_owner_tag[0] ? s_owner_tag : "?", OS_DATA_DIR);
    os_launch(&os_launcher);
    for (int waited = 0; os_current() != &os_launcher && waited < HOME_WAIT_MS; waited += 50) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    os_outbox_clear();
    os_data_wipe();
}

// The owner tag is kept in NVS and on the card (OS_DATA_DIR/owner). Either
// one naming someone else means the card holds a previous owner's data: an
// NVS-only erase (/stock "Erase all data") leaves the card as it was, and a
// card can move between badges.
static void check_owner(const char *user)
{
    char sd[16] = "";
    char *raw = os_data_read(OWNER_FILE, sizeof(sd) - 1, NULL);
    if (raw) {
        strlcpy(sd, raw, sizeof(sd));
        sd[strcspn(sd, "\r\n ")] = 0;
        free(raw);
    }
    bool other = (s_owner_tag[0] && strcmp(user, s_owner_tag)) || (sd[0] && strcmp(user, sd));
    if (other) {
        owner_changed(user);
    }
    if (other || strcmp(sd, user)) {
        os_data_write(OWNER_FILE, user, strlen(user));
    }
}

// ---------------------------------------------------------------------------
// Task

// Apply a hello result. Returns the delay until the next one, in seconds.
static int apply(const char *server, const hello_t *r, int *errors, int *backoff)
{
    os_cloud_state_t prev;
    bool opened = false;
    static bool s_auto_opened;          // auto-open My Badge once per boot
    static char s_update_told[32];      // the update toasted this boot
    char toast[80] = "", id[sizeof(s_info.id)];

    if (r->deferred) {
        return DEFER_S;                 // not the server's fault; keep the current state
    }
    if (r->ok && r->claimed && r->user[0]) {
        check_owner(r->user);
    }
    lock();
    if (strcmp(server, s_info.server)) {
        unlock();                       // the server changed while we asked the old one
        return 0;
    }
    prev = s_info.state;
    s_info.http_status = r->status;
    if (r->ok) {
        *errors = 0;
        *backoff = BACKOFF_MIN_S;
        if (strcmp(s_info.id, r->id)) {
            strlcpy(s_info.id, r->id, sizeof(s_info.id));
            nvs_save("id", r->id);
        }
        if (r->secret[0] && strcmp(s_secret, r->secret)) {
            strlcpy(s_secret, r->secret, sizeof(s_secret));
            nvs_save("secret", r->secret);
            ESP_LOGI(TAG, "new secret stored");
        }
        if (r->key[0]) {
            uint8_t k[16];
            hex_decode(r->key, k, sizeof(k));
            if (!s_info.has_key || memcmp(k, s_key, sizeof(k))) {
                memcpy(s_key, k, sizeof(k));
                s_info.has_key = true;
                nvs_save("key", r->key);
                ESP_LOGI(TAG, "mesh key stored");
            }
        }
        if (r->mesh >= 1 && r->mesh <= 13 && r->mesh != s_info.mesh_channel) {
            s_info.mesh_channel = r->mesh;
            nvs_save_int("mesh", r->mesh);
        }
        if (r->time) {
            s_time_off = r->time;
        }
        strlcpy(s_info.claim_url, r->claim_url, sizeof(s_info.claim_url));
        if (r->site[0]) {
            strlcpy(s_info.site, r->site, sizeof(s_info.site));
        }
        if (r->claimed) {
            if (strcmp(s_info.owner, r->owner)) {
                nvs_save("owner", r->owner);
            }
            if (r->user[0] && strcmp(s_owner_tag, r->user)) {
                strlcpy(s_owner_tag, r->user, sizeof(s_owner_tag));
                nvs_save("user", r->user);
            }
            if (s_info.lamps != r->lamps || s_info.level != r->level || s_info.packs != r->packs || !s_info.claimed) {
                nvs_save_int("lamps", r->lamps);
                nvs_save_int("level", r->level);
                nvs_save_int("packs", r->packs);
                nvs_save_int("claimed", 1);
            }
            strlcpy(s_info.owner, r->owner, sizeof(s_info.owner));
            strlcpy(s_info.user, s_owner_tag, sizeof(s_info.user));
            s_info.lamps = r->lamps;
            s_info.level = r->level;
            s_info.packs = r->packs;
        } else if (s_info.claimed) {
            // Released: forget the figures, keep the tag (the same owner may claim it again).
            nvs_save("owner", "");
            nvs_save_int("claimed", 0);
            s_info.owner[0] = s_info.user[0] = 0;
            s_info.lamps = s_info.level = s_info.packs = 0;
        }
        s_info.claimed = r->claimed;
        s_info.welcome_needs_x = r->claimed && r->needs_x;
        strlcpy(s_info.update, r->update, sizeof(s_info.update));
        strlcpy(s_info.update_sha, r->update_sha, sizeof(s_info.update_sha));
        s_info.update_size = r->update_size;
        if (r->update[0] && strcmp(r->update, s_update_told)) {
            strlcpy(s_update_told, r->update, sizeof(s_update_told));
            snprintf(toast, sizeof(toast), "Solana OS %s is out: Settings > Software update", r->update);
        }
        s_info.synced_ms = now_ms();
        s_info.state = r->claimed ? OS_CLOUD_CLAIMED : OS_CLOUD_UNCLAIMED;
        s_info.error = NULL;
    } else if (r->linked) {
        // Our secret is gone (NVS erased) but someone owns this badge on the
        // server: nothing to do here until they release it on the web.
        s_info.error = r->error;
        strlcpy(s_info.message, r->message[0] ? r->message : "Release it on the web, then restart the badge.",
                sizeof(s_info.message));
        s_info.state = OS_CLOUD_LINKED;
    } else {
        s_info.error = r->error;
        // Keep the QR / owner on screen through a blip; the data is still valid.
        bool good = prev == OS_CLOUD_UNCLAIMED || prev == OS_CLOUD_CLAIMED;
        if (!good || ++*errors >= ERRORS_TO_SHOW) {
            s_info.state = OS_CLOUD_ERROR;
        }
    }
    if (s_info.state != OS_CLOUD_LINKED) {
        s_info.message[0] = 0;
    }
    os_cloud_state_t now = s_info.state;
    strlcpy(id, s_info.id, sizeof(id));
    if (now == OS_CLOUD_UNCLAIMED && prev != OS_CLOUD_UNCLAIMED && !s_auto_opened) {
        s_auto_opened = true;
        const os_app_t *cur = os_current();
        opened = cur == &os_launcher || cur == &app_wifi;
    }
    if (now == OS_CLOUD_CLAIMED && prev == OS_CLOUD_UNCLAIMED) {
        s_update_told[0] = 0;           // the update's toast waits for the next hello
        snprintf(toast, sizeof(toast), "Linked to %s", s_info.owner);
    }
    unlock();

    if (now != prev) {
        ESP_LOGI(TAG, "%s id=%s %s", now == OS_CLOUD_CLAIMED ? "claimed" : now == OS_CLOUD_UNCLAIMED ? "unclaimed"
                 : now == OS_CLOUD_LINKED ? "linked elsewhere" : "error", id, now == OS_CLOUD_ERROR ? r->error : "");
    }
    if (opened) {
        os_launch(&app_badge);
    }
    if (toast[0]) {
        os_toast("%s", toast);
    }

    if (r->ok) {
        s_poll_s = r->poll < 5 ? 5 : r->poll > 3600 ? 3600 : r->poll;
        return s_poll_s;
    }
    if (r->linked) {
        *errors = 0;
        return LINKED_POLL_S;
    }
    int cap = s_poll_s > BACKOFF_MIN_S ? s_poll_s : s_poll_s ? BACKOFF_MIN_S : BACKOFF_MAX_S;
    int d = *backoff < cap ? *backoff : cap;
    *backoff = *backoff * 2;
    if (*backoff > cap) {
        *backoff = cap;
    }
    return d;
}

static void set_offline(void)
{
    lock();
    s_info.state = OS_CLOUD_OFFLINE;
    s_info.error = NULL;
    s_info.busy = false;
    s_next_ms = -1;
    unlock();
}

// Factory reset (docs/HHGTS.md). Online, the server releases the owner and
// rotates the secret and mesh key; offline the owner releases it on the web.
static void factory_reset(void)
{
    char server[sizeof(s_info.server)], secret[sizeof(s_secret)];
    lock();
    strlcpy(server, s_info.server, sizeof(server));
    strlcpy(secret, s_secret, sizeof(secret));
    unlock();
    ESP_LOGW(TAG, "factory reset");
    if (server[0] && secret[0] && os_wifi_state() == OS_WIFI_CONNECTED) {
        char url[128], resp[96];
        int status = 0;
        snprintf(url, sizeof(url), "%s/api/device/reset", server);
        esp_err_t err = os_http_request("POST", url, secret, "{}", resp, sizeof(resp), &status);
        ESP_LOGI(TAG, "reset on the server: %s, HTTP %d", esp_err_to_name(err), status);
    }
    os_outbox_clear();
    os_data_wipe();                     // before NVS, so the owner tag outlives a half-done wipe
    nvs_flash_erase();
    ESP_LOGW(TAG, "erased; rebooting");
    esp_restart();
}

static void cloud_task(void *arg)
{
    bool online = false;
    int errors = 0, backoff = BACKOFF_MIN_S;
    char server[sizeof(s_info.server)], secret[sizeof(s_secret)], prev_server[sizeof(s_info.server)] = "";
    hello_t *r = malloc(sizeof(hello_t));
    assert(r);

    for (;;) {
        uint32_t bits = 0;
        lock();
        strlcpy(server, s_info.server, sizeof(server));
        strlcpy(secret, s_secret, sizeof(secret));
        bool need_key = secret[0] && !s_info.has_key;
        int64_t next = s_next_ms;
        unlock();
        if (strcmp(server, prev_server)) {
            strlcpy(prev_server, server, sizeof(prev_server));
            s_poll_s = 0;
            errors = 0;
            backoff = BACKOFF_MIN_S;
        }

        if (!server[0] || os_wifi_state() != OS_WIFI_CONNECTED) {
            if (online) {
                online = false;
                set_offline();
                os_http_close();        // the connection died with the Wi-Fi; free its TLS state
            }
            while (os_api_service(false)) {
                // fail what was queued: no network
            }
            os_socket_service(false);   // and an app's socket that hadn't started
            xTaskNotifyWait(0, UINT32_MAX, &bits, pdMS_TO_TICKS(WIFI_CHECK_MS));
            if (bits & BIT_RESET) {
                factory_reset();
            }
            continue;
        }
        if (!online) {
            // Fresh connection: hello right away.
            online = true;
            errors = 0;
            backoff = BACKOFF_MIN_S;
            next = 0;
            lock();
            if (s_info.state == OS_CLOUD_OFFLINE || s_info.state == OS_CLOUD_ERROR) {
                s_info.state = OS_CLOUD_CONNECTING;
            }
            unlock();
        }

        int64_t now = now_ms();
        if (next >= 0 && now < next) {
            // Between hellos: API requests and the outbox, then sleep until
            // due, a refresh, new work, or the next Wi-Fi check.
            if (os_api_service(os_cloud_registered())) {
                continue;
            }
            if (os_socket_service(os_cloud_registered())) {
                continue;               // an app's socket ran (in place of the server connection) until it ended
            }
            os_http_tick();             // close the server connection when it has been idle a while
            int64_t wait = next - now;
            xTaskNotifyWait(0, UINT32_MAX, &bits, pdMS_TO_TICKS(wait > WIFI_CHECK_MS ? WIFI_CHECK_MS : wait));
            if (bits & BIT_RESET) {
                factory_reset();
            }
            if (bits & BIT_REFRESH) {
                lock();
                s_next_ms = 0;
                unlock();
            }
            continue;
        }

        lock();
        s_info.busy = true;
        if (s_info.state == OS_CLOUD_OFFLINE) {
            s_info.state = OS_CLOUD_CONNECTING;
        }
        unlock();
        memset(r, 0, sizeof(*r));
        hello(server, secret, need_key, r);
        int delay_s = apply(server, r, &errors, &backoff);
        lock();
        s_info.busy = false;
        // A server change or wifi drop during the request resets the schedule.
        if (!strcmp(server, s_info.server)) {
            s_next_ms = now_ms() + delay_s * 1000LL;
        }
        unlock();
        ESP_LOGD(TAG, "next hello in %d s, stack left %u", delay_s, (unsigned)uxTaskGetStackHighWaterMark(NULL));
    }
}

static int cmd_reset(int argc, char **argv)
{
    if (argc != 2 || strcmp(argv[1], "yes")) {
        printf("Erases this badge: releases its owner (if online), wipes %s and NVS, reboots.\nRun: reset yes\n",
               OS_DATA_DIR);
        return 1;
    }
    os_cloud_factory_reset();
    return 0;
}

// ---------------------------------------------------------------------------
// API

void os_cloud_init(void)
{
    char key[33];
    s_lock = xSemaphoreCreateMutex();
    nvs_load("server", s_info.server, sizeof(s_info.server));
    if (!s_info.server[0]) {
        strlcpy(s_info.server, CONFIG_SOLANA_OS_SERVER_URL, sizeof(s_info.server));
    }
    nvs_load("id", s_info.id, sizeof(s_info.id));
    nvs_load("secret", s_secret, sizeof(s_secret));
    nvs_load("key", key, sizeof(key));
    s_info.has_key = hex_decode(key, s_key, sizeof(s_key));
    s_info.mesh_channel = nvs_load_int("mesh", CONFIG_SOLANA_OS_MESH_CHANNEL);
    nvs_load("user", s_owner_tag, sizeof(s_owner_tag));
    s_info.claimed = nvs_load_int("claimed", 0) == 1;
    if (s_info.claimed) {
        nvs_load("owner", s_info.owner, sizeof(s_info.owner));
        strlcpy(s_info.user, s_owner_tag, sizeof(s_info.user));
        s_info.lamps = nvs_load_int("lamps", 0);
        s_info.level = nvs_load_int("level", 0);
        s_info.packs = nvs_load_int("packs", 0);
    }
    s_info.state = OS_CLOUD_OFFLINE;
    ESP_LOGI(TAG, "server '%s', badge id '%s'%s%s%s", s_info.server, s_info.id, s_secret[0] ? ", has secret" : "",
             s_info.has_key ? ", has mesh key" : "", s_info.claimed ? ", claimed" : "");
    os_api_init();
    os_socket_init();
    os_cmd_register("reset", "reset yes - factory reset: release the owner, wipe /sdcard/hh and NVS", cmd_reset);
    // HTTPS peaks at ~3.4 KB of stack; outbox lines and downloads add file I/O.
    xTaskCreate(cloud_task, "cloud", 6144, NULL, 2, &s_task);
}

void os_cloud_get_info(os_cloud_info_t *out)
{
    lock();
    *out = s_info;
    out->retry_s = s_next_ms < 0 ? -1 : s_next_ms <= now_ms() ? 0 : (int)((s_next_ms - now_ms() + 999) / 1000);
    unlock();
}

void os_cloud_refresh(void)
{
    if (s_task) {
        xTaskNotify(s_task, BIT_REFRESH, eSetBits);
    }
}

void os_cloud_kick(void)
{
    if (s_task) {
        xTaskNotify(s_task, BIT_WORK, eSetBits);
    }
}

void os_cloud_factory_reset(void)
{
    os_socket_abort();                  // the cloud task may be on an app's socket: back to us
    if (s_task) {
        xTaskNotify(s_task, BIT_RESET, eSetBits);
    }
}

bool os_cloud_registered(void)
{
    lock();
    bool ok = s_info.server[0] && s_info.id[0] && s_secret[0];
    unlock();
    return ok;
}

void os_cloud_server(char *out, size_t n)
{
    lock();
    strlcpy(out, s_info.server, n);
    unlock();
}

void os_cloud_set_server(const char *url)
{
    char clean[sizeof(s_info.server)];
    strlcpy(clean, url ? url : "", sizeof(clean));
    size_t n = strlen(clean);
    while (n && clean[n - 1] == '/') {
        clean[--n] = 0;
    }
    bool clear = !clean[0];
    if (clear) {
        strlcpy(clean, CONFIG_SOLANA_OS_SERVER_URL, sizeof(clean));     // back to the build default
    }
    lock();
    bool changed = strcmp(clean, s_info.server) != 0;
    if (changed) {
        // The ID, secret, mesh key and owner belong to the old server. The
        // owner tag stays: a different account there still means new owner.
        strlcpy(s_info.server, clean, sizeof(s_info.server));
        s_info.id[0] = s_info.claim_url[0] = s_info.owner[0] = s_info.user[0] = 0;
        s_secret[0] = 0;
        s_info.has_key = s_info.claimed = false;
        s_info.lamps = s_info.level = s_info.packs = 0;
        s_info.state = OS_CLOUD_OFFLINE;
        s_info.error = NULL;
        s_next_ms = -1;
    }
    unlock();
    if (changed || clear) {
        nvs_save("server", clear ? "" : clean);
    }
    if (changed) {
        nvs_save("id", "");
        nvs_save("secret", "");
        nvs_save("key", "");
        nvs_save("owner", "");
        nvs_save_int("claimed", 0);
        ESP_LOGI(TAG, "server set to '%s'", clean);
    }
    os_cloud_refresh();
}

bool os_cloud_bearer(char *out, size_t n)
{
    lock();
    bool ok = s_secret[0] != 0;
    strlcpy(out, s_secret, n);
    unlock();
    return ok;
}

bool os_cloud_mesh_key(uint8_t out[16])
{
    lock();
    bool ok = s_info.has_key;
    if (ok) {
        memcpy(out, s_key, sizeof(s_key));
    }
    unlock();
    return ok;
}

int64_t os_cloud_time_ms(void)
{
    lock();
    int64_t off = s_time_off;
    unlock();
    return off ? now_ms() + off : 0;
}

void os_cloud_note_lamps(int lamps, int level)
{
    lock();
    bool changed = s_info.claimed && (s_info.lamps != lamps || s_info.level != level);
    s_info.lamps = lamps;
    s_info.level = level;
    unlock();
    if (changed) {
        nvs_save_int("lamps", lamps);
        nvs_save_int("level", level);
    }
}

void os_cloud_note_packs(int packs)
{
    lock();
    bool changed = s_info.claimed && s_info.packs != packs;
    s_info.packs = packs;
    unlock();
    if (changed) {
        nvs_save_int("packs", packs);
    }
}
