#pragma once

// System Wi-Fi service. Owns the driver (STA, dual-band), reconnects to the
// saved network, and lets radio-hungry apps (ESP-NOW sniffer) borrow the
// radio with suspend/resume.

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "esp_wifi_types.h"

typedef enum {
    OS_WIFI_IDLE,           // no saved network / disconnected
    OS_WIFI_CONNECTING,
    OS_WIFI_CONNECTED,
    OS_WIFI_FAILED,         // gave up; see os_wifi_fail_reason()
    OS_WIFI_SUSPENDED,      // radio lent to an app
} os_wifi_state_t;

typedef struct {
    os_wifi_state_t state;
    char ssid[33];
    char ip[16];
    int rssi;
    int channel;
    const char *fail_reason;
} os_wifi_info_t;

esp_err_t os_wifi_init(void);
os_wifi_state_t os_wifi_state(void);
void os_wifi_get_info(os_wifi_info_t *out);
bool os_wifi_saved_ssid(char *out, size_t n);
bool os_wifi_saved_pass(char *out, size_t n);

// Async scan (both bands). Poll os_wifi_scan_done(), then fetch results
// sorted by RSSI.
esp_err_t os_wifi_scan_start(void);
bool os_wifi_scan_done(void);
int os_wifi_scan_results(wifi_ap_record_t *out, int max);

// Credentials are saved to NVS once the connection gets an IP.
esp_err_t os_wifi_connect(const char *ssid, const char *pass);
void os_wifi_forget(void);

void os_wifi_suspend(void);
void os_wifi_resume(void);
