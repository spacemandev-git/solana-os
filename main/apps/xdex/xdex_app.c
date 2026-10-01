// X Dex: swap cards with nearby badges (game/xdex) and browse the Yearbook of
// cards collected so far. Each unique badge pays a bounty on upload; the
// number of unique badges is the owner's Level.
//
// Nearby lists incoming requests first, then the badges in range. Yearbook
// merges the cards on this badge with GET /api/device/xdex when online.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "core/os.h"
#include "core/os_api.h"
#include "core/os_cloud.h"
#include "core/os_data.h"
#include "core/os_wifi.h"
#include "core/ui.h"
#include "game/xdex.h"

#define STATUS_H    18
#define LIST_Y      (UI_TABS_H + STATUS_H)
#define LIST_H      (OS_ROOT_H - LIST_Y - UI_HINT_H)
#define PEERS_MAX   16
#define CARDS_MAX   64
#define REFRESH_MS  1000
#define SYNC_WAIT_MS 8000       // longest the radio waits for the Yearbook sync

enum { TAB_NEARBY, TAB_YEARBOOK };
static const char *const tab_names[] = {"Nearby", "Yearbook"};

typedef struct {
    lv_obj_t *tabs, *status, *hint;
    ui_list_t *list;
    int tab;
    int tick;
    xdex_phase_t phase;     // last seen, to reload the Yearbook after a swap
    bool card;              // microSD card present (checked on enter)
    bool acquired;          // holding the mesh radio
    bool me_pending;        // GET /api/device/me in flight
    int64_t opened;
    // Nearby
    os_mesh_peer_t peers[PEERS_MAX];
    int npeers, npending;
    xdex_req_t pending[XDEX_PENDING_MAX];
    // Yearbook
    xdex_card_t *cards;
    int ncards;
    char cursor[48];        // next server page, "" = none
    bool loading;
    int next_bounty;        // -1 = unknown
    int64_t synced_at;      // cached server data, unix ms
} xdex_ui_t;

static xdex_ui_t *s;

// ---------------------------------------------------------------------------
// Nearby

static bool collected(const char *id)
{
    for (int i = 0; i < s->ncards; i++) {
        if (!strcmp(s->cards[i].id, id)) {
            return true;
        }
    }
    return false;
}

static void nearby_row(int i, ui_row_t *r)
{
    char who[GAME_CARD_NAME + 8];
    if (i < s->npending) {
        const xdex_req_t *q = &s->pending[i];
        game_peer_label(q->card.n, q->card.id, who, sizeof(who));
        lv_snprintf(r->title, sizeof(r->title), "%s wants your card", who);
        r->title_color = COL_PINK;
        if (q->accepted) {
            strlcpy(r->sub, "Swapping...", sizeof(r->sub));
        } else {
            int left = (int)((XDEX_REQ_TTL_MS - (game_now_ms() - q->at)) / 1000);
            snprintf(r->sub, sizeof(r->sub), "Lv %d  " LV_SYMBOL_BULLET "  %d s left", q->card.lv, left > 0 ? left : 0);
        }
        strlcpy(r->right, q->accepted ? "" : "SELECT", sizeof(r->right));
        r->right_color = COL_PINK;
        return;
    }
    const os_mesh_peer_t *p = &s->peers[i - s->npending];
    if (p->name[0]) {
        strlcpy(r->title, p->name, sizeof(r->title));
        snprintf(r->sub, sizeof(r->sub), "Lv %d  " LV_SYMBOL_BULLET "  %s  " LV_SYMBOL_BULLET "  %s", p->level, p->id,
                 collected(p->id) ? "in your Yearbook" : "new card");
    } else {
        strlcpy(r->title, "Unclaimed badge", sizeof(r->title));
        r->title_color = COL_DIM;
        snprintf(r->sub, sizeof(r->sub), "%s  " LV_SYMBOL_BULLET "  not linked yet", p->id);
    }
    strlcpy(r->right, game_signal(p->rssi), sizeof(r->right));
}

