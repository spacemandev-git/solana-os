// Settings: user preferences (core/os_settings) plus shortcuts to system
// tools. One row per item; SELECT (or LEFT/RIGHT on a choice) changes it.

#include <string.h>
#include "core/os.h"
#include "core/os_settings.h"
#include "core/ui.h"

enum { ROW_THEME, ROW_CALIB, ROW_COUNT };

static ui_list_t *s_list;

static void row(int i, ui_row_t *r, void *ctx)
{
    switch (i) {
    case ROW_THEME: {
        bool light = ui_light();
        strlcpy(r->title, "Theme", sizeof(r->title));
        strlcpy(r->sub, light ? "Black on white, QR code included" : "White on black, QR code included",
                sizeof(r->sub));
        strlcpy(r->right, light ? "Light" : "Dark", sizeof(r->right));
        r->right_color = COL_TEXT;
        break;
    }
    case ROW_CALIB:
        strlcpy(r->title, "Calibrate buttons", sizeof(r->title));
        strlcpy(r->sub, "Map the front buttons again", sizeof(r->sub));
        strlcpy(r->right, LV_SYMBOL_RIGHT, sizeof(r->right));
        break;
    }
}

static void enter(lv_obj_t *root)
{
    s_list = ui_list_create(root, 4, OS_ROOT_H - 4 - UI_HINT_H, row, NULL);
    ui_list_set_count(s_list, ROW_COUNT);
    ui_hint(root, LV_SYMBOL_UP LV_SYMBOL_DOWN " pick  SELECT change  CANCEL exit");
}

static bool on_key(os_key_t k, bool repeat)
{
    if (ui_list_key(s_list, k, repeat)) {
        return true;
    }
    int i = ui_list_selected(s_list);
    bool change = k == OS_KEY_SELECT || ((k == OS_KEY_LEFT || k == OS_KEY_RIGHT) && i == ROW_THEME);
    if (!change || repeat) {
        return k != OS_KEY_BACK;
    }
    switch (i) {
    case ROW_THEME:
        os_setting_set(OS_SET_THEME, ui_light() ? OS_THEME_DARK : OS_THEME_LIGHT);
        os_theme_apply();       // rebuilds this app too; s_list is new afterwards
        break;
    case ROW_CALIB:
        os_launch(&os_calibrate);
        break;
    }
    return true;
}

const os_app_t app_settings = {
    .id = "settings",
    .name = "Settings",
    .symbol = LV_SYMBOL_LIST,
    .accent = COL_BLUE,
    .on_enter = enter,
    .on_key = on_key,
};
