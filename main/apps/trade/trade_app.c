// Trade: swap !LAMPs, items and Guide pages with a nearby badge (game/trade).
//
// Home has two tabs: Nearby (invites first, then badges in range or bridged,
// then "Bridge a badge" for an emulator, core/os_bridge.h) and Trades
// (signed trades and whether the server settled them). During a trade the
// screen shows the other side's offer live on top and yours below; both lock,
// and the badges sign on their own. The server settles the trade when either
// badge next syncs.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "core/os.h"
#include "core/os_api.h"
#include "core/os_bridge.h"
#include "core/os_cloud.h"
#include "core/os_data.h"
#include "core/os_wifi.h"
#include "core/ui.h"
#include "game/trade.h"

#define LIST_Y          UI_TABS_H
#define LIST_H          (OS_ROOT_H - LIST_Y - UI_HINT_H)
#define PEERS_MAX       12
#define RECS_MAX        16
#define GOODS_MAX       40
#define REFRESH_MS      1000
#define THEIRS_Y        20
#define THEIRS_H        48
#define MINE_Y          (THEIRS_Y + THEIRS_H + 4)
#define MINE_H          (OS_ROOT_H - MINE_Y - UI_HINT_H)
#define LAMPS_STEP      5
#define CANCEL_ARM_MS   3000
#define GOODS_PAGES     6           // server pages followed per list
#define GOODS_FRESH_MS  (10 * 60000)
#define GOODS_WAIT_MS   20000       // longest the radio waits for the goods to load

enum { TAB_NEARBY, TAB_TRADES };
static const char *const tab_names[] = {"Nearby", "Trades"};
typedef enum { V_HOME, V_TRADE, V_PICK } view_t;

// Rows of the offer list.
enum { R_LOCK, R_LAMPS, R_ADD, R_FIXED };

typedef struct {
    bool page;
    char id[TRADE_ID_LEN + 1];
    char name[TRADE_NAME_LEN + 1];
    char detail[24];                // item kind, or rarity of a page
    int count;                      // copies owned (items: 1)
} good_t;

typedef struct {
    lv_obj_t *root, *home, *tabs, *hint;
    ui_list_t *list;
    int tab, tick;
    view_t view;
    os_mesh_peer_t peers[PEERS_MAX];
    int npeers;
    trade_invite_t invites[TRADE_INVITES_MAX];
    int ninvites;
    trade_rec_t *recs;
    int nrecs;
    // Trade screen
    lv_obj_t *screen, *title, *phase, *theirs_head, *theirs;
    ui_list_t *mine;
    char sig[96];                   // what the trade screen shows, to redraw on change
    int64_t cancel_armed;
    int ask_mine, ask_theirs;       // revisions shown when the lock / sign confirm opened
    bool sign_asked;                // the sign confirm opened for this pair of locks
    // Picker
    lv_obj_t *picker;
    ui_list_t *goods_list;
    good_t *goods;
    int ngoods;
    bool goods_loading;
    int goods_pages;
    char goods_cursor[48];
    bool goods_items_done;
    bool dir_started;               // cache/pagedir.txt rewritten this load
    int64_t goods_at;
    bool acquired;                  // holding the mesh radio
    int64_t opened;
} trade_ui_t;

static trade_ui_t *s;

static void render_trade(bool force);
static void set_hint(void);

static int balance(void)
{
    os_cloud_info_t *in = malloc(sizeof(*in));
    int lamps = 0;
    if (in) {
        os_cloud_get_info(in);
        lamps = in->lamps;
        free(in);
    }
    return lamps;
}

// ---------------------------------------------------------------------------
// Home: Nearby and Trades

static const char *status_word(trade_rec_status_t st, uint32_t *col)
{
    switch (st) {
    case TRADE_REC_SETTLED:     *col = COL_GREEN; return "settled";
    case TRADE_REC_REJECTED:    *col = COL_ERR; return "rejected";
    case TRADE_REC_UNCONFIRMED: *col = COL_DIM; return "unconfirmed";
    default:                    *col = COL_YELLOW; return "pending";
    }
}

// The last Nearby row, once the list is ready: show the bridge code.
static bool bridge_row(int i)
{
    return s->view == V_HOME && s->tab == TAB_NEARBY && game_claimed() && s->acquired && i == s->ninvites + s->npeers;
}

