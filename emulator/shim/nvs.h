#pragma once

// NVS for the emulator: same API, stored in the browser's localStorage so
// Wi-Fi credentials, the button map and the cloud secret survive reloads.

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

typedef uint32_t nvs_handle_t;
typedef nvs_handle_t nvs_handle;
typedef enum { NVS_READONLY, NVS_READWRITE } nvs_open_mode_t;
typedef nvs_open_mode_t nvs_open_mode;

#define ESP_ERR_NVS_BASE                0x1100
#define ESP_ERR_NVS_NOT_INITIALIZED     (ESP_ERR_NVS_BASE + 0x01)
#define ESP_ERR_NVS_NOT_FOUND           (ESP_ERR_NVS_BASE + 0x02)
#define ESP_ERR_NVS_TYPE_MISMATCH       (ESP_ERR_NVS_BASE + 0x03)
#define ESP_ERR_NVS_READ_ONLY           (ESP_ERR_NVS_BASE + 0x04)
#define ESP_ERR_NVS_NOT_ENOUGH_SPACE    (ESP_ERR_NVS_BASE + 0x05)
#define ESP_ERR_NVS_INVALID_NAME        (ESP_ERR_NVS_BASE + 0x06)
#define ESP_ERR_NVS_INVALID_HANDLE      (ESP_ERR_NVS_BASE + 0x07)
#define ESP_ERR_NVS_INVALID_LENGTH      (ESP_ERR_NVS_BASE + 0x0c)
#define ESP_ERR_NVS_NO_FREE_PAGES       (ESP_ERR_NVS_BASE + 0x0d)
#define ESP_ERR_NVS_NEW_VERSION_FOUND   (ESP_ERR_NVS_BASE + 0x10)

esp_err_t nvs_open(const char *ns, nvs_open_mode_t mode, nvs_handle_t *out);
void nvs_close(nvs_handle_t h);
esp_err_t nvs_commit(nvs_handle_t h);
esp_err_t nvs_erase_key(nvs_handle_t h, const char *key);
esp_err_t nvs_erase_all(nvs_handle_t h);

esp_err_t nvs_set_str(nvs_handle_t h, const char *key, const char *value);
esp_err_t nvs_get_str(nvs_handle_t h, const char *key, char *out, size_t *len);
esp_err_t nvs_set_blob(nvs_handle_t h, const char *key, const void *value, size_t len);
esp_err_t nvs_get_blob(nvs_handle_t h, const char *key, void *out, size_t *len);

esp_err_t nvs_set_i8(nvs_handle_t h, const char *key, int8_t v);
esp_err_t nvs_set_u8(nvs_handle_t h, const char *key, uint8_t v);
esp_err_t nvs_set_i16(nvs_handle_t h, const char *key, int16_t v);
esp_err_t nvs_set_u16(nvs_handle_t h, const char *key, uint16_t v);
esp_err_t nvs_set_i32(nvs_handle_t h, const char *key, int32_t v);
esp_err_t nvs_set_u32(nvs_handle_t h, const char *key, uint32_t v);
esp_err_t nvs_set_i64(nvs_handle_t h, const char *key, int64_t v);
esp_err_t nvs_set_u64(nvs_handle_t h, const char *key, uint64_t v);
esp_err_t nvs_get_i8(nvs_handle_t h, const char *key, int8_t *v);
esp_err_t nvs_get_u8(nvs_handle_t h, const char *key, uint8_t *v);
esp_err_t nvs_get_i16(nvs_handle_t h, const char *key, int16_t *v);
esp_err_t nvs_get_u16(nvs_handle_t h, const char *key, uint16_t *v);
esp_err_t nvs_get_i32(nvs_handle_t h, const char *key, int32_t *v);
esp_err_t nvs_get_u32(nvs_handle_t h, const char *key, uint32_t *v);
esp_err_t nvs_get_i64(nvs_handle_t h, const char *key, int64_t *v);
esp_err_t nvs_get_u64(nvs_handle_t h, const char *key, uint64_t *v);
