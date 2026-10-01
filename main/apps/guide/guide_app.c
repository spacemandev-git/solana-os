// Guide: the Hitchhiker's Guide to Solana pages you've collected, the packs
// you haven't opened yet and the shop (server/API.md "Device API").
//
// Collection: categories with owned/total -> the pages of one category
// (locked ones read "???") -> a page reader. Packs: SELECT opens one and
// reveals its pages one card at a time, with an LED flash in the rarity
// colour. Shop: buy packs with !LAMPs.
//
// Every list shows the last good server answer from /sdcard/hh/cache/ first
// ("synced 5 min ago") and refreshes when online. Page bodies are fetched
// once and kept in /sdcard/hh/guide/<id>.txt.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "core/os.h"
#include "core/os_api.h"
#include "core/os_cloud.h"
#include "core/os_data.h"
#include "core/os_wifi.h"
#include "core/ui.h"
#include "game/game.h"

#define HEAD_H      18
#define LIST_Y      (UI_TABS_H + HEAD_H)
#define LIST_H      (OS_ROOT_H - LIST_Y - UI_HINT_H)
#define VIEW_H      (OS_ROOT_H - UI_HINT_H)
#define PAGES_MAX   64
#define PACKS_MAX   10
#define SHOP_MAX    12
#define REVEAL_MAX  10
#define BODY_MAX    1600
#define SCROLL_STEP 40
#define BUY_ARM_MS  3000

enum { TAB_COLLECTION, TAB_PACKS, TAB_SHOP, TAB_COUNT };
static const char *const tab_names[TAB_COUNT] = {"Collection", "Packs", "Shop"};
typedef enum { V_TABS, V_PAGES, V_READ, V_REVEAL, V_WELCOME } view_t;

// docs/HHGTS.md "One look": category and rarity colours.
static const struct {
    const char *id, *name;
    uint32_t col;
} cats[] = {
    {"events", "Events", COL_CAT_EVENTS},          {"culture", "Culture", COL_CAT_CULTURE},
    {"tech", "Tech docs", COL_CAT_TECH},           {"releases", "Releases", COL_CAT_RELEASES},
    {"ecosystem", "Ecosystem", COL_CAT_ECOSYSTEM}, {"history", "History", COL_CAT_HISTORY},
    {"people", "People", COL_CAT_PEOPLE},          {"lore", "Lore", COL_CAT_LORE},
};
#define CAT_COUNT   ((int)(sizeof(cats) / sizeof(cats[0])))

static const struct {
    const char *id;
    uint32_t col;
} rarities[] = {
    {"common", COL_RARITY_COMMON}, {"rare", COL_RARITY_RARE}, {"epic", COL_RARITY_EPIC}, {"legendary", COL_RARITY_LEGEND},
};
enum { R_COMMON, R_RARE, R_EPIC, R_LEGENDARY };

typedef struct {
    char id[49];
    char title[49];
    int number, count;
    uint8_t cat, rarity;
    bool fresh;                 // first copy, from a pack just opened
} page_t;

typedef struct {
    page_t page;
    char summary[144];
} card_t;

typedef struct {
    char id[24];
    char name[33];
    char from[12];              // welcome | shop | quest | trade | admin
    char sku[24];
    int size;
} pack_t;

typedef struct {
    char sku[24];
    char name[33];
    char desc[96];
    int price;
    bool available;
} item_t;

typedef struct {
    lv_obj_t *root, *home, *tabs, *head, *hint, *view;
    ui_list_t *list;
    int tab, tick;
    view_t mode;
    bool touched;               // the user moved: don't jump to Packs any more
    bool welcomed;              // the welcome showed this time
    bool was_online;
    // Collection
    struct { int owned, total; } catn[CAT_COUNT];
    int owned, total;
    int64_t guide_at;
    // Pages of one category
    int cat;
    page_t *pages;
    int npages;
    char cursor[48];
    bool loading;
    int64_t pages_at;
    // Reader
    lv_obj_t *body_scroll, *body;
    char reading[49];
    // Packs
    pack_t packs[PACKS_MAX];
    int npacks;
    int64_t packs_at;
    bool opening;
    // Reveal
    card_t *cards;
    int ncards, shown;
    // Shop
    item_t items[SHOP_MAX];
    int nitems;
    int64_t shop_at;
    int64_t buy_armed;
    int buy_sel;
} guide_ui_t;

static guide_ui_t *s;

static void set_hint(void);
static void show_tab(int tab);
static void tab_render(void);
static void welcome_open(void);

static int cat_index(const char *id)
{
    for (int i = 0; i < CAT_COUNT; i++) {
        if (!strcmp(cats[i].id, id)) {
            return i;
        }
    }
    return 0;
}

static int rarity_index(const char *id)
{
    for (int i = 0; i < 4; i++) {
        if (!strcmp(rarities[i].id, id)) {
            return i;
        }
    }
    return R_COMMON;
}

