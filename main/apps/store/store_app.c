// App Store: browse the server's Lua apps (server/API.md "Device API"),
// read the details, buy and install them onto the microSD card, and update
// installed ones.
//
// The list's first rows are controls (search, sort, updates); LEFT/RIGHT
// picks the category. Pages of 10 load as the selection reaches the end.
// Installing downloads main.lua (os_api_download writes a .part file and
// renames it) and then writes app.json from the X-App-Manifest header. A
// download keeps going if the store is closed: its answer only touches the
// card unless the store is still open.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "cJSON.h"
#include "core/os.h"
#include "core/os_api.h"
#include "core/os_cloud.h"
#include "core/os_data.h"
#include "core/ui.h"
#include "lua/lua_host.h"
#include "lua/lua_pkg.h"

#define ENTRIES_MAX     50
#define PAGE_APPS       10
#define INSTALLED_MAX   40
#define HEAD_H          22
#define BROWSE_H        (OS_ROOT_H - HEAD_H - UI_HINT_H)

typedef struct {
    char id[LUA_PKG_ID_MAX + 1];
    char name[21];
    char summary[81];
    char version[17];
    const char *symbol;
    int price;
    bool owned;
} entry_t;

typedef struct {
    char id[LUA_PKG_ID_MAX + 1];
    char version[17];
} ver_t;

typedef struct {
    entry_t e;
    char description[601];
    char author[33];
    char category[8];
    uint32_t perms;
    int installs, ratings, rating10;    // rating × 10, -1 = none
} detail_t;

enum { CTRL_SEARCH, CTRL_SORT, CTRL_UPDATES };
static const char *const s_cats[] = {NULL, "games", "tools", "social", "quests", "demos"};
static const char *const s_cat_names[] = {"All apps", "Games", "Tools", "Social", "Quests", "Demos"};
static const char *const s_sorts[] = {"popular", "new", "top"};
static const char *const s_sort_names[] = {"Popular", "New", "Top rated"};
#define CAT_COUNT   (int)(sizeof(s_cats) / sizeof(s_cats[0]))

// Worker hand-over (installed versions), as in the Apps library.
static SemaphoreHandle_t s_mx;
static bool s_open, s_busy;
static ver_t *s_inbox;
static int s_inbox_n = -1;
static char s_job_rm[4][LUA_PKG_ID_MAX + 1];

// Requests: each kind remembers the token of its newest call; older answers are ignored.
static uint32_t s_tok, s_tok_list, s_tok_lib, s_tok_detail, s_tok_buy;
static uint32_t s_tok_dl;               // survives closing the store
static char s_dl_id[LUA_PKG_ID_MAX + 1], s_dl_name[21];

static lv_obj_t *s_root, *s_cat_l, *s_sort_l, *s_hint, *s_offline, *s_detail_box, *s_action;
static ui_list_t *s_list;
static ui_kb_t *s_kb;
static entry_t *s_entries;
static int s_n;
static char s_next[64];
static bool s_loading, s_loaded;
static const char *s_err;
static int s_cat, s_sort;
static char s_q[41];
static ver_t *s_installed;
static int s_installed_n = -1;          // -1: not scanned yet
static ver_t *s_updates;                // library entries newer than what's installed
static int s_updates_n;
static bool s_updates_only;             // the list shows updates instead of the store
static detail_t *s_detail;              // the details view, NULL in the list

// ---------------------------------------------------------------------------
// Small helpers

static void jstr(const cJSON *o, const char *key, char *out, size_t n)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    strlcpy(out, cJSON_IsString(v) ? v->valuestring : "", n);
}

static int jint(const cJSON *o, const char *key, int def)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsNumber(v) ? v->valueint : def;
}

// ui_list's sub line is one line of ~42 Semi-Mono characters: cut longer
// text at a character boundary and end it with "...".
static void clip(char *out, size_t n, const char *s)
{
    enum { CHARS = 40 };
    size_t i = 0, chars = 0;
    while (s[i] && chars < CHARS && i + 4 < n) {
        i++;
        while ((s[i] & 0xC0) == 0x80) {
            i++;            // continuation bytes of one UTF-8 character
        }
        chars++;
    }
    memcpy(out, s, i);
    out[i] = 0;
    if (s[i]) {
        while (i > 0 && out[i - 1] == ' ') {
            out[--i] = 0;
        }
        strlcat(out, "...", n);
    }
}