// Selected row's peer id, to keep the selection on it across refreshes.
static const char *selected_id(void)
{
    int sel = ui_list_selected(s->list);
    if (sel < 0) {
        return NULL;
    }
    return sel < s->npending ? s->pending[sel].card.id : s->peers[sel - s->npending].id;
}

static void nearby_refresh(void)
{
    char keep[OS_MESH_ID_LEN + 1] = "";
    bool keep_pending = ui_list_selected(s->list) >= 0 && ui_list_selected(s->list) < s->npending;
    const char *sel = selected_id();
    if (sel) {
        strlcpy(keep, sel, sizeof(keep));
    }
    int before = s->npending;
    s->npending = 0;
    for (int i = 0; i < xdex_pending_count() && i < XDEX_PENDING_MAX; i++) {
        s->pending[s->npending++] = *xdex_pending(i);
    }
    int n = os_mesh_peers(s->peers, PEERS_MAX);
    s->npeers = 0;
    for (int i = 0; i < n; i++) {
        if (s->peers[i].kind == OS_PEER_BADGE) {
            s->peers[s->npeers++] = s->peers[i];
        }
    }
    // A new request goes to the top and takes the selection.
    int pick = s->npending > before ? 0 : -1;
    for (int i = 0; keep[0] && i < s->npending + s->npeers && pick < 0; i++) {
        bool pending = i < s->npending;
        const char *id = pending ? s->pending[i].card.id : s->peers[i - s->npending].id;
        pick = !strcmp(id, keep) && pending == keep_pending ? i : -1;
    }
    ui_list_set_count(s->list, s->npending + s->npeers);
    if (pick >= 0) {
        ui_list_select(s->list, pick);
    }
}

// One line under the tabs: why we can't swap, the swap in progress, or its result.
static void status_render(void)
{
    char line[96] = "";
    uint32_t col = COL_DIM;
    os_cloud_info_t *in = malloc(sizeof(*in));
    if (!in) {
        return;
    }
    os_cloud_get_info(in);
    const xdex_swap_t *x = xdex_last();
    if (!game_claimed()) {
        strlcpy(line, "Not linked yet: claim this badge first", sizeof(line));
        col = COL_WARN;
    } else if (!in->has_key) {
        strlcpy(line, "Turn on Wi-Fi once so the badge can get its key", sizeof(line));
        col = COL_WARN;
    } else if (!s->acquired) {
        strlcpy(line, "Syncing your Yearbook...", sizeof(line));
    } else if (!os_mesh_ready()) {
        strlcpy(line, "Starting the radio...", sizeof(line));
    } else {
        switch (x->phase) {
        case XDEX_ASKING:
            snprintf(line, sizeof(line), "Asking %s for their card...", x->label);
            col = COL_PINK;
            break;
        case XDEX_FINISHING:
            snprintf(line, sizeof(line), "Swapping with %s...", x->label);
            col = COL_PINK;
            break;
        case XDEX_DECLINED:
            snprintf(line, sizeof(line), "%s said no", x->label);
            break;
        case XDEX_NO_ANSWER:
            snprintf(line, sizeof(line), "No answer from %s", x->label);
            break;
        case XDEX_FAILED:
            snprintf(line, sizeof(line), "The swap with %s didn't finish", x->label);
            col = COL_ERR;
            break;
        case XDEX_SWAPPED:
            col = COL_PINK;
            switch (x->upload) {
            case XDEX_UP_DONE:
                if (x->bounty > 0) {
                    snprintf(line, sizeof(line), "Got %s's card  +%d !LAMPs", x->label, x->bounty);
                    col = COL_LAMPS;
                } else {
                    snprintf(line, sizeof(line), "%s's card updated", x->label);
                }
                break;
            case XDEX_UP_QUEUED:
                snprintf(line, sizeof(line), "Got %s's card. Uploads when Wi-Fi is on", x->label);
                break;
            case XDEX_UP_REFUSED:
                snprintf(line, sizeof(line), "%s's card: %s", x->label, x->error);
                col = COL_ERR;
                break;
            default:
                snprintf(line, sizeof(line), "Got %s's card. Uploading...", x->label);
                break;
            }
            break;
        default:
            if (!s->card) {
                // HHGTS.md: without a card, swaps live in RAM until they upload.
                strlcpy(line, "No microSD card: unsent swaps are lost on reboot", sizeof(line));
                col = COL_WARN;
            } else {
                strlcpy(line, s->tab == TAB_NEARBY ? "Hold two badges close and swap cards" : "", sizeof(line));
            }
            break;
        }
    }
    if (s->tab == TAB_YEARBOOK && x->phase == XDEX_IDLE && game_claimed() && in->has_key) {
        if (s->next_bounty >= 0) {
            snprintf(line, sizeof(line), "Lv %d  " LV_SYMBOL_BULLET "  next new card +%d !LAMPs", in->level, s->next_bounty);
        } else {
            snprintf(line, sizeof(line), "Lv %d", in->level);
        }
        col = COL_TEXT;
    }
    free(in);
    lv_label_set_text(s->status, line);
    lv_obj_set_style_text_color(s->status, ui_col(col), 0);
}