static void page_from_json(page_t *p, const cJSON *j)
{
    memset(p, 0, sizeof(*p));
    game_ascii(p->id, sizeof(p->id), game_jstr(j, "id"));
    game_ascii(p->title, sizeof(p->title), game_jstr(j, "title"));
    p->number = game_jint(j, "number", 0);
    p->count = game_jint(j, "count", 0);
    p->cat = cat_index(game_jstr(j, "category"));
    p->rarity = rarity_index(game_jstr(j, "rarity"));
    p->fresh = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(j, "new"));
}

// Header line: what the tab shows and how fresh it is.
static void head_set(const char *left, int64_t at, bool cached)
{
    char synced[32] = "";
    if (cached && !os_api_online()) {
        game_synced(at, synced, sizeof(synced));
    }
    lv_label_set_text_fmt(s->head, "%s%s%s", left, synced[0] ? "  " LV_SYMBOL_BULLET "  " : "", synced);
}

// No data and no Wi-Fi: say so, and SELECT turns Wi-Fi on.
static const char *offline_text(void)
{
    static char text[80];
    char ssid[33];
    if (os_wifi_saved_ssid(ssid, sizeof(ssid))) {
        snprintf(text, sizeof(text), "Wi-Fi is off. SELECT to connect to %s", ssid);
    } else {
        strlcpy(text, "Wi-Fi is off. Join a network in the Wi-Fi app", sizeof(text));
    }
    return text;
}

static bool wifi_on(void)
{
    char ssid[33];
    if (os_api_online() || os_wifi_state() == OS_WIFI_CONNECTING) {
        return false;
    }
    if (os_wifi_saved_ssid(ssid, sizeof(ssid))) {
        os_wifi_set_enabled(true);
        os_toast("Connecting to %s...", ssid);
    } else {
        os_launch_id("wifi");
    }
    return true;
}

static bool not_linked(void)
{
    if (game_claimed()) {
        return false;
    }
    ui_list_set_empty(s->list, "Not linked yet: claim this badge first");
    ui_list_set_count(s->list, 0);
    return true;
}

// ---------------------------------------------------------------------------
// Collection: GET /api/device/guide

static void guide_apply(const cJSON *j)
{
    memset(s->catn, 0, sizeof(s->catn));
    const cJSON *c;
    cJSON_ArrayForEach(c, cJSON_GetObjectItemCaseSensitive(j, "categories")) {
        int i = cat_index(game_jstr(c, "id"));
        s->catn[i].owned = game_jint(c, "owned", 0);
        s->catn[i].total = game_jint(c, "total", 0);
    }
    s->owned = game_jint(j, "owned", 0);
    s->total = game_jint(j, "total", 0);
}

static void collection_render(void)
{
    if (s->tab != TAB_COLLECTION || s->mode != V_TABS) {
        return;
    }
    char left[48];
    if (s->total) {
        snprintf(left, sizeof(left), "%d of %d pages collected", s->owned, s->total);
    } else {
        left[0] = 0;
    }
    head_set(left, s->guide_at, true);
    if (not_linked()) {
        return;
    }
    ui_list_set_empty(s->list, s->total ? "Nothing here yet" : os_api_online() ? "Loading..." : offline_text());
    ui_list_set_count(s->list, s->total ? CAT_COUNT : 0);
}

static void got_guide(int status, const char *body, void *ctx)
{
    if (status == 200) {
        cJSON *j = cJSON_Parse(body);
        guide_apply(j);
        cJSON_Delete(j);
        game_cache_put("guide", body);
        s->guide_at = os_cloud_time_ms();
    }
    collection_render();
}

static void collection_load(void)
{
    cJSON *j = game_cache_get("guide", &s->guide_at);
    if (j) {
        guide_apply(j);
        cJSON_Delete(j);
    }
    if (os_api_online()) {
        os_api_call("GET", "/api/device/guide", NULL, 0, got_guide, s);
    }
}

// ---------------------------------------------------------------------------
// Pages of a category: GET /api/device/guide/pages?category=

static void pages_add(const cJSON *items)
{
    const cJSON *e;
    cJSON_ArrayForEach(e, items) {
        if (s->npages < PAGES_MAX) {
            page_from_json(&s->pages[s->npages++], e);
        }
    }
}

static void pages_render(void)
{
    if (s->mode != V_PAGES) {
        return;
    }
    char left[48];
    snprintf(left, sizeof(left), "%s  " LV_SYMBOL_BULLET "  %d of %d", cats[s->cat].name, s->catn[s->cat].owned,
             s->catn[s->cat].total);
    head_set(left, s->pages_at, true);
    lv_obj_set_style_text_color(s->head, ui_col(cats[s->cat].col), 0);
    ui_list_set_empty(s->list, s->loading ? "Loading..." : os_api_online() ? "Nothing here yet" : offline_text());
    ui_list_set_count(s->list, s->npages);
}

