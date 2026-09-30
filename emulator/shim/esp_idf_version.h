#pragma once

// There is no ESP-IDF in the browser; say so wherever firmware prints its version.
#define ESP_IDF_VERSION_MAJOR   5
#define ESP_IDF_VERSION_MINOR   5
#define ESP_IDF_VERSION_PATCH   4
#define ESP_IDF_VERSION_VAL(major, minor, patch) (((major) << 16) | ((minor) << 8) | (patch))
#define ESP_IDF_VERSION ESP_IDF_VERSION_VAL(ESP_IDF_VERSION_MAJOR, ESP_IDF_VERSION_MINOR, ESP_IDF_VERSION_PATCH)

const char *esp_get_idf_version(void);
