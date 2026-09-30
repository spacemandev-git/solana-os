// Shared widgets: labels, panels, hint bar, tabs, virtual list, keyboard.

#include <stdio.h>
#include <string.h>
#include "ui.h"
#include "os_settings.h"

bool ui_light(void)
{
    return os_setting(OS_SET_THEME) == OS_THEME_LIGHT;
}

// Light theme: surfaces and text swap, and the lighter brand accents get
// deeper shades so they stay legible on white. Purple and the gradients are
// unchanged.
lv_color_t ui_col(uint32_t c)
{
    if (ui_light()) {
        switch (c) {
        case COL_BG:        c = 0xFFFFFF; break;
        case COL_PANEL:     c = 0xF4F3F8; break;
        case COL_PANEL_HI:  c = 0xE8E6F0; break;
        case COL_BORDER:    c = 0xD5D2E0; break;
        case COL_TEXT:      c = 0x000000; break;
        case COL_DIM:       c = 0x6B6880; break;
        case COL_GREEN:     c = 0x0A9E62; break;
        case COL_YELLOW:    c = 0x8F8A00; break;
        case COL_AQUA:      c = 0x0089A8; break;
        case COL_LIME:      c = 0x3A9A28; break;
        case COL_LPURPLE:   c = 0x6B5CF0; break;
        case COL_BLUE:      c = 0x2A76D2; break;
        case COL_ORANGE:    c = 0xE04E1E; break;
        case COL_PINK:      c = 0xC8319A; break;
        default:            break;
        }
    }
    return lv_color_hex(c);
}

lv_obj_t *ui_label(lv_obj_t *parent, const lv_font_t *font, uint32_t color, const char *text)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, ui_col(color), 0);
    lv_label_set_text(l, text ? text : "");
    return l;
}

lv_obj_t *ui_panel(lv_obj_t *parent, int x, int y, int w, int h)
{
    lv_obj_t *p = lv_obj_create(parent);
    lv_obj_remove_style_all(p);
    lv_obj_set_pos(p, x, y);
    lv_obj_set_size(p, w, h);
    lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(p, ui_col(COL_PANEL), 0);
    lv_obj_set_style_border_width(p, 1, 0);
    lv_obj_set_style_border_color(p, ui_col(COL_BORDER), 0);
    lv_obj_set_style_radius(p, 6, 0);
    lv_obj_set_style_pad_all(p, 6, 0);
    lv_obj_set_scrollable(p, false);
    return p;
}

lv_obj_t *ui_gradient_bar(lv_obj_t *parent, int x, int y, int w, int h)
{
    lv_obj_t *b = lv_obj_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, ui_col(COL_PURPLE), 0);
    lv_obj_set_style_bg_grad_color(b, lv_color_hex(COL_GREEN), 0);
    lv_obj_set_style_bg_grad_dir(b, LV_GRAD_DIR_HOR, 0);
    lv_obj_set_style_radius(b, h / 2, 0);
    return b;
}

lv_obj_t *ui_hint(lv_obj_t *root, const char *text)
{
    lv_obj_t *bar = lv_obj_create(root);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, OS_ROOT_W, UI_HINT_H);
    lv_obj_align(bar, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(bar, ui_col(COL_PANEL), 0);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_width(bar, 1, 0);
    lv_obj_set_style_border_color(bar, ui_col(COL_BORDER), 0);
    lv_obj_t *l = ui_label(bar, &sol_mono_12, COL_DIM, text);
    lv_obj_align(l, LV_ALIGN_LEFT_MID, 6, 0);
    return l;
}

// ---------------------------------------------------------------------------
// Tabs