static void got_pages(int status, const char *body, void *ctx)
{
    bool first = !s->cursor[0];
    s->loading = false;
    if (status == 200) {
        cJSON *j = cJSON_Parse(body);
        if (first) {
            s->npages = 0;
            char name[32];
            snprintf(name, sizeof(name), "guide-%s", cats[s->cat].id);
            game_cache_put(name, body);
            s->pages_at = os_cloud_time_ms();
        }
        pages_add(cJSON_GetObjectItemCaseSensitive(j, "items"));
        strlcpy(s->cursor, game_jstr(j, "next"), sizeof(s->cursor));
        cJSON_Delete(j);
    }
    pages_render();
}

static void pages_more(void)
{
    char path[112];
    snprintf(path, sizeof(path), "/api/device/guide/pages?category=%s%s%s", cats[s->cat].id,
             s->cursor[0] ? "&cursor=" : "", s->cursor);
    if (os_api_call("GET", path, NULL, 0, got_pages, s->pages) == ESP_OK) {
        s->loading = true;
    }
}

static void pages_open(int cat)
{
    if (!s->pages && !(s->pages = calloc(PAGES_MAX, sizeof(page_t)))) {
        os_toast("Out of memory");
        return;
    }
    s->mode = V_PAGES;
    s->cat = cat;
    s->npages = 0;
    s->cursor[0] = 0;
    char name[32];
    snprintf(name, sizeof(name), "guide-%s", cats[cat].id);
    cJSON *j = game_cache_get(name, &s->pages_at);
    pages_add(cJSON_GetObjectItemCaseSensitive(j, "items"));
    cJSON_Delete(j);
    ui_list_select(s->list, 0);
    if (os_api_online()) {
        pages_more();
    }
    pages_render();
    set_hint();
}

static void pages_close(void)
{
    s->mode = V_TABS;
    os_api_cancel(s->pages);            // a page load in flight
    s->loading = false;
    lv_obj_set_style_text_color(s->head, ui_col(COL_DIM), 0);
    show_tab(TAB_COLLECTION);
    ui_list_select(s->list, s->cat);
}

// ---------------------------------------------------------------------------
// Card look (docs/HHGTS.md "Page card"): #042 in mono, title, category chip,
// a 3 px rarity edge (the gradient for legendary).

static void rarity_edge(lv_obj_t *parent, int h, int rarity)
{
    lv_obj_t *e = lv_obj_create(parent);
    lv_obj_remove_style_all(e);
    lv_obj_set_size(e, 3, h);
    lv_obj_set_style_bg_opa(e, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(e, ui_col(rarities[rarity].col), 0);
    if (rarity == R_LEGENDARY) {
        lv_obj_set_style_bg_color(e, ui_col(COL_PURPLE), 0);
        lv_obj_set_style_bg_grad_color(e, lv_color_hex(COL_GREEN), 0);
        lv_obj_set_style_bg_grad_dir(e, LV_GRAD_DIR_VER, 0);
    }
}

static lv_obj_t *chip(lv_obj_t *parent, const char *text, uint32_t col)
{
    lv_obj_t *c = ui_label(parent, &sol_mono_12, COL_BG, text);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(c, ui_col(col), 0);
    lv_obj_set_style_radius(c, 8, 0);
    lv_obj_set_style_pad_hor(c, 6, 0);
    lv_obj_set_style_pad_ver(c, 1, 0);
    return c;
}

// Number, NEW marker, title, then the category chip and rarity on one row.
static void card_head(lv_obj_t *col, const page_t *p, int width)
{
    lv_obj_t *top = lv_obj_create(col);
    lv_obj_remove_style_all(top);
    lv_obj_set_size(top, width, 16);
    char num[8];
    snprintf(num, sizeof(num), "#%03d", p->number);
    lv_obj_t *n = ui_label(top, &sol_mono_12, COL_DIM, num);
    lv_obj_align(n, LV_ALIGN_LEFT_MID, 0, 0);
    if (p->fresh) {
        lv_obj_t *nw = chip(top, "NEW", COL_GREEN);
        lv_obj_align(nw, LV_ALIGN_RIGHT_MID, 0, 0);
    }
    lv_obj_t *t = ui_label(col, &sol_font_20, COL_TEXT, p->title);
    lv_obj_set_width(t, width);
    lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_t *row = lv_obj_create(col);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, width, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 8, 0);
    chip(row, cats[p->cat].name, cats[p->cat].col);
    lv_obj_t *r = ui_label(row, &sol_mono_12, rarities[p->rarity].col, rarities[p->rarity].id);
    if (p->count > 1) {
        lv_label_set_text_fmt(r, "%s  x%d", rarities[p->rarity].id, p->count);
    }
}

static lv_obj_t *column(lv_obj_t *parent, int w, int h)
{
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_remove_style_all(c);
    lv_obj_set_size(c, w, h);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(c, 6, 0);
    return c;
}