// ---------------------------------------------------------------------------
// Yearbook

static void yearbook_row(int i, ui_row_t *r)
{
    const xdex_card_t *c = &s->cards[i];
    game_peer_label(c->n, c->id, r->title, sizeof(r->title));
    if (c->x[0]) {
        snprintf(r->sub, sizeof(r->sub), "@%s  " LV_SYMBOL_BULLET "  Lv %d  " LV_SYMBOL_BULLET "  %s", c->x, c->lv, c->id);
    } else {
        snprintf(r->sub, sizeof(r->sub), "Lv %d  " LV_SYMBOL_BULLET "  %s", c->lv, c->id);
    }
    if (c->bounty > 0) {
        snprintf(r->right, sizeof(r->right), "+%d", c->bounty);
        r->right_color = COL_LAMPS;
    }
}

static void merge_card(const xdex_card_t *c)
{
    for (int i = 0; i < s->ncards; i++) {
        if (!strcmp(s->cards[i].id, c->id)) {
            if (c->bounty >= 0) {
                s->cards[i].bounty = c->bounty;
            }
            if (!s->cards[i].n[0]) {
                strlcpy(s->cards[i].n, c->n, sizeof(c->n));
            }
            return;
        }
    }
    if (s->ncards < CARDS_MAX) {
        s->cards[s->ncards++] = *c;
    }
}

// Server page of XdexEntry {badge, card {n, x, lv}, at, bounty}.
static void merge_server(const cJSON *items)
{
    const cJSON *e;
    cJSON_ArrayForEach(e, items) {
        const cJSON *card = cJSON_GetObjectItemCaseSensitive(e, "card");
        xdex_card_t c = {0};
        strlcpy(c.id, game_jstr(e, "badge"), sizeof(c.id));
        game_ascii(c.n, sizeof(c.n), game_jstr(card, "n"));
        game_ascii(c.x, sizeof(c.x), game_jstr(card, "x"));
        c.lv = game_jint(card, "lv", 0);
        c.bounty = game_jint(e, "bounty", -1);
        const cJSON *at = cJSON_GetObjectItemCaseSensitive(e, "at");
        c.at = cJSON_IsNumber(at) ? (int64_t)at->valuedouble : 0;
        if (c.id[0]) {
            merge_card(&c);
        }
    }
}

static int by_newest(const void *a, const void *b)
{
    int64_t x = ((const xdex_card_t *)a)->at, y = ((const xdex_card_t *)b)->at;
    return x < y ? 1 : x > y ? -1 : 0;
}

static void yearbook_show(void)
{
    qsort(s->cards, s->ncards, sizeof(xdex_card_t), by_newest);
    if (s->tab == TAB_YEARBOOK) {
        ui_list_set_count(s->list, s->ncards);
    }
}

