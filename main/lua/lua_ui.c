// ui.* for Lua apps (docs/LUA_API.md "ui").
//
// Every object a script creates lives in a pool slot (≤ LH_OBJ_MAX); the Lua
// side holds a {slot, gen} handle, so a removed object's handle fails cleanly
// instead of touching freed LVGL memory. Lists use the OS's virtual ui_list:
// rows are read straight from the script's items table with raw, non-allocating
// Lua calls, because ui_list calls back from inside LVGL code where a Lua
// error must not unwind.

#include <stdio.h>
#include <string.h>
#include "lua_priv.h"

typedef struct {
    uint16_t slot, gen;
} lh_handle_t;

static const char *const s_meta[LH_KIND_COUNT] = {"ui.label", "ui.rect", "ui.bar", "ui.gradient", "ui.list", "ui.qr"};

static lh_t *s_ui;              // session whose lists call row_cb
static lua_State *s_row_L;      // thread to read rows with (the caller's)

static const struct {
    const char *name;
    uint32_t col;
} s_colors[] = {
    {"text", COL_TEXT}, {"dim", COL_DIM}, {"bg", COL_BG}, {"panel", COL_PANEL}, {"border", COL_BORDER},
    {"purple", COL_PURPLE}, {"green", COL_GREEN}, {"pink", COL_PINK}, {"lpurple", COL_LPURPLE},
    {"blue", COL_BLUE}, {"aqua", COL_AQUA}, {"lime", COL_LIME}, {"yellow", COL_YELLOW}, {"orange", COL_ORANGE},
};

lv_color_t lh_color(lua_State *L, int idx)
{
    int is_int;
    lua_Integer hex = lua_tointegerx(L, idx, &is_int);
    if (is_int) {
        return lv_color_hex((uint32_t)hex & 0xFFFFFF);
    }
    const char *name = lua_type(L, idx) == LUA_TSTRING ? lua_tostring(L, idx) : "";
    for (size_t i = 0; i < sizeof(s_colors) / sizeof(s_colors[0]); i++) {
        if (!strcmp(name, s_colors[i].name)) {
            return ui_col(s_colors[i].col);
        }
    }
    luaL_error(L, "colour must be a theme name or 0xRRGGBB, not '%s'", name[0] ? name : luaL_typename(L, idx));
    return lv_color_black();
}

// Options-table helpers: missing fields give the default.
static int opt_int(lua_State *L, int t, const char *key, int def)
{
    if (lua_type(L, t) != LUA_TTABLE) {
        return def;
    }
    lua_getfield(L, t, key);
    int is_int;
    lua_Integer v = lua_tointegerx(L, -1, &is_int);
    if (!is_int && !lua_isnil(L, -1)) {
        luaL_error(L, "option '%s' must be a whole number", key);
    }
    lua_pop(L, 1);
    return is_int ? (int)v : def;
}

static bool opt_field(lua_State *L, int t, const char *key)
{
    if (lua_type(L, t) != LUA_TTABLE) {
        return false;
    }
    lua_getfield(L, t, key);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        return false;
    }
    return true;    // value left on the stack
}

// ---------------------------------------------------------------------------
// Pool

static lh_t *ui_session(lua_State *L)
{
    lh_t *s = lh_get(L);
    return s->ui_off || !s->area ? NULL : s;
}

static void keep_dialogs_on_top(lh_t *s)
{
    if (s->modal) {
        lv_obj_move_foreground(s->modal);
    }
}

