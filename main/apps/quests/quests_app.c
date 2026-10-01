// Quests: talk to the characters at nearby beacons (game/quests) and keep a
// quest log. Nothing quest-specific ships on the badge: the server sends the
// dialog, the options and their effects through the beacon.
//
// The dialog screen is one scrolling column: who, the quest, the text, then
// the options. DOWN reads on until the options show, then moves through them;
// UP goes back.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "core/os.h"
#include "core/os_api.h"
#include "core/ui.h"
#include "game/quests.h"

#define LIST_Y      UI_TABS_H
#define LIST_H      (OS_ROOT_H - LIST_Y - UI_HINT_H)
#define PEERS_MAX   12
#define SCROLL_STEP 48
#define REFRESH_MS  1000

enum { TAB_NEARBY, TAB_LOG };
static const char *const tab_names[] = {"Nearby", "Quest log"};

typedef struct {
    lv_obj_t *root, *tabs, *hint, *foot;
    ui_list_t *list;
    int tab, tick;
    os_mesh_peer_t beacons[PEERS_MAX];
    int nbeacons;
    // Dialog
    lv_obj_t *dlg, *col, *who, *quest, *text, *opts[QUEST_OPTS_MAX], *state, *bar;
    int sel;
    char shown_text[sizeof(((quest_dialog_t *)0)->text)];
    bool shown_waiting, shown_end;
    int shown_nopts;
    int dl_off;
    quest_dl_state_t dl_state;
} quests_ui_t;

static quests_ui_t *s;

// ---------------------------------------------------------------------------
// Lists

static uint32_t status_color(const char *st)
{
    if (!strcmp(st, "active")) {
        return COL_ORANGE;
    }
    if (!strcmp(st, "completed")) {
        return COL_GREEN;
    }
    return !strcmp(st, "locked") ? COL_DIM : COL_TEXT;
}

static void row(int i, ui_row_t *r, void *ctx)
{
    if (s->tab == TAB_NEARBY) {
        const os_mesh_peer_t *b = &s->beacons[i];
        game_ascii(r->title, sizeof(r->title), b->name[0] ? b->name : b->id);
        r->title_color = b->color ? game_ink(b->color) : COL_TEXT;
        game_ascii(r->sub, sizeof(r->sub), b->title);
        strlcpy(r->right, game_signal(b->rssi), sizeof(r->right));
        return;
    }
    const quest_entry_t *log;
    quests_log(&log);
    const quest_entry_t *e = &log[i];
    strlcpy(r->title, e->title, sizeof(r->title));
    r->title_color = status_color(e->status);
    if (!strcmp(e->status, "active") && e->step[0]) {
        lv_snprintf(r->sub, sizeof(r->sub), "%s%s%s", e->step, e->at[0] ? " @ " : "", e->at);
    } else if (!strcmp(e->status, "locked")) {
        strlcpy(r->sub, "Finish other quests first", sizeof(r->sub));
    } else if (!strcmp(e->status, "completed")) {
        strlcpy(r->sub, "Done", sizeof(r->sub));
    } else if (e->at[0]) {
        lv_snprintf(r->sub, sizeof(r->sub), "Talk to %s to start", e->at);
    } else {
        strlcpy(r->sub, "Talk to its character to start", sizeof(r->sub));
    }
    strlcpy(r->right, e->status, sizeof(r->right));
    r->right_color = status_color(e->status);
}

static void beacons_refresh(void)
{
    char keep[OS_MESH_ID_LEN + 1] = "";
    int sel = ui_list_selected(s->list);
    if (sel >= 0 && sel < s->nbeacons) {
        strlcpy(keep, s->beacons[sel].id, sizeof(keep));
    }
    int n = os_mesh_peers(s->beacons, PEERS_MAX);
    s->nbeacons = 0;
    for (int i = 0; i < n; i++) {
        if (s->beacons[i].kind == OS_PEER_BEACON) {
            s->beacons[s->nbeacons++] = s->beacons[i];
        }
    }
    ui_list_set_count(s->list, s->nbeacons);
    for (int i = 0; keep[0] && i < s->nbeacons; i++) {
        if (!strcmp(s->beacons[i].id, keep)) {
            ui_list_select(s->list, i);
        }
    }
}

static void log_render(void)
{
    const quest_entry_t *log;
    int n = quests_log(&log);
    char foot[64] = "";
    const char *err = quests_log_error();
    if (quests_log_busy()) {
        strlcpy(foot, "Checking...", sizeof(foot));
    } else if (err[0]) {
        strlcpy(foot, err, sizeof(foot));
    } else if (n && quests_log_saved()) {
        game_synced(quests_log_at(), foot, sizeof(foot));
    }
    lv_label_set_text(s->foot, foot);
    if (s->tab == TAB_LOG) {
        ui_list_set_count(s->list, n);
    }
}

