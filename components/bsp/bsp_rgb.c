// 8x WS2812B via RMT. Keeps a shadow copy so the UI can mirror the LEDs.

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "led_strip.h"
#include "esp_check.h"
#include "bsp.h"

static const char *TAG = "bsp_rgb";

static led_strip_handle_t s_strip;
static SemaphoreHandle_t s_lock;
static uint32_t s_color[BOARD_RGB_COUNT];

esp_err_t bsp_rgb_init(void)
{
    led_strip_config_t strip_cfg = {
        .strip_gpio_num = BOARD_RGB_GPIO,
        .max_leds = BOARD_RGB_COUNT,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
    };
    led_strip_rmt_config_t rmt_cfg = { .resolution_hz = 10 * 1000 * 1000 };
    ESP_RETURN_ON_ERROR(led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &s_strip), TAG, "led strip");
    s_lock = xSemaphoreCreateMutex();
    bsp_rgb_fill(0, 0, 0);
    bsp_rgb_show();
    return ESP_OK;
}

void bsp_rgb_set(int index, uint8_t r, uint8_t g, uint8_t b)
{
    if (!s_strip || index < 0 || index >= BOARD_RGB_COUNT) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_color[index] = (r << 16) | (g << 8) | b;
    led_strip_set_pixel(s_strip, index, r, g, b);
    xSemaphoreGive(s_lock);
}

void bsp_rgb_fill(uint8_t r, uint8_t g, uint8_t b)
{
    for (int i = 0; i < BOARD_RGB_COUNT; i++) {
        bsp_rgb_set(i, r, g, b);
    }
}

void bsp_rgb_show(void)
{
    if (!s_strip) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    led_strip_refresh(s_strip);
    xSemaphoreGive(s_lock);
}

uint32_t bsp_rgb_get(int index)
{
    return (index >= 0 && index < BOARD_RGB_COUNT) ? s_color[index] : 0;
}