static lh_obj_t *new_obj(lua_State *L, lh_t *s, lh_kind_t kind, lv_obj_t *obj)
{
    for (int i = 0; i < LH_OBJ_MAX; i++) {
        lh_obj_t *o = &s->objs[i];
        if (o->obj) {
            continue;
        }
        o->obj = obj;
        o->kind = kind;
        o->order = ++s->obj_order;
        o->list = NULL;
        o->items_ref = o->select_ref = LUA_NOREF;
        lh_handle_t *h = lua_newuserdatauv(L, sizeof(*h), 0);
        h->slot = i;
        h->gen = o->gen;
        luaL_setmetatable(L, s_meta[kind]);
        keep_dialogs_on_top(s);
        return o;
    }
    lv_obj_delete(obj);
    luaL_error(L, "too many UI objects (%d)", LH_OBJ_MAX);
    return NULL;
}

static void free_obj(lua_State *L, lh_t *s, lh_obj_t *o)
{
    if (L) {
        luaL_unref(L, LUA_REGISTRYINDEX, o->items_ref);
        luaL_unref(L, LUA_REGISTRYINDEX, o->select_ref);
    }
    s->text_total -= o->text_len;
    o->text_len = 0;
    o->obj = NULL;
    o->list = NULL;
    o->items_ref = o->select_ref = LUA_NOREF;
    o->gen++;
}

// LVGL copies label text onto the system heap, outside the script's 48 KB,
// so each label holds at most LH_TEXT_MAX bytes (cut at a character, with
// "...") and all of them together LH_TEXT_TOTAL.
static void clip_text(char out[LH_TEXT_MAX + 1], const char *in)
{
    size_t n = strlen(in);
    if (n <= LH_TEXT_MAX) {
        memcpy(out, in, n + 1);
        return;
    }
    size_t cut = LH_TEXT_MAX - 3;
    while (cut > 0 && ((unsigned char)in[cut] & 0xC0) == 0x80) {
        cut--;          // inside a UTF-8 character
    }
    memcpy(out, in, cut);
    memcpy(out + cut, "...", 4);
}

static void check_text(lua_State *L, lh_t *s, size_t old, size_t len)
{
    if (s->text_total - old + len > LH_TEXT_TOTAL) {
        luaL_error(L, "too much text on screen (%d KB per app)", LH_TEXT_TOTAL / 1024);
    }
}

// The object behind handle 1, or NULL while shutting down (calls do nothing).
static lh_obj_t *check(lua_State *L)
{
    lh_handle_t *h = NULL;
    for (int k = 0; k < LH_KIND_COUNT && !h; k++) {
        h = luaL_testudata(L, 1, s_meta[k]);
    }
    if (!h) {
        luaL_typeerror(L, 1, "UI object");
    }
    lh_t *s = ui_session(L);
    if (!s) {
        return NULL;
    }
    lh_obj_t *o = &s->objs[h->slot];
    if (!o->obj || o->gen != h->gen) {
        luaL_error(L, "this object was removed");
    }
    return o;
}

// The object behind handle 1 if it is a `kind`: a method taken from one kind
// and called on another's object (bar.set(label, 5)) must not reach LVGL,
// which would write a bar's fields into a label.
static lh_obj_t *check_kind(lua_State *L, lh_kind_t kind, const char *what)
{
    lh_obj_t *o = check(L);
    if (o && o->kind != kind) {
        luaL_typeerror(L, 1, what);
    }
    return o;
}

static lv_obj_t *base_obj(lv_obj_t *parent, int x, int y, int w, int h)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_scrollable(o, false);
    return o;
}

// ---------------------------------------------------------------------------
// Methods

static int m_pos(lua_State *L)
{
    lh_obj_t *o = check(L);
    int x = luaL_checkinteger(L, 2), y = luaL_checkinteger(L, 3);
    if (o) {
        lv_obj_set_pos(o->obj, x, y);
    }
    return 0;
}

static int m_size(lua_State *L)
{
    lh_obj_t *o = check(L);
    int w = luaL_checkinteger(L, 2), h = luaL_checkinteger(L, 3);
    if (o) {
        lv_obj_set_size(o->obj, w, h);
        if (o->kind == LH_GRADIENT || o->kind == LH_BAR) {
            lv_obj_set_style_radius(o->obj, o->kind == LH_BAR ? h / 2 : 0, 0);
        }
    }
    return 0;
}

