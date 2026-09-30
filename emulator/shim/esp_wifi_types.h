#pragma once

// The subset of ESP-IDF's Wi-Fi types that portable firmware uses: scan
// records (os_wifi.h) and promiscuous packets (ESP-NOW scanner).

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    WIFI_AUTH_OPEN = 0,
    WIFI_AUTH_WEP,
    WIFI_AUTH_WPA_PSK,
    WIFI_AUTH_WPA2_PSK,
    WIFI_AUTH_WPA_WPA2_PSK,
    WIFI_AUTH_ENTERPRISE,
    WIFI_AUTH_WPA2_ENTERPRISE = WIFI_AUTH_ENTERPRISE,
    WIFI_AUTH_WPA3_PSK,
    WIFI_AUTH_WPA2_WPA3_PSK,
    WIFI_AUTH_WAPI_PSK,
    WIFI_AUTH_OWE,
    WIFI_AUTH_WPA3_ENT_192,
    WIFI_AUTH_WPA3_EXT_PSK,
    WIFI_AUTH_WPA3_EXT_PSK_MIXED_MODE,
    WIFI_AUTH_DPP,
    WIFI_AUTH_WPA3_ENTERPRISE,
    WIFI_AUTH_WPA2_WPA3_ENTERPRISE,
    WIFI_AUTH_MAX
} wifi_auth_mode_t;

typedef enum { WIFI_SECOND_CHAN_NONE = 0, WIFI_SECOND_CHAN_ABOVE, WIFI_SECOND_CHAN_BELOW } wifi_second_chan_t;
typedef enum { WIFI_CIPHER_TYPE_NONE = 0, WIFI_CIPHER_TYPE_CCMP = 4 } wifi_cipher_type_t;

typedef struct {
    uint8_t bssid[6];
    uint8_t ssid[33];
    uint8_t primary;
    wifi_second_chan_t second;
    int8_t rssi;
    wifi_auth_mode_t authmode;
    wifi_cipher_type_t pairwise_cipher;
    wifi_cipher_type_t group_cipher;
} wifi_ap_record_t;

typedef enum { WIFI_PKT_MGMT, WIFI_PKT_CTRL, WIFI_PKT_DATA, WIFI_PKT_MISC } wifi_promiscuous_pkt_type_t;

typedef struct {
    signed rssi : 8;
    unsigned rate : 5;
    unsigned : 1;
    unsigned sig_mode : 2;
    unsigned : 16;
    unsigned channel : 8;
    unsigned secondary_channel : 4;
    unsigned sig_len : 12;
    unsigned rx_state : 8;
    unsigned timestamp;
    signed noise_floor : 8;
} wifi_pkt_rx_ctrl_t;

typedef struct {
    wifi_pkt_rx_ctrl_t rx_ctrl;
    uint8_t payload[0];
} wifi_promiscuous_pkt_t;

#define WIFI_PROMIS_FILTER_MASK_ALL     0xFFFFFFFF
#define WIFI_PROMIS_FILTER_MASK_MGMT    (1 << 0)
#define WIFI_PROMIS_FILTER_MASK_CTRL    (1 << 1)
#define WIFI_PROMIS_FILTER_MASK_DATA    (1 << 2)

typedef struct {
    uint32_t filter_mask;
} wifi_promiscuous_filter_t;

typedef void (*wifi_promiscuous_cb_t)(void *buf, wifi_promiscuous_pkt_type_t type);