static void home_row(int i, ui_row_t *r, void *ctx)
{
    if (s->tab == TAB_TRADES) {
        const trade_rec_t *t = &s->recs[i];
        snprintf(r->title, sizeof(r->title), "%s", t->label);
        if (t->status == TRADE_REC_REJECTED && t->reason[0]) {
            snprintf(r->sub, sizeof(r->sub), "%s", t->reason);
        } else if (t->status == TRADE_REC_UNCONFIRMED) {
            lv_snprintf(r->sub, sizeof(r->sub), "Waiting for %s's badge to sync", t->label);
        } else {
            lv_snprintf(r->sub, sizeof(r->sub), "Gave %s, got %s", t->gave, t->got);
        }
        strlcpy(r->right, status_word(t->status, &r->right_color), sizeof(r->right));
        return;
    }
    if (bridge_row(i)) {
        game_bridge_row(r, s->ninvites + s->npeers == 0);
        return;
    }
    if (i < s->ninvites) {
        snprintf(r->title, sizeof(r->title), "%s wants to trade", s->invites[i].label);
        r->title_color = COL_AQUA;
        strlcpy(r->sub, "SELECT join  " LV_SYMBOL_LEFT " decline", sizeof(r->sub));
        return;
    }
    const os_mesh_peer_t *p = &s->peers[i - s->ninvites];
    if (p->name[0]) {
        strlcpy(r->title, p->name, sizeof(r->title));
        snprintf(r->sub, sizeof(r->sub), "Lv %d  " LV_SYMBOL_BULLET "  %s", p->level, p->id);
    } else {
        strlcpy(r->title, "Unclaimed badge", sizeof(r->title));
        r->title_color = COL_DIM;
        snprintf(r->sub, sizeof(r->sub), "%s  " LV_SYMBOL_BULLET "  not linked yet", p->id);
    }
    strlcpy(r->right, game_peer_reach(p), sizeof(r->right));
}

static void nearby_refresh(void)
{
    int before = s->ninvites;
    s->ninvites = 0;
    for (int i = 0; i < trade_invite_count(); i++) {
        s->invites[s->ninvites++] = *trade_invite(i);
    }
    int n = os_mesh_peers(s->peers, PEERS_MAX);
    s->npeers = 0;
    for (int i = 0; i < n; i++) {
        if (s->peers[i].kind == OS_PEER_BADGE) {
            s->peers[s->npeers++] = s->peers[i];
        }
    }
    ui_list_set_count(s->list, s->ninvites + s->npeers + (game_claimed() && s->acquired));
    if (s->ninvites > before) {
        ui_list_select(s->list, 0);     // a new invite goes to the top and takes the selection
    }
}

static void trades_refresh(void)
{
    free(s->recs);
    s->nrecs = trade_records_load(&s->recs, RECS_MAX);
    ui_list_set_count(s->list, s->nrecs);
}

static void show_tab(int tab)
{
    s->tab = tab;
    ui_tabs_select(s->tabs, tab);
    ui_list_set_count(s->list, 0);      // the rows belong to the other tab until it reloads
    if (tab == TAB_NEARBY) {
        ui_list_set_empty(s->list, !game_claimed() ? "Not linked yet: claim this badge first"
                                   : !s->acquired ? "Loading your items and pages, then looking for badges..."
                                                  : "No badges nearby yet. Badges show up here when they're in range.");
        nearby_refresh();
    } else {
        ui_list_set_empty(s->list, os_data_ready() ? "Nothing here yet. Trades you sign show up here."
                                                   : "No microSD card: trades still upload, but aren't kept here.");
        trade_poll_now();
        trades_refresh();
    }
    set_hint();
}

// ---------------------------------------------------------------------------
// Tradable goods: items (inventory) and pages you own, from the server,
// cached for offline trading. The page list has every page; keep the owned ones.

static void goods_add(bool page, const char *id, const char *name, const char *detail, int count)
{
    if (s->ngoods == GOODS_MAX || !id[0] || count <= 0) {
        return;
    }
    good_t *g = &s->goods[s->ngoods++];
    g->page = page;
    strlcpy(g->id, id, sizeof(g->id));
    game_ascii(g->name, sizeof(g->name), name);
    game_ascii(g->detail, sizeof(g->detail), detail);
    g->count = count;
}

static void goods_row(int i, ui_row_t *r, void *ctx)
{
    const good_t *g = &s->goods[i];
    int in = trade_count_of(g->page, g->id);
    strlcpy(r->title, g->name, sizeof(r->title));
    if (g->page) {
        snprintf(r->sub, sizeof(r->sub), "Page  " LV_SYMBOL_BULLET "  %s  " LV_SYMBOL_BULLET "  you have %d", g->detail,
                 g->count);
    } else {
        snprintf(r->sub, sizeof(r->sub), "Item  " LV_SYMBOL_BULLET "  %s", g->detail);
    }
    if (in) {
        snprintf(r->right, sizeof(r->right), "offered x%d", in);
        r->right_color = COL_AQUA;
    }
}