static int m_show(lua_State *L)
{
    lh_obj_t *o = check(L);
    bool show = lua_isnone(L, 2) || lua_toboolean(L, 2);
    if (o) {
        lv_obj_set_hidden(o->obj, !show);
    }
    return 0;
}

static int m_remove(lua_State *L)
{
    lh_handle_t *h = NULL;
    for (int k = 0; k < LH_KIND_COUNT && !h; k++) {
        h = luaL_testudata(L, 1, s_meta[k]);
    }
    lh_t *s = ui_session(L);
    if (!h || !s) {
        return 0;
    }
    lh_obj_t *o = &s->objs[h->slot];
    if (o->obj && o->gen == h->gen) {
        lv_obj_delete(o->obj);
        free_obj(L, s, o);
    }
    return 0;
}

static int m_text(lua_State *L)
{
    lh_obj_t *o = check_kind(L, LH_LABEL, "label");
    char t[LH_TEXT_MAX + 1];
    clip_text(t, luaL_checkstring(L, 2));
    if (o) {
        lh_t *s = lh_get(L);
        size_t len = strlen(t);
        check_text(L, s, o->text_len, len);
        lv_label_set_text(o->obj, t);
        s->text_total = s->text_total - o->text_len + len;
        o->text_len = len;
    }
    return 0;
}

static int m_color(lua_State *L)
{
    lh_obj_t *o = check(L);
    lv_color_t c = lh_color(L, 2);
    if (o) {
        if (o->kind == LH_LABEL) {
            lv_obj_set_style_text_color(o->obj, c, 0);
        } else {
            lv_obj_set_style_bg_color(o->obj, c, 0);
        }
    }
    return 0;
}

static int m_set(lua_State *L)
{
    lh_obj_t *o = check_kind(L, LH_BAR, "bar");
    int v = luaL_checkinteger(L, 2);
    if (o) {
        lv_bar_set_value(o->obj, v < 0 ? 0 : v > 100 ? 100 : v, LV_ANIM_OFF);
    }
    return 0;
}

// ---- Lists

static void copy_cell(lua_State *L, int idx, char *out, size_t n)
{
    switch (lua_type(L, idx)) {
    case LUA_TSTRING:
        strlcpy(out, lua_tostring(L, idx), n);
        break;
    case LUA_TNUMBER:
        if (lua_isinteger(L, idx)) {
            snprintf(out, n, LUA_INTEGER_FMT, (LUAI_UACINT)lua_tointeger(L, idx));
        } else {
            snprintf(out, n, "%.7g", (double)lua_tonumber(L, idx));
        }
        break;
    default:
        out[0] = 0;
    }
}

static void row_cb(int index, ui_row_t *row, void *ctx)
{
    lh_obj_t *o = ctx;
    lua_State *L = s_row_L;
    if (!s_ui || !L || !lua_checkstack(L, 4)) {
        return;
    }
    int top = lua_gettop(L);
    lua_rawgeti(L, LUA_REGISTRYINDEX, o->items_ref);
    if (lua_type(L, -1) == LUA_TTABLE) {
        lua_rawgeti(L, -1, index + 1);
        int item = lua_gettop(L);
        if (lua_type(L, item) == LUA_TTABLE) {
            char *cells[3] = {row->title, row->sub, row->right};
            size_t sizes[3] = {sizeof(row->title), sizeof(row->sub), sizeof(row->right)};
            for (int k = 0; k < 3; k++) {
                lua_rawgeti(L, LUA_REGISTRYINDEX, s_ui->key_refs[k]);
                lua_rawget(L, item);
                copy_cell(L, -1, cells[k], sizes[k]);
                lua_pop(L, 1);
            }
        } else {
            copy_cell(L, item, row->title, sizeof(row->title));
        }
    }
    lua_settop(L, top);
}

