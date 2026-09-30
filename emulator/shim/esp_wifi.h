#pragma once

// Raw-radio calls used by portable apps (the ESP-NOW scanner). The emulated
// radio (platform/esp_wifi_emu.c) delivers simulated frames for the channel
// the app tunes to. Station mode lives behind core/os_wifi.h.

#include "esp_err.h"
#include "esp_wifi_types.h"

esp_err_t esp_wifi_set_promiscuous(bool en);
esp_err_t esp_wifi_set_promiscuous_rx_cb(wifi_promiscuous_cb_t cb);
esp_err_t esp_wifi_set_promiscuous_filter(const wifi_promiscuous_filter_t *filter);
esp_err_t esp_wifi_set_channel(uint8_t primary, wifi_second_chan_t second);
esp_err_t esp_wifi_get_channel(uint8_t *primary, wifi_second_chan_t *second);
