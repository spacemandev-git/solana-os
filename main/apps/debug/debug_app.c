// Hardware Debug: live check of every peripheral on the badge (buttons, RGB
// LEDs, microSD, display, battery, system). Port of the reference dashboard.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_app_desc.h"
#include "esp_idf_version.h"
#include "bsp.h"
#include "core/os.h"
#include "core/os_wifi.h"
#include "core/ui.h"

enum { PAGE_INPUT, PAGE_SD, PAGE_DISPLAY, PAGE_SYSTEM, PAGE_COUNT };
static const char *const page_names[PAGE_COUNT] = {"Input", "microSD", "Display", "System"};

#define PAGE_Y      UI_TABS_H
#define PAGE_H      (OS_ROOT_H - UI_TABS_H - UI_HINT_H)

static lv_obj_t *s_root, *s_tabs, *s_page, *s_hint;
static int s_cur;
static int s_tick;

static void build_page(int page);

// ---------------------------------------------------------------------------
// Input: buttons + LED mirror

#define N_BTN   (BOARD_BTN_COUNT + 1)
static lv_obj_t *s_btn[N_BTN], *s_led[BOARD_RGB_COUNT];
static uint32_t s_last_buttons = UINT32_MAX;

// Per-button LED colour: brand palette, one hue per button.
static const uint32_t btn_col[N_BTN] = {COL_PURPLE, COL_BLUE, COL_AQUA, COL_GREEN, COL_YELLOW, COL_PINK, COL_TEXT};

static void led_scaled(int i, uint32_t rgb)
{
    bsp_rgb_set(i, ((rgb >> 16) & 0xFF) * OS_LED_MAX / 255, ((rgb >> 8) & 0xFF) * OS_LED_MAX / 255,
                (rgb & 0xFF) * OS_LED_MAX / 255);
}

static void leds_off(void)
{
    bsp_rgb_fill(0, 0, 0);
    bsp_rgb_show();
}

static uint32_t led_preview(int i)
{
    // LEDs run dim; scale up so the on-screen mirror is readable.
    uint32_t c = bsp_rgb_get(i);
    uint8_t r = c >> 16, g = c >> 8, b = c;
    uint8_t mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
    return mx ? (((r * 255 / mx) << 16) | ((g * 255 / mx) << 8) | (b * 255 / mx)) : COL_PANEL_HI;
}

