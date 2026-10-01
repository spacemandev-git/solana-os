// Emulator counterpart of core/os_wifi.c: the same os_wifi.h service over a
// simulated radio. Scans return a fixed set of obviously fake demo networks,
// joining any of them succeeds and hands out a fake private IP. Credentials
// use the badge's NVS keys ("wifi/ssid|pass"), saved only once "connected".
// Like the badge, Wi-Fi is off at boot, and the emulated radio
// (esp_wifi_emu.c, which os_radio_emu.c sends on) sits on the access point's
// channel while joined and on the park channel otherwise.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_wifi.h"
#include "nvs.h"
#include "core/os_wifi.h"
#include "emu.h"

static const char *TAG = "os_wifi";

#define NVS_NS          "wifi"
#define SCAN_MS         1800        // dual-band scan
#define CONNECT_MS      1400        // auth + DHCP
#define FAIL_MS         4000

typedef struct {
    const char *ssid;
    uint8_t channel;
    int8_t rssi;
    wifi_auth_mode_t auth;
} demo_ap_t;

// Clearly fake networks. Several BSSIDs share an SSID so the app's
// SSID + band de-duplication has something to do.
static const demo_ap_t s_aps[] = {
    {"Solana Demo", 6, -46, WIFI_AUTH_WPA2_WPA3_PSK},
    {"Solana Demo", 11, -69, WIFI_AUTH_WPA2_WPA3_PSK},
    {"Solana Demo", 36, -55, WIFI_AUTH_WPA2_WPA3_PSK},
    {"Emulator Guest", 1, -61, WIFI_AUTH_OPEN},
    {"Breakpoint Demo 5G", 149, -64, WIFI_AUTH_WPA3_PSK},
    {"Demo Cafe", 11, -72, WIFI_AUTH_WPA2_PSK},
    {"", 44, -78, WIFI_AUTH_WPA2_PSK},
    {"Not A Real Network", 3, -83, WIFI_AUTH_WPA_WPA2_PSK},
    {"demo-iot-sensor", 9, -89, WIFI_AUTH_WPA2_PSK},
};
#define N_APS (int)(sizeof(s_aps) / sizeof(s_aps[0]))

static os_wifi_state_t s_state = OS_WIFI_IDLE;
static char s_ssid[33], s_pass[65], s_ip[16];
static bool s_save_on_ip, s_suspended, s_enabled;
static int s_park;
static const char *s_fail;
static int64_t s_scan_done_at, s_connect_at;
static bool s_scanning, s_scan_done;
static const demo_ap_t *s_ap;

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

static const demo_ap_t *strongest(const char *ssid)
{
    const demo_ap_t *best = NULL;
    for (int i = 0; ssid[0] && i < N_APS; i++) {
        if (!strcmp(s_aps[i].ssid, ssid) && (!best || s_aps[i].rssi > best->rssi)) {
            best = &s_aps[i];
        }
    }
    return best;
}

// Not joined, not joining, not lent out: wait on the park channel.
static void park(void)
{
    if (s_park && !s_suspended && s_state != OS_WIFI_CONNECTING && s_state != OS_WIFI_CONNECTED) {
        esp_wifi_set_channel(s_park, WIFI_SECOND_CHAN_NONE);
    }
}

static void start_connect(void)
{
    s_state = OS_WIFI_CONNECTING;
    s_ip[0] = 0;
    s_fail = NULL;
    s_ap = NULL;
    s_connect_at = emu_now_us() + (strongest(s_ssid) ? CONNECT_MS : FAIL_MS) * 1000LL;
}