static void urlenc(char *out, size_t n, const char *s)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    for (; *s && o + 4 < n; s++) {
        unsigned char c = *s;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || strchr("-_.~", c)) {
            out[o++] = c;
        } else {
            out[o++] = '%';
            out[o++] = hex[c >> 4];
            out[o++] = hex[c & 15];
        }
    }
    out[o] = 0;
}

static const char *installed_version(const char *id)
{
    for (int i = 0; s_installed && i < s_installed_n; i++) {
        if (!strcmp(s_installed[i].id, id)) {
            return s_installed[i].version;
        }
    }
    return NULL;
}

static void price_text(char *out, size_t n, int price, bool owned)
{
    if (price <= 0) {
        strlcpy(out, "Free", n);
    } else if (owned) {
        strlcpy(out, "Owned", n);
    } else {
        ui_fmt_lamps(out, n, price);
    }
}

static int ctrl_count(void)
{
    return s_updates_only ? 0 : s_updates_n > 0 ? 3 : 2;
}

static int row_count(void)
{
    return s_updates_only ? s_updates_n : ctrl_count() + s_n;
}

// ---------------------------------------------------------------------------
// Installed apps (worker)

static void worker(void *arg)
{
    for (int i = 0; i < 4; i++) {
        if (s_job_rm[i][0]) {
            lua_pkg_remove(s_job_rm[i]);
            s_job_rm[i][0] = 0;
        }
    }
    lua_pkg_t *pkgs = malloc(INSTALLED_MAX * sizeof(*pkgs));
    ver_t *vers = malloc(INSTALLED_MAX * sizeof(*vers));
    int n = pkgs && vers ? lua_pkg_scan(pkgs, INSTALLED_MAX) : 0;
    for (int i = 0; i < n; i++) {
        strlcpy(vers[i].id, pkgs[i].id, sizeof(vers[i].id));
        strlcpy(vers[i].version, pkgs[i].version, sizeof(vers[i].version));
    }
    free(pkgs);
    xSemaphoreTake(s_mx, portMAX_DELAY);
    if (s_open && vers) {
        free(s_inbox);
        s_inbox = vers;
        s_inbox_n = n < 0 ? 0 : n;
        vers = NULL;
    }
    s_busy = false;
    xSemaphoreGive(s_mx);
    free(vers);
    vTaskDelete(NULL);
}

static void scan_installed(void)
{
    xSemaphoreTake(s_mx, portMAX_DELAY);
    if (!s_busy) {
        s_busy = xTaskCreate(worker, "store_scan", 4096, NULL, 3, NULL) == pdPASS;
    }
    xSemaphoreGive(s_mx);
}

// ---------------------------------------------------------------------------
// List

static void row(int i, ui_row_t *r, void *ctx)
{
    char price[24];
    if (s_updates_only) {
        const ver_t *u = &s_updates[i];
        snprintf(r->title, sizeof(r->title), "%s", u->id);
        const char *have = installed_version(u->id);
        snprintf(r->sub, sizeof(r->sub), "v%s " LV_SYMBOL_RIGHT " v%s", have ? have : "?", u->version);
        strlcpy(r->right, "Update", sizeof(r->right));
        r->right_color = COL_GREEN;
        return;
    }
    int c = ctrl_count();
    if (i == CTRL_SEARCH) {
        strlcpy(r->title, LV_SYMBOL_EYE_OPEN "  Search", sizeof(r->title));
        strlcpy(r->sub, s_q[0] ? s_q : "Find an app by name", sizeof(r->sub));
        r->title_color = COL_BLUE;
        return;
    }
    if (i == CTRL_SORT) {
        snprintf(r->title, sizeof(r->title), LV_SYMBOL_SHUFFLE "  Sort: %s", s_sort_names[s_sort]);
        strlcpy(r->sub, "SELECT changes the order", sizeof(r->sub));
        r->title_color = COL_BLUE;
        return;
    }
    if (i == CTRL_UPDATES && c == 3) {
        snprintf(r->title, sizeof(r->title), LV_SYMBOL_DOWNLOAD "  Updates (%d)", s_updates_n);
        strlcpy(r->sub, "New versions of your apps", sizeof(r->sub));
        r->title_color = COL_GREEN;
        return;
    }
    const entry_t *e = &s_entries[i - c];
    snprintf(r->title, sizeof(r->title), "%s  %s", e->symbol, e->name);
    clip(r->sub, sizeof(r->sub), e->summary);
    const char *have = installed_version(e->id);
    if (have) {
        bool update = strcmp(have, e->version) != 0;
        strlcpy(r->right, update ? "Update" : "Installed", sizeof(r->right));
        r->right_color = update ? COL_GREEN : COL_DIM;
    } else {
        price_text(price, sizeof(price), e->price, e->owned);
        strlcpy(r->right, price, sizeof(r->right));
        r->right_color = e->price > 0 && !e->owned ? COL_LAMPS : COL_DIM;
    }
}

