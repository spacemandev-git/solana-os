// System Wi-Fi service: STA on both bands, saved network in NVS, joined only
// while the user has Wi-Fi on (off at every boot), auto reconnect, async
// scans, suspend/resume for apps that need the radio, and the park channel
// the radio returns to whenever no network is joined (ESP-NOW listens there).
//
// Venue networks are one SSID on many APs and both bands. A join first scans
// the mesh channel (or the channel of the row the user picked) for the SSID
// and pins that AP: on the mesh channel the badge is online and on the mesh
// at once, with no suspend. Without one (or only a very weak one on the mesh
// channel) it joins the strongest AP anywhere.

#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_check.h"
#include "esp_log.h"
#include "nvs.h"
#include "os_wifi.h"

static const char *TAG = "os_wifi";

#define NVS_NS          "wifi"
#define MAX_RETRIES     4
#define SCAN_MAX        32
#define PIN_MIN_RSSI    -80     // a weaker AP on the mesh channel joins slowly and drops: take the strongest

static SemaphoreHandle_t s_lock;
static os_wifi_state_t s_state = OS_WIFI_IDLE;
static char s_ssid[33], s_pass[65], s_ip[16];
static bool s_save_on_ip, s_suspended, s_started, s_enabled, s_scanning;
static bool s_join_scan;        // the one-channel scan before a join is running
static bool s_join_pending;     // a join waits for the app's scan to finish
static bool s_pinned;           // the STA config names one AP (BSSID + channel)
static int s_retries, s_park, s_want_ch;
static wifi_ap_record_t *s_scan_cache;  // the app's scan, read at SCAN_DONE
static uint16_t s_scan_n;
static const char *s_fail;
static volatile bool s_scan_done;

#define LOCK()   xSemaphoreTake(s_lock, portMAX_DELAY)
#define UNLOCK() xSemaphoreGive(s_lock)

static bool nvs_get(const char *key, char *out, size_t n)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    size_t len = n;
    esp_err_t err = nvs_get_str(h, key, out, &len);
    nvs_close(h);
    return err == ESP_OK;
}

bool os_wifi_saved_ssid(char *out, size_t n)
{
    return nvs_get("ssid", out, n) && out[0];
}

bool os_wifi_saved_pass(char *out, size_t n)
{
    return nvs_get("pass", out, n);
}

static void save_creds(const char *ssid, const char *pass)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, "ssid", ssid);
        nvs_set_str(h, "pass", pass);
        nvs_commit(h);
        nvs_close(h);
        ESP_LOGI(TAG, "saved network '%s'", ssid);
    }
}

static const char *reason_str(int reason)
{
    switch (reason) {
    case WIFI_REASON_NO_AP_FOUND:
    case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
    case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
    case WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD:
        return "network not found";
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_MIC_FAILURE:
        return "wrong password?";
    case WIFI_REASON_ASSOC_FAIL:
    case WIFI_REASON_CONNECTION_FAIL:
        return "access point refused";
    default:
        return "connection lost";
    }
}

// Connect to one AP (pinned BSSID and channel) or, with ap NULL, to the
// strongest AP of the SSID on any channel.
static void join_locked(const wifi_ap_record_t *ap)
{
    wifi_config_t cfg = {0};
    strlcpy((char *)cfg.sta.ssid, s_ssid, sizeof(cfg.sta.ssid));
    strlcpy((char *)cfg.sta.password, s_pass, sizeof(cfg.sta.password));
    cfg.sta.threshold.authmode = s_pass[0] ? WIFI_AUTH_WEP : WIFI_AUTH_OPEN;
    cfg.sta.pmf_cfg.capable = true;
    cfg.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    if (ap) {
        cfg.sta.bssid_set = true;
        memcpy(cfg.sta.bssid, ap->bssid, sizeof(cfg.sta.bssid));
        cfg.sta.channel = ap->primary;
        cfg.sta.scan_method = WIFI_FAST_SCAN;
        ESP_LOGI(TAG, "joining '%s' on ch %d (%d dBm)", s_ssid, ap->primary, ap->rssi);
    } else {
        cfg.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
        cfg.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    }
    s_pinned = ap != NULL;
    esp_wifi_set_config(WIFI_IF_STA, &cfg);
    esp_wifi_connect();
}

// Look for the SSID on the wanted channel first (the row the user picked,
// else the mesh channel); SCAN_DONE then joins.
static void start_connect_locked(void)
{
    s_state = OS_WIFI_CONNECTING;
    s_ip[0] = 0;
    s_fail = NULL;
    int ch = s_want_ch ? s_want_ch : s_park;
    if (!ch) {
        join_locked(NULL);
        return;
    }
    if (s_scanning) {
        s_join_pending = true;      // the app's scan finishes first
        return;
    }
    wifi_scan_config_t sc = {
        .ssid = (uint8_t *)s_ssid,
        .channel = ch,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active = { .min = 40, .max = 120 },
    };
    if (esp_wifi_scan_start(&sc, false) == ESP_OK) {
        s_join_scan = true;
    } else {
        join_locked(NULL);
    }
}

