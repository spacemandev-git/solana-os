// Emulated WS2812B strip. Like the real strip, a colour only reaches the LED
// on bsp_rgb_show(); the page reads the shown colours to light the drawing.

#include <emscripten.h>
#include "bsp.h"

static uint32_t s_color[BOARD_RGB_COUNT];   // set, not yet shown
static uint32_t s_shown[BOARD_RGB_COUNT];   // what the LEDs display
static bool s_ready;
static uint32_t s_version;

esp_err_t bsp_rgb_init(void)
{
    s_ready = true;
    bsp_rgb_fill(0, 0, 0);
    bsp_rgb_show();
    return ESP_OK;
}

void bsp_rgb_set(int index, uint8_t r, uint8_t g, uint8_t b)
{
    if (!s_ready || index < 0 || index >= BOARD_RGB_COUNT) {
        return;
    }
    s_color[index] = (r << 16) | (g << 8) | b;
}

void bsp_rgb_fill(uint8_t r, uint8_t g, uint8_t b)
{
    for (int i = 0; i < BOARD_RGB_COUNT; i++) {
        bsp_rgb_set(i, r, g, b);
    }
}

void bsp_rgb_show(void)
{
    if (!s_ready) {
        return;
    }
    for (int i = 0; i < BOARD_RGB_COUNT; i++) {
        s_shown[i] = s_color[i];
    }
    s_version++;
}

uint32_t bsp_rgb_get(int index)
{
    return (index >= 0 && index < BOARD_RGB_COUNT) ? s_color[index] : 0;
}

// Page API: 8 x 0xRRGGBB as sent to the strip (firmware caps them at OS_LED_MAX).
EMSCRIPTEN_KEEPALIVE uint32_t *emu_leds(void)
{
    return s_shown;
}

EMSCRIPTEN_KEEPALIVE uint32_t emu_leds_version(void)
{
    return s_version;
}