static void render_head(void)
{
    char t[40];
    snprintf(t, sizeof(t), LV_SYMBOL_LEFT " %s " LV_SYMBOL_RIGHT, s_updates_only ? "Updates" : s_cat_names[s_cat]);
    lv_label_set_text(s_cat_l, t);
    lv_label_set_text(s_sort_l, s_updates_only ? "" : s_sort_names[s_sort]);
}

static void render_list(void)
{
    render_head();
    ui_list_set_empty(s_list, s_loading ? "Loading..." : s_err ? s_err : "No apps found");
    ui_list_set_count(s_list, row_count());
    lv_label_set_text(s_hint, !lv_obj_is_hidden(s_offline) ? "CANCEL exit"
                              : s_updates_only ? "SELECT details   CANCEL back"
                              : "SELECT open   " LV_SYMBOL_LEFT LV_SYMBOL_RIGHT " category   CANCEL exit");
}

static void list_cb(int status, const char *body, void *ctx);

static void fetch_page(bool more)
{
    char path[320], q[128] = "", cursor[112] = "";
    if (!more) {
        s_n = 0;
        s_next[0] = 0;
    }
    if (s_q[0]) {
        char enc[124];
        urlenc(enc, sizeof(enc), s_q);
        snprintf(q, sizeof(q), "&q=%s", enc);
    }
    if (more && s_next[0]) {
        char enc[90];
        urlenc(enc, sizeof(enc), s_next);
        snprintf(cursor, sizeof(cursor), "&cursor=%s", enc);
    }
    snprintf(path, sizeof(path), "/api/device/apps?sort=%s&limit=%d%s%s%s%s", s_sorts[s_sort], PAGE_APPS,
             s_cats[s_cat] ? "&category=" : "", s_cats[s_cat] ? s_cats[s_cat] : "", q, cursor);
    s_tok_list = ++s_tok;
    s_err = NULL;
    s_loading = os_api_call("GET", path, NULL, 0, list_cb, (void *)(uintptr_t)s_tok_list) == ESP_OK;
    if (!s_loading) {
        s_err = "Can't reach the server";
    }
    render_list();
}

static void list_cb(int status, const char *body, void *ctx)
{
    if ((uintptr_t)ctx != s_tok_list || !s_open) {
        return;
    }
    s_loading = false;
    cJSON *j = status == 200 ? cJSON_Parse(body) : NULL;
    const cJSON *items = cJSON_GetObjectItemCaseSensitive(j, "items");
    if (!cJSON_IsArray(items)) {
        s_err = status == 0 ? "Can't reach the server" : "The store didn't answer. Try again later";
    } else {
        const cJSON *it;
        cJSON_ArrayForEach(it, items) {
            if (s_n >= ENTRIES_MAX) {
                break;
            }
            entry_t *e = &s_entries[s_n];
            jstr(it, "id", e->id, sizeof(e->id));
            if (!lua_pkg_valid_id(e->id)) {
                continue;
            }
            jstr(it, "name", e->name, sizeof(e->name));
            jstr(it, "summary", e->summary, sizeof(e->summary));
            jstr(it, "version", e->version, sizeof(e->version));
            const cJSON *icon = cJSON_GetObjectItemCaseSensitive(it, "icon");
            e->symbol = lua_pkg_symbol(cJSON_IsString(icon) ? icon->valuestring : NULL);
            e->price = jint(it, "price", 0);
            e->owned = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(it, "owned"));
            s_n++;
        }
        jstr(j, "next", s_next, sizeof(s_next));
        if (s_n >= ENTRIES_MAX) {
            s_next[0] = 0;
        }
        s_loaded = true;
    }
    cJSON_Delete(j);
    if (!s_detail && !s_kb) {
        render_list();
    }
}