static lh_obj_t *check_list(lua_State *L)
{
    return check_kind(L, LH_LIST, "list");
}

static int m_items(lua_State *L)
{
    lh_obj_t *o = check_list(L);
    luaL_checktype(L, 2, LUA_TTABLE);
    if (o) {
        lua_pushvalue(L, 2);
        lua_rawseti(L, LUA_REGISTRYINDEX, o->items_ref);
        s_row_L = L;
        ui_list_set_count(o->list, (int)lua_rawlen(L, 2));
    }
    return 0;
}

static int m_selected(lua_State *L)
{
    lh_obj_t *o = check_list(L);
    int sel = o ? ui_list_selected(o->list) : -1;
    if (sel < 0) {
        lua_pushnil(L);
    } else {
        lua_pushinteger(L, sel + 1);
    }
    return 1;
}

static int m_select(lua_State *L)
{
    lh_obj_t *o = check_list(L);
    int i = luaL_checkinteger(L, 2);
    if (o) {
        s_row_L = L;
        ui_list_select(o->list, i - 1);
    }
    return 0;
}

// ---- QR codes

static const char *qr_text(lua_State *L, int idx, size_t *len)
{
    const char *t = luaL_checklstring(L, idx, len);
    if (*len > LH_QR_TEXT_MAX) {
        luaL_error(L, "ui.qr: text is %d bytes (at most %d)", (int)*len, LH_QR_TEXT_MAX);
    }
    return t;
}

static int m_qr_text(lua_State *L)
{
    lh_obj_t *o = check_kind(L, LH_QR, "QR code");
    size_t len;
    const char *t = qr_text(L, 2, &len);
    if (o && lv_qrcode_update(o->obj, t, len) != LV_RESULT_OK) {
        luaL_error(L, "ui.qr: %d bytes don't fit a %d px code", (int)len, (int)lv_obj_get_width(o->obj));
    }
    return 0;
}

// ---------------------------------------------------------------------------
// ui.* constructors

static const lv_font_t *font_for(lua_State *L, int size, bool mono)
{
    switch (size) {
    case 12: return &sol_mono_12;
    case 14: return mono ? &sol_mono_12 : &sol_font_14;
    case 20: return &sol_font_20;
    case 32: return &sol_font_32;
    }
    luaL_error(L, "size must be 12, 14, 20 or 32");
    return NULL;
}

static int l_label(lua_State *L)
{
    int x = luaL_checkinteger(L, 1), y = luaL_checkinteger(L, 2);
    char text[LH_TEXT_MAX + 1];
    clip_text(text, luaL_checkstring(L, 3));
    int size = opt_int(L, 4, "size", 14);
    int w = opt_int(L, 4, "w", 0);
    bool mono = false;
    if (opt_field(L, 4, "mono")) {
        mono = lua_toboolean(L, -1);
        lua_pop(L, 1);
    }
    const lv_font_t *font = font_for(L, size, mono);
    lv_color_t color = ui_col(COL_TEXT);
    if (opt_field(L, 4, "color")) {
        color = lh_color(L, -1);
        lua_pop(L, 1);
    }
    lv_text_align_t align = LV_TEXT_ALIGN_LEFT;
    if (opt_field(L, 4, "align")) {
        const char *a = lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : "";
        align = !strcmp(a, "center") ? LV_TEXT_ALIGN_CENTER : !strcmp(a, "right") ? LV_TEXT_ALIGN_RIGHT
              : LV_TEXT_ALIGN_LEFT;
        lua_pop(L, 1);
    }
    lh_t *s = ui_session(L);
    if (!s) {
        return 0;
    }
    size_t len = strlen(text);
    check_text(L, s, 0, len);
    lv_obj_t *l = lv_label_create(s->area);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_obj_set_style_text_align(l, align, 0);
    lv_label_set_text(l, text);
    lv_obj_set_pos(l, x, y);
    if (w > 0) {
        lv_obj_set_width(l, w);
    }
    lh_obj_t *o = new_obj(L, s, LH_LABEL, l);
    o->text_len = len;
    s->text_total += len;
    return 1;
}