// Cache: {"g":[[page, id, name, detail, count], ...]}
static void goods_save(void)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(root, "g");
    for (int i = 0; i < s->ngoods; i++) {
        const good_t *g = &s->goods[i];
        cJSON *e = cJSON_CreateArray();
        cJSON_AddItemToArray(e, cJSON_CreateBool(g->page));
        cJSON_AddItemToArray(e, cJSON_CreateString(g->id));
        cJSON_AddItemToArray(e, cJSON_CreateString(g->name));
        cJSON_AddItemToArray(e, cJSON_CreateString(g->detail));
        cJSON_AddItemToArray(e, cJSON_CreateNumber(g->count));
        cJSON_AddItemToArray(arr, e);
    }
    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (body) {
        game_cache_put("tradables", body);
    }
    cJSON_free(body);
    s->goods_at = os_cloud_time_ms();
}

static const char *arr_str(const cJSON *arr, int i)
{
    const char *v = cJSON_GetStringValue(cJSON_GetArrayItem(arr, i));
    return v ? v : "";
}

static void goods_from_cache(void)
{
    cJSON *body = game_cache_get("tradables", &s->goods_at);
    const cJSON *e;
    cJSON_ArrayForEach(e, cJSON_GetObjectItemCaseSensitive(body, "g")) {
        goods_add(cJSON_IsTrue(cJSON_GetArrayItem(e, 0)), arr_str(e, 1), arr_str(e, 2), arr_str(e, 3),
                  (int)cJSON_GetNumberValue(cJSON_GetArrayItem(e, 4)));
    }
    cJSON_Delete(body);
}

// The picker's list, if it's open.
static void goods_show(void)
{
    if (!s->goods_list) {
        return;
    }
    if (s->goods_loading) {
        ui_list_set_empty(s->goods_list, "Loading your items and pages...");
    } else if (!os_api_online() && !s->ngoods) {
        ui_list_set_empty(s->goods_list, "Wi-Fi is off and nothing is saved yet. Turn on Wi-Fi once to load your items.");
    } else {
        ui_list_set_empty(s->goods_list, "Nothing to trade yet. Items and pages you own show up here.");
    }
    ui_list_set_count(s->goods_list, s->ngoods);
}

// Page directory for trade_label() (trade.h), written from the full page
// list the goods loader reads.

static void pagedir_add(const cJSON *items)
{
    char *buf = malloc(2048);
    size_t len = 0;
    const cJSON *e;
    cJSON_ArrayForEach(e, items) {
        if (buf && len < 2048) {
            len += snprintf(buf + len, 2048 - len, "%s%s|%d|%s", len ? "\n" : "", game_jstr(e, "id"),
                            game_jint(e, "number", 0), game_jint(e, "count", 0) ? game_jstr(e, "title") : "");
        }
    }
    if (buf && len && len < 2048) {
        if (s->dir_started) {
            os_data_append(TRADE_PAGEDIR, buf);
        } else {
            buf[len++] = '\n';
            os_data_write(TRADE_PAGEDIR, buf, len);
            s->dir_started = true;
        }
    }
    free(buf);
}

static void goods_next(void);

static void goods_page(int status, const char *body, void *ctx)
{
    if (status != 200) {
        s->goods_loading = false;
        if (!s->ngoods) {
            goods_from_cache();
        }
        os_toast("Couldn't load your items: %s", status ? "server error" : "no connection");
        goods_show();
        return;
    }
    cJSON *j = cJSON_Parse(body);
    const cJSON *e;
    cJSON_ArrayForEach(e, cJSON_GetObjectItemCaseSensitive(j, "items")) {
        if (!s->goods_items_done) {
            if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(e, "tradable"))) {
                goods_add(false, game_jstr(e, "id"), game_jstr(e, "name"), game_jstr(e, "kind"), 1);
            }
        } else {
            char title[TRADE_NAME_LEN + 1];
            snprintf(title, sizeof(title), "#%03d %s", game_jint(e, "number", 0), game_jstr(e, "title"));
            goods_add(true, game_jstr(e, "id"), title, game_jstr(e, "rarity"), game_jint(e, "count", 0));
        }
    }
    if (s->goods_items_done) {
        pagedir_add(cJSON_GetObjectItemCaseSensitive(j, "items"));
    }
    strlcpy(s->goods_cursor, game_jstr(j, "next"), sizeof(s->goods_cursor));
    cJSON_Delete(j);
    if (!s->goods_cursor[0] || ++s->goods_pages >= GOODS_PAGES) {
        if (s->goods_items_done) {
            s->goods_loading = false;
            goods_save();
            goods_show();
            return;
        }
        s->goods_items_done = true;     // items done: on to pages
        s->goods_pages = 0;
        s->goods_cursor[0] = 0;
    }
    goods_next();
}

static void goods_next(void)
{
    char path[112];
    if (!s->goods_items_done) {
        snprintf(path, sizeof(path), "/api/device/inventory%s%s", s->goods_cursor[0] ? "?cursor=" : "", s->goods_cursor);
    } else {
        snprintf(path, sizeof(path), "/api/device/guide/pages?limit=20%s%s",
                 s->goods_cursor[0] ? "&cursor=" : "", s->goods_cursor);
    }
    if (os_api_call("GET", path, NULL, 0, goods_page, s) != ESP_OK) {
        goods_page(0, NULL, s);
    }
}

