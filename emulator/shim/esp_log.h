#pragma once

// ESP_LOGx print to the browser console (stdout -> console.log), in the same
// "I (1234) tag: message" shape as the badge's UART log.

#include <stdint.h>
#include <stdio.h>
#include "esp_err.h"

typedef enum {
    ESP_LOG_NONE, ESP_LOG_ERROR, ESP_LOG_WARN, ESP_LOG_INFO, ESP_LOG_DEBUG, ESP_LOG_VERBOSE,
} esp_log_level_t;

void esp_log_level_set(const char *tag, esp_log_level_t level);
void esp_log_write(esp_log_level_t level, const char *tag, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
uint32_t esp_log_timestamp(void);

#define ESP_LOGE(tag, fmt, ...) esp_log_write(ESP_LOG_ERROR, tag, fmt, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) esp_log_write(ESP_LOG_WARN, tag, fmt, ##__VA_ARGS__)
#define ESP_LOGI(tag, fmt, ...) esp_log_write(ESP_LOG_INFO, tag, fmt, ##__VA_ARGS__)
#define ESP_LOGD(tag, fmt, ...) esp_log_write(ESP_LOG_DEBUG, tag, fmt, ##__VA_ARGS__)
#define ESP_LOGV(tag, fmt, ...) esp_log_write(ESP_LOG_VERBOSE, tag, fmt, ##__VA_ARGS__)
#define ESP_EARLY_LOGE ESP_LOGE
#define ESP_EARLY_LOGW ESP_LOGW
#define ESP_EARLY_LOGI ESP_LOGI
#define ESP_LOG_BUFFER_HEX(tag, buf, len) ((void)(tag), (void)(buf), (void)(len))