static void library_cb(int status, const char *body, void *ctx)
{
    if ((uintptr_t)ctx != s_tok_lib || !s_open) {
        return;
    }
    cJSON *j = status == 200 ? cJSON_Parse(body) : NULL;
    const cJSON *it;
    int64_t now = os_cloud_time_ms();
    int rm = 0;
    s_updates_n = 0;
    cJSON_ArrayForEach(it, cJSON_GetObjectItemCaseSensitive(j, "items")) {
        char id[LUA_PKG_ID_MAX + 1], ver[17];
        jstr(it, "id", id, sizeof(id));
        jstr(it, "version", ver, sizeof(ver));
        const char *have = installed_version(id);
        const cJSON *exp = cJSON_GetObjectItemCaseSensitive(it, "expires");
        bool expired = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(it, "temp")) && cJSON_IsNumber(exp) && now
                       && (int64_t)exp->valuedouble <= now;
        if (!have || !lua_pkg_valid_id(id)) {
            continue;
        }
        if (expired && rm < 4) {
            strlcpy(s_job_rm[rm++], id, sizeof(s_job_rm[0]));     // a quest app ran out: drop it
        } else if (strcmp(have, ver) && s_updates_n < INSTALLED_MAX) {
            strlcpy(s_updates[s_updates_n].id, id, sizeof(s_updates[0].id));
            strlcpy(s_updates[s_updates_n].version, ver, sizeof(s_updates[0].version));
            s_updates_n++;
        }
    }
    cJSON_Delete(j);
    if (rm) {
        scan_installed();
    }
    if (!s_detail && !s_kb) {
        render_list();
    }
}

static void load_all(void)
{
    s_loaded = true;            // until an answer says otherwise
    fetch_page(false);
    s_tok_lib = ++s_tok;
    os_api_call("GET", "/api/device/library", NULL, 0, library_cb, (void *)(uintptr_t)s_tok_lib);
}

// ---------------------------------------------------------------------------
// Details, buying, installing

static void render_action(void)
{
    if (!s_detail || !s_action) {
        return;
    }
    const entry_t *e = &s_detail->e;
    char t[64], price[24];
    const char *have = installed_version(e->id);
    if (s_tok_dl && !strcmp(s_dl_id, e->id)) {
        strlcpy(t, "Installing...", sizeof(t));
    } else if (s_tok_buy) {
        strlcpy(t, "Buying...", sizeof(t));
    } else if (have && strcmp(have, e->version)) {
        snprintf(t, sizeof(t), "SELECT update to v%s", e->version);
    } else if (have) {
        strlcpy(t, "SELECT open", sizeof(t));
    } else if (e->price > 0 && !e->owned) {
        price_text(price, sizeof(price), e->price, false);
        snprintf(t, sizeof(t), "SELECT buy for %s", price);
    } else {
        strlcpy(t, "SELECT install", sizeof(t));
    }
    lv_label_set_text(s_action, t);
}

static void close_detail(void)
{
    if (s_detail_box) {
        lv_obj_delete(s_detail_box);
    }
    s_detail_box = s_action = NULL;
    free(s_detail);
    s_detail = NULL;
    s_tok_detail = 0;
    render_list();
}