static void goods_load(void)
{
    if (!s->goods && !(s->goods = calloc(GOODS_MAX, sizeof(good_t)))) {
        return;
    }
    s->ngoods = 0;
    if (!os_api_online()) {
        goods_from_cache();
        return;
    }
    s->goods_loading = true;
    s->dir_started = false;
    s->goods_items_done = false;
    s->goods_pages = 0;
    s->goods_cursor[0] = 0;
    goods_next();
}

static void picker_open(void)
{
    s->view = V_PICK;
    s->picker = lv_obj_create(s->root);
    lv_obj_remove_style_all(s->picker);
    lv_obj_set_size(s->picker, OS_ROOT_W, OS_ROOT_H - UI_HINT_H);
    lv_obj_set_style_bg_opa(s->picker, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s->picker, ui_col(COL_BG), 0);
    lv_obj_set_scrollable(s->picker, false);
    lv_obj_t *t = ui_label(s->picker, &sol_mono_12, COL_DIM, "ADD TO YOUR OFFER");
    lv_obj_set_pos(t, 8, 4);
    s->goods_list = ui_list_create(s->picker, 20, OS_ROOT_H - UI_HINT_H - 20, goods_row, NULL);
    if (!s->goods || (!s->ngoods && !s->goods_loading)) {
        goods_load();
    }
    goods_show();
    set_hint();
}

static void picker_close(void)
{
    lv_obj_delete(s->picker);
    s->picker = NULL;
    s->goods_list = NULL;
    s->view = V_TRADE;
    render_trade(true);
    set_hint();
}

// ---------------------------------------------------------------------------
// Trade screen

// Our names come from the server; theirs only as trade_label() vouches.
static void bundle_text(const trade_bundle_t *b, bool theirs, char *out, size_t n)
{
    size_t len = 0;
    out[0] = 0;
    if (b->lamps) {
        char amt[24];
        len = snprintf(out, n, "%s", ui_fmt_lamps(amt, sizeof(amt), b->lamps));
    }
    for (int i = 0; i < b->nitems + b->npages && len < n; i++) {
        bool page = i >= b->nitems;
        const trade_thing_t *th = page ? &b->pages[i - b->nitems] : &b->items[i];
        char name[TRADE_NAME_LEN + 24];
        trade_label(theirs, page, th, name, sizeof(name));
        len += snprintf(out + len, n - len, "%s%s", len ? ", " : "", name);
    }
    if (!out[0]) {
        strlcpy(out, "nothing", n);
    }
}

static void mine_row(int i, ui_row_t *r, void *ctx)
{
    const trade_view_t *t = trade_current();
    if (!t) {
        return;
    }
    switch (i) {
    case R_LOCK: {
        char line[64] = "";
        r->title_color = COL_AQUA;
        if (t->phase != TRADE_OPEN) {
            strlcpy(r->title, "Lock offer", sizeof(r->title));
            r->title_color = COL_DIM;
        } else if (t->can_sign) {
            strlcpy(r->title, "Sign the trade", sizeof(r->title));
            strlcpy(line, "Both locked", sizeof(line));
            strlcpy(r->right, "SELECT sign", sizeof(r->right));
        } else if (t->i_locked) {
            strlcpy(r->title, LV_SYMBOL_OK " Locked", sizeof(r->title));
            snprintf(line, sizeof(line), t->they_locked ? "Signing..." : "Waiting for %s to lock", t->label);
            strlcpy(r->right, "SELECT unlock", sizeof(r->right));
        } else {
            strlcpy(r->title, "Lock offer", sizeof(r->title));
            // The joiner's lock is its signature; the inviter signs after both lock.
            strlcpy(line, t->inviter ? "Lock, then sign once they lock too" : "Locking signs the trade", sizeof(line));
        }
        strlcpy(r->sub, line, sizeof(r->sub));
        break;
    }
    case R_LAMPS: {
        char bal[24];
        ui_fmt_lamps(r->title, sizeof(r->title), t->mine.lamps);
        r->title_color = COL_LAMPS;
        snprintf(r->sub, sizeof(r->sub), "You have %s", ui_fmt_lamps(bal, sizeof(bal), balance()));
        strlcpy(r->right, LV_SYMBOL_LEFT " " LV_SYMBOL_RIGHT, sizeof(r->right));
        break;
    }
    case R_ADD:
        strlcpy(r->title, LV_SYMBOL_PLUS " Add items or pages", sizeof(r->title));
        snprintf(r->sub, sizeof(r->sub), "%d of %d items, %d of %d pages", t->mine.nitems, TRADE_MAX_ITEMS, t->mine.npages,
                 TRADE_MAX_PAGES);
        break;
    default: {
        int k = i - R_FIXED;
        bool page = k >= t->mine.nitems;
        const trade_thing_t *th = page ? &t->mine.pages[k - t->mine.nitems] : &t->mine.items[k];
        strlcpy(r->title, th->name, sizeof(r->title));
        strlcpy(r->sub, page ? "Page" : "Item", sizeof(r->sub));
        strlcpy(r->right, "SELECT remove", sizeof(r->right));
        break;
    }
    }
}