// SCAN_DONE of a join scan: the strongest AP of the SSID on that channel.
static void join_scan_done_locked(void)
{
    static wifi_ap_record_t recs[4];        // the event task's stack is small
    uint16_t n = sizeof(recs) / sizeof(recs[0]);
    s_join_scan = false;
    if (esp_wifi_scan_get_ap_records(&n, recs) != ESP_OK) {
        n = 0;
    }
    const wifi_ap_record_t *best = NULL;
    for (int i = 0; i < n; i++) {
        if (!strcmp((const char *)recs[i].ssid, s_ssid) && (!best || recs[i].rssi > best->rssi)) {
            best = &recs[i];
        }
    }
    if (best && best->rssi < PIN_MIN_RSSI && !s_want_ch) {
        ESP_LOGI(TAG, "'%s' on ch %d is weak (%d dBm): joining the strongest AP", s_ssid, best->primary, best->rssi);
        best = NULL;
    }
    if (s_enabled && !s_suspended && s_state == OS_WIFI_CONNECTING) {
        join_locked(best);
    }
}

// Not joined, not joining, not scanning and not lent out: tune the radio to
// the park channel. The driver refuses channel changes in the other states.
static void park_locked(void)
{
    if (!s_started || !s_park || s_suspended || s_scanning || s_join_scan || s_state == OS_WIFI_CONNECTING ||
            s_state == OS_WIFI_CONNECTED) {
        return;
    }
    uint8_t ch = 0;
    wifi_second_chan_t second;
    if (esp_wifi_get_channel(&ch, &second) == ESP_OK && ch == s_park) {
        return;
    }
    esp_err_t err = esp_wifi_set_channel(s_park, WIFI_SECOND_CHAN_NONE);
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "park on ch %d: %s", s_park, esp_err_to_name(err));
    }
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    LOCK();
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        s_started = true;
        park_locked();      // Wi-Fi is off at boot: nothing to join yet
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *d = data;
        s_ip[0] = 0;
        if (s_suspended || !s_enabled || !s_ssid[0]) {
            // Intentional: radio lent out, Wi-Fi turned off or network forgotten.
            if (!s_suspended) {
                s_state = OS_WIFI_IDLE;
                park_locked();
            }
        } else if (s_join_scan || s_join_pending) {
            // Dropping the old network before a new join: the scan connects.
        } else if (s_retries++ < MAX_RETRIES) {
            ESP_LOGW(TAG, "disconnected (reason %d), retry %d", d->reason, s_retries);
            s_state = OS_WIFI_CONNECTING;
            if (s_pinned && s_retries > 1) {
                join_locked(NULL);          // the pinned AP keeps failing: any AP will do
            } else {
                esp_wifi_connect();
            }
        } else {
            s_fail = reason_str(d->reason);
            s_state = OS_WIFI_FAILED;
            s_save_on_ip = false;
            ESP_LOGW(TAG, "giving up on '%s': %s (reason %d)", s_ssid, s_fail, d->reason);
            park_locked();
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_SCAN_DONE) {
        if (s_join_scan) {
            join_scan_done_locked();
        } else if (s_scanning) {
            // Read the app's results now: a join scan may follow and would
            // replace them before the app asks.
            free(s_scan_cache);
            s_scan_n = SCAN_MAX;
            s_scan_cache = malloc(SCAN_MAX * sizeof(wifi_ap_record_t));
            if (!s_scan_cache || esp_wifi_scan_get_ap_records(&s_scan_n, s_scan_cache) != ESP_OK) {
                s_scan_n = 0;
            }
            s_scan_done = true;
            s_scanning = false;
            if (s_join_pending) {
                s_join_pending = false;
                if (s_enabled && !s_suspended) {
                    start_connect_locked();
                }
            }
        }
        park_locked();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        esp_ip4addr_ntoa(&e->ip_info.ip, s_ip, sizeof(s_ip));
        s_state = OS_WIFI_CONNECTED;
        s_retries = 0;
        ESP_LOGI(TAG, "connected to '%s', ip %s", s_ssid, s_ip);
        if (s_save_on_ip) {
            save_creds(s_ssid, s_pass);
            s_save_on_ip = false;
        }
    }
    UNLOCK();
}

esp_err_t os_wifi_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "event loop");
    esp_netif_t *sta = esp_netif_create_default_wifi_sta();
    esp_netif_set_hostname(sta, "solana-badge");

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "wifi init");
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL);
    esp_wifi_set_storage(WIFI_STORAGE_RAM);     // credentials live in our own NVS namespace
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "mode");

    if (os_wifi_saved_ssid(s_ssid, sizeof(s_ssid))) {
        os_wifi_saved_pass(s_pass, sizeof(s_pass));
        ESP_LOGI(TAG, "saved network '%s' (Wi-Fi stays off until turned on)", s_ssid);
    }
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "start");
    if (esp_wifi_set_band_mode(WIFI_BAND_MODE_AUTO) != ESP_OK) {
        ESP_LOGW(TAG, "dual-band mode unavailable");
    }
    return ESP_OK;
}