static void show_detail(void)
{
    detail_t *d = s_detail;
    lv_obj_t *box = s_detail_box;
    lv_obj_clean(box);
    char line[96];
    lv_obj_t *t = ui_label(box, &sol_font_20, COL_TEXT, "");
    lv_label_set_text_fmt(t, "%s  %s", d->e.symbol, d->e.name);
    lv_obj_set_pos(t, 10, 4);
    lv_obj_set_width(t, OS_ROOT_W - 20);
    lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_DOTS);
    snprintf(line, sizeof(line), "v%s  by %s", d->e.version, d->author[0] ? d->author : "?");
    lv_obj_t *m = ui_label(box, &sol_mono_12, COL_DIM, line);
    lv_obj_set_pos(m, 10, 32);
    char num[16];
    if (d->rating10 >= 0) {
        snprintf(line, sizeof(line), "%d.%d/5 (%d)  " LV_SYMBOL_BULLET "  %s installs", d->rating10 / 10,
                 d->rating10 % 10, d->ratings, ui_fmt_num(num, sizeof(num), d->installs));
    } else {
        snprintf(line, sizeof(line), "No ratings yet  " LV_SYMBOL_BULLET "  %s installs",
                 ui_fmt_num(num, sizeof(num), d->installs));
    }
    m = ui_label(box, &sol_mono_12, COL_DIM, line);
    lv_obj_set_pos(m, 10, 48);

    // Scrollable body: summary, description, permissions.
    lv_obj_t *body = lv_obj_create(box);
    lv_obj_remove_style_all(body);
    lv_obj_set_pos(body, 0, 66);
    lv_obj_set_size(body, OS_ROOT_W, OS_ROOT_H - 66 - UI_HINT_H - 22);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_hor(body, 10, 0);
    lv_obj_set_style_pad_row(body, 6, 0);
    lv_obj_set_scrollbar_mode(body, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_t *s = ui_label(body, &sol_font_14, COL_TEXT, d->e.summary);
    lv_obj_set_width(s, lv_pct(100));
    if (d->description[0]) {
        s = ui_label(body, &sol_font_14, COL_DIM, d->description);
        lv_obj_set_width(s, lv_pct(100));
    }
    ui_label(body, &sol_mono_12, COL_DIM, d->perms ? "It can:" : "It needs no permissions.");
    for (int b = 0; b < LUA_PERM_COUNT; b++) {
        if (d->perms & (1u << b)) {
            snprintf(line, sizeof(line), LV_SYMBOL_BULLET "  %s", lua_perm_text(b));
            ui_label(body, &sol_font_14, COL_TEXT, line);
        }
    }
    lv_obj_set_user_data(box, body);

    lv_obj_t *bar = lv_obj_create(box);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, OS_ROOT_W, 22);
    lv_obj_align(bar, LV_ALIGN_BOTTOM_LEFT, 0, -UI_HINT_H);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_width(bar, 1, 0);
    lv_obj_set_style_border_color(bar, ui_col(COL_BORDER), 0);
    s_action = ui_label(bar, &sol_font_14, COL_GREEN, "");
    lv_obj_align(s_action, LV_ALIGN_LEFT_MID, 10, 1);
    ui_hint(box, LV_SYMBOL_UP LV_SYMBOL_DOWN " scroll   CANCEL back");
    render_action();
}

static void detail_cb(int status, const char *body, void *ctx)
{
    if ((uintptr_t)ctx != s_tok_detail || !s_detail || !s_open) {
        return;
    }
    cJSON *j = status == 200 ? cJSON_Parse(body) : NULL;
    if (!cJSON_IsObject(j)) {
        cJSON_Delete(j);
        os_toast(status == 0 ? "Can't reach the server" : "The store didn't answer");
        close_detail();
        return;
    }
    detail_t *d = s_detail;
    jstr(j, "name", d->e.name, sizeof(d->e.name));
    jstr(j, "summary", d->e.summary, sizeof(d->e.summary));
    jstr(j, "version", d->e.version, sizeof(d->e.version));
    jstr(j, "description", d->description, sizeof(d->description));
    jstr(j, "category", d->category, sizeof(d->category));
    const cJSON *icon = cJSON_GetObjectItemCaseSensitive(j, "icon");
    d->e.symbol = lua_pkg_symbol(cJSON_IsString(icon) ? icon->valuestring : NULL);
    d->e.price = jint(j, "price", 0);
    d->e.owned = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(j, "owned"));
    d->installs = jint(j, "installs", 0);
    d->ratings = jint(j, "ratings", 0);
    const cJSON *rating = cJSON_GetObjectItemCaseSensitive(j, "rating");
    d->rating10 = cJSON_IsNumber(rating) ? (int)(rating->valuedouble * 10 + 0.5) : -1;
    const cJSON *author = cJSON_GetObjectItemCaseSensitive(j, "author");
    char user[24] = "";
    jstr(author, "username", user, sizeof(user));
    if (user[0]) {
        snprintf(d->author, sizeof(d->author), "@%s", user);
    } else {
        jstr(author, "name", d->author, sizeof(d->author));
    }
    const cJSON *perm;
    cJSON_ArrayForEach(perm, cJSON_GetObjectItemCaseSensitive(j, "permissions")) {
        d->perms |= cJSON_IsString(perm) ? lua_perm_parse(perm->valuestring) : 0;
    }
    cJSON_Delete(j);
    show_detail();
}

static void open_detail(const char *id)
{
    s_detail = calloc(1, sizeof(*s_detail));
    if (!s_detail) {
        os_toast("Not enough memory");
        return;
    }
    strlcpy(s_detail->e.id, id, sizeof(s_detail->e.id));
    s_detail_box = lv_obj_create(s_root);
    lv_obj_remove_style_all(s_detail_box);
    lv_obj_set_size(s_detail_box, OS_ROOT_W, OS_ROOT_H);
    lv_obj_set_style_bg_opa(s_detail_box, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_detail_box, ui_col(COL_BG), 0);
    lv_obj_set_scrollable(s_detail_box, false);
    lv_obj_t *l = ui_label(s_detail_box, &sol_font_14, COL_DIM, "Loading...");
    lv_obj_center(l);
    ui_hint(s_detail_box, "CANCEL back");
    if (s_offline) {
        lv_obj_move_foreground(s_offline);
    }
    char path[80];
    snprintf(path, sizeof(path), "/api/device/apps/%s", id);
    s_tok_detail = ++s_tok;
    if (os_api_call("GET", path, NULL, 0, detail_cb, (void *)(uintptr_t)s_tok_detail) != ESP_OK) {
        os_toast("Can't reach the server");
        close_detail();
    }
}