static void trade_open_screen(void)
{
    s->view = V_TRADE;
    s->sig[0] = 0;
    s->cancel_armed = 0;
    lv_obj_set_hidden(s->home, true);
    s->screen = lv_obj_create(s->root);
    lv_obj_remove_style_all(s->screen);
    lv_obj_set_size(s->screen, OS_ROOT_W, OS_ROOT_H - UI_HINT_H);
    lv_obj_set_scrollable(s->screen, false);
    s->title = ui_label(s->screen, &sol_font_14, COL_AQUA, "");
    lv_obj_set_pos(s->title, 8, 2);
    lv_obj_set_width(s->title, 200);
    lv_label_set_long_mode(s->title, LV_LABEL_LONG_MODE_DOTS);
    s->phase = ui_label(s->screen, &sol_mono_12, COL_DIM, "");
    lv_obj_align(s->phase, LV_ALIGN_TOP_RIGHT, -8, 4);
    lv_obj_t *p = ui_panel(s->screen, 4, THEIRS_Y, OS_ROOT_W - 8, THEIRS_H);
    lv_obj_set_style_pad_ver(p, 3, 0);
    s->theirs_head = ui_label(p, &sol_mono_12, COL_DIM, "");
    s->theirs = ui_label(p, &sol_font_14, COL_TEXT, "");
    lv_obj_set_pos(s->theirs, 0, 15);
    lv_obj_set_width(s->theirs, OS_ROOT_W - 22);
    lv_label_set_long_mode(s->theirs, LV_LABEL_LONG_MODE_DOTS);
    s->mine = ui_list_create(s->screen, MINE_Y, MINE_H, mine_row, NULL);
    render_trade(true);
    set_hint();
}

static void trade_close_screen(void)
{
    trade_dismiss();
    if (s->picker) {
        lv_obj_delete(s->picker);
        s->picker = NULL;
        s->goods_list = NULL;
    }
    lv_obj_delete(s->screen);
    s->screen = NULL;
    s->mine = NULL;
    s->view = V_HOME;
    lv_obj_set_hidden(s->home, false);
    show_tab(TAB_TRADES);
}

static void render_trade(bool force)
{
    const trade_view_t *t = trade_current();
    if (!t || s->view == V_HOME) {
        return;
    }
    char sig[sizeof(s->sig)];
    snprintf(sig, sizeof(sig), "%d|%d|%d|%d|%d|%d|%d|%d|%d%d", t->phase, t->my_rev, t->their_rev, t->i_locked,
             t->they_locked, t->mine.lamps, t->mine.nitems + t->mine.npages, balance(), t->settled, t->rejected);
    if (!force && !strcmp(sig, s->sig)) {
        return;
    }
    strlcpy(s->sig, sig, sizeof(s->sig));
    lv_label_set_text_fmt(s->title, "Trade with %s", t->label);

    static const char *const phases[] = {"", "INVITED", "OPEN", "WAITING", "SIGNED", "CANCELLED"};
    lv_label_set_text(s->phase, t->settled ? "SETTLED" : t->rejected ? "REJECTED" : t->unconfirmed ? "PENDING"
                              : phases[t->phase]);
    lv_obj_set_style_text_color(s->phase, ui_col(t->rejected ? COL_ERR : t->phase == TRADE_DONE ? COL_GREEN
                                                 : t->unconfirmed ? COL_YELLOW
                                                 : t->phase == TRADE_CANCELLED ? COL_ERR : COL_DIM), 0);

    char text[160];
    switch (t->phase) {
    case TRADE_INVITING:
        lv_label_set_text(s->theirs_head, "WAITING");
        snprintf(text, sizeof(text), "Waiting for %s to join...", t->label);
        break;
    case TRADE_DONE:
        lv_label_set_text(s->theirs_head, "SIGNED BY BOTH");
        if (t->settled) {
            strlcpy(text, LV_SYMBOL_OK " Settled. Everything moved", sizeof(text));
        } else if (t->rejected) {
            snprintf(text, sizeof(text), "Rejected: %s", t->reason[0] ? t->reason : "the server said no");
        } else {
            strlcpy(text, os_api_online() ? "Settling with the server..." : "Settles when either badge syncs",
                    sizeof(text));
        }
        break;
    case TRADE_CANCELLED:
        if (t->unconfirmed) {
            // We signed, so the server may still settle it: Trades shows the outcome.
            lv_label_set_text(s->theirs_head, "SIGNED, NOT CONFIRMED");
            if (t->settled || t->rejected) {
                snprintf(text, sizeof(text), "%s. The server %s it after all.", t->reason,
                         t->settled ? "settled" : "rejected");
            } else {
                snprintf(text, sizeof(text), "%s. It may still settle; Trades shows the outcome.", t->reason);
            }
        } else {
            lv_label_set_text(s->theirs_head, "CANCELLED");
            strlcpy(text, t->reason, sizeof(text));
        }
        break;
    case TRADE_SIGNING:
        // Our tag is out: the trade can settle now, so there's no cancel.
        lv_label_set_text(s->theirs_head, "SIGNED");
        snprintf(text, sizeof(text), "Signed. Waiting for %s", t->label);
        break;
    default:
        lv_label_set_text_fmt(s->theirs_head, "%s GIVES%s", t->label, t->they_locked ? "  " LV_SYMBOL_OK " LOCKED" : "");
        if (t->theirs.lamps || t->theirs.nitems || t->theirs.npages) {
            bundle_text(&t->theirs, true, text, sizeof(text));
        } else {
            strlcpy(text, "Nothing yet", sizeof(text));
        }
        break;
    }
    lv_label_set_text(s->theirs, text);
    lv_obj_set_style_text_color(s->theirs_head, ui_col(t->they_locked ? COL_AQUA : COL_DIM), 0);
    bool open = t->phase == TRADE_OPEN;
    ui_list_show(s->mine, open);
    if (open) {
        ui_list_set_count(s->mine, R_FIXED + t->mine.nitems + t->mine.npages);
    }
    set_hint();
}