static void view_close(void)
{
    if (s->view) {
        lv_obj_delete(s->view);
        s->view = NULL;
    }
    s->body = s->body_scroll = NULL;
    lv_obj_set_hidden(s->home, false);
}

static lv_obj_t *view_open(void)
{
    view_close();
    lv_obj_set_hidden(s->home, true);
    s->view = lv_obj_create(s->root);
    lv_obj_remove_style_all(s->view);
    lv_obj_set_size(s->view, OS_ROOT_W, VIEW_H);
    lv_obj_set_scrollable(s->view, false);
    return s->view;
}

// ---------------------------------------------------------------------------
// Reader: body from guide/<id>.txt, else GET /api/device/guide/pages/:id

static void body_set(const char *text, uint32_t col)
{
    if (s->body) {
        lv_label_set_text(s->body, text);
        lv_obj_set_style_text_color(s->body, ui_col(col), 0);
    }
}

static void got_body(int status, const char *body, void *ctx)
{
    if (status != 200) {
        body_set(status == 403 ? "Collect this page to read it." : "Couldn't load this page. Try again later.", COL_DIM);
        return;
    }
    cJSON *j = cJSON_Parse(body);
    const char *text = game_jstr(j, "body");
    if (text[0]) {
        char rel[64];
        snprintf(rel, sizeof(rel), "guide/%s.txt", game_jstr(j, "id"));
        os_data_write(rel, text, strlen(text));
        if (!strcmp(game_jstr(j, "id"), s->reading)) {
            body_set(text, COL_TEXT);
        }
    }
    cJSON_Delete(j);
}

static void reader_open(const page_t *p)
{
    s->mode = V_READ;
    strlcpy(s->reading, p->id, sizeof(s->reading));
    lv_obj_t *v = view_open();
    rarity_edge(v, VIEW_H, p->rarity);
    s->body_scroll = column(v, OS_ROOT_W - 3, VIEW_H);
    lv_obj_set_x(s->body_scroll, 3);
    lv_obj_set_style_pad_hor(s->body_scroll, 12, 0);
    lv_obj_set_style_pad_ver(s->body_scroll, 8, 0);
    lv_obj_set_scrollbar_mode(s->body_scroll, LV_SCROLLBAR_MODE_ACTIVE);
    page_t head = *p;
    head.fresh = false;
    card_head(s->body_scroll, &head, OS_ROOT_W - 30);
    s->body = ui_label(s->body_scroll, &sol_font_14, COL_DIM, "");
    lv_obj_set_width(s->body, OS_ROOT_W - 30);
    lv_label_set_long_mode(s->body, LV_LABEL_LONG_MODE_WRAP);

    char rel[64];
    snprintf(rel, sizeof(rel), "guide/%s.txt", p->id);
    char *text = os_data_read(rel, BODY_MAX, NULL);
    if (text) {
        body_set(text, COL_TEXT);
        free(text);
    } else if (os_api_online()) {
        body_set("Loading...", COL_DIM);
        char path[96];
        snprintf(path, sizeof(path), "/api/device/guide/pages/%s", p->id);
        os_api_call("GET", path, NULL, 0, got_body, s->reading);
    } else {
        body_set("Wi-Fi is off. Pages you've opened before read offline.", COL_DIM);
    }
    set_hint();
}

static void reader_close(void)
{
    view_close();
    s->mode = V_PAGES;
    s->reading[0] = 0;
    pages_render();
    set_hint();
}

// ---------------------------------------------------------------------------
// Packs: GET /api/device/packs, POST /api/device/packs/:id/open

static bool has_starter(void)
{
    for (int i = 0; i < s->npacks; i++) {
        if (!strcmp(s->packs[i].from, "welcome") || !strcmp(s->packs[i].sku, "starter")) {
            return true;
        }
    }
    return false;
}

static void packs_apply(const cJSON *j)
{
    s->npacks = 0;
    const cJSON *e;
    cJSON_ArrayForEach(e, cJSON_GetObjectItemCaseSensitive(j, "items")) {
        if (s->npacks == PACKS_MAX) {
            break;
        }
        pack_t *p = &s->packs[s->npacks++];
        game_ascii(p->id, sizeof(p->id), game_jstr(e, "id"));
        game_ascii(p->name, sizeof(p->name), game_jstr(e, "name"));
        game_ascii(p->sku, sizeof(p->sku), game_jstr(e, "sku"));
        game_ascii(p->from, sizeof(p->from), game_jstr(e, "source"));
        p->size = game_jint(e, "size", 0);
    }
}

static void packs_render(void)
{
    if (s->tab != TAB_PACKS || s->mode != V_TABS) {
        return;
    }
    char left[32];
    snprintf(left, sizeof(left), "%d unopened", s->npacks);
    head_set(left, s->packs_at, true);
    if (not_linked()) {
        return;
    }
    ui_list_set_empty(s->list, s->packs_at || os_api_online() ? "No packs. Get one in the Shop." : offline_text());
    ui_list_set_count(s->list, s->npacks);
}

