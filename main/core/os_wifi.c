// System Wi-Fi service: STA on both bands, saved network in NVS, auto
// reconnect, async scans, and suspend/resume for apps that need the radio.

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

static SemaphoreHandle_t s_lock;
static os_wifi_state_t s_state = OS_WIFI_IDLE;
static char s_ssid[33], s_pass[65], s_ip[16];
static bool s_save_on_ip, s_suspended, s_started;
static int s_retries;
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

static void start_connect_locked(void)
{
    wifi_config_t cfg = {0};
    strlcpy((char *)cfg.sta.ssid, s_ssid, sizeof(cfg.sta.ssid));
    strlcpy((char *)cfg.sta.password, s_pass, sizeof(cfg.sta.password));
    cfg.sta.threshold.authmode = s_pass[0] ? WIFI_AUTH_WEP : WIFI_AUTH_OPEN;
    cfg.sta.pmf_cfg.capable = true;
    cfg.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    cfg.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    cfg.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    esp_wifi_set_config(WIFI_IF_STA, &cfg);
    s_state = OS_WIFI_CONNECTING;
    s_ip[0] = 0;
    s_fail = NULL;
    esp_wifi_connect();
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    LOCK();
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        s_started = true;
        if (s_ssid[0] && !s_suspended) {
            start_connect_locked();
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *d = data;
        s_ip[0] = 0;
        if (s_suspended || !s_ssid[0]) {
            // Intentional: radio lent out or network forgotten.
        } else if (s_retries++ < MAX_RETRIES) {
            ESP_LOGW(TAG, "disconnected (reason %d), retry %d", d->reason, s_retries);
            s_state = OS_WIFI_CONNECTING;
            esp_wifi_connect();
        } else {
            s_fail = reason_str(d->reason);
            s_state = OS_WIFI_FAILED;
            s_save_on_ip = false;
            ESP_LOGW(TAG, "giving up on '%s': %s (reason %d)", s_ssid, s_fail, d->reason);
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_SCAN_DONE) {
        s_scan_done = true;
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
        ESP_LOGI(TAG, "saved network '%s'", s_ssid);
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
    UNLOCK();
    wifi_ap_record_t ap;
    if (out->state == OS_WIFI_CONNECTED && esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        out->rssi = ap.rssi;
        out->channel = ap.primary;
    }
}

esp_err_t os_wifi_scan_start(void)
{
    if (s_suspended) {
        return ESP_ERR_INVALID_STATE;
    }
    s_scan_done = false;
    wifi_scan_config_t cfg = { .show_hidden = true };
    esp_err_t err = esp_wifi_scan_start(&cfg, false);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "scan: %s", esp_err_to_name(err));
    }
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
    uint16_t n = max;
    if (esp_wifi_scan_get_ap_records(&n, out) != ESP_OK) {
        return 0;
    }
    qsort(out, n, sizeof(*out), by_rssi);
    return n;
}

esp_err_t os_wifi_connect(const char *ssid, const char *pass)
{
    if (!ssid || !ssid[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    LOCK();
    char saved[33] = "";
    os_wifi_saved_ssid(saved, sizeof(saved));
    strlcpy(s_ssid, ssid, sizeof(s_ssid));
    strlcpy(s_pass, pass ? pass : "", sizeof(s_pass));
    s_save_on_ip = true;
    s_retries = 0;
    s_suspended = false;
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
    UNLOCK();
    esp_wifi_disconnect();
}

void os_wifi_suspend(void)
{
    LOCK();
    s_suspended = true;
    s_state = OS_WIFI_SUSPENDED;
    s_ip[0] = 0;
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
    if (s_ssid[0]) {
        start_connect_locked();
    }
    UNLOCK();
}

// STUB for the HH GtS contract; firmware agent "fw-core" makes Wi-Fi off at boot.
void os_wifi_set_enabled(bool on)
{
    char ssid[33], pass[65];
    if (on && os_wifi_saved_ssid(ssid, sizeof(ssid))) {
        if (!os_wifi_saved_pass(pass, sizeof(pass))) {
            pass[0] = 0;
        }
        os_wifi_connect(ssid, pass);
    }
}

bool os_wifi_enabled(void)
{
    return os_wifi_state() == OS_WIFI_CONNECTED || os_wifi_state() == OS_WIFI_CONNECTING;
}