// Inviter, both locked: the deliberate step that sends our signature.
static void sign_confirmed(bool yes, void *ctx)
{
    const trade_view_t *t = trade_current();
    if (!s || !t || !t->can_sign) {
        return;
    }
    if (!yes) {
        trade_lock(false);              // not now: unlock, so they see it
    } else if (t->my_rev != s->ask_mine || t->their_rev != s->ask_theirs) {
        os_toast("The offers changed. Check them and lock again");
        return;
    } else {
        trade_sign();
    }
    render_trade(false);
}

// The confirm repeats both offers as they stand, and remembers their
// revisions: a yes only counts for exactly what it showed.
static void ask(const trade_view_t *t, const char *question, os_confirm_cb_t done)
{
    trade_flush();
    char mine[72], theirs[72], q[300];
    bundle_text(&t->mine, false, mine, sizeof(mine));
    bundle_text(&t->theirs, true, theirs, sizeof(theirs));
    lv_snprintf(q, sizeof(q), "You give: %s\n%s gives: %s\n\n%s", mine, t->label, theirs, question);
    s->ask_mine = t->my_rev;
    s->ask_theirs = t->their_rev;
    os_confirm(q, done, NULL);
}

static void ask_sign(const trade_view_t *t)
{
    s->sign_asked = true;
    ask(t, "Sign the trade? Once you sign, it can settle even if you cancel.", sign_confirmed);
}

static void lock_confirmed(bool yes, void *ctx)
{
    const trade_view_t *t = trade_current();
    if (!yes || !s || !t) {
        return;
    }
    if (t->my_rev != s->ask_mine || t->their_rev != s->ask_theirs) {
        os_toast("The offers changed. Check them and lock again");
        return;
    }
    trade_lock(true);
    render_trade(false);
}

