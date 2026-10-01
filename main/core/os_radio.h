#pragma once

// Raw ESP-NOW radio under os_mesh (docs/MESH.md "Radio"). The badge
// implements it over esp_now (core/os_radio.c); the emulator over a
// BroadcastChannel shared by the browser tabs of one origin
// (emulator/platform/os_radio_emu.c, MESH.md "Emulator transport").
//
// Only os_mesh uses this. Apps talk through os_mesh.h.

#include <stdint.h>
#include "esp_err.h"

#define OS_RADIO_FRAME_MAX  250         // ESP-NOW v1 payload limit

// Called for every frame heard, from the radio's own task (the Wi-Fi task on
// the badge): copy the frame and return quickly.
typedef void (*os_radio_rx_cb_t)(const uint8_t mac[6], const uint8_t *data, int len, int rssi);

// After os_wifi_init(): the Wi-Fi driver must be started.
esp_err_t os_radio_init(os_radio_rx_cb_t cb);

// The channel the radio is tuned to (the AP's while Wi-Fi is joined), 0 if unknown.
int os_radio_channel(void);
// Tune the radio. Only valid while Wi-Fi is neither joined nor joining nor
// scanning (os_wifi parks it on the mesh channel; os_mesh while it holds the radio).
esp_err_t os_radio_set_channel(int channel);

// Send one frame on the current channel. mac NULL = broadcast. Unicast peers
// are registered on demand (the oldest is evicted at the driver's limit).
// ESP_ERR_NO_MEM when the driver's queue is full: retry shortly.
esp_err_t os_radio_send(const uint8_t *mac, const uint8_t *data, int len);

// This radio's MAC (the Wi-Fi STA address).
void os_radio_mac(uint8_t mac[6]);