lv_obj_t *ui_tabs(lv_obj_t *root, const char *const *names, int count)
{
    lv_obj_t *t = lv_obj_create(root);
    lv_obj_remove_style_all(t);
    lv_obj_set_size(t, OS_ROOT_W, UI_TABS_H);
    lv_obj_set_style_border_side(t, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(t, 1, 0);
    lv_obj_set_style_border_color(t, ui_col(COL_BORDER), 0);
    lv_obj_set_flex_flow(t, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_left(t, 4, 0);
    lv_obj_set_style_pad_column(t, 2, 0);
    for (int i = 0; i < count; i++) {
        lv_obj_t *l = ui_label(t, &sol_mono_12, COL_DIM, names[i]);
        lv_obj_set_height(l, UI_TABS_H);
        lv_obj_set_style_pad_hor(l, 6, 0);
        lv_obj_set_style_pad_top(l, 4, 0);
        lv_obj_set_style_border_side(l, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_color(l, ui_col(COL_GREEN), 0);
    }
    ui_tabs_select(t, 0);
    return t;
}

void ui_tabs_select(lv_obj_t *tabs, int index)
{
    for (uint32_t i = 0; i < lv_obj_get_child_count(tabs); i++) {
        lv_obj_t *l = lv_obj_get_child(tabs, i);
        bool on = (int)i == index;
        lv_obj_set_style_text_color(l, ui_col(on ? COL_TEXT : COL_DIM), 0);
        lv_obj_set_style_border_width(l, on ? 2 : 0, 0);
    }
}

// ---------------------------------------------------------------------------
// Virtual list

#define ROW_H       36
#define MAX_ROWS    8

typedef struct {
    lv_obj_t *obj, *mark, *title, *sub, *right;
} row_objs_t;

struct ui_list {
    lv_obj_t *box, *empty, *scroll;
    row_objs_t rows[MAX_ROWS];
    int visible, count, sel, top;
    ui_row_cb_t cb;
    void *ctx;
};

static void list_deleted(lv_event_t *e)
{
    lv_free(lv_event_get_user_data(e));
}

ui_list_t *ui_list_create(lv_obj_t *parent, int y, int h, ui_row_cb_t cb, void *ctx)
{
    ui_list_t *l = lv_malloc_zeroed(sizeof(*l));
    l->cb = cb;
    l->ctx = ctx;
    l->visible = h / ROW_H;
    if (l->visible > MAX_ROWS) {
        l->visible = MAX_ROWS;
    }
    l->box = lv_obj_create(parent);
    lv_obj_remove_style_all(l->box);
    lv_obj_set_pos(l->box, 0, y);
    lv_obj_set_size(l->box, OS_ROOT_W, h);
    lv_obj_set_scrollable(l->box, false);
    lv_obj_add_event_cb(l->box, list_deleted, LV_EVENT_DELETE, l);

    for (int i = 0; i < l->visible; i++) {
        row_objs_t *r = &l->rows[i];
        r->obj = lv_obj_create(l->box);
        lv_obj_remove_style_all(r->obj);
        lv_obj_set_pos(r->obj, 0, i * ROW_H);
        lv_obj_set_size(r->obj, OS_ROOT_W - 6, ROW_H);
        lv_obj_set_style_bg_color(r->obj, ui_col(COL_PANEL_HI), 0);
        lv_obj_set_style_border_side(r->obj, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_width(r->obj, 1, 0);
        lv_obj_set_style_border_color(r->obj, ui_col(COL_PANEL), 0);
        r->mark = lv_obj_create(r->obj);
        lv_obj_remove_style_all(r->mark);
        lv_obj_set_size(r->mark, 3, ROW_H - 8);
        lv_obj_align(r->mark, LV_ALIGN_LEFT_MID, 0, 0);
        lv_obj_set_style_bg_opa(r->mark, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(r->mark, ui_col(COL_PURPLE), 0);
        lv_obj_set_style_bg_grad_color(r->mark, lv_color_hex(COL_GREEN), 0);
        lv_obj_set_style_bg_grad_dir(r->mark, LV_GRAD_DIR_VER, 0);
        r->title = ui_label(r->obj, &sol_font_14, COL_TEXT, "");
        lv_obj_set_pos(r->title, 10, 2);
        lv_obj_set_width(r->title, 220);
        lv_label_set_long_mode(r->title, LV_LABEL_LONG_MODE_DOTS);
        r->sub = ui_label(r->obj, &sol_mono_12, COL_DIM, "");
        lv_obj_set_pos(r->sub, 10, 19);
        lv_obj_set_width(r->sub, 300);
        lv_label_set_long_mode(r->sub, LV_LABEL_LONG_MODE_DOTS);
        r->right = ui_label(r->obj, &sol_mono_12, COL_DIM, "");
        lv_obj_align(r->right, LV_ALIGN_TOP_RIGHT, -4, 4);
    }

    l->scroll = lv_obj_create(l->box);
    lv_obj_remove_style_all(l->scroll);
    lv_obj_set_style_bg_opa(l->scroll, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(l->scroll, ui_col(COL_PURPLE), 0);
    lv_obj_set_style_radius(l->scroll, 1, 0);

    l->empty = ui_label(l->box, &sol_font_14, COL_DIM, "Nothing here yet");
    lv_obj_set_width(l->empty, OS_ROOT_W - 32);
    lv_obj_set_style_text_align(l->empty, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(l->empty, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_align(l->empty, LV_ALIGN_CENTER, 0, 0);
    ui_list_refresh(l);
    return l;
}

void ui_list_refresh(ui_list_t *l)
{
    if (l->sel >= l->count) {
        l->sel = l->count - 1;
    }
    if (l->sel < 0 && l->count) {
        l->sel = 0;
    }
    if (l->sel < l->top) {
        l->top = l->sel;
    } else if (l->sel >= l->top + l->visible) {
        l->top = l->sel - l->visible + 1;
    }
    if (l->top > l->count - l->visible) {
        l->top = l->count - l->visible;
    }
    if (l->top < 0) {
        l->top = 0;
    }

    for (int i = 0; i < l->visible; i++) {
        row_objs_t *r = &l->rows[i];
        int idx = l->top + i;
        if (idx >= l->count) {
            lv_obj_set_hidden(r->obj, true);
            continue;
        }
        ui_row_t row = {0};
        l->cb(idx, &row, l->ctx);
        lv_obj_set_hidden(r->obj, false);
        bool sel = idx == l->sel;
        lv_obj_set_style_bg_opa(r->obj, sel ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_hidden(r->mark, !sel);
        lv_label_set_text(r->title, row.title);
        lv_obj_set_style_text_color(r->title, ui_col(row.title_color ? row.title_color : COL_TEXT), 0);
        lv_label_set_text(r->sub, row.sub);
        lv_label_set_text(r->right, row.right);
        lv_obj_set_style_text_color(r->right, ui_col(row.right_color ? row.right_color : COL_DIM), 0);
        // Keep the title clear of the right-hand label.
        lv_obj_set_width(r->title, OS_ROOT_W - 26 - (row.right[0] ? lv_obj_get_width(r->right) + 8 : 0));
    }

    lv_obj_set_hidden(l->empty, l->count > 0);
    bool scroll = l->count > l->visible;
    lv_obj_set_hidden(l->scroll, !scroll);
    if (scroll) {
        int track = l->visible * ROW_H;
        int len = track * l->visible / l->count;
        if (len < 10) {
            len = 10;
        }
        lv_obj_set_size(l->scroll, 3, len);
        lv_obj_set_pos(l->scroll, OS_ROOT_W - 4, (track - len) * l->top / (l->count - l->visible));
    }
}

void ui_list_set_count(ui_list_t *l, int count)
{
    l->count = count;
    ui_list_refresh(l);
}

void ui_list_set_empty(ui_list_t *l, const char *text)
{
    lv_label_set_text(l->empty, text);
}

int ui_list_selected(const ui_list_t *l)
{
    return l->count ? l->sel : -1;
}

void ui_list_select(ui_list_t *l, int index)
{
    l->sel = index;
    ui_list_refresh(l);
}

bool ui_list_key(ui_list_t *l, os_key_t key, bool repeat)
{
    if (!l->count || (key != OS_KEY_UP && key != OS_KEY_DOWN)) {
        return false;
    }
    int n = l->sel + (key == OS_KEY_DOWN ? 1 : -1);
    if (n < 0) {
        n = repeat ? 0 : l->count - 1;          // wrap on a fresh press only
    } else if (n >= l->count) {
        n = repeat ? l->count - 1 : 0;
    }
    l->sel = n;
    ui_list_refresh(l);
    return true;
}

void ui_list_show(ui_list_t *l, bool show)
{
    lv_obj_set_hidden(l->box, !show);
}

// ---------------------------------------------------------------------------
// Keyboard: four 10-key character rows plus a 5-key function row.

#define KB_COLS     10
#define KB_ROWS     4
#define KB_FN_ROW   KB_ROWS
#define KB_FN_COUNT 5
#define KB_MAX      64

enum { FN_CASE, FN_MODE, FN_SPACE, FN_DEL, FN_OK };

static const char *const kb_sets[3][KB_ROWS] = {
    {"1234567890", "qwertyuiop", "asdfghjkl@", "zxcvbnm,.-"},
    {"1234567890", "QWERTYUIOP", "ASDFGHJKL@", "ZXCVBNM,.-"},
    {"1234567890", "!@#$%^&*()", "-_=+[]{}\\|", ";:'\"<>/?`~"},
};

struct ui_kb {
    lv_obj_t *box, *text, *btnm;
    char key_text[KB_ROWS * KB_COLS][2];
    const char *map[KB_ROWS * (KB_COLS + 1) + KB_FN_COUNT + 1];
    int mode, row, col;
    size_t max_len;
    char buf[KB_MAX + 1];
};

static void kb_build_map(ui_kb_t *kb)
{
    int m = 0;
    for (int r = 0; r < KB_ROWS; r++) {
        for (int c = 0; c < KB_COLS; c++) {
            char *k = kb->key_text[r * KB_COLS + c];
            k[0] = kb_sets[kb->mode][r][c];
            k[1] = 0;
            kb->map[m++] = k;
        }
        kb->map[m++] = "\n";
    }
    kb->map[m++] = kb->mode == 1 ? "abc" : "ABC";
    kb->map[m++] = kb->mode == 2 ? "abc" : "#+=";
    kb->map[m++] = "space";
    kb->map[m++] = LV_SYMBOL_BACKSPACE;
    kb->map[m++] = LV_SYMBOL_OK;
    kb->map[m] = "";
    lv_buttonmatrix_set_map(kb->btnm, kb->map);
}

static void kb_render(ui_kb_t *kb)
{
    int id = kb->row == KB_FN_ROW ? KB_ROWS * KB_COLS + kb->col : kb->row * KB_COLS + kb->col;
    lv_buttonmatrix_set_selected_button(kb->btnm, id);
    lv_label_set_text_fmt(kb->text, "%s_", kb->buf);
}

static void kb_deleted(lv_event_t *e)
{
    lv_free(lv_event_get_user_data(e));
}

ui_kb_t *ui_kb_create(lv_obj_t *parent, const char *title, size_t max_len)
{
    ui_kb_t *kb = lv_malloc_zeroed(sizeof(*kb));
    kb->max_len = max_len < KB_MAX ? max_len : KB_MAX;
    kb->row = 1;

    kb->box = lv_obj_create(parent);
    lv_obj_remove_style_all(kb->box);
    lv_obj_set_size(kb->box, OS_ROOT_W, OS_ROOT_H);
    lv_obj_set_style_bg_opa(kb->box, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(kb->box, ui_col(COL_BG), 0);
    lv_obj_set_scrollable(kb->box, false);
    lv_obj_add_event_cb(kb->box, kb_deleted, LV_EVENT_DELETE, kb);

    lv_obj_t *t = ui_label(kb->box, &sol_mono_12, COL_DIM, title);
    lv_obj_set_pos(t, 8, 3);
    lv_obj_t *field = ui_panel(kb->box, 6, 19, OS_ROOT_W - 12, 28);
    lv_obj_set_style_border_color(field, ui_col(COL_PURPLE), 0);
    lv_obj_set_style_pad_ver(field, 4, 0);
    kb->text = ui_label(field, &sol_font_14, COL_TEXT, "");
    lv_obj_set_width(kb->text, OS_ROOT_W - 26);
    lv_label_set_long_mode(kb->text, LV_LABEL_LONG_MODE_CLIP);

    kb->btnm = lv_buttonmatrix_create(kb->box);
    lv_obj_set_pos(kb->btnm, 2, 50);
    lv_obj_set_size(kb->btnm, OS_ROOT_W - 4, OS_ROOT_H - 50 - UI_HINT_H);
    lv_obj_set_style_bg_opa(kb->btnm, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(kb->btnm, 0, 0);
    lv_obj_set_style_pad_all(kb->btnm, 2, 0);
    lv_obj_set_style_pad_gap(kb->btnm, 3, 0);
    lv_obj_set_style_radius(kb->btnm, 0, 0);
    lv_obj_set_style_bg_color(kb->btnm, ui_col(COL_PANEL_HI), LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(kb->btnm, LV_OPA_COVER, LV_PART_ITEMS);
    lv_obj_set_style_text_color(kb->btnm, ui_col(COL_TEXT), LV_PART_ITEMS);
    lv_obj_set_style_text_font(kb->btnm, &sol_mono_12, LV_PART_ITEMS);
    lv_obj_set_style_radius(kb->btnm, 4, LV_PART_ITEMS);
    lv_obj_set_style_shadow_width(kb->btnm, 0, LV_PART_ITEMS);
    lv_obj_set_style_border_width(kb->btnm, 0, LV_PART_ITEMS);
    lv_obj_set_style_outline_width(kb->btnm, 0, LV_PART_ITEMS | LV_STATE_FOCUS_KEY);
    lv_obj_set_style_bg_color(kb->btnm, ui_col(COL_GREEN), LV_PART_ITEMS | LV_STATE_FOCUSED);
    lv_obj_set_style_text_color(kb->btnm, ui_col(COL_BG), LV_PART_ITEMS | LV_STATE_FOCUSED);
    lv_obj_add_state(kb->btnm, LV_STATE_FOCUSED);
    kb_build_map(kb);

    lv_obj_t *hint = ui_hint(kb->box, "SELECT type   CANCEL delete / close");
    (void)hint;
    kb_render(kb);
    return kb;
}

static void kb_type(ui_kb_t *kb, char c)
{
    size_t n = strlen(kb->buf);
    if (n < kb->max_len) {
        kb->buf[n] = c;
        kb->buf[n + 1] = 0;
    }
}

ui_kb_result_t ui_kb_key(ui_kb_t *kb, os_key_t key, bool repeat)
{
    int cols = kb->row == KB_FN_ROW ? KB_FN_COUNT : KB_COLS;
    switch (key) {
    case OS_KEY_LEFT:
        kb->col = (kb->col + cols - 1) % cols;
        break;
    case OS_KEY_RIGHT:
        kb->col = (kb->col + 1) % cols;
        break;
    case OS_KEY_UP:
        if (kb->row == KB_FN_ROW) {
            kb->col *= 2;
        }
        kb->row = (kb->row + KB_ROWS) % (KB_ROWS + 1);
        if (kb->row == KB_FN_ROW) {
            kb->col /= 2;
        }
        break;
    case OS_KEY_DOWN:
        if (kb->row == KB_FN_ROW) {
            kb->col *= 2;
        }
        kb->row = (kb->row + 1) % (KB_ROWS + 1);
        if (kb->row == KB_FN_ROW) {
            kb->col /= 2;
        }
        break;
    case OS_KEY_BACK: {
        size_t n = strlen(kb->buf);
        if (!n) {
            return repeat ? UI_KB_BUSY : UI_KB_CANCEL;
        }
        kb->buf[n - 1] = 0;
        break;
    }
    case OS_KEY_SELECT:
        if (kb->row < KB_ROWS) {
            kb_type(kb, kb_sets[kb->mode][kb->row][kb->col]);
            break;
        }
        switch (kb->col) {
        case FN_CASE:
            kb->mode = kb->mode == 0 ? 1 : 0;
            kb_build_map(kb);
            break;
        case FN_MODE:
            kb->mode = kb->mode == 2 ? 0 : 2;
            kb_build_map(kb);
            break;
        case FN_SPACE:
            kb_type(kb, ' ');
            break;
        case FN_DEL:
            if (kb->buf[0]) {
                kb->buf[strlen(kb->buf) - 1] = 0;
            }
            break;
        case FN_OK:
            return UI_KB_DONE;
        }
        break;
    default:
        return UI_KB_BUSY;
    }
    kb_render(kb);
    return UI_KB_BUSY;
}

const char *ui_kb_text(const ui_kb_t *kb)
{
    return kb->buf;
}

void ui_kb_delete(ui_kb_t *kb)
{
    lv_obj_delete(kb->box);     // frees kb via kb_deleted
}
