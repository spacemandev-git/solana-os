// Emulated ILI9341 + esp_lvgl_port: LVGL renders 320x240 RGB565 in the same
// 2 x 320x24 partial buffers as the badge; the flush converts each area into
// an RGBA framebuffer that the page blits to a <canvas>. The badge's panel
// rotation (swap_xy + mirror) only undoes how the glass is mounted, so LVGL
// sees the same landscape screen here without any transform.

#include <string.h>
#include <emscripten.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include "bsp.h"
#include "emu.h"

static const char *TAG = "bsp_display";

#define BUF_PX  (BOARD_LCD_H_RES * 24)

static lv_display_t *s_disp;
static uint32_t s_fb[BOARD_LCD_H_RES * BOARD_LCD_V_RES];   // RGBA, as ImageData wants it
static uint8_t s_buf[2][BUF_PX * 2] __attribute__((aligned(4)));
static volatile uint32_t s_frame;       // bumped on every flush; the page redraws when it changes
static bool s_backlight;
static int s_next_ms = 5;

// esp_lvgl_port's lock is a recursive mutex; lvgl_port_lock(0) waits forever.
static TaskHandle_t s_owner;
static int s_depth;

static bool lock_free(void *arg)
{
    (void)arg;
    return s_depth == 0;
}

bool lvgl_port_lock(uint32_t timeout_ms)
{
    TaskHandle_t me = xTaskGetCurrentTaskHandle();
    if (s_depth && s_owner == me) {
        s_depth++;
        return true;
    }
    int64_t deadline = timeout_ms ? emu_ticks_to_deadline(pdMS_TO_TICKS(timeout_ms)) : INT64_MAX;
    if (!emu_block_until(lock_free, NULL, deadline)) {
        return false;
    }
    s_owner = me;
    s_depth = 1;
    return true;
}

void lvgl_port_unlock(void)
{
    if (s_depth > 0 && --s_depth == 0) {
        s_owner = NULL;
    }
}

bool emu_lvgl_locked(void)
{
    return s_depth > 0;
}

static void flush(lv_display_t *disp, const lv_area_t *area, uint8_t *px)
{
    const uint16_t *src = (const uint16_t *)px;
    int w = lv_area_get_width(area);
    for (int y = area->y1; y <= area->y2; y++) {
        uint32_t *dst = &s_fb[y * BOARD_LCD_H_RES + area->x1];
        for (int x = 0; x < w; x++) {
            uint16_t c = *src++;
            uint32_t r = (c >> 11) & 0x1F, g = (c >> 5) & 0x3F, b = c & 0x1F;
            // Expand 5/6-bit channels the way tools/badge.py decodes screenshots.
            r = (r << 3) | (r >> 2);
            g = (g << 2) | (g >> 4);
            b = (b << 3) | (b >> 2);
            dst[x] = 0xFF000000u | (b << 16) | (g << 8) | r;
        }
    }
    if (lv_display_flush_is_last(disp)) {
        s_frame++;
    }
    lv_display_flush_ready(disp);
}

static uint32_t tick_ms(void)
{
    return (uint32_t)(emu_now_us() / 1000);
}

void bsp_backlight(bool on)
{
    s_backlight = on;
}

lv_display_t *bsp_display_start(void)
{
    bsp_backlight(false);
    lv_init();
    lv_tick_set_cb(tick_ms);
    s_disp = lv_display_create(BOARD_LCD_H_RES, BOARD_LCD_V_RES);
    lv_display_set_color_format(s_disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(s_disp, s_buf[0], s_buf[1], sizeof(s_buf[0]), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(s_disp, flush);
    bsp_backlight(true);
    ESP_LOGI(TAG, "LCD %dx%d ready (emulated)", BOARD_LCD_H_RES, BOARD_LCD_V_RES);
    return s_disp;
}

void emu_display_pump(void)
{
    if (!s_disp || s_depth) {
        return;         // a task holds the LVGL lock: wait, like the port's LVGL task
    }
    uint32_t next = lv_timer_handler();
    s_next_ms = next == LV_NO_TIMER_READY ? 33 : (int)next;
}

int emu_display_next_ms(void)
{
    return s_disp ? s_next_ms : 33;
}

// ---- Page API ----

EMSCRIPTEN_KEEPALIVE uint32_t *emu_framebuffer(void)
{
    return s_fb;
}

EMSCRIPTEN_KEEPALIVE uint32_t emu_frame(void)
{
    return s_frame;
}

EMSCRIPTEN_KEEPALIVE int emu_backlight(void)
{
    return s_backlight;
}