static int l_rect(lua_State *L)
{
    int x = luaL_checkinteger(L, 1), y = luaL_checkinteger(L, 2);
    int w = luaL_checkinteger(L, 3), h = luaL_checkinteger(L, 4);
    lv_color_t color = ui_col(COL_PANEL);
    if (opt_field(L, 5, "color")) {
        color = lh_color(L, -1);
        lua_pop(L, 1);
    }
    int radius = opt_int(L, 5, "radius", 0);
    bool border = false;
    lv_color_t border_color = ui_col(COL_BORDER);
    if (opt_field(L, 5, "border")) {
        border = true;
        border_color = lh_color(L, -1);
        lua_pop(L, 1);
    }
    lh_t *s = ui_session(L);
    if (!s) {
        return 0;
    }
    lv_obj_t *r = base_obj(s->area, x, y, w, h);
    lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(r, color, 0);
    lv_obj_set_style_radius(r, radius, 0);
    if (border) {
        lv_obj_set_style_border_width(r, 1, 0);
        lv_obj_set_style_border_color(r, border_color, 0);
    }
    new_obj(L, s, LH_RECT, r);
    return 1;
}

static int l_bar(lua_State *L)
{
    int x = luaL_checkinteger(L, 1), y = luaL_checkinteger(L, 2);
    int w = luaL_checkinteger(L, 3), h = luaL_checkinteger(L, 4);
    int v = luaL_optinteger(L, 5, 0);
    lh_t *s = ui_session(L);
    if (!s) {
        return 0;
    }
    lv_obj_t *b = lv_bar_create(s->area);
    lv_obj_remove_style_all(b);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, ui_col(COL_PANEL_HI), 0);
    lv_obj_set_style_radius(b, h / 2, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(b, ui_col(COL_PURPLE), LV_PART_INDICATOR);
    lv_obj_set_style_bg_grad_color(b, lv_color_hex(COL_GREEN), LV_PART_INDICATOR);
    lv_obj_set_style_bg_grad_dir(b, LV_GRAD_DIR_HOR, LV_PART_INDICATOR);
    lv_obj_set_style_radius(b, h / 2, LV_PART_INDICATOR);
    lv_bar_set_range(b, 0, 100);
    lv_bar_set_value(b, v < 0 ? 0 : v > 100 ? 100 : v, LV_ANIM_OFF);
    new_obj(L, s, LH_BAR, b);
    return 1;
}

static int l_gradient(lua_State *L)
{
    int x = luaL_checkinteger(L, 1), y = luaL_checkinteger(L, 2);
    int w = luaL_checkinteger(L, 3), h = luaL_checkinteger(L, 4);
    lh_t *s = ui_session(L);
    if (!s) {
        return 0;
    }
    lv_obj_t *g = ui_gradient_bar(s->area, x, y, w, h);
    lv_obj_set_style_radius(g, 0, 0);
    new_obj(L, s, LH_GRADIENT, g);
    return 1;
}

static int l_list(lua_State *L)
{
    int x = luaL_checkinteger(L, 1), y = luaL_checkinteger(L, 2);
    int w = luaL_checkinteger(L, 3), h = luaL_checkinteger(L, 4);
    luaL_checktype(L, 5, LUA_TTABLE);
    bool has_select = opt_field(L, 6, "on_select");
    if (has_select && !lua_isfunction(L, -1)) {
        luaL_error(L, "on_select must be a function");
    }
    lh_t *s = ui_session(L);
    if (!s) {
        return 0;
    }
    // A box of our own holds the list, so pos/show/remove work like any object.
    lv_obj_t *box = base_obj(s->area, x, y, w, h);
    lh_obj_t *o = new_obj(L, s, LH_LIST, box);      // pushes the handle
    if (has_select) {
        lua_pushvalue(L, -2);
        o->select_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    }
    lua_pushvalue(L, 5);
    o->items_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    s_row_L = L;
    o->list = ui_list_create_at(box, 0, 0, w, h, row_cb, o);
    ui_list_set_count(o->list, (int)lua_rawlen(L, 5));
    return 1;
}

