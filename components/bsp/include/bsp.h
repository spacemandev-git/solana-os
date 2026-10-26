#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "lvgl.h"
#include "board.h"

// ---- Shared SPI bus ----
esp_err_t bsp_spi_bus_init(void);

// ---- microSD ----
#define BSP_SD_MOUNT "/sdcard"

typedef enum {
    BSP_SD_NOT_MOUNTED,
    BSP_SD_MOUNTED,
    BSP_SD_NO_CARD,     // no response to CMD0
    BSP_SD_BAD_FS,      // card works but has no FAT volume ESP-IDF can mount
    BSP_SD_ERROR,       // card responded incorrectly (reinsert it)
    BSP_SD_INCOMPATIBLE,// card obeys commands while deselected: unsafe on a shared bus
} bsp_sd_state_t;

typedef struct {
    bsp_sd_state_t state;
    esp_err_t last_err;
    char name[8];
    const char *type;   // "SDSC", "SDHC/SDXC"
    uint32_t card_mb;
    uint64_t total_bytes;
    uint64_t free_bytes;
} bsp_sd_info_t;

esp_err_t bsp_sd_mount(void);
esp_err_t bsp_sd_unmount(void);
esp_err_t bsp_sd_format(void);
void bsp_sd_get_info(bsp_sd_info_t *out);  // cheap; usage figures are cached
void bsp_sd_refresh_usage(void);
const char *bsp_sd_state_str(bsp_sd_state_t s);
void bsp_sd_print_card(void);

// ---- Display (ILI9341 + LVGL) ----
lv_display_t *bsp_display_start(void);
void bsp_backlight(bool on);
// Panels differ between batches: the prototype's needs colour inversion (the
// default), others show inverted colours with it. Safe from any task.
void bsp_display_invert(bool on);

// ---- Buttons ----
#define BSP_BTN_PB(n)   (1u << ((n) - 1))   // n = 1..6
#define BSP_BTN_BOOT    (1u << 6)
typedef void (*bsp_btn_cb_t)(uint32_t pressed, uint32_t changed);
esp_err_t bsp_buttons_start(bsp_btn_cb_t cb);
uint32_t bsp_buttons_get(void);

// ---- RGB LEDs ----
esp_err_t bsp_rgb_init(void);
void bsp_rgb_set(int index, uint8_t r, uint8_t g, uint8_t b);
void bsp_rgb_fill(uint8_t r, uint8_t g, uint8_t b);
void bsp_rgb_show(void);
uint32_t bsp_rgb_get(int index);    // 0xRRGGBB

// ---- Battery ----
esp_err_t bsp_battery_init(void);
int bsp_battery_mv(void);           // -1 on error
int bsp_battery_percent(int mv);