static void got_packs(int status, const char *body, void *ctx)
{
    if (status == 200) {
        cJSON *j = cJSON_Parse(body);
        packs_apply(j);
        cJSON_Delete(j);
        game_cache_put("packs", body);
        s->packs_at = os_cloud_time_ms();
        os_cloud_note_packs(s->npacks);
        if (!s->touched && !s->welcomed && has_starter() && s->mode == V_TABS) {
            show_tab(TAB_PACKS);
            welcome_open();
        }
    }
    packs_render();
}

static void packs_load(void)
{
    cJSON *j = game_cache_get("packs", &s->packs_at);
    if (j) {
        packs_apply(j);
        cJSON_Delete(j);
    }
    if (os_api_online()) {
        os_api_call("GET", "/api/device/packs", NULL, 0, got_packs, s);
    }
}

static void reveal_show(void)
{
    const card_t *c = &s->cards[s->shown];
    const page_t *p = &c->page;
    lv_obj_t *v = view_open();
    lv_obj_t *card = ui_panel(v, 14, 6, OS_ROOT_W - 28, VIEW_H - 30);
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_set_style_border_color(card, ui_col(rarities[p->rarity].col), 0);
    rarity_edge(card, VIEW_H - 32, p->rarity);
    lv_obj_t *col = column(card, OS_ROOT_W - 50, VIEW_H - 40);
    lv_obj_set_pos(col, 14, 8);
    lv_obj_set_scrollable(col, false);
    if (p->rarity == R_LEGENDARY) {
        ui_gradient_bar(col, 0, 0, OS_ROOT_W - 50, 4);
    }
    card_head(col, p, OS_ROOT_W - 50);
    lv_obj_t *sum = ui_label(col, &sol_font_14, COL_DIM, c->summary);
    lv_obj_set_width(sum, OS_ROOT_W - 50);
    lv_label_set_long_mode(sum, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_t *foot = ui_label(v, &sol_mono_12, COL_DIM, "");
    lv_label_set_text_fmt(foot, "%d of %d", s->shown + 1, s->ncards);
    lv_obj_align(foot, LV_ALIGN_BOTTOM_MID, 0, -4);

    if (p->rarity == R_LEGENDARY) {
        game_led_sweep();
    } else {
        game_led_flash(rarities[p->rarity].col);
    }
    set_hint();
}

static void got_open(int status, const char *body, void *ctx)
{
    s->opening = false;
    if (status != 200) {
        cJSON *j = cJSON_Parse(body);
        const char *err = game_jstr(j, "error");
        os_toast("Couldn't open it: %s", err[0] ? err : status ? "server error" : "no connection");
        cJSON_Delete(j);
        return;
    }
    cJSON *j = cJSON_Parse(body);
    s->ncards = 0;
    if (!s->cards) {
        s->cards = calloc(REVEAL_MAX, sizeof(card_t));
    }
    const cJSON *e;
    cJSON_ArrayForEach(e, cJSON_GetObjectItemCaseSensitive(j, "pages")) {
        if (s->cards && s->ncards < REVEAL_MAX) {
            card_t *c = &s->cards[s->ncards++];
            page_from_json(&c->page, e);
            game_ascii(c->summary, sizeof(c->summary), game_jstr(e, "summary"));
        }
    }
    cJSON_Delete(j);
    if (!s->ncards) {
        os_toast("The pack was empty");
        return;
    }
    // The collection changed: drop the cached copies.
    os_data_remove("cache/guide.json");
    for (int i = 0; i < CAT_COUNT; i++) {
        char rel[40];
        snprintf(rel, sizeof(rel), "cache/guide-%s.json", cats[i].id);
        os_data_remove(rel);
    }
    s->mode = V_REVEAL;
    s->shown = 0;
    reveal_show();
}

static void pack_open(int i)
{
    if (s->opening || i < 0 || i >= s->npacks) {
        return;
    }
    if (!os_api_online()) {
        if (!wifi_on()) {
            os_toast("Packs open online");
        }
        return;
    }
    char path[80];
    snprintf(path, sizeof(path), "/api/device/packs/%s/open", s->packs[i].id);
    if (os_api_call("POST", path, NULL, 0, got_open, s) == ESP_OK) {
        s->opening = true;
        os_toast("Opening %s...", s->packs[i].name);
    }
}

static void reveal_next(void)
{
    if (++s->shown < s->ncards) {
        reveal_show();
        return;
    }
    view_close();
    s->mode = V_TABS;
    free(s->cards);
    s->cards = NULL;
    s->ncards = 0;
    packs_load();
    collection_load();
    show_tab(TAB_PACKS);
}

// ---------------------------------------------------------------------------
// Shop: GET /api/device/shop, POST /api/device/shop/buy

static void shop_apply(const cJSON *j)
{
    s->nitems = 0;
    const cJSON *e;
    cJSON_ArrayForEach(e, cJSON_GetObjectItemCaseSensitive(j, "items")) {
        if (s->nitems == SHOP_MAX) {
            break;
        }
        item_t *it = &s->items[s->nitems++];
        game_ascii(it->sku, sizeof(it->sku), game_jstr(e, "sku"));
        game_ascii(it->name, sizeof(it->name), game_jstr(e, "name"));
        game_ascii(it->desc, sizeof(it->desc), game_jstr(e, "description"));
        it->price = game_jint(e, "price", 0);
        it->available = !cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(e, "available"));
    }
}