static bool trade_key(os_key_t k, bool repeat)
{
    const trade_view_t *t = trade_current();
    if (!t) {
        return false;
    }
    bool open = t->phase == TRADE_OPEN;
    if (t->phase == TRADE_DONE || t->phase == TRADE_CANCELLED) {
        if ((k == OS_KEY_SELECT || k == OS_KEY_BACK) && !repeat) {
            trade_close_screen();
        }
        return true;
    }
    if (k == OS_KEY_BACK) {
        if (t->phase == TRADE_SIGNING) {
            return repeat;              // leave; the signed trade is recorded on exit
        }
        if (repeat) {
            return true;
        }
        int64_t now = game_now_ms();
        if (s->cancel_armed && now - s->cancel_armed < CANCEL_ARM_MS) {
            trade_cancel();
            render_trade(true);
        } else {
            s->cancel_armed = now;
            os_toast("CANCEL again to cancel the trade");
        }
        return true;
    }
    if (!open) {
        return true;
    }
    if (ui_list_key(s->mine, k, repeat)) {
        set_hint();
        return true;
    }
    int sel = ui_list_selected(s->mine);
    if ((k == OS_KEY_LEFT || k == OS_KEY_RIGHT) && sel == R_LAMPS) {
        int step = repeat ? LAMPS_STEP * 4 : LAMPS_STEP;
        int v = t->mine.lamps + (k == OS_KEY_RIGHT ? step : -step);
        int max = balance();
        v = v < 0 ? 0 : v > max ? max : v;
        if (v != t->mine.lamps) {
            trade_set_lamps(v);
            render_trade(false);
        } else if (!repeat && k == OS_KEY_RIGHT) {
            os_toast("That's all your !LAMPs");
        }
        return true;
    }
    if (k != OS_KEY_SELECT || repeat) {
        return true;
    }
    switch (sel) {
    case R_LOCK:
        if (t->can_sign) {
            ask_sign(t);
        } else if (t->i_locked) {
            trade_lock(false);
        } else if (t->inviter) {
            trade_lock(true);           // nothing is signed until trade_sign()
        } else {
            // The joiner signs as soon as the terms arrive: this is its commitment.
            ask(t, "Locking signs the trade. Once you both lock, it can settle even if you cancel.", lock_confirmed);
        }
        break;
    case R_LAMPS:
        break;
    case R_ADD:
        picker_open();
        return true;
    default: {
        int idx = sel - R_FIXED;
        bool page = idx >= t->mine.nitems;
        trade_remove(page, page ? idx - t->mine.nitems : idx);
        break;
    }
    }
    render_trade(false);
    return true;
}

static bool picker_key(os_key_t k, bool repeat)
{
    if (ui_list_key(s->goods_list, k, repeat)) {
        return true;
    }
    if (k == OS_KEY_BACK) {
        if (!repeat) {
            picker_close();
        }
        return true;
    }
    int sel = ui_list_selected(s->goods_list);
    if (k != OS_KEY_SELECT || repeat || sel < 0) {
        return true;
    }
    const good_t *g = &s->goods[sel];
    const trade_view_t *t = trade_current();
    if (!t || t->phase != TRADE_OPEN) {
        picker_close();
        return true;
    }
    const char *why;
    if (trade_count_of(g->page, g->id) >= g->count) {
        os_toast(g->page ? "You don't have more copies" : "Already in your offer");
    } else if (!trade_add(g->page, g->id, g->name, &why)) {
        os_toast("%s", why);
    }
    ui_list_refresh(s->goods_list);
    return true;
}

// ---------------------------------------------------------------------------

static void set_hint(void)
{
    const char *h = "";
    const trade_view_t *t = trade_current();
    if (s->view == V_PICK) {
        h = "SELECT add  CANCEL back";
    } else if (s->view == V_TRADE && t) {
        int sel = ui_list_selected(s->mine);
        switch (t->phase) {
        case TRADE_DONE:
        case TRADE_CANCELLED:
            h = "SELECT done";
            break;
        case TRADE_INVITING:
            h = "CANCEL x2 cancel";
            break;
        case TRADE_SIGNING:
            h = "CANCEL leave";
            break;
        default:
            h = sel == R_LAMPS ? LV_SYMBOL_LEFT LV_SYMBOL_RIGHT " amount  CANCEL x2 cancel"
              : sel == R_LOCK  ? (t->can_sign ? "SELECT sign  CANCEL x2 cancel" : "SELECT lock / unlock  CANCEL x2 cancel")
              : sel == R_ADD   ? "SELECT add  CANCEL x2 cancel"
                               : "SELECT remove  CANCEL x2 cancel";
            break;
        }
    } else if (s->tab == TAB_NEARBY) {
        int sel = ui_list_selected(s->list);
        h = sel >= 0 && sel < s->ninvites ? "SELECT join  " LV_SYMBOL_LEFT " decline  CANCEL exit"
          : bridge_row(sel) ? "SELECT show code  " LV_SYMBOL_RIGHT " Trades  CANCEL exit"
          : sel >= 0 ? "SELECT invite  " LV_SYMBOL_RIGHT " Trades  CANCEL exit" : LV_SYMBOL_RIGHT " Trades  CANCEL exit";
    } else {
        h = LV_SYMBOL_LEFT " Nearby  CANCEL exit";
    }
    lv_label_set_text(s->hint, h);
}

// Trading needs the radio on the mesh channel, which pauses Wi-Fi joined on
// another channel. Load what you can offer first in that case, so the picker
// has it while the server is out of reach.
static bool goods_first(void)
{
    os_wifi_info_t w;
    os_wifi_get_info(&w);
    if (!os_api_online() || w.channel == os_mesh_channel() || !(s->goods = calloc(GOODS_MAX, sizeof(good_t)))) {
        return false;
    }
    goods_from_cache();
    int64_t now = os_cloud_time_ms();
    if (s->goods_at && now && now - s->goods_at < GOODS_FRESH_MS) {
        return false;
    }
    goods_load();
    return s->goods_loading;
}