static void got_page(int status, const char *body, void *ctx)
{
    s->loading = false;
    if (status != 200) {
        return;
    }
    cJSON *j = cJSON_Parse(body);
    if (!s->cursor[0]) {
        game_cache_put("xdex", body);       // the first page, for offline viewing
        s->synced_at = os_cloud_time_ms();
    }
    merge_server(cJSON_GetObjectItemCaseSensitive(j, "items"));
    strlcpy(s->cursor, game_jstr(j, "next"), sizeof(s->cursor));
    cJSON_Delete(j);
    yearbook_show();
}

static void got_me(int status, const char *body, void *ctx)
{
    s->me_pending = false;
    if (status != 200) {
        return;
    }
    cJSON *j = cJSON_Parse(body);
    s->next_bounty = game_jint(j, "nextBounty", -1);
    os_cloud_info_t *in = malloc(sizeof(*in));
    if (in) {
        os_cloud_get_info(in);
        os_cloud_note_lamps(game_jint(j, "lamps", in->lamps), game_jint(j, "level", in->level));
        free(in);
    }
    game_cache_put("me", body);
    cJSON_Delete(j);
}

static void load_more(void)
{
    char path[96];
    snprintf(path, sizeof(path), "/api/device/xdex%s%s", s->cursor[0] ? "?cursor=" : "", s->cursor);
    if (os_api_call("GET", path, NULL, 0, got_page, s) == ESP_OK) {
        s->loading = true;
    }
}

static void yearbook_load(void)
{
    free(s->cards);
    s->cards = NULL;
    xdex_card_t *local;
    int n = xdex_cards_load(&local, CARDS_MAX);
    s->cards = calloc(CARDS_MAX, sizeof(xdex_card_t));
    if (!s->cards) {
        free(local);
        s->ncards = 0;
        return;
    }
    memcpy(s->cards, local, n * sizeof(xdex_card_t));
    s->ncards = n;
    free(local);
    s->cursor[0] = 0;

    // Cached server data first (bounties, cards from other badges of this owner).
    cJSON *cached = game_cache_get("xdex", &s->synced_at);
    merge_server(cJSON_GetObjectItemCaseSensitive(cached, "items"));
    cJSON_Delete(cached);
    cJSON *me = game_cache_get("me", NULL);
    s->next_bounty = game_jint(me, "nextBounty", -1);
    cJSON_Delete(me);
    yearbook_show();
    if (os_api_online()) {
        load_more();
        s->me_pending = os_api_call("GET", "/api/device/me", NULL, 0, got_me, s) == ESP_OK;
    }
}

// ---------------------------------------------------------------------------

static void row(int i, ui_row_t *r, void *ctx)
{
    if (s->tab == TAB_NEARBY) {
        nearby_row(i, r);
    } else {
        yearbook_row(i, r);
    }
}

static void set_hint(void)
{
    const char *h;
    int sel = ui_list_selected(s->list);
    if (s->tab == TAB_YEARBOOK) {
        h = LV_SYMBOL_LEFT " Nearby  " LV_SYMBOL_UP LV_SYMBOL_DOWN " browse  CANCEL exit";
    } else if (sel >= 0 && sel < s->npending) {
        h = "SELECT accept  " LV_SYMBOL_LEFT " decline  CANCEL exit";
    } else if (sel >= 0) {
        h = "SELECT ask for card  " LV_SYMBOL_RIGHT " Yearbook";
    } else {
        h = LV_SYMBOL_RIGHT " Yearbook  CANCEL exit";
    }
    lv_label_set_text(s->hint, h);
}

static void show_tab(int tab)
{
    s->tab = tab;
    ui_tabs_select(s->tabs, tab);
    ui_list_select(s->list, 0);
    if (tab == TAB_NEARBY) {
        ui_list_set_empty(s->list, "No badges nearby yet. Badges show up here when they're in range.");
        nearby_refresh();
    } else {
        ui_list_set_empty(s->list, game_claimed() ? "Nothing here yet. Swap cards with a nearby badge."
                                                  : "Not linked yet: claim this badge first");
        yearbook_show();
    }
    status_render();
    set_hint();
}