// The "driver": finishes scans and connection attempts when they are due.
static void wifi_task(void *arg)
{
    for (;;) {
        int64_t now = emu_now_us();
        if (s_scanning && now >= s_scan_done_at) {
            s_scanning = false;
            s_scan_done = true;
        }
        if (s_state == OS_WIFI_CONNECTING && now >= s_connect_at) {
            s_ap = strongest(s_ssid);
            if (s_ap) {
                const uint8_t *mac = emu_mac();
                snprintf(s_ip, sizeof(s_ip), "192.168.77.%d", 20 + mac[5] % 200);
                s_state = OS_WIFI_CONNECTED;
                esp_wifi_set_channel(s_ap->channel, WIFI_SECOND_CHAN_NONE);
                ESP_LOGI(TAG, "connected to '%s', ip %s (simulated)", s_ssid, s_ip);
                if (s_save_on_ip) {
                    save_creds(s_ssid, s_pass);
                    s_save_on_ip = false;
                }
            } else {
                s_fail = "network not found";
                s_state = OS_WIFI_FAILED;
                s_save_on_ip = false;
                ESP_LOGW(TAG, "giving up on '%s': %s", s_ssid, s_fail);
                park();
            }
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

esp_err_t os_wifi_init(void)
{
    if (os_wifi_saved_ssid(s_ssid, sizeof(s_ssid))) {
        os_wifi_saved_pass(s_pass, sizeof(s_pass));
        ESP_LOGI(TAG, "saved network '%s' (Wi-Fi stays off until turned on)", s_ssid);
    }
    xTaskCreate(wifi_task, "wifi", 3072, NULL, 23, NULL);
    return ESP_OK;
}

os_wifi_state_t os_wifi_state(void)
{
    return s_state;
}

void os_wifi_get_info(os_wifi_info_t *out)
{
    memset(out, 0, sizeof(*out));
    out->state = s_state;
    strlcpy(out->ssid, s_ssid, sizeof(out->ssid));
    strlcpy(out->ip, s_ip, sizeof(out->ip));
    out->fail_reason = s_fail;
    out->enabled = s_enabled;
    if (s_state == OS_WIFI_CONNECTED && s_ap) {
        out->rssi = s_ap->rssi + (int)(esp_random() % 5) - 2;
        out->channel = s_ap->channel;
    }
}

esp_err_t os_wifi_scan_start(void)
{
    if (s_suspended) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_state == OS_WIFI_CONNECTING) {
        // The badge's driver refuses scans while it is connecting, too.
        ESP_LOGW(TAG, "scan: busy connecting");
        return ESP_ERR_INVALID_STATE;
    }
    s_scan_done = false;
    s_scanning = true;
    s_scan_done_at = emu_now_us() + SCAN_MS * 1000LL;
    return ESP_OK;
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
    int n = 0;
    for (int i = 0; i < N_APS && n < max; i++, n++) {
        wifi_ap_record_t *r = &out[n];
        memset(r, 0, sizeof(*r));
        uint8_t bssid[6] = {0x02, 0xde, 0x40, 0x00, (uint8_t)(s_aps[i].channel), (uint8_t)i};
        memcpy(r->bssid, bssid, 6);
        strlcpy((char *)r->ssid, s_aps[i].ssid, sizeof(r->ssid));
        r->primary = s_aps[i].channel;
        r->rssi = s_aps[i].rssi + (int)(esp_random() % 7) - 3;
        r->authmode = s_aps[i].auth;
    }
    qsort(out, n, sizeof(*out), by_rssi);
    return n;
}

esp_err_t os_wifi_connect(const char *ssid, const char *pass)
{
    if (!ssid || !ssid[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    char saved[33] = "", saved_pass[65] = "";
    os_wifi_saved_ssid(saved, sizeof(saved));
    os_wifi_saved_pass(saved_pass, sizeof(saved_pass));
    strlcpy(s_ssid, ssid, sizeof(s_ssid));
    strlcpy(s_pass, pass ? pass : "", sizeof(s_pass));
    s_save_on_ip = strcmp(saved, s_ssid) || strcmp(saved_pass, s_pass);
    s_suspended = false;
    s_enabled = true;
    start_connect();
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
    s_ssid[0] = s_pass[0] = s_ip[0] = 0;
    s_state = OS_WIFI_IDLE;
    s_save_on_ip = false;
    s_enabled = false;
    s_ap = NULL;
    park();
}

void os_wifi_suspend(void)
{
    s_suspended = true;
    s_state = OS_WIFI_SUSPENDED;
    s_ip[0] = 0;
    s_scanning = false;
}

void os_wifi_resume(void)
{
    s_suspended = false;
    s_state = OS_WIFI_IDLE;
    if (s_enabled && s_ssid[0]) {
        start_connect();
    } else {
        park();
    }
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
    if (s_enabled) {
        ESP_LOGI(TAG, "wifi off");
    }
    s_enabled = false;
    s_save_on_ip = false;
    if (!s_suspended) {
        s_state = OS_WIFI_IDLE;
        s_ip[0] = 0;
        s_fail = NULL;
        s_ap = NULL;
    }
    park();
}

bool os_wifi_enabled(void)
{
    return s_enabled;
}

void os_wifi_set_park_channel(int channel)
{
    s_park = channel;
    park();
}
