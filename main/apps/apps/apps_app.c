// Apps: the Lua apps installed on the microSD card (docs/HHGTS.md). SELECT
// starts one in the Lua host, RIGHT shows what it is and what it may do,
// LEFT twice removes it with its saved data. Temporary quest apps show the
// time they have left.
//
// Reading every app.json walks the card, so a worker task does it and hands
// the list over under a mutex; on_tick picks it up. Online, the library
// (GET /api/device/library) first moves quest apps' expiry forward: the
// server extends a grant while its quest is active.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "core/os.h"
#include "core/os_api.h"
#include "core/os_cloud.h"
#include "core/ui.h"
#include "lua/lua_host.h"
#include "lua/lua_pkg.h"

#define APPS_MAX        40
#define REMOVE_WAIT_MS  3000

typedef enum { V_LIST, V_INFO } view_t;

static SemaphoreHandle_t s_mx;          // the worker's hand-over
static bool s_open, s_busy;             // app on screen; a worker runs
static lua_pkg_t *s_inbox;              // finished scan, not taken yet
static int s_inbox_n;
static char s_job_rm[LUA_PKG_ID_MAX + 1];   // worker: remove this first

static lua_pkg_t *s_pkgs;
static int s_n = -2;                    // -2 loading, -1 no card
static view_t s_view;
static lv_obj_t *s_root, *s_info, *s_hint;
static ui_list_t *s_list;
static int s_rm_index = -1;             // LEFT pressed once on this row
static int64_t s_rm_until;
static void *s_lib_req;                 // library request in flight (os_api ctx), NULL = none
static bool s_lib_done;                 // expiry times refreshed this visit

static int64_t now_ms(void)
{
    return xTaskGetTickCount() * portTICK_PERIOD_MS;
}

// ---------------------------------------------------------------------------
// Worker

static void worker(void *arg)
{
    if (s_job_rm[0]) {
        lua_pkg_remove(s_job_rm);
    }
    lua_pkg_t *pkgs = malloc(APPS_MAX * sizeof(*pkgs));
    int n = pkgs ? lua_pkg_scan(pkgs, APPS_MAX) : 0;
    xSemaphoreTake(s_mx, portMAX_DELAY);
    if (s_open) {
        free(s_inbox);
        s_inbox = pkgs;
        s_inbox_n = n;
        pkgs = NULL;
    }
    s_busy = false;
    xSemaphoreGive(s_mx);
    free(pkgs);
    vTaskDelete(NULL);
}

static void start_worker(const char *remove_id)
{
    xSemaphoreTake(s_mx, portMAX_DELAY);
    bool busy = s_busy;
    if (!busy) {
        strlcpy(s_job_rm, remove_id ? remove_id : "", sizeof(s_job_rm));
        s_busy = xTaskCreate(worker, "apps_scan", 4096, NULL, 3, NULL) == pdPASS;
    }
    xSemaphoreGive(s_mx);
    if (busy) {
        os_toast("One moment...");
    }
}

// ---------------------------------------------------------------------------
// List

static void time_left(const lua_pkg_t *p, char *out, size_t n)
{
    int64_t now = os_cloud_time_ms();
    if (!p->expires || !now) {
        strlcpy(out, "Temporary", n);
    } else if (now >= p->expires) {
        strlcpy(out, "Expired", n);
    } else {
        int min = (int)((p->expires - now) / 60000);
        if (min >= 60) {
            snprintf(out, n, "%d h %d min left", min / 60, min % 60);
        } else {
            snprintf(out, n, "%d min left", min > 0 ? min : 1);
        }
    }
}

static void row(int i, ui_row_t *r, void *ctx)
{
    const lua_pkg_t *p = &s_pkgs[i];
    snprintf(r->title, sizeof(r->title), "%s  %s", p->symbol, p->name);
    if (p->temp) {
        time_left(p, r->sub, sizeof(r->sub));
        strlcpy(r->right, "Quest", sizeof(r->right));
        r->right_color = lua_pkg_expired(p) ? COL_DIM : COL_ORANGE;
    } else {
        snprintf(r->sub, sizeof(r->sub), "v%s%s%s", p->version, p->author[0] ? "  by " : "", p->author);
    }
    if (i == s_rm_index) {
        r->title_color = COL_ERR;
    }
}

