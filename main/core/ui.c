// Shared widgets: labels, panels, hint bar, tabs, virtual list, keyboard.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_random.h"
#include "ui.h"
#include "os.h"
#include "os_api.h"
#include "os_cloud.h"
#include "os_settings.h"
#include "os_wifi.h"

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
    int w, visible, count, sel, top;
    ui_row_cb_t cb;
    void *ctx;
};

static void list_deleted(lv_event_t *e)
{
    lv_free(lv_event_get_user_data(e));
}

ui_list_t *ui_list_create(lv_obj_t *parent, int y, int h, ui_row_cb_t cb, void *ctx)
{
    return ui_list_create_at(parent, 0, y, OS_ROOT_W, h, cb, ctx);
}

ui_list_t *ui_list_create_at(lv_obj_t *parent, int x, int y, int w, int h, ui_row_cb_t cb, void *ctx)
{
    ui_list_t *l = lv_malloc_zeroed(sizeof(*l));
    l->cb = cb;
    l->ctx = ctx;
    l->w = w;
    l->visible = h / ROW_H;
    if (l->visible > MAX_ROWS) {
        l->visible = MAX_ROWS;
    }
    l->box = lv_obj_create(parent);
    lv_obj_remove_style_all(l->box);
    lv_obj_set_pos(l->box, x, y);
    lv_obj_set_size(l->box, w, h);
    lv_obj_set_scrollable(l->box, false);
    lv_obj_add_event_cb(l->box, list_deleted, LV_EVENT_DELETE, l);

    for (int i = 0; i < l->visible; i++) {
        row_objs_t *r = &l->rows[i];
        r->obj = lv_obj_create(l->box);
        lv_obj_remove_style_all(r->obj);
        lv_obj_set_pos(r->obj, 0, i * ROW_H);
        lv_obj_set_size(r->obj, w - 6, ROW_H);
        lv_obj_set_style_bg_color(r->obj, ui_col(COL_PANEL_HI), 0);
        lv_obj_set_style_border_side(r->obj, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_width(r->obj, 1, 0);
        lv_obj_set_style_border_color(r->obj, ui_col(COL_PANEL), 0);
        r->mark = lv_obj_create(r->obj);
        lv_obj_remove_style_all(r->mark);
        lv_obj_set_size(r->mark, 3, ROW_H - 8);
        lv_obj_align(r->mark, LV_ALIGN_LEFT_MID, 0, 0);
        lv_obj_set_style_bg_opa(r->mark, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(r->mark, ui_col(COL_GREEN), 0);
        // One line each: a fixed height makes long text end in "..." instead of wrapping.
        r->title = ui_label(r->obj, &sol_font_14, COL_TEXT, "");
        lv_obj_set_pos(r->title, 10, 2);
        lv_obj_set_size(r->title, w - 100, lv_font_get_line_height(&sol_font_14));
        lv_label_set_long_mode(r->title, LV_LABEL_LONG_MODE_DOTS);
        r->sub = ui_label(r->obj, &sol_mono_12, COL_DIM, "");
        lv_obj_set_pos(r->sub, 10, 19);
        lv_obj_set_size(r->sub, w - 20, lv_font_get_line_height(&sol_mono_12));
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
    lv_obj_set_width(l->empty, w - 32);
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
        lv_obj_set_width(r->title, l->w - 26 - (row.right[0] ? lv_obj_get_width(r->right) + 8 : 0));
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
        lv_obj_set_pos(l->scroll, l->w - 4, (track - len) * l->top / (l->count - l->visible));
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

// ---------------------------------------------------------------------------
// Amounts, categories, rarities

char *ui_fmt_num(char *out, size_t n, long v)
{
    char digits[24];
    unsigned long u = v < 0 ? -(unsigned long)v : (unsigned long)v;
    int len = snprintf(digits, sizeof(digits), "%lu", u);
    size_t j = 0;
    if (v < 0 && j + 1 < n) {
        out[j++] = '-';
    }
    for (int i = 0; i < len && j + 1 < n; i++) {
        if (i && (len - i) % 3 == 0 && j + 1 < n) {
            out[j++] = ',';
        }
        if (j + 1 < n) {
            out[j++] = digits[i];
        }
    }
    if (n) {
        out[j < n ? j : n - 1] = 0;
    }
    return out;
}

char *ui_fmt_lamps(char *out, size_t n, long amount)
{
    char num[24];
    snprintf(out, n, "%s !LAMPs", ui_fmt_num(num, sizeof(num), amount));
    return out;
}

char *ui_fmt_delta(char *out, size_t n, long delta)
{
    char num[24];
    snprintf(out, n, "%s%s", delta > 0 ? "+" : "", ui_fmt_num(num, sizeof(num), delta));
    return out;
}

uint32_t ui_delta_color(long delta)
{
    return delta > 0 ? COL_LAMPS : COL_DIM;
}

char *ui_fmt_ago(char *out, size_t n, int64_t ms)
{
    int64_t min = ms / 60000;
    if (min < 1) {
        snprintf(out, n, "just now");
    } else if (min < 60) {
        snprintf(out, n, "%d min ago", (int)min);
    } else if (min < 48 * 60) {
        snprintf(out, n, "%d h ago", (int)(min / 60));
    } else {
        snprintf(out, n, "%d d ago", (int)(min / (24 * 60)));
    }
    return out;
}

static const struct {
    const char *id, *name;
    uint32_t color;
} s_cats[] = {
    {"events", "Events", COL_CAT_EVENTS},
    {"culture", "Culture", COL_CAT_CULTURE},
    {"tech", "Tech docs", COL_CAT_TECH},
    {"releases", "Releases", COL_CAT_RELEASES},
    {"ecosystem", "Ecosystem", COL_CAT_ECOSYSTEM},
    {"history", "History", COL_CAT_HISTORY},
    {"people", "People", COL_CAT_PEOPLE},
    {"lore", "Lore", COL_CAT_LORE},
};

uint32_t ui_cat_color(const char *id)
{
    for (size_t i = 0; id && i < sizeof(s_cats) / sizeof(s_cats[0]); i++) {
        if (!strcmp(id, s_cats[i].id)) {
            return s_cats[i].color;
        }
    }
    return COL_DIM;
}

const char *ui_cat_name(const char *id)
{
    for (size_t i = 0; id && i < sizeof(s_cats) / sizeof(s_cats[0]); i++) {
        if (!strcmp(id, s_cats[i].id)) {
            return s_cats[i].name;
        }
    }
    return id ? id : "";
}

uint32_t ui_rarity_color(const char *r)
{
    if (!r) {
        return COL_DIM;
    }
    return !strcmp(r, "rare") ? COL_RARITY_RARE : !strcmp(r, "epic") ? COL_RARITY_EPIC
         : !strcmp(r, "legendary") ? COL_RARITY_LEGEND : COL_RARITY_COMMON;
}

// ---------------------------------------------------------------------------
// Offline panel

typedef struct {
    bool need_owner;
    ui_net_t shown;
    char ssid[33];
    lv_obj_t *ring, *icon, *title, *body;
} offline_t;

ui_net_t ui_net_state(bool need_owner)
{
    os_cloud_info_t *in = lv_malloc(sizeof(*in));
    if (!in) {
        return UI_NET_REGISTERING;
    }
    os_cloud_get_info(in);
    bool server = in->server[0], claimed = in->claimed;
    lv_free(in);
    if (os_api_online()) {
        return need_owner && !claimed ? UI_NET_NOT_LINKED : UI_NET_OK;
    }
    if (!server) {
        return UI_NET_NO_SERVER;
    }
    char ssid[33];
    bool saved = os_wifi_saved_ssid(ssid, sizeof(ssid));
    switch (os_wifi_state()) {
    case OS_WIFI_CONNECTED:  return UI_NET_REGISTERING;
    case OS_WIFI_CONNECTING: return UI_NET_JOINING;
    case OS_WIFI_FAILED:     return UI_NET_FAILED;
    case OS_WIFI_SUSPENDED:  return UI_NET_PAUSED;
    default:                 return saved ? UI_NET_WIFI_OFF : UI_NET_NO_NETWORK;
    }
}

static void offline_deleted(lv_event_t *e)
{
    lv_free(lv_event_get_user_data(e));
}

lv_obj_t *ui_offline_create(lv_obj_t *parent, int y, int h, bool need_owner)
{
    offline_t *o = lv_malloc_zeroed(sizeof(*o));
    o->need_owner = need_owner;
    o->shown = -1;
    lv_obj_t *p = lv_obj_create(parent);
    lv_obj_remove_style_all(p);
    lv_obj_set_pos(p, 0, y);
    lv_obj_set_size(p, OS_ROOT_W, h);
    lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(p, ui_col(COL_BG), 0);
    lv_obj_set_scrollable(p, false);
    lv_obj_set_user_data(p, o);
    lv_obj_add_event_cb(p, offline_deleted, LV_EVENT_DELETE, o);

    int top = h > 150 ? (h - 150) / 2 : 4;
    o->ring = lv_obj_create(p);
    lv_obj_remove_style_all(o->ring);
    lv_obj_set_size(o->ring, 44, 44);
    lv_obj_set_style_radius(o->ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(o->ring, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(o->ring, ui_col(COL_PANEL_HI), 0);
    lv_obj_set_style_border_width(o->ring, 2, 0);
    lv_obj_align(o->ring, LV_ALIGN_TOP_MID, 0, top);
    o->icon = ui_label(o->ring, &lv_font_montserrat_20, COL_TEXT, "");
    lv_obj_center(o->icon);
    o->title = ui_label(p, &sol_font_20, COL_TEXT, "");
    lv_obj_set_width(o->title, OS_ROOT_W - 24);
    lv_label_set_long_mode(o->title, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_align(o->title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(o->title, LV_ALIGN_TOP_MID, 0, top + 56);
    o->body = ui_label(p, &sol_font_14, COL_DIM, "");
    lv_obj_set_width(o->body, OS_ROOT_W - 40);
    lv_label_set_long_mode(o->body, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(o->body, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(o->body, LV_ALIGN_TOP_MID, 0, top + 86);
    ui_offline_update(p);
    return p;
}

ui_net_t ui_offline_update(lv_obj_t *panel)
{
    offline_t *o = lv_obj_get_user_data(panel);
    ui_net_t st = ui_net_state(o->need_owner);
    char ssid[33] = "";
    if (st == UI_NET_JOINING || st == UI_NET_FAILED) {
        os_wifi_info_t w;
        os_wifi_get_info(&w);
        strlcpy(ssid, w.ssid, sizeof(ssid));
    } else {
        os_wifi_saved_ssid(ssid, sizeof(ssid));
    }
    if (st == o->shown && !strcmp(ssid, o->ssid)) {
        return st;
    }
    o->shown = st;
    strlcpy(o->ssid, ssid, sizeof(o->ssid));
    lv_obj_set_hidden(panel, st == UI_NET_OK);

    const char *icon = LV_SYMBOL_WIFI, *title = "", *body = "";
    char tbuf[64], bbuf[96];
    uint32_t col = COL_DIM;
    switch (st) {
    case UI_NET_OK:
        return st;
    case UI_NET_NO_SERVER:
        icon = LV_SYMBOL_SETTINGS;
        title = "No server set";
        body = "This badge has no server. Set one over USB: server https://<host>";
        col = COL_YELLOW;
        break;
    case UI_NET_NO_NETWORK:
        title = "No network saved";
        body = "SELECT to open Wi-Fi";
        break;
    case UI_NET_WIFI_OFF:
        title = "Wi-Fi is off";
        snprintf(bbuf, sizeof(bbuf), "SELECT to connect to %s", ssid);
        body = bbuf;
        break;
    case UI_NET_JOINING:
        snprintf(tbuf, sizeof(tbuf), "Connecting to %s", ssid);
        title = tbuf;
        body = "One moment...";
        col = COL_AQUA;
        break;
    case UI_NET_FAILED:
        snprintf(tbuf, sizeof(tbuf), "Can't join %s", ssid);
        title = tbuf;
        body = "SELECT to try again";
        icon = LV_SYMBOL_WARNING;
        col = COL_ERR;
        break;
    case UI_NET_PAUSED:
        title = "Wi-Fi is paused";
        body = "The radio is busy with the mesh. It comes back when you leave the app.";
        icon = LV_SYMBOL_SHUFFLE;
        col = COL_PINK;
        break;
    case UI_NET_REGISTERING:
        title = "Connecting to the server";
        body = "One moment...";
        icon = LV_SYMBOL_REFRESH;
        col = COL_PURPLE;
        break;
    case UI_NET_NOT_LINKED:
        title = "Not linked yet";
        body = "Claim this badge first. SELECT to open My Badge";
        icon = LV_SYMBOL_LOOP;
        col = COL_LPURPLE;
        break;
    }
    lv_label_set_text(o->icon, icon);
    lv_obj_set_style_text_color(o->icon, ui_col(col), 0);
    lv_obj_set_style_border_color(o->ring, ui_col(col), 0);
    lv_label_set_text(o->title, title);
    lv_label_set_text(o->body, body);
    return st;
}

bool ui_offline_key(lv_obj_t *panel, os_key_t key)
{
    if (key != OS_KEY_SELECT || !panel || lv_obj_is_hidden(panel)) {
        return false;
    }
    offline_t *o = lv_obj_get_user_data(panel);
    switch (o->shown) {
    case UI_NET_WIFI_OFF:
    case UI_NET_FAILED:
        os_wifi_set_enabled(true);
        os_toast("Connecting to %s", o->ssid);
        break;
    case UI_NET_NO_NETWORK:
        os_launch_id("wifi");
        break;
    case UI_NET_NOT_LINKED:
        os_launch_id("badge");
        break;
    default:
        break;
    }
    ui_offline_update(panel);
    return true;
}

// ---- Confetti ----
// One object draws every piece in its DRAW_MAIN event, rather than an object
// per piece: a few hundred bytes instead of ~200 B a piece. Each frame
// invalidates only the pieces' old + new rects (LVGL falls back to the whole
// screen when there are more than its 32 invalid areas). Fixed point (1/16 px)
// because the C5 has no FPU; the object owns its ~30 fps timer and frees it
// on LV_EVENT_DELETE.

#define FX_FP       16
#define FX_FRAME_MS 33
#define FX_FRAMES   100                 // hard stop after ~3.3 s
#define FX_GRAVITY  5                   // 1/16 px per frame^2
#define FX_FALL     40                  // flutter: terminal fall speed
#define FX_SWAY     6                   // side-to-side drift while tumbling

typedef struct {
    int16_t x, y, vx, vy;               // 1/16 px, 1/16 px per frame
    uint8_t spin, rate;                 // tumble phase (0..15) and speed
    uint8_t size, col;                  // px; index into fx_cols
    bool flat;                          // tumbles around the horizontal axis
} fx_piece_t;

typedef struct {
    lv_timer_t *timer;
    int n, frame;
    fx_piece_t p[];
} fx_t;

static const uint32_t fx_cols[] = {
    COL_PURPLE, COL_GREEN, COL_PINK, COL_LPURPLE, COL_BLUE, COL_AQUA, COL_LIME, COL_YELLOW, COL_ORANGE,
};

static int fx_rand(int lo, int hi)
{
    return lo + (int)(esp_random() % (uint32_t)(hi - lo + 1));
}

// The piece's rect in screen coordinates: one side shrinks and grows with
// the tumble phase, so flat squares look like they flip in the air.
static void fx_area(const lv_area_t *box, const fx_piece_t *q, lv_area_t *a)
{
    int t = q->spin < 8 ? q->spin : 15 - q->spin;
    int thin = 1 + t * (q->size - 1) / 7;
    a->x1 = box->x1 + q->x / FX_FP;
    a->y1 = box->y1 + q->y / FX_FP;
    a->x2 = a->x1 + (q->flat ? q->size : thin) - 1;
    a->y2 = a->y1 + (q->flat ? thin : q->size) - 1;
}

static void fx_draw(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_target_obj(e);
    fx_t *fx = lv_obj_get_user_data(obj);
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t box;
    lv_obj_get_coords(obj, &box);
    lv_draw_fill_dsc_t d;
    lv_draw_fill_dsc_init(&d);
    d.opa = LV_OPA_COVER;
    for (int i = 0; i < fx->n; i++) {
        lv_area_t a;
        fx_area(&box, &fx->p[i], &a);
        d.color = ui_col(fx_cols[fx->p[i].col]);
        lv_draw_fill(layer, &d, &a);
    }
}

static void fx_deleted(lv_event_t *e)
{
    fx_t *fx = lv_obj_get_user_data(lv_event_get_target_obj(e));
    lv_timer_delete(fx->timer);
    free(fx);
}

static void fx_frame(lv_timer_t *t)
{
    lv_obj_t *obj = lv_timer_get_user_data(t);
    fx_t *fx = lv_obj_get_user_data(obj);
    lv_area_t box;
    lv_obj_get_coords(obj, &box);
    int h = lv_area_get_height(&box);
    for (int i = 0; i < fx->n;) {
        fx_piece_t *q = &fx->p[i];
        lv_area_t old, now;
        fx_area(&box, q, &old);
        q->vy = q->vy + FX_GRAVITY > FX_FALL ? FX_FALL : q->vy + FX_GRAVITY;
        q->vx -= q->vx / 16;            // air drag
        q->x += q->vx + (q->spin < 8 ? FX_SWAY : -FX_SWAY);
        q->y += q->vy;
        q->spin = (q->spin + q->rate) & 15;
        fx_area(&box, q, &now);
        lv_area_t both = {
            LV_MIN(old.x1, now.x1), LV_MIN(old.y1, now.y1), LV_MAX(old.x2, now.x2), LV_MAX(old.y2, now.y2),
        };
        lv_obj_invalidate_area(obj, &both);
        if (q->y / FX_FP >= h) {
            fx->p[i] = fx->p[--fx->n];  // fell out of view
        } else {
            i++;
        }
    }
    if (!fx->n || ++fx->frame >= FX_FRAMES) {
        lv_obj_delete(obj);             // fx_deleted stops this timer
    }
}

void ui_confetti(lv_obj_t *parent, int x, int y, int count)
{
    count = LV_CLAMP(1, count, UI_CONFETTI_MAX);
    fx_t *fx = calloc(1, sizeof(*fx) + count * sizeof(fx_piece_t));
    if (!fx) {
        return;
    }
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_set_size(obj, lv_pct(100), lv_pct(100));
    lv_obj_set_scrollable(obj, false);
    lv_obj_set_user_data(obj, fx);
    fx->timer = lv_timer_create(fx_frame, FX_FRAME_MS, obj);
    if (!fx->timer) {
        free(fx);
        lv_obj_delete(obj);
        return;
    }
    lv_obj_add_event_cb(obj, fx_draw, LV_EVENT_DRAW_MAIN, NULL);
    lv_obj_add_event_cb(obj, fx_deleted, LV_EVENT_DELETE, NULL);

    // A fan upwards: 1.5-7 px/frame up, up to 4.5 px/frame sideways.
    fx->n = count;
    for (int i = 0; i < count; i++) {
        fx_piece_t *q = &fx->p[i];
        q->x = (int16_t)((x + fx_rand(-6, 6)) * FX_FP);
        q->y = (int16_t)((y + fx_rand(-4, 4)) * FX_FP);
        q->vx = (int16_t)fx_rand(-72, 72);
        q->vy = (int16_t)-fx_rand(24, 112);
        q->spin = (uint8_t)fx_rand(0, 15);
        q->rate = (uint8_t)fx_rand(1, 2);
        q->size = (uint8_t)fx_rand(4, 7);
        q->col = (uint8_t)(i % (int)(sizeof(fx_cols) / sizeof(fx_cols[0])));
        q->flat = i & 1;
    }
}