static void set_hint(void)
{
    const char *h;
    if (s->dlg) {
        const quest_dialog_t *d = quests_dialog();
        h = !d || d->error[0] ? "SELECT try again  CANCEL leave"
          : d->end ? "SELECT close" : d->waiting ? "CANCEL leave" : LV_SYMBOL_UP LV_SYMBOL_DOWN " read / pick  SELECT answer";
    } else if (s->tab == TAB_NEARBY) {
        h = s->nbeacons ? "SELECT talk  " LV_SYMBOL_RIGHT " Quest log  CANCEL exit" : LV_SYMBOL_RIGHT " Quest log  CANCEL exit";
    } else {
        h = LV_SYMBOL_LEFT " Nearby  SELECT refresh  CANCEL exit";
    }
    lv_label_set_text(s->hint, h);
}

static void show_tab(int tab)
{
    s->tab = tab;
    ui_tabs_select(s->tabs, tab);
    ui_list_set_count(s->list, 0);      // the rows belong to the other tab until it reloads
    lv_obj_set_hidden(s->foot, tab != TAB_LOG);
    if (tab == TAB_NEARBY) {
        ui_list_set_empty(s->list, "No beacons nearby. Characters wait at beacons around the venue.");
        beacons_refresh();
    } else {
        ui_list_set_empty(s->list, "Nothing here yet");
        quests_log_refresh();
        log_render();
    }
    set_hint();
}

// ---------------------------------------------------------------------------
// Dialog screen

static lv_obj_t *col_label(const lv_font_t *font, uint32_t color)
{
    lv_obj_t *l = ui_label(s->col, font, color, "");
    lv_obj_set_width(l, OS_ROOT_W - 24);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    return l;
}