os_wifi_state_t os_wifi_state(void)
{
    return s_state;
}

void os_wifi_get_info(os_wifi_info_t *out)
{
    memset(out, 0, sizeof(*out));
    LOCK();
    out->state = s_state;
    strlcpy(out->ssid, s_ssid, sizeof(out->ssid));
    strlcpy(out->ip, s_ip, sizeof(out->ip));
    out->fail_reason = s_fail;
    out->enabled = s_enabled;
    UNLOCK();
    wifi_ap_record_t ap;
    if (out->state == OS_WIFI_CONNECTED && esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        out->rssi = ap.rssi;
        out->channel = ap.primary;
    }
}

esp_err_t os_wifi_scan_start(void)
{
    LOCK();
    if (s_suspended || s_join_scan || s_scanning) {
        UNLOCK();
        return ESP_ERR_INVALID_STATE;
    }
    s_scan_done = false;
    s_scanning = true;
    wifi_scan_config_t cfg = { .show_hidden = true };
    esp_err_t err = esp_wifi_scan_start(&cfg, false);
    if (err != ESP_OK) {
        s_scanning = false;
        ESP_LOGW(TAG, "scan: %s", esp_err_to_name(err));
    }
    UNLOCK();
    return err;
}

bool os_wifi_scan_done(void)
{
    return s_scan_done;
}

static int by_rssi(const void *a, const void *b)
{
    return ((const wifi_ap_record_t *)b)->rssi - ((const wifi_ap_record_t *)a)->rssi;
}

int os_wifi_scan_results(wifi_ap_record_t *out, int max)
{
    LOCK();
    int n = s_scan_n < max ? s_scan_n : max;
    if (s_scan_cache) {
        memcpy(out, s_scan_cache, n * sizeof(*out));
    } else {
        n = 0;
    }
    free(s_scan_cache);
    s_scan_cache = NULL;
    s_scan_n = 0;
    UNLOCK();
    qsort(out, n, sizeof(*out), by_rssi);
    return n;
}

esp_err_t os_wifi_connect(const char *ssid, const char *pass)
{
    return os_wifi_connect_ch(ssid, pass, 0);
}

esp_err_t os_wifi_connect_ch(const char *ssid, const char *pass, int channel)
{
    if (!ssid || !ssid[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    LOCK();
    s_want_ch = channel;
    char saved[33] = "";
    os_wifi_saved_ssid(saved, sizeof(saved));
    strlcpy(s_ssid, ssid, sizeof(s_ssid));
    strlcpy(s_pass, pass ? pass : "", sizeof(s_pass));
    // Rejoining the saved network with its stored password needs no save.
    char saved_pass[65] = "";
    s_save_on_ip = strcmp(saved, s_ssid) || !os_wifi_saved_pass(saved_pass, sizeof(saved_pass)) ||
                   strcmp(saved_pass, s_pass);
    s_retries = 0;
    s_suspended = false;
    s_enabled = true;
    esp_wifi_disconnect();
    if (s_started) {
        start_connect_locked();
    }
    UNLOCK();
    return ESP_OK;
}

void os_wifi_forget(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
    }
    LOCK();
    s_ssid[0] = s_pass[0] = s_ip[0] = 0;
    s_state = OS_WIFI_IDLE;
    s_save_on_ip = false;
    s_enabled = false;
    UNLOCK();
    esp_wifi_disconnect();
    LOCK();
    park_locked();
    UNLOCK();
}

void os_wifi_suspend(void)
{
    LOCK();
    s_suspended = true;
    s_state = OS_WIFI_SUSPENDED;
    s_ip[0] = 0;
    s_scanning = s_join_pending = false;
    UNLOCK();
    esp_wifi_scan_stop();
    esp_wifi_disconnect();
}

void os_wifi_resume(void)
{
    LOCK();
    s_suspended = false;
    s_state = OS_WIFI_IDLE;
    s_retries = 0;
    if (s_enabled && s_ssid[0]) {
        start_connect_locked();
    } else {
        park_locked();
    }
    UNLOCK();
}

void os_wifi_set_enabled(bool on)
{
    if (on) {
        char ssid[33], pass[65] = "";
        if (os_wifi_saved_ssid(ssid, sizeof(ssid))) {
            os_wifi_saved_pass(pass, sizeof(pass));
            os_wifi_connect(ssid, pass);
        }
        return;
    }
    LOCK();
    bool was = s_enabled;
    s_enabled = false;
    s_save_on_ip = false;
    s_join_pending = false;
    if (!s_suspended) {
        s_state = OS_WIFI_IDLE;
        s_ip[0] = 0;
        s_fail = NULL;
    }
    UNLOCK();
    if (was) {
        ESP_LOGI(TAG, "wifi off");
        esp_wifi_disconnect();
    }
    LOCK();
    park_locked();          // the disconnect event parks again if the driver was busy
    UNLOCK();
}

bool os_wifi_enabled(void)
{
    return s_enabled;
}

void os_wifi_set_park_channel(int channel)
{
    LOCK();
    s_park = channel;
    park_locked();
    UNLOCK();
}