static void shop_render(void)
{
    if (s->tab != TAB_SHOP || s->mode != V_TABS) {
        return;
    }
    os_cloud_info_t *in = malloc(sizeof(*in));
    char left[48] = "", amt[16];
    if (in) {
        os_cloud_get_info(in);
        game_fmt_lamps(amt, sizeof(amt), in->lamps);
        snprintf(left, sizeof(left), "You have %s !LAMPs", amt);
        free(in);
    }
    head_set(left, s->shop_at, true);
    if (not_linked()) {
        return;
    }
    ui_list_set_empty(s->list, s->shop_at || os_api_online() ? "The shop is empty" : offline_text());
    ui_list_set_count(s->list, s->nitems);
}

static void got_shop(int status, const char *body, void *ctx)
{
    if (status == 200) {
        cJSON *j = cJSON_Parse(body);
        shop_apply(j);
        cJSON_Delete(j);
        game_cache_put("shop", body);
        s->shop_at = os_cloud_time_ms();
    }
    shop_render();
}

static void shop_load(void)
{
    cJSON *j = game_cache_get("shop", &s->shop_at);
    if (j) {
        shop_apply(j);
        cJSON_Delete(j);
    }
    if (os_api_online()) {
        os_api_call("GET", "/api/device/shop", NULL, 0, got_shop, s);
    }
}

static void got_buy(int status, const char *body, void *ctx)
{
    cJSON *j = cJSON_Parse(body);
    if (status == 200) {
        os_cloud_info_t *in = malloc(sizeof(*in));
        if (in) {
            os_cloud_get_info(in);
            os_cloud_note_lamps(game_jint(j, "balance", in->lamps), in->level);
            free(in);
        }
        const cJSON *pack = cJSON_GetObjectItemCaseSensitive(j, "pack");
        if (cJSON_IsObject(pack)) {
            os_toast("Got %s. Open it in Packs", game_jstr(pack, "name"));
        } else {
            os_toast("Bought it");
        }
        game_led_flash(COL_LAMPS);
        packs_load();
    } else if (status == 402) {
        os_toast("Not enough !LAMPs");
    } else {
        const char *err = game_jstr(j, "error");
        os_toast("Couldn't buy it: %s", err[0] ? err : status ? "server error" : "no connection");
    }
    cJSON_Delete(j);
    shop_render();
}

static void shop_select(int i)
{
    if (i < 0 || i >= s->nitems) {
        return;
    }
    const item_t *it = &s->items[i];
    if (!it->available) {
        os_toast("Not available right now");
        return;
    }
    if (!os_api_online()) {
        if (!wifi_on()) {
            os_toast("The shop needs Wi-Fi");
        }
        return;
    }
    // Spending: SELECT twice.
    int64_t now = game_now_ms();
    if (!s->buy_armed || s->buy_sel != i || now - s->buy_armed > BUY_ARM_MS) {
        char amt[16];
        game_fmt_lamps(amt, sizeof(amt), it->price);
        s->buy_armed = now;
        s->buy_sel = i;
        os_toast("SELECT again to buy %s for %s !LAMPs", it->name, amt);
        return;
    }
    s->buy_armed = 0;
    char body[64];
    snprintf(body, sizeof(body), "{\"sku\":\"%s\"}", it->sku);
    if (os_api_call("POST", "/api/device/shop/buy", body, 0, got_buy, s) == ESP_OK) {
        os_toast("Buying %s...", it->name);
    }
}

// ---------------------------------------------------------------------------
// Lists

