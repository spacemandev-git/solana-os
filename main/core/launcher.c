// App browser: paged 3x2 grid of every registered app.

#include "os.h"
#include "ui.h"

#define COLS        3
#define ROWS        2
#define PER_PAGE    (COLS * ROWS)
#define TILE_W      96
#define TILE_H      80
#define GAP         8
#define X0          ((OS_ROOT_W - COLS * TILE_W - (COLS - 1) * GAP) / 2)
#define Y0          8

static lv_obj_t *s_root, *s_tiles[PER_PAGE], *s_icons[PER_PAGE], *s_names[PER_PAGE], *s_dots, *s_hint;
static int s_sel, s_page = -1;

static void style_tile(int slot, const os_app_t *app, bool focused)
{
    lv_obj_t *t = s_tiles[slot];
    // The focused tile is solid Solana Green, like the web's primary buttons.
    lv_obj_set_style_bg_color(t, ui_col(focused ? COL_GREEN : COL_PANEL), 0);
    lv_obj_set_style_border_width(t, focused ? 0 : 1, 0);
    lv_obj_set_style_text_color(s_names[slot], ui_col(focused ? COL_BG : COL_TEXT), 0);
    if (app->symbol) {
        lv_obj_set_style_text_color(s_icons[slot], ui_col(focused ? COL_BG : app->accent), 0);
    } else {
        // A colour logo on the green tile is low contrast; show it as the
        // brand's solid black variant instead.
        lv_obj_set_style_image_recolor(s_icons[slot], ui_col(COL_BG), 0);
        lv_obj_set_style_image_recolor_opa(s_icons[slot], focused ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    }
}

static void render(void)
{
    int page = s_sel / PER_PAGE;
    if (page != s_page) {
        s_page = page;
        for (int i = 0; i < PER_PAGE; i++) {
            int idx = page * PER_PAGE + i;
            lv_obj_t *t = s_tiles[i];
            if (idx >= os_app_count) {
                lv_obj_set_hidden(t, true);
                continue;
            }
            const os_app_t *app = os_apps[idx];
            lv_obj_set_hidden(t, false);
            lv_label_set_text(s_names[i], app->name);
            // Icons are either a symbol label or an image; swap the widget as needed.
            if (s_icons[i]) {
                lv_obj_delete(s_icons[i]);
            }
            if (app->symbol) {
                s_icons[i] = ui_label(t, &lv_font_montserrat_20, app->accent, app->symbol);
            } else {
                s_icons[i] = lv_image_create(t);
                lv_image_set_src(s_icons[i], app->icon);
            }
            lv_obj_align(s_icons[i], LV_ALIGN_CENTER, 0, -12);
        }
        int pages = (os_app_count + PER_PAGE - 1) / PER_PAGE;
        lv_obj_clean(s_dots);
        lv_obj_set_hidden(s_dots, pages < 2);
        for (int p = 0; p < pages; p++) {
            lv_obj_t *d = lv_obj_create(s_dots);
            lv_obj_remove_style_all(d);
            lv_obj_set_size(d, p == page ? 16 : 6, 6);
            lv_obj_set_style_radius(d, 3, 0);
            lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
            lv_obj_set_style_bg_color(d, ui_col(p == page ? COL_GREEN : COL_BORDER), 0);
        }
    }
    for (int i = 0; i < PER_PAGE; i++) {
        int idx = page * PER_PAGE + i;
        if (idx < os_app_count) {
            style_tile(i, os_apps[idx], idx == s_sel);
        }
    }
}

static void enter(lv_obj_t *root)
{
    s_root = root;
    s_page = -1;
    if (s_sel >= os_app_count) {
        s_sel = 0;
    }
    for (int i = 0; i < PER_PAGE; i++) {
        lv_obj_t *t = ui_panel(root, X0 + (i % COLS) * (TILE_W + GAP), Y0 + (i / COLS) * (TILE_H + GAP), TILE_W, TILE_H);
        lv_obj_set_style_radius(t, 10, 0);
        s_tiles[i] = t;
        s_icons[i] = NULL;
        s_names[i] = ui_label(t, &sol_font_14, COL_TEXT, "");
        lv_obj_set_width(s_names[i], TILE_W - 8);
        lv_obj_set_style_text_align(s_names[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_long_mode(s_names[i], LV_LABEL_LONG_MODE_DOTS);
        lv_obj_align(s_names[i], LV_ALIGN_BOTTOM_MID, 0, -2);
    }
    s_dots = lv_obj_create(root);
    lv_obj_remove_style_all(s_dots);
    lv_obj_set_size(s_dots, 120, 8);
    lv_obj_align(s_dots, LV_ALIGN_BOTTOM_MID, 0, -UI_HINT_H - 6);
    lv_obj_set_flex_flow(s_dots, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_dots, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(s_dots, 4, 0);
    s_hint = ui_hint(root, LV_SYMBOL_UP LV_SYMBOL_DOWN LV_SYMBOL_LEFT LV_SYMBOL_RIGHT " move   SELECT open   BOOT home");
    render();
}

static bool key(os_key_t k, bool repeat)
{
    int n = s_sel;
    switch (k) {
    case OS_KEY_LEFT:  n--; break;
    case OS_KEY_RIGHT: n++; break;
    case OS_KEY_UP:    n -= COLS; break;
    case OS_KEY_DOWN:  n += COLS; break;
    case OS_KEY_SELECT:
        os_launch(os_apps[s_sel]);
        return true;
    default:
        return true;    // Back on the home screen does nothing
    }
    if (n < 0) {
        n = repeat ? 0 : (k == OS_KEY_LEFT ? os_app_count - 1 : s_sel);
    } else if (n >= os_app_count) {
        n = repeat ? os_app_count - 1 : (k == OS_KEY_RIGHT ? 0 : os_app_count - 1);
    }
    s_sel = n;
    render();
    return true;
}

const os_app_t os_launcher = {
    .id = "launcher",
    .name = "Apps",
    .on_enter = enter,
    .on_key = key,
};