// Download answer: body is the decoded X-App-Manifest. Runs even after the
// store closed (the request isn't cancelled), then touches only the card.
static void download_cb(int status, const char *body, void *ctx)
{
    if ((uintptr_t)ctx != s_tok_dl) {
        return;
    }
    s_tok_dl = 0;
    char rel[64];
    lua_pkg_t pkg;
    bool ok = status == 200 && body && lua_pkg_parse(body, s_dl_id, &pkg);
    if (ok) {
        lua_pkg_rel(rel, sizeof(rel), s_dl_id, "app.json");
        ok = os_data_write(rel, body, strlen(body));
    }
    if (!ok) {
        if (status == 0) {
            os_toast("Download failed. Check Wi-Fi and the card");
        } else if (status == 403) {
            os_toast("You don't own this app");
        } else {
            os_toast("Download failed (%d)", status);
        }
        lua_pkg_rel(rel, sizeof(rel), s_dl_id, "main.lua");
        if (status == 200 && !lua_pkg_load(s_dl_id, &pkg)) {
            os_data_remove(rel);            // a new main.lua without any manifest
        }
    } else {
        os_toast("Installed %s", s_dl_name);
    }
    if (!s_open) {
        return;
    }
    if (ok) {
        // Note the new version without rescanning the card.
        int i = 0;
        while (i < s_installed_n && strcmp(s_installed[i].id, pkg.id)) {
            i++;
        }
        if (i < INSTALLED_MAX && s_installed) {
            strlcpy(s_installed[i].id, pkg.id, sizeof(s_installed[i].id));
            strlcpy(s_installed[i].version, pkg.version, sizeof(s_installed[i].version));
            if (i == s_installed_n) {
                s_installed_n++;
            }
        }
        for (int u = 0; u < s_updates_n; u++) {
            if (!strcmp(s_updates[u].id, pkg.id)) {
                s_updates[u] = s_updates[--s_updates_n];
                break;
            }
        }
    }
    if (s_detail) {
        render_action();
    } else if (!s_kb) {
        render_list();
    }
}

static void install(const entry_t *e)
{
    char rel[64], path[160], src[80];
    if (!os_data_ready()) {
        os_toast("Insert a microSD card");
        return;
    }
    if (s_tok_dl) {
        os_toast("Still installing %s", s_dl_name);
        return;
    }
    lua_pkg_rel(rel, sizeof(rel), e->id, "main.lua");
    os_data_mkdirs(rel);
    os_data_path(path, sizeof(path), rel);
    snprintf(src, sizeof(src), "/api/device/apps/%s/source", e->id);
    strlcpy(s_dl_id, e->id, sizeof(s_dl_id));
    strlcpy(s_dl_name, e->name, sizeof(s_dl_name));
    s_tok_dl = ++s_tok;
    if (os_api_download(src, path, LUA_PKG_SRC_MAX, download_cb, (void *)(uintptr_t)s_tok_dl) != ESP_OK) {
        s_tok_dl = 0;
        os_toast("Can't reach the server");
    }
    render_action();
}

static void buy_cb(int status, const char *body, void *ctx)
{
    if ((uintptr_t)ctx != s_tok_buy || !s_open) {
        return;
    }
    s_tok_buy = 0;
    cJSON *j = status == 200 ? cJSON_Parse(body) : NULL;
    const cJSON *bal = cJSON_GetObjectItemCaseSensitive(j, "balance");
    bool owned = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(j, "owned"));
    if (cJSON_IsNumber(bal)) {
        os_cloud_info_t *in = malloc(sizeof(*in));
        if (in) {
            os_cloud_get_info(in);
            os_cloud_note_lamps(bal->valueint, in->level);
            free(in);
        }
    }
    cJSON_Delete(j);
    if (!owned) {
        if (status == 402) {
            os_toast("Not enough !LAMPs");
        } else if (status == 0) {
            os_toast("Can't reach the server");
        } else {
            os_toast("Couldn't buy it (%d)", status);
        }
        render_action();
        return;
    }
    if (s_detail) {
        s_detail->e.owned = true;
        for (int i = 0; i < s_n; i++) {
            if (!strcmp(s_entries[i].id, s_detail->e.id)) {
                s_entries[i].owned = true;
            }
        }
        install(&s_detail->e);
    }
}

