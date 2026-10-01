#pragma once

// Shared widgets for apps. Everything here must be called with the LVGL lock
// held (i.e. from app callbacks).

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "lvgl.h"
#include "os_keys.h"
#include "theme.h"
#include "assets/assets.h"

// Theme colour: pass a COL_* value (the dark palette) and get the colour for
// the active theme (Settings: Dark or Light). Use it for every UI colour;
// only brand gradients and true colours (LED previews, colour checks) should
// use lv_color_hex directly.
lv_color_t ui_col(uint32_t c);
bool ui_light(void);

lv_obj_t *ui_label(lv_obj_t *parent, const lv_font_t *font, uint32_t color, const char *text);
lv_obj_t *ui_panel(lv_obj_t *parent, int x, int y, int w, int h);
lv_obj_t *ui_gradient_bar(lv_obj_t *parent, int x, int y, int w, int h);

// One-line key hint pinned to the bottom of an app root.
lv_obj_t *ui_hint(lv_obj_t *root, const char *text);
#define UI_HINT_H       16

// Tab strip across the top of an app root (Left/Right switches).
lv_obj_t *ui_tabs(lv_obj_t *root, const char *const *names, int count);
void ui_tabs_select(lv_obj_t *tabs, int index);
#define UI_TABS_H       22

// ---- Virtual list ----
// Only the visible rows exist as objects; the app supplies row contents on
// demand, so lists can hold any number of items.

typedef struct {
    char title[40];
    char sub[56];
    char right[20];
    uint32_t title_color;   // 0 = default
    uint32_t right_color;   // 0 = dim
} ui_row_t;

typedef void (*ui_row_cb_t)(int index, ui_row_t *row, void *ctx);
typedef struct ui_list ui_list_t;

ui_list_t *ui_list_create(lv_obj_t *parent, int y, int h, ui_row_cb_t cb, void *ctx);
void ui_list_set_count(ui_list_t *l, int count);   // re-renders
void ui_list_refresh(ui_list_t *l);
void ui_list_set_empty(ui_list_t *l, const char *text);
int ui_list_selected(const ui_list_t *l);           // -1 when empty
void ui_list_select(ui_list_t *l, int index);
bool ui_list_key(ui_list_t *l, os_key_t key, bool repeat);  // Up/Down
void ui_list_show(ui_list_t *l, bool show);

// ---- On-screen keyboard (driven by the d-pad) ----

typedef enum { UI_KB_BUSY, UI_KB_DONE, UI_KB_CANCEL } ui_kb_result_t;
typedef struct ui_kb ui_kb_t;

ui_kb_t *ui_kb_create(lv_obj_t *parent, const char *title, size_t max_len);
ui_kb_result_t ui_kb_key(ui_kb_t *kb, os_key_t key, bool repeat);
const char *ui_kb_text(const ui_kb_t *kb);
void ui_kb_delete(ui_kb_t *kb);

// ---- Amounts, categories, rarities (docs/HHGTS.md "One look") ----
// Same names and colours as the web. Amounts use sol_mono_12 or the
// Semi-Mono digits; pick the colour with ui_delta_color().

char *ui_fmt_num(char *out, size_t n, long v);          // "1,250"
char *ui_fmt_lamps(char *out, size_t n, long amount);   // "1,250 !LAMPs"
char *ui_fmt_delta(char *out, size_t n, long delta);    // "+50", "-100"
uint32_t ui_delta_color(long delta);                    // gains COL_LAMPS, spends COL_DIM
char *ui_fmt_ago(char *out, size_t n, int64_t ms);      // "just now", "5 min ago", "2 h ago", "3 d ago"

// Category ids ("tech") and rarities ("legendary") from the API. Unknown
// names get COL_DIM / the id itself. Legendary also wants the gradient
// (ui_gradient_bar) where a bar is drawn.
uint32_t ui_cat_color(const char *id);
const char *ui_cat_name(const char *id);                // "Tech docs"
uint32_t ui_rarity_color(const char *rarity);

// ---- Offline panel ----
// For apps that need the server (os_api). Create it over the content area;
// call ui_offline_update() from on_tick (it redraws only on a change and
// hides itself while the badge is online) and ui_offline_key() first in
// on_key. Texts, as on the web:
//   "Wi-Fi is off"          SELECT to connect to <ssid>       (turns Wi-Fi on)
//   "No network saved"      SELECT to open Wi-Fi
//   "Connecting to <ssid>"  / "Can't join <ssid>" SELECT to try again
//   "Wi-Fi is paused"       while a mesh app holds the radio
//   "Not linked yet"        claim this badge first; SELECT to open My Badge (need_owner)

typedef enum {
    UI_NET_OK,              // os_api_online() (and claimed, if need_owner)
    UI_NET_NO_SERVER,       // build without a server and none set
    UI_NET_NO_NETWORK,      // no saved network
    UI_NET_WIFI_OFF,        // saved network, Wi-Fi off
    UI_NET_JOINING,
    UI_NET_FAILED,
    UI_NET_PAUSED,          // radio lent to a mesh app or the ESP-NOW scanner
    UI_NET_REGISTERING,     // joined; first hello with the server not done yet
    UI_NET_NOT_LINKED,      // need_owner and nobody claimed this badge
} ui_net_t;

ui_net_t ui_net_state(bool need_owner);
lv_obj_t *ui_offline_create(lv_obj_t *parent, int y, int h, bool need_owner);
ui_net_t ui_offline_update(lv_obj_t *panel);
// SELECT while the panel shows: turn Wi-Fi on, retry, or open Wi-Fi / My
// Badge. Returns true if it used the key (only SELECT, only while it shows).
bool ui_offline_key(lv_obj_t *panel, os_key_t key);