static void radio_on(void)
{
    if (!s->acquired) {
        s->acquired = true;
        os_mesh_acquire();
    }
}

static void enter(lv_obj_t *root)
{
    s = calloc(1, sizeof(*s));
    if (!s) {
        ui_label(root, &sol_font_14, COL_ERR, "Out of memory");
        return;
    }
    s->opened = game_now_ms();
    if (!goods_first()) {
        radio_on();
    }
    s->root = root;
    s->home = lv_obj_create(root);
    lv_obj_remove_style_all(s->home);
    lv_obj_set_size(s->home, OS_ROOT_W, OS_ROOT_H - UI_HINT_H);
    lv_obj_set_scrollable(s->home, false);
    s->tabs = ui_tabs(s->home, tab_names, 2);
    s->list = ui_list_create(s->home, LIST_Y, LIST_H, home_row, NULL);
    s->hint = ui_hint(root, "");
    show_tab(TAB_NEARBY);
    if (!os_data_ready()) {
        os_toast("No microSD card: unsent trades are lost on reboot");
    }
    if (trade_current()) {
        trade_open_screen();            // e.g. a finished trade not yet dismissed
    }
}

static void leave(void)
{
    if (!s) {
        return;
    }
    trade_dismiss();                    // cancels an open trade; the peer is told
    os_api_cancel(s);
    if (s->acquired) {
        os_mesh_release();
    }
    free(s->recs);
    free(s->goods);
    free(s);
    s = NULL;
}

static bool on_key(os_key_t k, bool repeat)
{
    if (!s) {
        return false;
    }
    if (s->view == V_PICK) {
        return picker_key(k, repeat);
    }
    if (s->view == V_TRADE) {
        return trade_key(k, repeat);
    }
    if (ui_list_key(s->list, k, repeat)) {
        set_hint();
        return true;
    }
    int sel = ui_list_selected(s->list);
    bool invite = s->tab == TAB_NEARBY && sel >= 0 && sel < s->ninvites;
    const char *why = NULL;
    switch (k) {
    case OS_KEY_LEFT:
        if (invite && !repeat) {
            trade_decline(s->invites[sel].tid);
            nearby_refresh();
        } else if (s->tab == TAB_TRADES) {
            show_tab(TAB_NEARBY);
        }
        return true;
    case OS_KEY_RIGHT:
        if (s->tab == TAB_NEARBY) {
            show_tab(TAB_TRADES);
        }
        return true;
    case OS_KEY_SELECT:
        if (repeat || s->tab != TAB_NEARBY || sel < 0) {
            return true;
        }
        if (bridge_row(sel)) {
            os_bridge_show();
            return true;
        }
        if (invite ? trade_join(s->invites[sel].tid, &why) : trade_invite_peer(s->peers[sel - s->ninvites].id, &why)) {
            trade_open_screen();
        } else if (why) {
            os_toast("%s", why);
        }
        return true;
    default:
        return false;
    }
}

static void on_tick(void)
{
    if (!s) {
        return;
    }
    s->tick++;
    if (!s->acquired && (!s->goods_loading || game_now_ms() - s->opened > GOODS_WAIT_MS)) {
        radio_on();
        show_tab(s->tab);
    }
    if (s->view == V_HOME) {
        const trade_view_t *t = trade_current();
        if (t && t->phase != TRADE_IDLE) {
            trade_open_screen();        // joined from the console
        } else if (s->tick % (REFRESH_MS / OS_TICK_MS) == 0) {
            if (s->tab == TAB_NEARBY) {
                nearby_refresh();
            } else if (s->tick % (5 * REFRESH_MS / OS_TICK_MS) == 0) {
                trades_refresh();       // outcomes arrive in the background
            }
            set_hint();
        }
    } else if (s->view == V_TRADE && s->tick % 3 == 0) {
        render_trade(false);
        // A confirm whose question no longer applies goes away (the trade
        // moved on: signed or locked from the console, cancelled, settled).
        const trade_view_t *t = trade_current();
        if (!t || !t->can_sign) {
            os_confirm_dismiss(sign_confirmed);
        }
        if (!t || t->phase != TRADE_OPEN || t->i_locked) {
            os_confirm_dismiss(lock_confirmed);
        }
        // Both locked: ask the inviter to sign, once per pair of locks.
        if (t && !t->can_sign) {
            s->sign_asked = false;
        } else if (t && !s->sign_asked && !os_confirm_open()) {
            ask_sign(t);
        }
    }
}

const os_app_t app_trade = {
    .id = "trade",
    .name = "Trade",
    .symbol = LV_SYMBOL_REFRESH,
    .accent = COL_AQUA,
    .on_enter = enter,
    .on_exit = leave,
    .on_key = on_key,
    .on_tick = on_tick,
};