static void buy_confirmed(bool yes, void *ctx)
{
    if (!yes || !s_open || !s_detail || (uintptr_t)ctx != s_tok_detail) {
        return;
    }
    char path[80];
    snprintf(path, sizeof(path), "/api/device/apps/%s/buy", s_detail->e.id);
    s_tok_buy = ++s_tok;
    if (os_api_call("POST", path, "{}", 0, buy_cb, (void *)(uintptr_t)s_tok_buy) != ESP_OK) {
        s_tok_buy = 0;
        os_toast("Can't reach the server");
    }
    render_action();
}

static void detail_select(void)
{
    const entry_t *e = &s_detail->e;
    if (!e->version[0] || s_tok_buy || (s_tok_dl && !strcmp(s_dl_id, e->id))) {
        return;                 // still loading, or busy
    }
    const char *have = installed_version(e->id);
    if (have && !strcmp(have, e->version)) {
        lua_host_launch(e->id);
    } else if (!have && e->price > 0 && !e->owned) {
        char q[96], price[24];
        snprintf(q, sizeof(q), "Buy %s for %s?", e->name, ui_fmt_lamps(price, sizeof(price), e->price));
        os_confirm(q, buy_confirmed, (void *)(uintptr_t)s_tok_detail);
    } else {
        install(e);
    }
}

// ---------------------------------------------------------------------------