static void enter(lv_obj_t *root)
{
    s = calloc(1, sizeof(*s));
    if (!s) {
        ui_label(root, &sol_font_14, COL_ERR, "Out of memory");
        return;
    }
    s->next_bounty = -1;
    s->card = os_data_ready();
    s->opened = game_now_ms();
    s->tabs = ui_tabs(root, tab_names, 2);
    s->status = ui_label(root, &sol_mono_12, COL_DIM, "");
    lv_obj_set_pos(s->status, 8, UI_TABS_H + 3);
    lv_obj_set_width(s->status, OS_ROOT_W - 16);
    lv_label_set_long_mode(s->status, LV_LABEL_LONG_MODE_DOTS);
    s->list = ui_list_create(root, LIST_Y, LIST_H, row, NULL);
    s->hint = ui_hint(root, "");
    yearbook_load();                    // also marks "in your Yearbook" on Nearby
    // The mesh pauses Wi-Fi joined on another channel: sync the Yearbook first.
    os_wifi_info_t w;
    os_wifi_get_info(&w);
    if (!os_api_online() || w.channel == os_mesh_channel()) {
        s->acquired = true;
        os_mesh_acquire();
    }
    show_tab(TAB_NEARBY);
}

static void leave(void)
{
    if (!s) {
        return;
    }
    os_api_cancel(s);
    if (s->acquired) {
        os_mesh_release();
    }
    free(s->cards);
    free(s);
    s = NULL;
}

static bool on_key(os_key_t k, bool repeat)
{
    if (!s) {
        return false;
    }
    if (ui_list_key(s->list, k, repeat)) {
        if (s->tab == TAB_YEARBOOK && s->cursor[0] && !s->loading
                && ui_list_selected(s->list) >= s->ncards - 3 && os_api_online()) {
            load_more();
        }
        set_hint();
        return true;
    }
    int sel = ui_list_selected(s->list);
    bool on_pending = s->tab == TAB_NEARBY && sel >= 0 && sel < s->npending;
    switch (k) {
    case OS_KEY_LEFT:
        if (on_pending && !repeat) {
            xdex_decline(s->pending[sel].card.id);
            nearby_refresh();
        } else if (s->tab == TAB_YEARBOOK) {
            show_tab(TAB_NEARBY);
        }
        return true;
    case OS_KEY_RIGHT:
        if (s->tab == TAB_NEARBY) {
            yearbook_load();
            show_tab(TAB_YEARBOOK);
        }
        return true;
    case OS_KEY_SELECT:
        if (s->tab != TAB_NEARBY || sel < 0) {
            return true;
        }
        if (on_pending) {
            xdex_accept(s->pending[sel].card.id);
        } else {
            const char *why;
            if (!xdex_request(s->peers[sel - s->npending].id, &why)) {
                os_toast("%s", why);
            }
        }
        status_render();
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
    if (!s->acquired && ((!s->loading && !s->me_pending) || game_now_ms() - s->opened > SYNC_WAIT_MS)) {
        s->acquired = true;
        os_mesh_acquire();
    }
    if (++s->tick % (REFRESH_MS / OS_TICK_MS) == 0 && s->tab == TAB_NEARBY) {
        nearby_refresh();
        set_hint();
    }
    if (s->tick % 3 == 0) {
        status_render();
    }
    xdex_phase_t phase = xdex_last()->phase;
    if (phase != s->phase) {
        s->phase = phase;
        if (phase == XDEX_SWAPPED) {
            yearbook_load();            // the new card, and its Nearby mark
            if (s->tab == TAB_NEARBY) {
                ui_list_refresh(s->list);
            }
        }
    }
}

const os_app_t app_xdex = {
    .id = "xdex",
    .name = "X Dex",
    .symbol = LV_SYMBOL_COPY,
    .accent = COL_PINK,
    .on_enter = enter,
    .on_exit = leave,
    .on_key = on_key,
    .on_tick = on_tick,
};
