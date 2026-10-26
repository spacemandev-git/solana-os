// App manager: status bar, input routing, app switching, toasts, screenshots.

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_app_desc.h"
#include "nvs.h"
#include "esp_lvgl_port.h"
#include "mbedtls/base64.h"
#include "lvgl_private.h"
#include "bsp.h"
#include "os.h"
#include "os_wifi.h"
#include "ui.h"

static const char *TAG = "os";

#define REPEAT_DELAY_MS     400
#define REPEAT_RATE_MS      90
#define TOAST_MS            2200
#define STATUS_EVERY_MS     1000

typedef enum { EV_BUTTONS, EV_KEY, EV_LAUNCH, EV_SHOT, EV_CALL } ev_kind_t;
typedef struct {
    ev_kind_t kind;
    uint32_t a, b;
    const os_app_t *app;
    void (*fn)(void *arg);
    void *arg;
} os_event_t;

static QueueHandle_t s_q;
static TaskHandle_t s_task;
static const os_app_t *s_app;
static lv_obj_t *s_root, *s_title, *s_wifi, *s_batt, *s_toast;
static int64_t s_toast_until;

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

// ---------------------------------------------------------------------------
// Key map: defaults from os_keys.h, overridden by calibration stored in NVS.

static uint32_t s_keymap[OS_KEY_COUNT] = {
    [OS_KEY_UP] = OS_BTN_UP, [OS_KEY_DOWN] = OS_BTN_DOWN, [OS_KEY_LEFT] = OS_BTN_LEFT,
    [OS_KEY_RIGHT] = OS_BTN_RIGHT, [OS_KEY_SELECT] = OS_BTN_SELECT, [OS_KEY_BACK] = OS_BTN_BACK,
    [OS_KEY_HOME] = OS_BTN_HOME,
};

os_key_t os_key_from_button(uint32_t bit)
{
    for (int k = 0; k < OS_KEY_COUNT; k++) {
        if (s_keymap[k] == bit) {
            return k;
        }
    }
    return OS_KEY_NONE;
}

uint32_t os_key_button(os_key_t k)
{
    return (k >= 0 && k < OS_KEY_COUNT) ? s_keymap[k] : 0;
}