static void enter(lv_obj_t *root)
{
    if (!s_mx) {
        s_mx = xSemaphoreCreateMutex();
    }
    s_root = root;
    s_entries = malloc(ENTRIES_MAX * sizeof(*s_entries));
    s_installed = malloc(INSTALLED_MAX * sizeof(*s_installed));
    s_updates = malloc(INSTALLED_MAX * sizeof(*s_updates));
    s_n = s_updates_n = 0;
    s_installed_n = -1;
    s_loaded = s_loading = s_updates_only = false;
    s_err = s_entries && s_installed && s_updates ? NULL : "Not enough memory";
    s_detail = NULL;
    s_kb = NULL;

    lv_obj_t *head = lv_obj_create(root);
    lv_obj_remove_style_all(head);
    lv_obj_set_size(head, OS_ROOT_W, HEAD_H);
    lv_obj_set_style_border_side(head, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(head, 1, 0);
    lv_obj_set_style_border_color(head, ui_col(COL_BORDER), 0);
    s_cat_l = ui_label(head, &sol_font_14, COL_BLUE, "");
    lv_obj_align(s_cat_l, LV_ALIGN_LEFT_MID, 8, 0);
    s_sort_l = ui_label(head, &sol_mono_12, COL_DIM, "");
    lv_obj_align(s_sort_l, LV_ALIGN_RIGHT_MID, -8, 0);
    s_list = ui_list_create(root, HEAD_H, BROWSE_H, row, NULL);
    s_hint = ui_hint(root, "");
    s_offline = ui_offline_create(root, 0, OS_ROOT_H - UI_HINT_H, true);
    xSemaphoreTake(s_mx, portMAX_DELAY);
    s_open = true;
    xSemaphoreGive(s_mx);
    render_list();
    if (!s_err) {
        scan_installed();
    }
}

static void leave(void)
{
    xSemaphoreTake(s_mx, portMAX_DELAY);
    s_open = false;
    free(s_inbox);
    s_inbox = NULL;
    xSemaphoreGive(s_mx);
    // The download, if any, goes on: its answer writes app.json.
    uint32_t toks[] = {s_tok_list, s_tok_lib, s_tok_detail, s_tok_buy};
    for (size_t i = 0; i < sizeof(toks) / sizeof(toks[0]); i++) {
        if (toks[i]) {
            os_api_cancel((void *)(uintptr_t)toks[i]);
        }
    }
    s_tok_list = s_tok_lib = s_tok_detail = s_tok_buy = 0;
    s_kb = NULL;            // deleted with the root
    free(s_detail);
    s_detail = NULL;
    s_detail_box = s_action = NULL;
    free(s_entries);
    free(s_installed);
    free(s_updates);
    s_entries = NULL;
    s_installed = s_updates = NULL;
    s_installed_n = -1;
    s_n = s_updates_n = 0;
}

static void tick(void)
{
    xSemaphoreTake(s_mx, portMAX_DELAY);
    ver_t *in = s_inbox;
    int n = s_inbox_n;
    s_inbox = NULL;
    xSemaphoreGive(s_mx);
    if (in) {
        memcpy(s_installed, in, n * sizeof(*in));
        s_installed_n = n;
        free(in);
        if (s_detail) {
            render_action();
        } else if (!s_kb) {
            render_list();
        }
    }
    bool was_hidden = lv_obj_is_hidden(s_offline);
    bool online = ui_offline_update(s_offline) == UI_NET_OK;
    if (online != was_hidden && !s_detail && !s_kb) {
        render_list();          // the hint follows the offline panel
    }
    if (online && !s_loaded && s_installed_n >= 0 && !s_err) {
        load_all();
    } else if (!online && s_loaded && !s_loading) {
        s_loaded = false;       // reload when the badge is back online
    }
}

static bool list_key(os_key_t k, bool repeat)
{
    int sel = ui_list_selected(s_list), c = ctrl_count();
    switch (k) {
    case OS_KEY_UP:
    case OS_KEY_DOWN:
        ui_list_key(s_list, k, repeat);
        if (!s_updates_only && ui_list_selected(s_list) == row_count() - 1 && s_next[0] && !s_loading) {
            fetch_page(true);
        }
        return true;
    case OS_KEY_LEFT:
    case OS_KEY_RIGHT:
        if (!s_updates_only && !repeat) {
            s_cat = (s_cat + (k == OS_KEY_RIGHT ? 1 : CAT_COUNT - 1)) % CAT_COUNT;
            fetch_page(false);
            ui_list_select(s_list, 0);
        }
        return true;
    case OS_KEY_BACK:
        if (s_updates_only) {
            s_updates_only = false;
            render_list();
            ui_list_select(s_list, CTRL_UPDATES);
            return true;
        }
        return false;
    case OS_KEY_SELECT:
        if (sel < 0) {
            return true;
        }
        if (s_updates_only) {
            open_detail(s_updates[sel].id);
        } else if (sel == CTRL_SEARCH) {
            s_kb = ui_kb_create(s_root, "Search the App Store", sizeof(s_q) - 1);
        } else if (sel == CTRL_SORT) {
            s_sort = (s_sort + 1) % 3;
            fetch_page(false);
        } else if (sel == CTRL_UPDATES && c == 3) {
            s_updates_only = true;
            render_list();
            ui_list_select(s_list, 0);
        } else if (sel >= c) {
            open_detail(s_entries[sel - c].id);
        }
        return true;
    default:
        return false;
    }
}

static bool key(os_key_t k, bool repeat)
{
    if (!s_entries || !s_installed || !s_updates) {
        return k != OS_KEY_BACK;        // "Not enough memory" is on screen
    }
    if (ui_offline_key(s_offline, k)) {
        return true;
    }
    if (!lv_obj_is_hidden(s_offline)) {
        return k != OS_KEY_BACK;
    }
    if (s_kb) {
        ui_kb_result_t r = ui_kb_key(s_kb, k, repeat);
        if (r != UI_KB_BUSY) {
            if (r == UI_KB_DONE) {
                strlcpy(s_q, ui_kb_text(s_kb), sizeof(s_q));
            }
            ui_kb_delete(s_kb);
            s_kb = NULL;
            if (r == UI_KB_DONE) {
                fetch_page(false);
            }
            render_list();
        }
        return true;
    }
    if (s_detail) {
        lv_obj_t *body = s_detail_box ? lv_obj_get_user_data(s_detail_box) : NULL;
        if (k == OS_KEY_BACK && !repeat) {
            close_detail();
        } else if (k == OS_KEY_SELECT) {
            detail_select();
        } else if ((k == OS_KEY_UP || k == OS_KEY_DOWN) && body) {
            lv_obj_scroll_by_bounded(body, 0, k == OS_KEY_UP ? 40 : -40, LV_ANIM_ON);
        }
        return true;
    }
    return list_key(k, repeat);
}

const os_app_t app_store = {
    .id = "store",
    .name = "App Store",
    .symbol = LV_SYMBOL_DOWNLOAD,
    .accent = COL_BLUE,
    .on_enter = enter,
    .on_exit = leave,
    .on_key = key,
    .on_tick = tick,
};