// Always dark modules on white with a quiet zone, whatever the theme: phone
// wallets' scanners don't all read inverted codes. One at a time, because the
// canvas (up to 218 x 218 at 1 bit) sits on the system heap, not the script's.
static int l_qr(lua_State *L)
{
    int x = luaL_checkinteger(L, 1), y = luaL_checkinteger(L, 2);
    int size = luaL_checkinteger(L, 3);
    size_t len;
    const char *text = qr_text(L, 4, &len);
    if (size < LH_QR_MIN || size > OS_ROOT_H) {
        luaL_error(L, "ui.qr: size must be %d to %d", LH_QR_MIN, OS_ROOT_H);
    }
    lh_t *s = ui_session(L);
    if (!s) {
        return 0;
    }
    int n = 0;
    for (int i = 0; i < LH_OBJ_MAX; i++) {
        n += s->objs[i].obj && s->objs[i].kind == LH_QR;
    }
    if (n >= LH_QR_MAX) {
        luaL_error(L, "ui.qr: %d QR code at a time (change its text, or remove it first)", LH_QR_MAX);
    }
    lv_obj_t *q = lv_qrcode_create(s->area);
    lv_qrcode_set_size(q, size);
    if (!lv_canvas_get_draw_buf(q)) {
        lv_obj_delete(q);
        luaL_error(L, "ui.qr: not enough memory");
    }
    lv_qrcode_set_dark_color(q, lv_color_black());
    lv_qrcode_set_light_color(q, lv_color_white());
    lv_qrcode_set_quiet_zone(q, true);
    if (lv_qrcode_update(q, text, len) != LV_RESULT_OK) {
        lv_obj_delete(q);
        luaL_error(L, "ui.qr: %d bytes don't fit a %d px code", (int)len, size);
    }
    lv_obj_set_pos(q, x, y);
    new_obj(L, s, LH_QR, q);
    return 1;
}

static int l_bg(lua_State *L)
{
    lv_color_t c = lh_color(L, 1);
    lh_t *s = ui_session(L);
    if (s) {
        lv_obj_set_style_bg_color(s->area, c, 0);
    }
    return 0;
}

static int l_hint(lua_State *L)
{
    char text[LH_TEXT_MAX + 1];
    clip_text(text, luaL_optstring(L, 1, ""));
    lh_t *s = ui_session(L);
    if (!s) {
        return 0;
    }
    if (!text[0]) {
        if (s->hint) {
            lv_obj_delete(lv_obj_get_parent(s->hint));
            s->hint = NULL;
        }
    } else if (s->hint) {
        lv_label_set_text(s->hint, text);
    } else {
        s->hint = ui_hint(s->area, text);
        keep_dialogs_on_top(s);
    }
    return 0;
}

static int l_toast(lua_State *L)
{
    const char *text = luaL_checkstring(L, 1);
    if (ui_session(L)) {
        os_toast("%s", text);
    }
    return 0;
}

static void dialog_ref(lua_State *L, lh_t *s, int fn_idx)
{
    if (s->modal || s->kb) {
        luaL_error(L, "a dialog is already open");
    }
    lua_pushvalue(L, fn_idx);
    s->modal_ref = luaL_ref(L, LUA_REGISTRYINDEX);
}

