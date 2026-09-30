#pragma once

#include <stdint.h>
#include "esp_err.h"

typedef enum { ESP_MAC_WIFI_STA, ESP_MAC_WIFI_SOFTAP, ESP_MAC_BT, ESP_MAC_ETH, ESP_MAC_IEEE802154, ESP_MAC_BASE } esp_mac_type_t;

// The emulator's MAC is random and locally administered (02:xx:..), kept in
// localStorage so the emulated badge keeps its identity across reloads.
esp_err_t esp_read_mac(uint8_t *mac, esp_mac_type_t type);
esp_err_t esp_efuse_mac_get_default(uint8_t *mac);
#define esp_base_mac_addr_get(mac) esp_efuse_mac_get_default(mac)
#define MACSTR "%02x:%02x:%02x:%02x:%02x:%02x"
#define MAC2STR(a) (a)[0], (a)[1], (a)[2], (a)[3], (a)[4], (a)[5]
