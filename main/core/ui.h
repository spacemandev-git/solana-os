#pragma once

// Shared widgets for apps. Everything here must be called with the LVGL lock
// held (i.e. from app callbacks).

#include <stdbool.h>
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