static void set_hint(void)
{
    char h[64];
    if (s_view == V_INFO) {
        lv_label_set_text(s_hint, "SELECT open   CANCEL back");
    } else if (s_rm_index >= 0 && s_rm_index < s_n) {
        snprintf(h, sizeof(h), "LEFT again removes %s", s_pkgs[s_rm_index].name);
        lv_label_set_text(s_hint, h);
    } else {
        lv_label_set_text(s_hint, s_n > 0 ? "SELECT open   " LV_SYMBOL_RIGHT " info   " LV_SYMBOL_LEFT " remove"
                                          : "CANCEL exit");
    }
    lv_obj_set_style_text_color(s_hint, ui_col(s_rm_index >= 0 ? COL_ERR : COL_DIM), 0);
}

static void render_list(void)
{
    ui_list_set_empty(s_list, s_n == -2 ? "Loading..." : s_n == -1 ? "Insert a microSD card"
                              : "No apps yet. Get some in the App Store.");
    ui_list_set_count(s_list, s_n > 0 ? s_n : 0);
    set_hint();
}

// Library answer (OS task): refresh expiry times, then re-read the card.
static void library_cb(int status, const char *body, void *ctx)
{
    if (!ctx || ctx != s_lib_req || !s_open) {
        return;
    }
    s_lib_req = NULL;
    s_lib_done = status == 200;
    if (s_lib_done && lua_pkg_apply_library(body) > 0) {
        start_worker(NULL);
    }
}

static void refresh_library(void)
{
    if (s_lib_done || s_lib_req || !os_api_online()) {
        return;
    }
    s_lib_req = os_api_token();
    if (os_api_call("GET", "/api/device/library", NULL, 0, library_cb, s_lib_req) != ESP_OK) {
        s_lib_req = NULL;
    }
}

// ---------------------------------------------------------------------------
// Info

static void show_info(const lua_pkg_t *p)
{
    s_view = V_INFO;
    s_info = ui_panel(s_root, 0, 0, OS_ROOT_W, OS_ROOT_H - UI_HINT_H);
    lv_obj_set_style_radius(s_info, 0, 0);
    lv_obj_set_style_border_width(s_info, 0, 0);
    lv_obj_set_style_pad_all(s_info, 10, 0);
    lv_obj_set_flex_flow(s_info, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_info, 4, 0);
    lv_obj_set_scrollable(s_info, true);
    lv_obj_set_scrollbar_mode(s_info, LV_SCROLLBAR_MODE_AUTO);

    char line[96];
    snprintf(line, sizeof(line), "%s  %s", p->symbol, p->name);
    ui_label(s_info, &sol_font_20, COL_TEXT, line);
    char cat[8];
    strlcpy(cat, p->category, sizeof(cat));
    cat[0] = cat[0] >= 'a' && cat[0] <= 'z' ? cat[0] - 'a' + 'A' : cat[0];     // "games" -> "Games"
    snprintf(line, sizeof(line), "v%s%s%s%s%s", p->version, p->author[0] ? "  by " : "", p->author,
             cat[0] ? "  " LV_SYMBOL_BULLET "  " : "", cat);
    ui_label(s_info, &sol_mono_12, COL_DIM, line);
    if (p->temp) {
        char left[32];
        time_left(p, left, sizeof(left));
        snprintf(line, sizeof(line), "From a quest. %s", left);
        ui_label(s_info, &sol_mono_12, COL_ORANGE, line);
    }
    if (p->summary[0]) {
        lv_obj_t *s = ui_label(s_info, &sol_font_14, COL_TEXT, p->summary);
        lv_obj_set_width(s, lv_pct(100));
    }
    lv_obj_t *h = ui_label(s_info, &sol_mono_12, COL_DIM, p->perms ? "It can:" : "It needs no permissions.");
    lv_obj_set_style_pad_top(h, 6, 0);
    for (int b = 0; b < LUA_PERM_COUNT; b++) {
        if (p->perms & (1u << b)) {
            snprintf(line, sizeof(line), LV_SYMBOL_BULLET "  %s", lua_perm_text(b));
            ui_label(s_info, &sol_font_14, COL_TEXT, line);
        }
    }
    set_hint();
}