static void dialog_open(void)
{
    s->dlg = lv_obj_create(s->root);
    lv_obj_remove_style_all(s->dlg);
    lv_obj_set_size(s->dlg, OS_ROOT_W, OS_ROOT_H - UI_HINT_H);
    lv_obj_set_style_bg_opa(s->dlg, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s->dlg, ui_col(COL_BG), 0);
    lv_obj_set_scrollable(s->dlg, false);

    s->col = lv_obj_create(s->dlg);
    lv_obj_remove_style_all(s->col);
    lv_obj_set_size(s->col, OS_ROOT_W, OS_ROOT_H - UI_HINT_H);
    lv_obj_set_flex_flow(s->col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_hor(s->col, 12, 0);
    lv_obj_set_style_pad_ver(s->col, 8, 0);
    lv_obj_set_style_pad_row(s->col, 6, 0);
    lv_obj_set_scrollbar_mode(s->col, LV_SCROLLBAR_MODE_ACTIVE);

    s->who = col_label(&sol_font_20, COL_TEXT);
    s->quest = col_label(&sol_mono_12, COL_DIM);
    s->text = col_label(&sol_font_14, COL_TEXT);
    for (int i = 0; i < QUEST_OPTS_MAX; i++) {
        lv_obj_t *o = col_label(&sol_font_14, COL_TEXT);
        lv_obj_set_style_pad_hor(o, 8, 0);
        lv_obj_set_style_pad_ver(o, 4, 0);
        lv_obj_set_style_radius(o, 4, 0);
        lv_obj_set_style_bg_color(o, ui_col(COL_PANEL_HI), 0);
        lv_obj_set_style_border_side(o, LV_BORDER_SIDE_LEFT, 0);
        lv_obj_set_style_border_color(o, ui_col(COL_ORANGE), 0);
        s->opts[i] = o;
    }
    s->state = col_label(&sol_mono_12, COL_DIM);
    s->bar = lv_bar_create(s->col);
    lv_obj_set_size(s->bar, OS_ROOT_W - 24, 6);
    lv_obj_set_style_bg_color(s->bar, ui_col(COL_PANEL_HI), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s->bar, ui_col(COL_PURPLE), LV_PART_INDICATOR);
    lv_obj_set_style_bg_grad_color(s->bar, lv_color_hex(COL_GREEN), LV_PART_INDICATOR);
    lv_obj_set_style_bg_grad_dir(s->bar, LV_GRAD_DIR_HOR, LV_PART_INDICATOR);
    lv_obj_set_hidden(s->bar, true);
    s->shown_text[0] = 0;
    s->shown_nopts = -1;
    s->dl_state = QDL_NONE;
    s->sel = 0;
}

static void dialog_close(void)
{
    quests_hangup();
    if (s->dlg) {
        lv_obj_delete(s->dlg);
        s->dlg = NULL;
    }
    beacons_refresh();
    set_hint();
}

static void options_style(void)
{
    const quest_dialog_t *d = quests_dialog();
    for (int i = 0; i < QUEST_OPTS_MAX; i++) {
        bool on = d && !d->end && !d->waiting && i == s->sel;
        lv_obj_set_style_bg_opa(s->opts[i], on ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(s->opts[i], on ? 3 : 0, 0);
        lv_obj_set_style_text_color(s->opts[i], ui_col(d && d->waiting ? COL_DIM : COL_TEXT), 0);
    }
}

static void dialog_render(void)
{
    const quest_dialog_t *d = quests_dialog();
    if (!d) {
        return;
    }
    bool rebuilt = strcmp(d->text, s->shown_text) || d->nopts != s->shown_nopts || d->end != s->shown_end;
    if (rebuilt) {
        strlcpy(s->shown_text, d->text, sizeof(s->shown_text));
        s->shown_nopts = d->nopts;
        s->shown_end = d->end;
        s->sel = 0;
        lv_label_set_text(s->who, d->name[0] ? d->name : d->beacon);
        lv_obj_set_style_text_color(s->who, d->color ? lv_color_hex(game_ink(d->color)) : ui_col(COL_TEXT), 0);
        if (d->quest_title[0]) {
            lv_label_set_text_fmt(s->quest, "%s  " LV_SYMBOL_BULLET "  %s", d->title, d->quest_title);
        } else {
            lv_label_set_text(s->quest, d->title);
        }
        lv_label_set_text(s->text, d->shown ? d->text : "");
        for (int i = 0; i < QUEST_OPTS_MAX; i++) {
            lv_obj_set_hidden(s->opts[i], i >= d->nopts || d->end);
            if (i < d->nopts) {
                lv_label_set_text(s->opts[i], d->opts[i]);
            }
        }
        lv_obj_scroll_to_y(s->col, 0, LV_ANIM_OFF);
    }
    // Live state line: waiting, error, end, download.
    const quest_download_t *dl = quests_download();
    char line[80] = "";
    uint32_t c = COL_DIM;
    if (d->error[0]) {
        snprintf(line, sizeof(line), LV_SYMBOL_WARNING " %s", d->error);
        c = COL_ERR;
    } else if (d->waiting) {
        strlcpy(line, d->shown ? "..." : "Saying hello...", sizeof(line));
    } else if (dl->state == QDL_RUNNING) {
        int pct = dl->total ? dl->off * 100 / dl->total : 0;
        snprintf(line, sizeof(line), "Downloading %s  %d%%", dl->id, pct);
        c = COL_ORANGE;
    } else if (dl->state == QDL_DONE && s->dl_state != QDL_NONE) {
        snprintf(line, sizeof(line), LV_SYMBOL_OK " %s is ready. Open it from Apps", dl->id);
        c = COL_GREEN;
    } else if (dl->state == QDL_FAILED && s->dl_state != QDL_NONE) {
        snprintf(line, sizeof(line), "Download stopped: %s", dl->error);
        c = COL_ERR;
    } else if (d->end) {
        strlcpy(line, "End of conversation", sizeof(line));
    }
    if (dl->state == QDL_RUNNING) {
        s->dl_state = QDL_RUNNING;      // report the outcome of downloads seen in this conversation
    }
    lv_label_set_text(s->state, line);
    lv_obj_set_style_text_color(s->state, ui_col(c), 0);
    lv_obj_set_hidden(s->state, !line[0]);
    bool bar = dl->state == QDL_RUNNING && dl->total > 0;
    lv_obj_set_hidden(s->bar, !bar);
    if (bar && dl->off != s->dl_off) {
        s->dl_off = dl->off;
        lv_bar_set_value(s->bar, dl->off * 100 / dl->total, LV_ANIM_OFF);
    }
    bool waiting = d->waiting != s->shown_waiting;
    s->shown_waiting = d->waiting;
    if (rebuilt || waiting) {
        options_style();
        set_hint();
    }
}

// DOWN reads on until the last option is in view, then moves the selection;
// UP moves the selection back, then scrolls up.
static void dialog_scroll(bool down)
{
    const quest_dialog_t *d = quests_dialog();
    lv_obj_update_layout(s->col);
    int bottom = lv_obj_get_scroll_bottom(s->col);
    int n = d && !d->end ? d->nopts : 0;
    if (down) {
        if (bottom > 0 && (n == 0 || s->sel == n - 1 || bottom > SCROLL_STEP / 2)) {
            lv_obj_scroll_by_bounded(s->col, 0, -SCROLL_STEP, LV_ANIM_ON);
        } else if (s->sel < n - 1) {
            s->sel++;
            lv_obj_scroll_to_view(s->opts[s->sel], LV_ANIM_ON);
        }
    } else if (s->sel > 0) {
        s->sel--;
        lv_obj_scroll_to_view(s->opts[s->sel], LV_ANIM_ON);
    } else {
        lv_obj_scroll_by_bounded(s->col, 0, SCROLL_STEP, LV_ANIM_ON);
    }
    options_style();
}

static bool dialog_key(os_key_t k, bool repeat)
{
    const quest_dialog_t *d = quests_dialog();
    switch (k) {
    case OS_KEY_UP:
    case OS_KEY_DOWN:
        dialog_scroll(k == OS_KEY_DOWN);
        return true;
    case OS_KEY_SELECT: {
        if (repeat || !d) {
            return true;
        }
        if (d->error[0]) {
            const char *why;
            char beacon[OS_MESH_ID_LEN + 1];
            strlcpy(beacon, d->beacon, sizeof(beacon));
            // Reopen rather than repeat a choice: the server may have taken it.
            if (!quests_talk(beacon, &why)) {
                os_toast("%s", why);
            }
            s->shown_text[0] = 0;
            s->shown_nopts = -1;
        } else if (d->end) {
            dialog_close();
        } else if (!d->waiting) {
            quests_choose(s->sel);
        }
        dialog_render();
        return true;
    }
    case OS_KEY_BACK:
        if (!repeat) {
            dialog_close();
        }
        return true;
    default:
        return true;
    }
}

// ---------------------------------------------------------------------------

static void enter(lv_obj_t *root)
{
    s = calloc(1, sizeof(*s));
    if (!s) {
        ui_label(root, &sol_font_14, COL_ERR, "Out of memory");
        return;
    }
    os_mesh_acquire();
    s->root = root;
    s->tabs = ui_tabs(root, tab_names, 2);
    s->list = ui_list_create(root, LIST_Y, LIST_H - 16, row, NULL);
    s->foot = ui_label(root, &sol_mono_12, COL_DIM, "");
    lv_obj_set_pos(s->foot, 8, OS_ROOT_H - UI_HINT_H - 15);
    lv_obj_set_width(s->foot, OS_ROOT_W - 16);
    lv_label_set_long_mode(s->foot, LV_LABEL_LONG_MODE_DOTS);
    s->hint = ui_hint(root, "");
    show_tab(TAB_NEARBY);
}

static void leave(void)
{
    if (!s) {
        return;
    }
    quests_hangup();
    quests_log_free();
    os_mesh_release();
    free(s);
    s = NULL;
}

static bool on_key(os_key_t k, bool repeat)
{
    if (!s) {
        return false;
    }
    if (s->dlg) {
        return dialog_key(k, repeat);
    }
    if (ui_list_key(s->list, k, repeat)) {
        return true;
    }
    switch (k) {
    case OS_KEY_LEFT:
    case OS_KEY_RIGHT:
        show_tab(k == OS_KEY_RIGHT ? TAB_LOG : TAB_NEARBY);
        return true;
    case OS_KEY_SELECT: {
        int sel = ui_list_selected(s->list);
        if (repeat) {
            return true;
        }
        if (s->tab == TAB_LOG) {
            quests_log_refresh();
            log_render();
        } else if (sel >= 0) {
            const char *why;
            if (quests_talk(s->beacons[sel].id, &why)) {
                dialog_open();
                dialog_render();
                set_hint();
            } else {
                os_toast("%s", why);
            }
        }
        return true;
    }
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
    if (s->dlg) {
        dialog_render();
    } else if (s->tick % (REFRESH_MS / OS_TICK_MS) == 0) {
        if (s->tab == TAB_NEARBY) {
            beacons_refresh();
        } else {
            log_render();
        }
        set_hint();
    }
}

const os_app_t app_quests = {
    .id = "quests",
    .name = "Quests",
    .symbol = LV_SYMBOL_GPS,
    .accent = COL_ORANGE,
    .on_enter = enter,
    .on_exit = leave,
    .on_key = on_key,
    .on_tick = on_tick,
};