static void row(int i, ui_row_t *r, void *ctx)
{
    if (s->mode == V_PAGES) {
        const page_t *p = &s->pages[i];
        if (!p->count) {
            lv_snprintf(r->title, sizeof(r->title), "#%03d  %s", p->number, p->title[0] ? p->title : "???");
            r->title_color = COL_DIM;
            strlcpy(r->sub, "Not collected yet", sizeof(r->sub));
            return;
        }
        lv_snprintf(r->title, sizeof(r->title), "#%03d  %s", p->number, p->title);
        if (p->count > 1) {
            snprintf(r->sub, sizeof(r->sub), "%s  " LV_SYMBOL_BULLET "  %d copies", cats[p->cat].name, p->count);
        } else {
            strlcpy(r->sub, cats[p->cat].name, sizeof(r->sub));
        }
        strlcpy(r->right, rarities[p->rarity].id, sizeof(r->right));
        r->right_color = rarities[p->rarity].col;
        return;
    }
    switch (s->tab) {
    case TAB_COLLECTION:
        strlcpy(r->title, cats[i].name, sizeof(r->title));
        r->title_color = cats[i].col;
        if (s->catn[i].owned) {
            snprintf(r->sub, sizeof(r->sub), "%d of %d pages", s->catn[i].owned, s->catn[i].total);
        } else {
            strlcpy(r->sub, "Nothing collected yet", sizeof(r->sub));
        }
        snprintf(r->right, sizeof(r->right), "%d/%d", s->catn[i].owned, s->catn[i].total);
        r->right_color = s->catn[i].total && s->catn[i].owned == s->catn[i].total ? COL_GREEN : COL_DIM;
        break;
    case TAB_PACKS: {
        const pack_t *p = &s->packs[i];
        strlcpy(r->title, p->name, sizeof(r->title));
        snprintf(r->sub, sizeof(r->sub), "%d pages  " LV_SYMBOL_BULLET "  %s", p->size,
                 !strcmp(p->from, "welcome") ? "welcome gift" : !strcmp(p->from, "shop") ? "from the shop"
                 : !strcmp(p->from, "quest") ? "quest reward" : "gift");
        strlcpy(r->right, "SELECT open", sizeof(r->right));
        r->right_color = COL_LIME;
        break;
    }
    case TAB_SHOP: {
        const item_t *it = &s->items[i];
        char amt[16];
        strlcpy(r->title, it->name, sizeof(r->title));
        game_clip(r->sub, sizeof(r->sub), it->desc);
        if (it->available) {
            game_fmt_lamps(amt, sizeof(amt), it->price);
            lv_snprintf(r->right, sizeof(r->right), "%s !LAMPs", amt);
            r->right_color = COL_LAMPS;
        } else {
            strlcpy(r->right, "sold out", sizeof(r->right));
            r->title_color = COL_DIM;
        }
        break;
    }
    }
}

static void set_hint(void)
{
    const char *h = "";
    switch (s->mode) {
    case V_PAGES:
        h = "SELECT read  CANCEL back";
        break;
    case V_READ:
        h = LV_SYMBOL_UP LV_SYMBOL_DOWN " scroll  CANCEL back";
        break;
    case V_REVEAL:
        h = s->shown + 1 < s->ncards ? "SELECT next card" : "SELECT done";
        break;
    case V_WELCOME:
        h = "SELECT open your Starter Pack  CANCEL later";
        break;
    default:
        h = s->tab == TAB_COLLECTION ? "SELECT open  " LV_SYMBOL_LEFT LV_SYMBOL_RIGHT " tabs  CANCEL exit"
          : s->tab == TAB_PACKS      ? "SELECT open pack  " LV_SYMBOL_LEFT LV_SYMBOL_RIGHT " tabs  CANCEL exit"
                                     : "SELECT buy  " LV_SYMBOL_LEFT LV_SYMBOL_RIGHT " tabs  CANCEL exit";
        break;
    }
    lv_label_set_text(s->hint, h);
}

static void tab_render(void)
{
    switch (s->tab) {
    case TAB_COLLECTION: collection_render(); break;
    case TAB_PACKS:      packs_render(); break;
    default:             shop_render(); break;
    }
    set_hint();
}

static void show_tab(int tab)
{
    s->tab = tab;
    ui_tabs_select(s->tabs, tab);
    ui_list_select(s->list, 0);
    tab_render();
}

// First visit with the welcome gift unopened.
static void welcome_open(void)
{
    s->welcomed = true;
    s->mode = V_WELCOME;
    lv_obj_t *v = view_open();
    lv_obj_t *col = column(v, OS_ROOT_W - 32, VIEW_H - 16);
    lv_obj_set_pos(col, 16, 16);
    lv_obj_set_style_pad_row(col, 10, 0);
    lv_obj_t *t = ui_label(col, &sol_font_20, COL_TEXT, "Welcome to the Hitchhiker's Guide to Solana.");
    lv_obj_set_width(t, OS_ROOT_W - 32);
    lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_WRAP);
    ui_label(col, &sol_font_32, COL_LIME, "Don't panic.");
    ui_gradient_bar(col, 0, 0, 120, 3);
    lv_obj_t *b = ui_label(col, &sol_font_14, COL_DIM,
                           "Your Starter Pack is waiting. Open it to collect your first pages of the Guide.");
    lv_obj_set_width(b, OS_ROOT_W - 32);
    lv_label_set_long_mode(b, LV_LABEL_LONG_MODE_WRAP);
    set_hint();
}

static void welcome_key(os_key_t k, bool repeat)
{
    if (repeat || (k != OS_KEY_SELECT && k != OS_KEY_BACK)) {
        return;
    }
    view_close();
    s->mode = V_TABS;
    show_tab(TAB_PACKS);
    if (k == OS_KEY_SELECT) {
        for (int i = 0; i < s->npacks; i++) {
            if (!strcmp(s->packs[i].from, "welcome") || !strcmp(s->packs[i].sku, "starter")) {
                ui_list_select(s->list, i);
                pack_open(i);
                break;
            }
        }
    }
}

