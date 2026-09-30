#pragma once

#include <stdint.h>

typedef enum { CHIP_ESP32C5 = 23 } esp_chip_model_t;
#define CHIP_FEATURE_WIFI_BGN   (1u << 1)
#define CHIP_FEATURE_BLE        (1u << 4)
#define CHIP_FEATURE_IEEE802154 (1u << 6)

typedef struct {
    esp_chip_model_t model;
    uint32_t features;
    uint16_t revision;      // major * 100 + minor
    uint8_t cores;
} esp_chip_info_t;

void esp_chip_info(esp_chip_info_t *out);
