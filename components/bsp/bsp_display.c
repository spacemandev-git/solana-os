// ILI9341 on the shared SPI bus, driven by LVGL through esp_lvgl_port.

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_ili9341.h"
#include "esp_lvgl_port.h"
#include "esp_check.h"
#include "esp_log.h"
#include "bsp.h"
#include "bsp_bus.h"

static const char *TAG = "bsp_display";
static esp_lcd_panel_handle_t s_panel;
static SemaphoreHandle_t s_lcd_idle;    // see bsp_bus.h

void bsp_lcd_hold(void)
{
    if (s_lcd_idle) {
        xSemaphoreTake(s_lcd_idle, portMAX_DELAY);
    }
}

void bsp_lcd_release(void)
{
    if (s_lcd_idle) {
        xSemaphoreGive(s_lcd_idle);
    }
}

static void flush_start(lv_event_t *e)
{
    bsp_lcd_hold();
}

// Replaces esp_lvgl_port's own callback, which only calls lv_display_flush_ready()
// (esp_lcd logs a warning that it was overwritten).
static bool flush_done(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *edata, void *ctx)
{
    BaseType_t woken = pdFALSE;
    lv_display_flush_ready(ctx);
    xSemaphoreGiveFromISR(s_lcd_idle, &woken);
    return woken == pdTRUE;
}

void bsp_backlight(bool on)
{
    gpio_set_level(BOARD_LCD_BL, on);
}

lv_display_t *bsp_display_start(void)
{
    gpio_config_t bl = { .pin_bit_mask = BIT64(BOARD_LCD_BL), .mode = GPIO_MODE_OUTPUT };
    ESP_ERROR_CHECK(gpio_config(&bl));
    bsp_backlight(false);
    ESP_ERROR_CHECK(bsp_spi_bus_init());

    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = BOARD_LCD_CS,
        .dc_gpio_num = BOARD_LCD_DC,
        .spi_mode = 0,
        .pclk_hz = BOARD_LCD_PCLK_HZ,
        .trans_queue_depth = 10,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    esp_lcd_panel_io_handle_t io;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)BOARD_SPI_HOST, &io_cfg, &io));

    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = BOARD_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = 16,
    };
    esp_lcd_panel_handle_t panel;
    ESP_ERROR_CHECK(esp_lcd_new_panel_ili9341(io, &panel_cfg, &panel));
    s_panel = panel;
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel, true));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));

    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_stack = 8192;     // deep object trees (the Guide's pack reveal) left ~600 B of 7 KB
    ESP_ERROR_CHECK(lvgl_port_init(&port_cfg));

    lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = io,
        .panel_handle = panel,
        .buffer_size = BOARD_LCD_H_RES * 24,     // 2 x 15 KB DMA; RAM is tight
        .double_buffer = true,
        .hres = BOARD_LCD_H_RES,
        .vres = BOARD_LCD_V_RES,
        .rotation = { .swap_xy = true, .mirror_x = true, .mirror_y = true },
        .color_format = LV_COLOR_FORMAT_RGB565,
        .flags = { .buff_dma = true, .swap_bytes = true },
    };
    lv_display_t *disp = lvgl_port_add_disp(&disp_cfg);
    ESP_RETURN_ON_FALSE(disp, NULL, TAG, "lvgl display");
    s_lcd_idle = xSemaphoreCreateBinary();
    xSemaphoreGive(s_lcd_idle);
    const esp_lcd_panel_io_callbacks_t cbs = { .on_color_trans_done = flush_done };
    ESP_ERROR_CHECK(esp_lcd_panel_io_register_event_callbacks(io, &cbs, disp));
    lv_display_add_event_cb(disp, flush_start, LV_EVENT_FLUSH_START, NULL);
    bsp_backlight(true);
    ESP_LOGI(TAG, "LCD %dx%d ready", BOARD_LCD_H_RES, BOARD_LCD_V_RES);
    return disp;
}

void bsp_display_invert(bool on)
{
    if (s_panel) {
        esp_lcd_panel_invert_color(s_panel, on);
    }
}