static void enter(lv_obj_t *root)
{
    s = calloc(1, sizeof(*s));
    if (!s) {
        ui_label(root, &sol_font_14, COL_ERR, "Out of memory");
        return;
    }
    s->root = root;
    s->home = lv_obj_create(root);
    lv_obj_remove_style_all(s->home);
    lv_obj_set_size(s->home, OS_ROOT_W, VIEW_H);
    lv_obj_set_scrollable(s->home, false);
    s->tabs = ui_tabs(s->home, tab_names, TAB_COUNT);
    s->head = ui_label(s->home, &sol_mono_12, COL_DIM, "");
    lv_obj_set_pos(s->head, 8, UI_TABS_H + 3);
    lv_obj_set_width(s->head, OS_ROOT_W - 16);
    lv_label_set_long_mode(s->head, LV_LABEL_LONG_MODE_DOTS);
    s->list = ui_list_create(s->home, LIST_Y, LIST_H, row, NULL);
    s->hint = ui_hint(root, "");

    s->was_online = os_api_online();
    collection_load();
    packs_load();
    shop_load();
    show_tab(has_starter() ? TAB_PACKS : TAB_COLLECTION);
    if (has_starter()) {
        welcome_open();
    }
}

static void leave(void)
{
    if (!s) {
        return;
    }
    os_api_cancel(s);
    os_api_cancel(s->pages);
    os_api_cancel(s->reading);
    free(s->pages);
    free(s->cards);
    free(s);
    s = NULL;
}

static bool tabs_key(os_key_t k, bool repeat)
{
    if (ui_list_key(s->list, k, repeat)) {
        return true;
    }
    int sel = ui_list_selected(s->list);
    switch (k) {
    case OS_KEY_LEFT:
    case OS_KEY_RIGHT:
        show_tab((s->tab + (k == OS_KEY_RIGHT ? 1 : TAB_COUNT - 1)) % TAB_COUNT);
        return true;
    case OS_KEY_SELECT:
        if (repeat) {
            return true;
        }
        if (sel < 0 && !game_claimed()) {
            return true;
        }
        if (sel < 0) {
            wifi_on();
        } else if (s->tab == TAB_COLLECTION) {
            pages_open(sel);
        } else if (s->tab == TAB_PACKS) {
            pack_open(sel);
        } else {
            shop_select(sel);
        }
        return true;
    default:
        return false;
    }
}

static bool on_key(os_key_t k, bool repeat)
{
    if (!s) {
        return false;
    }
    s->touched = true;
    switch (s->mode) {
    case V_TABS:
        return tabs_key(k, repeat);
    case V_PAGES:
        if (ui_list_key(s->list, k, repeat)) {
            if (s->cursor[0] && !s->loading && os_api_online() && ui_list_selected(s->list) >= s->npages - 3) {
                pages_more();
            }
            return true;
        }
        if (k == OS_KEY_BACK && !repeat) {
            pages_close();
        } else if (k == OS_KEY_SELECT && !repeat) {
            int sel = ui_list_selected(s->list);
            if (sel >= 0 && s->pages[sel].count) {
                reader_open(&s->pages[sel]);
            } else if (sel >= 0) {
                os_toast("Not collected yet. Open packs to find it");
            } else {
                wifi_on();
            }
        }
        return true;
    case V_READ:
        if (k == OS_KEY_UP || k == OS_KEY_DOWN) {
            lv_obj_scroll_by_bounded(s->body_scroll, 0, k == OS_KEY_DOWN ? -SCROLL_STEP : SCROLL_STEP, LV_ANIM_ON);
        } else if (k == OS_KEY_BACK && !repeat) {
            reader_close();
        }
        return true;
    case V_REVEAL:
        if ((k == OS_KEY_SELECT || k == OS_KEY_BACK) && !repeat) {
            reveal_next();
        }
        return true;
    case V_WELCOME:
        welcome_key(k, repeat);
        return true;
    }
    return false;
}

static void on_tick(void)
{
    if (!s) {
        return;
    }
    // Wi-Fi came on: fetch everything again. Otherwise keep "synced" fresh.
    if (++s->tick % 20 == 0 && s->mode == V_TABS) {
        bool online = os_api_online();
        if (online && !s->was_online) {
            collection_load();
            packs_load();
            shop_load();
        }
        s->was_online = online;
        tab_render();
    }
}

const os_app_t app_guide = {
    .id = "guide",
    .name = "Guide",
    .symbol = LV_SYMBOL_FILE,
    .accent = COL_LIME,
    .on_enter = enter,
    .on_exit = leave,
    .on_key = on_key,
    .on_tick = on_tick,
};