static void close_info(void)
{
    if (s_info) {
        lv_obj_delete(s_info);
        s_info = NULL;
    }
    s_view = V_LIST;
    set_hint();
}

// ---------------------------------------------------------------------------

static void enter(lv_obj_t *root)
{
    if (!s_mx) {
        s_mx = xSemaphoreCreateMutex();
    }
    s_root = root;
    s_view = V_LIST;
    s_info = NULL;
    s_rm_index = -1;
    s_n = -2;
    s_list = ui_list_create(root, 0, OS_ROOT_H - UI_HINT_H, row, NULL);
    s_hint = ui_hint(root, "");
    xSemaphoreTake(s_mx, portMAX_DELAY);
    s_open = true;
    xSemaphoreGive(s_mx);
    render_list();
    start_worker(NULL);
    s_lib_done = false;
    s_lib_req = NULL;
    refresh_library();
}

static void leave(void)
{
    os_api_cancel(s_lib_req);           // NULL: nothing to cancel
    s_lib_req = NULL;
    xSemaphoreTake(s_mx, portMAX_DELAY);
    s_open = false;
    free(s_inbox);
    s_inbox = NULL;
    xSemaphoreGive(s_mx);
    free(s_pkgs);
    s_pkgs = NULL;
    s_n = -2;
}

static void tick(void)
{
    xSemaphoreTake(s_mx, portMAX_DELAY);
    lua_pkg_t *in = s_inbox;
    int n = s_inbox_n;
    s_inbox = NULL;
    xSemaphoreGive(s_mx);
    if (in) {
        int sel = ui_list_selected(s_list);
        free(s_pkgs);
        s_pkgs = in;
        s_n = n;
        render_list();
        if (sel > 0) {
            ui_list_select(s_list, sel);
        }
    }
    refresh_library();          // once the badge comes online
    if (s_rm_index >= 0 && now_ms() > s_rm_until) {
        s_rm_index = -1;
        ui_list_refresh(s_list);
        set_hint();
    }
}

static bool key(os_key_t k, bool repeat)
{
    if (s_view == V_INFO) {
        int sel = ui_list_selected(s_list);
        if (k == OS_KEY_BACK || k == OS_KEY_LEFT) {
            close_info();
        } else if (k == OS_KEY_SELECT && sel >= 0) {
            lua_host_launch(s_pkgs[sel].id);
        } else if (k == OS_KEY_UP || k == OS_KEY_DOWN) {
            lv_obj_scroll_by_bounded(s_info, 0, k == OS_KEY_UP ? 40 : -40, LV_ANIM_ON);
        }
        return true;
    }
    int sel = ui_list_selected(s_list);
    bool arming = k == OS_KEY_LEFT && !repeat && sel >= 0;
    if (s_rm_index >= 0 && !(arming && sel == s_rm_index)) {
        s_rm_index = -1;            // any other key cancels the removal
        ui_list_refresh(s_list);
        set_hint();
        if (k == OS_KEY_BACK) {
            return true;
        }
    }
    switch (k) {
    case OS_KEY_UP:
    case OS_KEY_DOWN:
        return ui_list_key(s_list, k, repeat);
    case OS_KEY_SELECT:
        if (sel >= 0) {
            lua_host_launch(s_pkgs[sel].id);
        }
        return true;
    case OS_KEY_RIGHT:
        if (sel >= 0) {
            show_info(&s_pkgs[sel]);
        }
        return true;
    case OS_KEY_LEFT:
        if (!arming) {
            return true;
        }
        if (s_rm_index == sel) {
            os_toast("Removed %s", s_pkgs[sel].name);
            start_worker(s_pkgs[sel].id);
            s_rm_index = -1;
        } else {
            s_rm_index = sel;
            s_rm_until = now_ms() + REMOVE_WAIT_MS;
        }
        ui_list_refresh(s_list);
        set_hint();
        return true;
    default:
        return false;
    }
}

const os_app_t app_apps = {
    .id = "apps",
    .name = "Apps",
    .symbol = LV_SYMBOL_LIST,
    .accent = COL_LPURPLE,
    .on_enter = enter,
    .on_exit = leave,
    .on_key = key,
    .on_tick = tick,
};