static void build_input(lv_obj_t *p)
{
    static const char *phys[N_BTN] = {"PB1", "PB2", "PB3", "PB4", "PB5", "PB6", "BOOT"};
    static const char *logical[N_BTN] = {"up", "left", "right", "down", "sel", "cancel", "home"};
    lv_obj_t *t = ui_label(p, &sol_mono_12, COL_DIM, "BUTTONS  TCA9534 @0x20 + GPIO28");
    lv_obj_set_pos(t, 8, 6);
    for (int i = 0; i < N_BTN; i++) {
        lv_obj_t *b = ui_panel(p, 8 + i * 44, 24, 40, 44);
        lv_obj_set_style_pad_all(b, 0, 0);
        lv_obj_t *n = ui_label(b, &sol_mono_12, COL_TEXT, phys[i]);
        lv_obj_align(n, LV_ALIGN_TOP_MID, 0, 6);
        lv_obj_t *l = ui_label(b, &sol_mono_12, COL_DIM, logical[i]);
        lv_obj_align(l, LV_ALIGN_BOTTOM_MID, 0, -5);
        s_btn[i] = b;
    }
    t = ui_label(p, &sol_mono_12, COL_DIM, "RGB  8x WS2812B on GPIO27");
    lv_obj_set_pos(t, 8, 80);
    for (int i = 0; i < BOARD_RGB_COUNT; i++) {
        lv_obj_t *d = lv_obj_create(p);
        lv_obj_remove_style_all(d);
        lv_obj_set_size(d, 22, 22);
        lv_obj_set_pos(d, 14 + i * 38, 100);
        lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(d, 1, 0);
        lv_obj_set_style_border_color(d, ui_col(COL_BORDER), 0);
        s_led[i] = d;
    }
    lv_obj_t *h = ui_label(p, &sol_font_14, COL_DIM,
                           "Press any button: its box and LED light up. If the arrows go the wrong way, press SELECT to calibrate.");
    lv_obj_set_width(h, OS_ROOT_W - 16);
    lv_label_set_long_mode(h, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_pos(h, 8, 134);
    s_last_buttons = UINT32_MAX;
}

static void tick_input(void)
{
    uint32_t pressed = bsp_buttons_get();
    if (pressed != s_last_buttons) {
        s_last_buttons = pressed;
        for (int i = 0; i < N_BTN; i++) {
            bool on = pressed & (1u << i);
            lv_obj_set_style_bg_color(s_btn[i], ui_col(on ? btn_col[i] : COL_PANEL), 0);
            lv_obj_set_style_text_color(lv_obj_get_child(s_btn[i], 0), ui_col(on ? COL_BG : COL_TEXT), 0);
            lv_obj_set_style_text_color(lv_obj_get_child(s_btn[i], 1), ui_col(on ? COL_BG : COL_DIM), 0);
            if (i < BOARD_BTN_COUNT) {
                led_scaled(i, on ? btn_col[i] : 0);
            }
        }
        bool boot = pressed & BSP_BTN_BOOT;
        led_scaled(6, boot ? COL_TEXT : 0);
        led_scaled(7, boot ? COL_TEXT : 0);
        bsp_rgb_show();
    }
    for (int i = 0; i < BOARD_RGB_COUNT; i++) {
        lv_obj_set_style_bg_color(s_led[i], lv_color_hex(led_preview(i)), 0);
    }
}

// ---------------------------------------------------------------------------
// microSD

enum { SD_ACT_MOUNT, SD_ACT_TEST, SD_ACT_FORMAT, SD_ACT_COUNT };

static lv_obj_t *s_sd_status, *s_sd_detail;
static ui_list_t *s_sd_list;
static volatile bool s_sd_busy;
static volatile int s_sd_result = -1;       // last action result, shown once
static char s_sd_files[160];
static int s_sd_file_count;
static int64_t s_format_armed_until;
static bsp_sd_info_t s_info;      // cached: never block on the SD lock while a worker runs

static void sd_refresh_info(void)
{
    if (!s_sd_busy) {
        bsp_sd_get_info(&s_info);
    }
}

static void list_files(void)
{
    s_sd_file_count = 0;
    s_sd_files[0] = 0;
    DIR *d = opendir(BSP_SD_MOUNT);
    if (!d) {
        return;
    }
    struct dirent *de;
    size_t len = 0;
    while ((de = readdir(d))) {
        s_sd_file_count++;
        int n = snprintf(s_sd_files + len, sizeof(s_sd_files) - len, "%s%s", len ? ", " : "", de->d_name);
        if (n > 0 && len + n < sizeof(s_sd_files)) {
            len += n;
        }
    }
    closedir(d);
}

// Write, read back and verify 256 KB.
static bool sd_speed_test(int *wr_kbs, int *rd_kbs)
{
    const char *path = BSP_SD_MOUNT "/speed.bin";
    const int chunk = 4096, total = 256 * 1024;
    uint8_t *buf = malloc(chunk);
    FILE *f = buf ? fopen(path, "wb") : NULL;
    if (!f) {
        free(buf);
        return false;
    }
    int64_t t0 = esp_timer_get_time();
    for (int off = 0; off < total; off += chunk) {
        for (int i = 0; i < chunk; i++) {
            buf[i] = (uint8_t)((off + i) * 7);
        }
        fwrite(buf, 1, chunk, f);
    }
    fclose(f);
    int64_t t1 = esp_timer_get_time();
    bool ok = (f = fopen(path, "rb")) != NULL;
    for (int off = 0; ok && off < total; off += chunk) {
        ok = fread(buf, 1, chunk, f) == chunk;
        for (int i = 0; ok && i < chunk; i++) {
            ok = buf[i] == (uint8_t)((off + i) * 7);
        }
    }
    if (f) {
        fclose(f);
    }
    int64_t t2 = esp_timer_get_time();
    unlink(path);
    free(buf);
    *wr_kbs = total * 1000LL / (t1 - t0);
    *rd_kbs = total * 1000LL / (t2 - t1);
    return ok;
}

static int s_test_wr, s_test_rd;

static void sd_worker(void *arg)
{
    int act = (int)arg;
    bsp_sd_info_t in;
    bsp_sd_get_info(&in);
    int rc = 0;
    if (act == SD_ACT_MOUNT) {
        rc = in.state == BSP_SD_MOUNTED ? bsp_sd_unmount() : bsp_sd_mount();
    } else if (act == SD_ACT_TEST) {
        rc = sd_speed_test(&s_test_wr, &s_test_rd) ? 0 : -2;
    } else if (act == SD_ACT_FORMAT) {
        rc = bsp_sd_format();
    }
    bsp_sd_get_info(&in);
    if (in.state == BSP_SD_MOUNTED) {
        bsp_sd_refresh_usage();
        list_files();
    }
    s_sd_result = act * 1000 + (rc == 0 ? 0 : rc == -2 ? 2 : 1);
    s_sd_busy = false;
    vTaskDelete(NULL);
}

static void sd_row(int i, ui_row_t *r, void *ctx)
{
    const bsp_sd_info_t in = s_info;
    switch (i) {
    case SD_ACT_MOUNT:
        strcpy(r->title, in.state == BSP_SD_MOUNTED ? "Unmount card" : "Mount / retry card");
        strcpy(r->sub, "Reinsert the card first if it was rejected");
        break;
    case SD_ACT_TEST:
        strcpy(r->title, "Speed test");
        strcpy(r->sub, "Write, read back and verify 256 KB");
        r->title_color = in.state == BSP_SD_MOUNTED ? 0 : COL_DIM;
        break;
    case SD_ACT_FORMAT:
        strcpy(r->title, "Format card");
        strcpy(r->sub, "Erases everything on the card");
        r->title_color = COL_ERR;
        if (s_format_armed_until) {
            strcpy(r->right, "SELECT again");
            r->right_color = COL_ERR;
        }
        break;
    }
}

static uint32_t sd_color(bsp_sd_state_t s)
{
    switch (s) {
    case BSP_SD_MOUNTED: return COL_OK;
    case BSP_SD_BAD_FS:  return COL_WARN;
    case BSP_SD_ERROR:
    case BSP_SD_INCOMPATIBLE:
        return COL_ERR;
    default:             return COL_DIM;
    }
}

static const char *sd_hint(bsp_sd_state_t s)
{
    switch (s) {
    case BSP_SD_BAD_FS:  return "Volume is not usable by FatFs (e.g. FAT32 < 65,525 clusters). Format it below.";
    case BSP_SD_NO_CARD: return "No card detected. Insert one and choose Mount.";
    case BSP_SD_ERROR:   return "Card answered incorrectly. Reinsert it and choose Mount.";
    case BSP_SD_INCOMPATIBLE:
        return "Card ignores chip-select and would be corrupted by display traffic. Use a name-brand card.";
    default:             return "Choose Mount.";
    }
}

static void sd_render_info(void)
{
    sd_refresh_info();
    const bsp_sd_info_t in = s_info;
    if (s_sd_busy) {
        lv_label_set_text(s_sd_status, "working...");
        lv_obj_set_style_text_color(s_sd_status, ui_col(COL_YELLOW), 0);
        return;
    }
    lv_label_set_text(s_sd_status, bsp_sd_state_str(in.state));
    lv_obj_set_style_text_color(s_sd_status, ui_col(sd_color(in.state)), 0);
    if (in.state == BSP_SD_MOUNTED) {
        lv_label_set_text_fmt(s_sd_detail, "%s  %s  %lu MB  |  FAT %llu MB, %llu MB free\n%d item(s): %s",
                              in.name, in.type, (unsigned long)in.card_mb, in.total_bytes >> 20,
                              in.free_bytes >> 20, s_sd_file_count, s_sd_file_count ? s_sd_files : "(empty)");
    } else {
        lv_label_set_text_fmt(s_sd_detail, "%s\n%s", esp_err_to_name(in.last_err), sd_hint(in.state));
    }
}

static void build_sd(lv_obj_t *p)
{
    lv_obj_t *box = ui_panel(p, 4, 4, OS_ROOT_W - 8, 66);
    lv_obj_t *t = ui_label(box, &sol_mono_12, COL_DIM, LV_SYMBOL_SD_CARD " microSD  SPI2  CS=GPIO4");
    lv_obj_set_pos(t, 0, -2);
    s_sd_status = ui_label(box, &sol_font_14, COL_TEXT, "");
    lv_obj_align(s_sd_status, LV_ALIGN_TOP_RIGHT, 0, -4);
    s_sd_detail = ui_label(box, &sol_mono_12, COL_TEXT, "");
    lv_obj_set_pos(s_sd_detail, 0, 16);
    lv_obj_set_width(s_sd_detail, OS_ROOT_W - 22);
    lv_label_set_long_mode(s_sd_detail, LV_LABEL_LONG_MODE_WRAP);
    s_sd_list = ui_list_create(p, 72, PAGE_H - 72, sd_row, NULL);
    sd_refresh_info();
    if (s_info.state == BSP_SD_MOUNTED && !s_sd_busy) {
        list_files();
    }
    ui_list_set_count(s_sd_list, SD_ACT_COUNT);
    sd_render_info();
}

static void sd_start(int act)
{
    if (s_sd_busy) {
        return;
    }
    s_sd_busy = true;
    if (xTaskCreate(sd_worker, "sd_act", 4096, (void *)act, 3, NULL) != pdPASS) {
        s_sd_busy = false;
        os_toast("Out of memory");
    }
    sd_render_info();
}

static bool key_sd(os_key_t k, bool repeat)
{
    if (ui_list_key(s_sd_list, k, repeat)) {
        return true;
    }
    if (k != OS_KEY_SELECT) {
        return false;
    }
    int act = ui_list_selected(s_sd_list);
    if (s_sd_busy) {
        os_toast("Busy...");
        return true;
    }
    sd_refresh_info();
    const bsp_sd_info_t in = s_info;
    if (act == SD_ACT_TEST && in.state != BSP_SD_MOUNTED) {
        os_toast("Mount a card first");
    } else if (act == SD_ACT_FORMAT) {
        if (in.state != BSP_SD_MOUNTED && in.state != BSP_SD_BAD_FS) {
            os_toast("No usable card to format");
        } else if (!s_format_armed_until) {
            s_format_armed_until = esp_timer_get_time() + 3000000;
            os_toast("Press SELECT again to erase the card");
        } else {
            s_format_armed_until = 0;
            sd_start(act);
        }
    } else {
        sd_start(act);
    }
    ui_list_refresh(s_sd_list);
    return true;
}

static void tick_sd(void)
{
    if (s_format_armed_until && esp_timer_get_time() > s_format_armed_until) {
        s_format_armed_until = 0;
        ui_list_refresh(s_sd_list);
    }
    int res = s_sd_result;
    if (res >= 0) {
        s_sd_result = -1;
        int act = res / 1000, rc = res % 1000;
        if (act == SD_ACT_TEST) {
            if (rc == 0) {
                os_toast("PASS  write %d KB/s  read %d KB/s", s_test_wr, s_test_rd);
            } else {
                os_toast("Speed test FAILED");
            }
        } else if (act == SD_ACT_FORMAT) {
            os_toast(rc ? "Format failed" : "Card formatted");
        }
        ui_list_refresh(s_sd_list);
        sd_render_info();
    } else if (s_tick % 10 == 0) {
        sd_render_info();
    }
}

// ---------------------------------------------------------------------------
// Display + LED patterns

enum { LED_OFF, LED_GRADIENT, LED_CHASE, LED_WHITE, LED_PATTERNS };
static const char *const led_names[LED_PATTERNS] = {"off", "Solana gradient", "rainbow chase", "white"};
static int s_led_mode;
static lv_obj_t *s_led_label;

static uint32_t lerp_rgb(uint32_t a, uint32_t b, int t, int n)
{
    int r = ((a >> 16) & 0xFF) + (((int)((b >> 16) & 0xFF) - (int)((a >> 16) & 0xFF)) * t) / n;
    int g = ((a >> 8) & 0xFF) + (((int)((b >> 8) & 0xFF) - (int)((a >> 8) & 0xFF)) * t) / n;
    int bl = (a & 0xFF) + (((int)(b & 0xFF) - (int)(a & 0xFF)) * t) / n;
    return (r << 16) | (g << 8) | bl;
}

static uint32_t hue(int h)
{
    h %= 360;
    int x = (h % 60) * 255 / 60;
    switch (h / 60) {
    case 0:  return 0xFF0000 | (x << 8);
    case 1:  return ((255 - x) << 16) | 0xFF00;
    case 2:  return 0x00FF00 | x;
    case 3:  return ((255 - x) << 8) | 0xFF;
    case 4:  return (x << 16) | 0xFF;
    default: return 0xFF0000 | (255 - x);
    }
}

static void led_pattern_step(void)
{
    for (int i = 0; i < BOARD_RGB_COUNT; i++) {
        uint32_t c = 0;
        switch (s_led_mode) {
        case LED_GRADIENT: {
            // Slowly sweep the purple -> green gradient along the strip.
            int t = (i * 32 + s_tick * 4) % 512;
            c = lerp_rgb(COL_PURPLE, COL_GREEN, t < 256 ? t : 511 - t, 255);
            break;
        }
        case LED_CHASE:  c = hue(i * 45 + s_tick * 12); break;
        case LED_WHITE:  c = 0xFFFFFF; break;
        default: break;
        }
        led_scaled(i, c);
    }
    bsp_rgb_show();
}

static void build_display(lv_obj_t *p)
{
    // Colour check: the bars must read red, green, blue left to right.
    static const uint32_t cols[3] = {0xFF0000, 0x00FF00, 0x0000FF};
    static const char *names[3] = {"RED", "GREEN", "BLUE"};
    for (int i = 0; i < 3; i++) {
        lv_obj_t *bar = lv_obj_create(p);
        lv_obj_remove_style_all(bar);
        lv_obj_set_pos(bar, 8 + i * 102, 6);
        lv_obj_set_size(bar, 98, 56);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(bar, lv_color_hex(cols[i]), 0);
        lv_obj_set_style_radius(bar, 4, 0);
        lv_obj_t *t = ui_label(bar, &sol_mono_12, COL_TEXT, names[i]);
        lv_obj_set_style_text_color(t, lv_color_hex(i == 1 ? 0x000000 : 0xFFFFFF), 0);
        lv_obj_center(t);
    }
    ui_gradient_bar(p, 8, 70, OS_ROOT_W - 16, 20);
    for (int i = 0; i < 8; i++) {
        lv_obj_t *g = lv_obj_create(p);
        lv_obj_remove_style_all(g);
        lv_obj_set_pos(g, 8 + i * 38, 98);
        lv_obj_set_size(g, 38, 16);
        lv_obj_set_style_bg_opa(g, LV_OPA_COVER, 0);
        int v = i * 255 / 7;
        lv_obj_set_style_bg_color(g, lv_color_make(v, v, v), 0);
    }
    lv_obj_t *info = ui_label(p, &sol_mono_12, COL_DIM, "ILI9341 320x240  SPI2 @20 MHz  CS=6 DC=7 BL=3");
    lv_obj_set_pos(info, 8, 122);
    s_led_label = ui_label(p, &sol_font_14, COL_TEXT, "");
    lv_obj_set_pos(s_led_label, 8, 142);
    lv_label_set_text_fmt(s_led_label, "LED pattern: %s", led_names[s_led_mode]);
}

static bool key_display(os_key_t k, bool repeat)
{
    if (k != OS_KEY_SELECT) {
        return false;
    }
    s_led_mode = (s_led_mode + 1) % LED_PATTERNS;
    lv_label_set_text_fmt(s_led_label, "LED pattern: %s", led_names[s_led_mode]);
    led_pattern_step();
    return true;
}

// ---------------------------------------------------------------------------
// System

static lv_obj_t *s_sys;

static void build_system(lv_obj_t *p)
{
    s_sys = ui_label(p, &sol_mono_12, COL_TEXT, "");
    lv_obj_set_pos(s_sys, 8, 6);
    lv_obj_set_width(s_sys, OS_ROOT_W - 16);
}

static const char *reset_reason(void)
{
    switch (esp_reset_reason()) {
    case ESP_RST_POWERON:  return "power on";
    case ESP_RST_SW:       return "software";
    case ESP_RST_PANIC:    return "panic";
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:      return "watchdog";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_EXT:      return "external";
    default:               return "other";
    }
}

static void tick_system(void)
{
    if (s_tick % 5) {
        return;
    }
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    uint32_t flash = 0;
    esp_flash_get_size(NULL, &flash);
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    int mv = bsp_battery_mv();
    os_wifi_info_t w;
    os_wifi_get_info(&w);
    uint32_t up = esp_timer_get_time() / 1000000;
    lv_label_set_text_fmt(s_sys,
        "Chip      ESP32-C5 rev %d.%d, %d core @ %d MHz\n"
        "Flash     %lu MB\n"
        "MAC       %02X:%02X:%02X:%02X:%02X:%02X\n"
        "Firmware  Solana OS %s\n"
        "ESP-IDF   %s\n"
        "Uptime    %luh %02lum %02lus\n"
        "Heap      %u KB free, %u KB min\n"
        "Battery   %d mV (%d%%)\n"
        "Wi-Fi     %s %s\n"
        "Reset     %s",
        chip.revision / 100, chip.revision % 100, chip.cores, CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
        (unsigned long)(flash >> 20), mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
        esp_app_get_description()->version, esp_get_idf_version(),
        (unsigned long)(up / 3600), (unsigned long)(up / 60 % 60), (unsigned long)(up % 60),
        (unsigned)(heap_caps_get_free_size(MALLOC_CAP_DEFAULT) / 1024),
        (unsigned)(heap_caps_get_minimum_free_size(MALLOC_CAP_DEFAULT) / 1024),
        mv, bsp_battery_percent(mv),
        w.state == OS_WIFI_CONNECTED ? w.ssid : "not connected", w.ip,
        reset_reason());
}

// ---------------------------------------------------------------------------

static void build_page(int page)
{
    s_cur = page;
    ui_tabs_select(s_tabs, page);
    lv_obj_clean(s_page);
    s_led_mode = page == PAGE_DISPLAY ? s_led_mode : LED_OFF;
    leds_off();
    switch (page) {
    case PAGE_INPUT:
        build_input(s_page);
        lv_label_set_text(s_hint, LV_SYMBOL_LEFT LV_SYMBOL_RIGHT " tab   SELECT calibrate   CANCEL exit");
        break;
    case PAGE_SD:
        build_sd(s_page);
        lv_label_set_text(s_hint, LV_SYMBOL_LEFT LV_SYMBOL_RIGHT " tab   " LV_SYMBOL_UP LV_SYMBOL_DOWN " choose   SELECT run");
        break;
    case PAGE_DISPLAY:
        build_display(s_page);
        lv_label_set_text(s_hint, LV_SYMBOL_LEFT LV_SYMBOL_RIGHT " tab   SELECT cycle LED pattern");
        break;
    case PAGE_SYSTEM:
        build_system(s_page);
        s_tick = 0;
        tick_system();
        lv_label_set_text(s_hint, LV_SYMBOL_LEFT LV_SYMBOL_RIGHT " tab   CANCEL exit");
        break;
    }
}

static void enter(lv_obj_t *root)
{
    s_root = root;
    s_tabs = ui_tabs(root, page_names, PAGE_COUNT);
    s_page = lv_obj_create(root);
    lv_obj_remove_style_all(s_page);
    lv_obj_set_pos(s_page, 0, PAGE_Y);
    lv_obj_set_size(s_page, OS_ROOT_W, PAGE_H);
    lv_obj_set_scrollable(s_page, false);
    s_hint = ui_hint(root, "");
    s_led_mode = LED_OFF;
    build_page(PAGE_INPUT);
}

static void on_leave(void)
{
    s_led_mode = LED_OFF;
    leds_off();
}

static bool on_key(os_key_t k, bool repeat)
{
    if (k == OS_KEY_LEFT || k == OS_KEY_RIGHT) {
        if (!repeat) {
            build_page((s_cur + (k == OS_KEY_RIGHT ? 1 : PAGE_COUNT - 1)) % PAGE_COUNT);
        }
        return true;
    }
    switch (s_cur) {
    case PAGE_INPUT:
        if (k == OS_KEY_SELECT) {
            os_launch(&os_calibrate);
            return true;
        }
        return k != OS_KEY_BACK;
    case PAGE_SD:      return key_sd(k, repeat);
    case PAGE_DISPLAY: return key_display(k, repeat);
    default:           return k != OS_KEY_BACK;     // swallow other keys, Back exits
    }
}

static void on_tick(void)
{
    s_tick++;
    switch (s_cur) {
    case PAGE_INPUT:   tick_input(); break;
    case PAGE_SD:      tick_sd(); break;
    case PAGE_DISPLAY:
        if (s_led_mode == LED_GRADIENT || s_led_mode == LED_CHASE) {
            led_pattern_step();
        }
        break;
    case PAGE_SYSTEM:  tick_system(); break;
    }
}

const os_app_t app_debug = {
    .id = "debug",
    .name = "HW Debug",
    .symbol = LV_SYMBOL_SETTINGS,
    .accent = COL_YELLOW,
    .on_enter = enter,
    .on_exit = on_leave,
    .on_key = on_key,
    .on_tick = on_tick,
};