static int l_confirm(lua_State *L)
{
    char text[LH_TEXT_MAX + 1];
    clip_text(text, luaL_checkstring(L, 1));
    luaL_checktype(L, 2, LUA_TFUNCTION);
    lh_t *s = ui_session(L);
    if (!s) {
        return 0;
    }
    dialog_ref(L, s, 2);
    lv_obj_t *dim = base_obj(s->area, 0, 0, OS_ROOT_W, OS_ROOT_H);
    lv_obj_set_style_bg_opa(dim, LV_OPA_70, 0);
    lv_obj_set_style_bg_color(dim, ui_col(COL_BG), 0);
    lv_obj_t *p = ui_panel(dim, 20, 0, OS_ROOT_W - 40, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(p, 12, 0);
    lv_obj_set_style_border_color(p, ui_col(COL_PURPLE), 0);
    lv_obj_t *t = ui_label(p, &sol_font_14, COL_TEXT, text);
    lv_obj_set_width(t, OS_ROOT_W - 66);
    lv_obj_t *k = ui_label(p, &sol_mono_12, COL_DIM, "SELECT yes   CANCEL no");
    lv_obj_align_to(k, t, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 10);
    lv_obj_center(p);
    s->modal = dim;
    return 0;
}

static int l_input(lua_State *L)
{
    char title[LH_TEXT_MAX + 1];
    clip_text(title, luaL_checkstring(L, 1));
    int max = luaL_checkinteger(L, 2);
    luaL_checktype(L, 3, LUA_TFUNCTION);
    lh_t *s = ui_session(L);
    if (!s) {
        return 0;
    }
    dialog_ref(L, s, 3);
    s->kb = ui_kb_create(s->area, title, max < 1 ? 1 : max);
    s->modal = lv_obj_get_child(s->area, -1);   // the keyboard's box, kept on top
    return 0;
}

static int l_clear(lua_State *L)
{
    lh_t *s = ui_session(L);
    if (s) {
        lh_ui_clear(s);
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Keys and lifetime

static void close_dialog(lh_t *s)
{
    if (s->kb) {
        ui_kb_delete(s->kb);
    } else if (s->modal) {
        lv_obj_delete(s->modal);
    }
    s->kb = NULL;
    s->modal = NULL;
}

// Calls the dialog's callback with the value on top of the stack.
static void dialog_done(lua_State *L, lh_t *s)
{
    int ref = s->modal_ref;
    s->modal_ref = LUA_NOREF;
    close_dialog(s);
    lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
    luaL_unref(L, LUA_REGISTRYINDEX, ref);
    lua_insert(L, -2);
    lua_call(L, 1, 0);
}

static lh_obj_t *focused_list(lh_t *s)
{
    lh_obj_t *best = NULL;
    for (int i = 0; i < LH_OBJ_MAX; i++) {
        lh_obj_t *o = &s->objs[i];
        if (o->obj && o->kind == LH_LIST && !lv_obj_is_hidden(o->obj)
            && (!best || o->order > best->order)) {
            best = o;
        }
    }
    return best;
}

bool lh_ui_key(lua_State *L, lh_t *s, os_key_t key, bool repeat)
{
    if (s->kb) {
        ui_kb_result_t r = ui_kb_key(s->kb, key, repeat);
        if (r == UI_KB_DONE) {
            lua_pushstring(L, ui_kb_text(s->kb));
            dialog_done(L, s);
        } else if (r == UI_KB_CANCEL) {
            lua_pushnil(L);
            dialog_done(L, s);
        }
        return true;
    }
    if (s->modal) {
        if (key == OS_KEY_SELECT || (key == OS_KEY_BACK && !repeat)) {
            lua_pushboolean(L, key == OS_KEY_SELECT);
            dialog_done(L, s);
        }
        return true;
    }
    lh_obj_t *o = focused_list(s);
    if (!o) {
        return false;
    }
    s_row_L = L;
    if (key == OS_KEY_UP || key == OS_KEY_DOWN) {
        ui_list_key(o->list, key, repeat);
        return true;
    }
    int sel = ui_list_selected(o->list);
    if (key != OS_KEY_SELECT || o->select_ref == LUA_NOREF || sel < 0) {
        return false;
    }
    lua_rawgeti(L, LUA_REGISTRYINDEX, o->select_ref);
    lua_pushinteger(L, sel + 1);
    lua_rawgeti(L, LUA_REGISTRYINDEX, o->items_ref);
    lua_rawgeti(L, -1, sel + 1);
    lua_remove(L, -2);
    lua_call(L, 2, 0);
    return true;
}

void lh_ui_clear(lh_t *s)
{
    for (int i = 0; i < LH_OBJ_MAX; i++) {
        lh_obj_t *o = &s->objs[i];
        if (o->obj) {
            lv_obj_delete(o->obj);
            free_obj(s->L, s, o);
        }
    }
    if (s->hint) {
        lv_obj_delete(lv_obj_get_parent(s->hint));
        s->hint = NULL;
    }
}

void lh_ui_release(lh_t *s)
{
    for (int i = 0; i < LH_OBJ_MAX; i++) {
        free_obj(NULL, s, &s->objs[i]);
    }
    s->text_total = 0;
    s->hint = s->modal = s->area = NULL;
    s->kb = NULL;
    s_ui = NULL;
    s_row_L = NULL;
}

void lh_ui_close_dialog(lh_t *s)
{
    close_dialog(s);
}

void lh_open_ui(lua_State *L)
{
    // Each kind's methods, the common ones first. luaL_newlib makes them
    // lazy (vendor/lauxlib.c): a method takes RAM once a script calls it.
#define COMMON {"pos", m_pos}, {"show", m_show}, {"remove", m_remove}
    static const luaL_Reg label[] = {COMMON, {"text", m_text}, {"color", m_color}, {NULL, NULL}};
    static const luaL_Reg rect[] = {COMMON, {"size", m_size}, {"color", m_color}, {NULL, NULL}};
    static const luaL_Reg bar[] = {COMMON, {"set", m_set}, {"size", m_size}, {NULL, NULL}};
    static const luaL_Reg gradient[] = {COMMON, {"size", m_size}, {NULL, NULL}};
    static const luaL_Reg list[] = {COMMON, {"items", m_items}, {"selected", m_selected}, {"select", m_select},
                                    {NULL, NULL}};
    static const luaL_Reg qr[] = {COMMON, {"text", m_qr_text}, {NULL, NULL}};
#undef COMMON
    static const luaL_Reg *const methods[LH_KIND_COUNT] = {label, rect, bar, gradient, list, qr};
    static const luaL_Reg fns[] = {
        {"label", l_label}, {"rect", l_rect}, {"bar", l_bar}, {"gradient", l_gradient}, {"list", l_list},
        {"qr", l_qr}, {"bg", l_bg}, {"hint", l_hint}, {"toast", l_toast}, {"confirm", l_confirm}, {"input", l_input},
        {"clear", l_clear}, {NULL, NULL},
    };
    lh_t *s = lh_get(L);
    for (int k = 0; k < LH_KIND_COUNT; k++) {
        luaL_newmetatable(L, s_meta[k]);
        luaL_newlib(L, methods[k]);
        lua_setfield(L, -2, "__index");
        lua_pushliteral(L, "UI object");
        lua_setfield(L, -2, "__metatable");     // getmetatable() can't reach the methods
        lua_pop(L, 1);
    }
    static const char *const keys[3] = {"title", "sub", "right"};
    for (int k = 0; k < 3; k++) {
        lua_pushstring(L, keys[k]);
        s->key_refs[k] = luaL_ref(L, LUA_REGISTRYINDEX);
    }
    luaL_newlib(L, fns);
    lua_pushinteger(L, OS_ROOT_W);
    lua_setfield(L, -2, "W");
    lua_pushinteger(L, OS_ROOT_H);
    lua_setfield(L, -2, "H");
    lua_setglobal(L, "ui");
    s_ui = s;
}