void os_keys_load(void)
{
    nvs_handle_t h;
    uint32_t map[OS_KEY_FRONT_COUNT];
    size_t len = sizeof(map);
    if (nvs_open("input", NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    if (nvs_get_blob(h, "map", map, &len) == ESP_OK && len == sizeof(map)) {
        memcpy(s_keymap, map, sizeof(map));
        ESP_LOGI(TAG, "using calibrated button map");
    }
    nvs_close(h);
}

void os_keys_save(const uint32_t map[OS_KEY_FRONT_COUNT])
{
    memcpy(s_keymap, map, OS_KEY_FRONT_COUNT * sizeof(uint32_t));
    nvs_handle_t h;
    if (nvs_open("input", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_blob(h, "map", map, OS_KEY_FRONT_COUNT * sizeof(uint32_t));
        nvs_commit(h);
        nvs_close(h);
    }
    for (int k = 0; k < OS_KEY_FRONT_COUNT; k++) {
        ESP_LOGI(TAG, "key %-6s = button 0x%02lx", os_key_name(k), (unsigned long)map[k]);
    }
}

static const char *const key_names[OS_KEY_COUNT] = {"up", "down", "left", "right", "select", "back", "home"};

const char *os_key_name(os_key_t k)
{
    return (k >= 0 && k < OS_KEY_COUNT) ? key_names[k] : "none";
}

os_key_t os_key_parse(const char *name)
{
    for (int i = 0; i < OS_KEY_COUNT; i++) {
        if (!strcmp(name, key_names[i])) {
            return i;
        }
    }
    return OS_KEY_NONE;
}

// ---------------------------------------------------------------------------
// Status bar

static lv_obj_t *s_bar;

static void status_create(lv_obj_t *scr)
{
    lv_obj_t *bar = s_bar = lv_obj_create(scr);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, BOARD_LCD_H_RES, OS_STATUS_H);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(bar, ui_col(COL_BG), 0);
    lv_obj_set_scrollable(bar, false);

    lv_obj_t *logo = lv_image_create(bar);
    lv_image_set_src(logo, &img_logomark_16);
    lv_obj_align(logo, LV_ALIGN_LEFT_MID, 6, 0);
    s_title = ui_label(bar, &sol_font_14, COL_TEXT, "Solana OS");
    lv_obj_align(s_title, LV_ALIGN_LEFT_MID, 30, 0);
    s_batt = ui_label(bar, &sol_mono_12, COL_TEXT, "");
    lv_obj_align(s_batt, LV_ALIGN_RIGHT_MID, -6, 0);
    s_wifi = ui_label(bar, &lv_font_montserrat_14, COL_DIM, LV_SYMBOL_WIFI);
    lv_obj_align(s_wifi, LV_ALIGN_RIGHT_MID, -76, 0);

    // Brand gradient hairline under the bar.
    lv_obj_t *line = ui_gradient_bar(bar, 0, OS_STATUS_H - 2, BOARD_LCD_H_RES, 2);
    lv_obj_set_style_radius(line, 0, 0);
}

static void status_update(void)
{
    int mv = bsp_battery_mv();
    if (mv < 0) {
        lv_label_set_text(s_batt, LV_SYMBOL_BATTERY_EMPTY " --");
    } else if (mv > 4300) {
        lv_label_set_text(s_batt, LV_SYMBOL_USB " USB");
    } else {
        int pct = bsp_battery_percent(mv);
        const char *sym = pct > 80 ? LV_SYMBOL_BATTERY_FULL : pct > 55 ? LV_SYMBOL_BATTERY_3
                        : pct > 30 ? LV_SYMBOL_BATTERY_2 : pct > 10 ? LV_SYMBOL_BATTERY_1 : LV_SYMBOL_BATTERY_EMPTY;
        lv_label_set_text_fmt(s_batt, "%s %d%%", sym, pct);
    }
    lv_obj_align(s_batt, LV_ALIGN_RIGHT_MID, -6, 0);

    uint32_t c = COL_DIM;
    switch (os_wifi_state()) {
    case OS_WIFI_CONNECTED:  c = COL_GREEN; break;
    case OS_WIFI_CONNECTING: c = COL_YELLOW; break;
    case OS_WIFI_FAILED:     c = COL_ERR; break;
    case OS_WIFI_SUSPENDED:  c = COL_PINK; break;
    default: break;
    }
    lv_obj_set_style_text_color(s_wifi, ui_col(c), 0);
    lv_obj_align_to(s_wifi, s_batt, LV_ALIGN_OUT_LEFT_MID, -8, 0);
}

// ---------------------------------------------------------------------------
// Toast

#define TOAST_LEN   96

static void toast_show(const char *msg)
{
    lv_label_set_text(s_toast, msg);
    lv_obj_set_hidden(s_toast, false);
    lv_obj_align(s_toast, LV_ALIGN_BOTTOM_MID, 0, -22);
    s_toast_until = now_ms() + TOAST_MS;
}

static void toast_posted(void *arg)
{
    toast_show(arg);
    free(arg);
}

// Other tasks hand the text to the OS task instead of waiting for the LVGL
// lock: a mesh or API handler must never block on the UI.
void os_toast(const char *fmt, ...)
{
    char msg[TOAST_LEN];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    if (s_task && xTaskGetCurrentTaskHandle() == s_task) {
        toast_show(msg);
        return;
    }
    char *copy = strdup(msg);
    if (copy && !os_post(toast_posted, copy)) {
        free(copy);
    }
}

static void toast_create(void)
{
    s_toast = ui_label(lv_layer_top(), &sol_font_14, COL_BG, "");
    lv_obj_set_style_bg_opa(s_toast, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_toast, ui_col(COL_GREEN), 0);
    lv_obj_set_style_radius(s_toast, 12, 0);
    lv_obj_set_style_pad_hor(s_toast, 12, 0);
    lv_obj_set_style_pad_ver(s_toast, 4, 0);
    lv_obj_set_style_max_width(s_toast, BOARD_LCD_H_RES - 20, 0);
    lv_label_set_long_mode(s_toast, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_hidden(s_toast, true);
}

// ---------------------------------------------------------------------------
// Confirm dialog (OS task, LVGL locked)

#define CONFIRM_W   280

static lv_obj_t *s_modal;
static const char *s_hold;              // os_hold(): why the app can't be left now
static os_confirm_cb_t s_modal_cb;
static void *s_modal_ctx;

typedef struct {
    os_confirm_cb_t done;
    void *ctx;
    char text[];
} confirm_req_t;

static void confirm_close(bool yes)
{
    if (!s_modal) {
        return;
    }
    lv_obj_delete(s_modal);
    s_modal = NULL;
    os_confirm_cb_t cb = s_modal_cb;
    s_modal_cb = NULL;
    if (cb) {
        cb(yes, s_modal_ctx);
    }
}

static void confirm_show(const char *text, os_confirm_cb_t done, void *ctx)
{
    if (s_modal) {
        if (done) {
            done(false, ctx);
        }
        return;
    }
    s_modal_cb = done;
    s_modal_ctx = ctx;
    s_modal = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_modal);
    lv_obj_set_size(s_modal, BOARD_LCD_H_RES, BOARD_LCD_V_RES);
    lv_obj_set_style_bg_opa(s_modal, LV_OPA_70, 0);
    lv_obj_set_style_bg_color(s_modal, lv_color_black(), 0);
    lv_obj_set_scrollable(s_modal, false);

    lv_obj_t *box = ui_panel(s_modal, 0, 0, CONFIRM_W, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(box, ui_col(COL_PANEL_HI), 0);
    lv_obj_set_style_border_color(box, ui_col(COL_PURPLE), 0);
    lv_obj_set_style_pad_all(box, 12, 0);
    lv_obj_set_style_pad_top(box, 16, 0);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(box, 12, 0);
    ui_gradient_bar(box, 0, 0, CONFIRM_W - 26, 3);
    lv_obj_t *msg = ui_label(box, &sol_font_14, COL_TEXT, text);
    lv_obj_set_width(msg, CONFIRM_W - 26);
    lv_label_set_long_mode(msg, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(msg, LV_TEXT_ALIGN_CENTER, 0);
    ui_label(box, &sol_mono_12, COL_DIM, "SELECT yes     CANCEL no");
    lv_obj_center(box);
}

static void confirm_posted(void *arg)
{
    confirm_req_t *r = arg;
    confirm_show(r->text, r->done, r->ctx);
    free(r);
}

void os_confirm(const char *text, os_confirm_cb_t done, void *ctx)
{
    text = text ? text : "";
    if (s_task && xTaskGetCurrentTaskHandle() == s_task) {
        confirm_show(text, done, ctx);
        return;
    }
    confirm_req_t *r = malloc(sizeof(*r) + strlen(text) + 1);
    if (r) {
        r->done = done;
        r->ctx = ctx;
        strcpy(r->text, text);
    }
    if (!r || !os_post(confirm_posted, r)) {
        free(r);
        ESP_LOGE(TAG, "confirm dropped: OS queue full");
    }
}

bool os_confirm_open(void)
{
    return s_modal != NULL;
}

void os_confirm_dismiss(os_confirm_cb_t done)
{
    if (s_modal && s_modal_cb == done) {
        confirm_close(false);
    }
}

// The dialog takes SELECT and CANCEL; everything else is swallowed. HOME
// answers no and goes home.
static bool confirm_key(os_key_t key, bool repeat)
{
    if (!s_modal) {
        return false;
    }
    if (!repeat && (key == OS_KEY_SELECT || key == OS_KEY_BACK || key == OS_KEY_HOME)) {
        confirm_close(key == OS_KEY_SELECT);
    }
    return key != OS_KEY_HOME;
}

// ---------------------------------------------------------------------------
// App switching (OS task, LVGL locked)

static void switch_to(const os_app_t *app)
{
    if (app == s_app) {
        return;
    }
    if (s_hold) {
        ESP_LOGW(TAG, "held: not switching to %s", app->id);
        return;
    }
    confirm_close(false);           // the dialog belongs to the app that is leaving
    if (s_app && s_app->on_exit) {
        s_app->on_exit();
    }
    lv_obj_clean(s_root);
    lv_obj_scroll_to(s_root, 0, 0, LV_ANIM_OFF);
    s_app = app;
    lv_label_set_text(s_title, app == &os_launcher ? "Solana OS" : app->name);
    ESP_LOGI(TAG, "app: %s", app->id);
    if (app->on_enter) {
        app->on_enter(s_root);
    }
}

static void dispatch(os_key_t key, bool repeat)
{
    if (confirm_key(key, repeat)) {
        return;
    }
    if (key == OS_KEY_HOME) {
        if (!repeat && s_hold) {
            os_toast("%s", s_hold);
        } else if (!repeat) {
            switch_to(&os_launcher);
        }
        return;
    }
    bool handled = s_app->on_key && s_app->on_key(key, repeat);
    if (!handled && key == OS_KEY_BACK && !repeat && s_app != &os_launcher) {
        switch_to(&os_launcher);
    }
}

static bool post(os_event_t ev)
{
    return s_q && xQueueSend(s_q, &ev, 0) == pdTRUE;
}

bool os_post(void (*fn)(void *arg), void *arg)
{
    return post((os_event_t){ .kind = EV_CALL, .fn = fn, .arg = arg });
}

void os_launch(const os_app_t *app)
{
    post((os_event_t){ .kind = EV_LAUNCH, .app = app });
}

void os_launch_id(const char *id)
{
    if (!strcmp(id, "home") || !strcmp(id, os_launcher.id)) {
        os_launch(&os_launcher);
        return;
    }
    if (!strcmp(id, os_calibrate.id)) {
        os_launch(&os_calibrate);
        return;
    }
    if (!strcmp(id, app_update.id)) {
        os_launch(&app_update);
        return;
    }
    for (int i = 0; i < os_app_count; i++) {
        if (!strcmp(os_apps[i]->id, id)) {
            os_launch(os_apps[i]);
            return;
        }
    }
    printf("no app '%s'\n", id);
}

void os_inject_key(os_key_t key)
{
    post((os_event_t){ .kind = EV_KEY, .a = key });
}

const os_app_t *os_current(void)
{
    return s_app;
}

static void on_buttons(uint32_t pressed, uint32_t changed)
{
    post((os_event_t){ .kind = EV_BUTTONS, .a = pressed, .b = changed });
}

// ---------------------------------------------------------------------------
// Screenshot: re-render the whole screen and print every flushed area as
// base64 RGB565 rows. tools/badge.py reassembles them into a PNG. The render
// happens on the LVGL task, like every frame: a deep object tree (the Guide's
// pack reveal) needs more stack than the OS task has.

static lv_display_flush_cb_t s_orig_flush;
static volatile bool s_capturing;

static void capture_flush(lv_display_t *disp, const lv_area_t *area, uint8_t *px)
{
    if (s_capturing) {
        int w = lv_area_get_width(area);
        static unsigned char b64[BOARD_LCD_H_RES * 2 * 4 / 3 + 8];
        for (int y = area->y1; y <= area->y2; y++) {
            size_t n = 0;
            mbedtls_base64_encode(b64, sizeof(b64), &n, px + (y - area->y1) * w * 2, w * 2);
            b64[n] = 0;
            printf("SHOT %d %d %d %s\n", (int)area->x1, y, w, b64);
        }
    }
    s_orig_flush(disp, area, px);
}

// LVGL task: the refresh that drew the invalidated screen is done.
static void capture_done(lv_event_t *e)
{
    if (!s_capturing) {
        return;
    }
    lv_display_t *disp = lv_display_get_default();
    s_capturing = false;
    disp->flush_cb = s_orig_flush;
    printf("SHOT-END\n");
    fflush(stdout);
    esp_log_level_set("*", ESP_LOG_INFO);
}

// OS task (LVGL locked): arm the capture; the LVGL task renders it next.
static void take_screenshot(void)
{
    if (s_capturing) {
        return;
    }
    lv_display_t *disp = lv_display_get_default();
    s_orig_flush = disp->flush_cb;
    disp->flush_cb = capture_flush;
    esp_log_level_set("*", ESP_LOG_NONE);
    printf("\nSHOT-BEGIN %d %d\n", BOARD_LCD_H_RES, BOARD_LCD_V_RES);
    s_capturing = true;
    lv_obj_invalidate(lv_screen_active());
    lv_obj_invalidate(lv_layer_top());
}

void os_screenshot(void)
{
    post((os_event_t){ .kind = EV_SHOT });
}

// ---------------------------------------------------------------------------

static void handle(const os_event_t *ev, os_key_t *held, int64_t *next_repeat, uint32_t *held_bit)
{
    switch (ev->kind) {
    case EV_BUTTONS: {
        uint32_t down = ev->a & ev->b;
        if (*held_bit && !(ev->a & *held_bit)) {
            *held = OS_KEY_NONE;
            *held_bit = 0;
        }
        for (uint32_t bit = 1; down; bit <<= 1) {
            if (!(down & bit)) {
                continue;
            }
            down &= ~bit;
            os_key_t k = os_key_from_button(bit);
            ESP_LOGI(TAG, "button 0x%02lx -> %s", (unsigned long)bit, os_key_name(k));
            if (s_app->on_button && k != OS_KEY_HOME && !s_modal) {
                s_app->on_button(bit);
                continue;
            }
            if (k == OS_KEY_NONE) {
                continue;
            }
            dispatch(k, false);
            if (k != OS_KEY_SELECT && k != OS_KEY_HOME) {
                *held = k;
                *held_bit = bit;
                *next_repeat = now_ms() + REPEAT_DELAY_MS;
            }
        }
        break;
    }
    case EV_KEY:
        dispatch(ev->a, false);
        break;
    case EV_LAUNCH:
        switch_to(ev->app);
        break;
    case EV_SHOT:
        take_screenshot();
        break;
    case EV_CALL:
        ev->fn(ev->arg);
        break;
    }
}

static void os_task(void *arg)
{
    os_key_t held = OS_KEY_NONE;
    uint32_t held_bit = 0;
    int64_t next_repeat = 0, next_tick = 0, next_status = 0;

    lvgl_port_lock(0);
    switch_to(&os_launcher);
    status_update();
    lvgl_port_unlock();

    for (;;) {
        int64_t now = now_ms();
        int64_t wake = next_tick;
        if (held != OS_KEY_NONE && next_repeat < wake) {
            wake = next_repeat;
        }
        os_event_t ev;
        bool got = xQueueReceive(s_q, &ev, wake > now ? pdMS_TO_TICKS(wake - now) : 0);

        lvgl_port_lock(0);
        if (got) {
            handle(&ev, &held, &next_repeat, &held_bit);
        }
        now = now_ms();
        if (held != OS_KEY_NONE && now >= next_repeat) {
            if (bsp_buttons_get() & held_bit) {
                dispatch(held, true);
                next_repeat = now + REPEAT_RATE_MS;
            } else {
                held = OS_KEY_NONE;
                held_bit = 0;
            }
        }
        if (now >= next_tick) {
            next_tick = now + OS_TICK_MS;
            if (s_app->on_tick) {
                s_app->on_tick();
            }
            if (s_toast_until && now >= s_toast_until) {
                lv_obj_set_hidden(s_toast, true);
                s_toast_until = 0;
            }
        }
        if (now >= next_status) {
            next_status = now + STATUS_EVERY_MS;
            status_update();
        }
        lvgl_port_unlock();
    }
}

void os_splash(void)
{
    lvgl_port_lock(0);
    lv_obj_t *scr = lv_screen_active();
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, ui_col(COL_BG), 0);
    lv_obj_t *logo = lv_image_create(scr);
    lv_image_set_src(logo, &img_logomark_72);
    lv_obj_align(logo, LV_ALIGN_CENTER, 0, -42);
    lv_obj_t *name = ui_label(scr, &sol_font_32, COL_TEXT, "Solana OS");
    lv_obj_align(name, LV_ALIGN_CENTER, 0, 28);
    lv_obj_t *ver = ui_label(scr, &sol_mono_12, COL_DIM, "");
    lv_label_set_text_fmt(ver, "v%s  " LV_SYMBOL_BULLET "  ESP32-C5", esp_app_get_description()->version);
    lv_obj_align(ver, LV_ALIGN_CENTER, 0, 58);
    ui_gradient_bar(scr, 60, 208, 200, 4);
    lvgl_port_unlock();
}

void os_start(void)
{
    s_q = xQueueCreate(32, sizeof(os_event_t));
    os_keys_load();

    lvgl_port_lock(0);
    lv_display_add_event_cb(lv_display_get_default(), capture_done, LV_EVENT_REFR_READY, NULL);
    lv_obj_t *scr = lv_screen_active();
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, ui_col(COL_BG), 0);
    lv_obj_set_scrollable(scr, false);
    status_create(scr);
    s_root = lv_obj_create(scr);
    lv_obj_remove_style_all(s_root);
    lv_obj_set_pos(s_root, 0, OS_STATUS_H);
    lv_obj_set_size(s_root, OS_ROOT_W, OS_ROOT_H);
    lv_obj_set_scrollable(s_root, false);
    toast_create();
    lvgl_port_unlock();

    if (bsp_buttons_start(on_buttons) != ESP_OK) {
        ESP_LOGE(TAG, "buttons unavailable; use the console 'key' command");
    }
    xTaskCreate(os_task, "os", 6144, NULL, 4, &s_task);
}

void os_hold(const char *why)
{
    s_hold = why;
}

// Settings changed the theme: rebuild everything that holds colours, then
// re-enter the current app so it draws itself in the new palette.
void os_theme_apply(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, ui_col(COL_BG), 0);
    lv_obj_delete(s_bar);
    status_create(scr);
    status_update();
    lv_obj_delete(s_toast);
    toast_create();
    if (s_app->on_exit) {
        s_app->on_exit();
    }
    lv_obj_clean(s_root);
    lv_label_set_text(s_title, s_app == &os_launcher ? "Solana OS" : s_app->name);
    if (s_app->on_enter) {
        s_app->on_enter(s_root);
    }
}
