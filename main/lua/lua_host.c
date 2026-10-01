// Lua app host, see lua_host.h and docs/LUA_API.md.
//
// One app runs at a time, in a fresh lua_State on the "lua" task:
//   boot      read main.lua, open the sandboxed libraries, compile (no lock)
//   run       lock LVGL, run the chunk, then loop: keys, API answers, mesh
//             messages, timers and the 50 ms on_tick, each a callback under
//             the lock with its own instruction budget
//   shutdown  the OS closes the app (HOME, CANCEL, sys.exit): on_exit runs
//             without the lock (UI calls do nothing then), storage is
//             written, the state is closed and the task ends
// An error stops the script at once: the error screen replaces its UI, and
// CANCEL goes back to the launcher.
//
// Numbers are 32-bit (LUA_32BITS in vendor/luaconf.h): integers and floats
// both. The C5 has no FPU, so floats are soft-float either way and single
// precision is the cheaper one, and 8-byte values instead of 16 shrink tables
// and stacks: with the sample apps the heap peaked about 20 % lower than with
// 64-bit numbers (snake 30.6 KB instead of 37.1 KB, towel-tycoon 28.2 KB
// instead of 35.3 KB). Integers wrap at ±2^31, which is fine for scores,
// !LAMPs and sys.time() seconds; bigger whole numbers in JSON (ms
// timestamps) arrive as floats.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_lvgl_port.h"
#include "core/os_data.h"
#include "vendor/lualib.h"
#include "lua_priv.h"

static const char *TAG = "lua";

#define LH_STACK            (12 * 1024)
#define LH_PRIO             2           // below the mesh task, above idle
#define LH_HOOK_EVERY       1000        // instructions between budget checks
#define LH_QUEUE_LEN        16
#define LH_EXIT_WAIT_MS     2000
#define LH_MAX_WAIT_MS      200         // re-check the quit flag at least this often
#define LH_LOCK_POLL_MS     20
#define LH_READ_CHUNK       256

SemaphoreHandle_t lh_mx;
lh_t *lh_cur;
lh_stats_t lh_stats;

static QueueHandle_t s_q;
static SemaphoreHandle_t s_done;
static uint32_t s_gen;
static lua_pkg_t s_next;            // app to start on the next enter (under lh_mx)
static char s_title[21] = "Lua";
static bool s_busy;                 // a lua task exists (under lh_mx)

int64_t lh_now(void)
{
    return esp_timer_get_time() / 1000;
}

bool lh_post(const lh_ev_t *ev)
{
    return s_q && xQueueSend(s_q, ev, 0) == pdTRUE;
}

void lh_need_perm(lua_State *L, uint32_t perm, const char *module)
{
    if (!(lh_get(L)->pkg.perms & perm)) {
        luaL_error(L, "%s: add the \"%s\" permission to app.json", module, module);
    }
}

// ---------------------------------------------------------------------------
// Limits: a capped allocator and an instruction budget per callback

static void *lh_alloc(void *ud, void *ptr, size_t osize, size_t nsize)
{
    lh_t *s = ud;
    size_t old = ptr ? osize : 0;
    if (nsize == 0) {
        free(ptr);
        s->mem -= old;
        return NULL;
    }
    if (nsize > old && s->mem - old + nsize > LUA_HOST_HEAP_MAX) {
        s->mem_hit = true;
        return NULL;    // Lua collects garbage and retries, then fails with "not enough memory"
    }
    void *p = realloc(ptr, nsize);
    if (p) {
        s->mem = s->mem - old + nsize;
        if (s->mem > s->mem_peak) {
            s->mem_peak = s->mem;
        }
    }
    return p;
}

static void step_hook(lua_State *L, lua_Debug *ar)
{
    lh_t *s = lh_get(L);
    s->steps += LH_HOOK_EVERY;
    if (s->steps > LUA_HOST_STEPS_MAX) {
        // From now on fail at every instruction, so pcall() can't keep it going.
        s->killed = true;
        lua_sethook(L, step_hook, LUA_MASKCOUNT, 1);
        luaL_where(L, 0);       // level 0: the script line running now
        lua_pushliteral(L, "Script took too long");
        lua_concat(L, 2);
        lua_error(L);
    }
}

// ---------------------------------------------------------------------------
// Error screen

static void show_error(lh_t *s, const char *title, const char *msg)
{
    lh_ui_close_dialog(s);
    lh_ui_clear(s);
    lv_obj_t *a = s->area;
    lv_obj_set_style_bg_color(a, ui_col(COL_BG), 0);
    lv_obj_t *t = ui_label(a, &sol_font_20, COL_ERR, title);
    lv_obj_set_pos(t, 12, 14);
    lv_obj_t *n = ui_label(a, &sol_mono_12, COL_DIM, s->pkg.name);
    lv_obj_set_pos(n, 12, 42);
    lv_obj_t *p = ui_panel(a, 8, 62, OS_ROOT_W - 16, OS_ROOT_H - 62 - UI_HINT_H - 8);
    lv_obj_t *m = ui_label(p, &sol_mono_12, COL_TEXT, msg);
    lv_obj_set_width(m, OS_ROOT_W - 30);
    lv_label_set_long_mode(m, LV_LABEL_LONG_MODE_WRAP);
    ui_hint(a, "CANCEL exit");
}

static int run_on_exit(lua_State *L)
{
    lua_getglobal(L, "on_exit");
    if (lua_isfunction(L, -1)) {
        lua_call(L, 0, 0);
    }
    return 0;
}

static void close_state(lh_t *s)
{
    if (!s->L) {
        return;
    }
    s->ui_off = true;
    lh_callback(s, run_on_exit, NULL);
    lh_storage_flush(s, true);
    s->steps = 0;
    lua_close(s->L);
    s->L = NULL;
}

// The script failed with rc and its error on the stack. LVGL lock held.
static void fail(lh_t *s, int rc)
{
    lua_State *L = s->L;
    char msg[200];
    const char *title = "Script error";
    if (rc == LUA_ERRMEM) {
        title = "Out of memory";
        snprintf(msg, sizeof(msg), s->mem_hit ? "The app needs more than %d KB." : "The badge is low on memory.",
                 LUA_HOST_HEAP_MAX / 1024);
    } else {
        strlcpy(msg, lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : "(error is not a string)", sizeof(msg));
        if (s->killed) {
            title = "Script took too long";
        }
    }
    lua_settop(L, 0);
    printf("[lua:%s] %s: %s\n", s->pkg.id, title, msg);
    strlcpy(s->err, msg, sizeof(s->err));
    s->failed = true;
    lh_mesh_mute();
    show_error(s, title, msg);
    s->killed = false;
    lua_sethook(L, step_hook, LUA_MASKCOUNT, LH_HOOK_EVERY);
    close_state(s);
    lh_led_off();
}

bool lh_callback(lh_t *s, lua_CFunction fn, void *arg)
{
    lua_State *L = s->L;
    if (!L) {
        return false;
    }
    s->steps = 0;
    lua_pushcfunction(L, fn);
    lua_pushlightuserdata(L, arg);
    int rc = lua_pcall(L, 1, 0, 0);
    if (s->steps > s->steps_peak) {
        s->steps_peak = s->steps;
    }
    if (rc != LUA_OK) {
        if (s->ui_off) {
            // Already closing (on_exit or a finalizer failed): just log it.
            printf("[lua:%s] %s\n", s->pkg.id, lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : "error");
            lua_settop(L, 0);
            return false;
        }
        fail(s, rc);
        return false;
    }
    lh_led_flush(s);
    return true;
}

// ---------------------------------------------------------------------------
// Boot: libraries, sandbox, storage, compile

typedef struct {
    FILE *f;
    char buf[LH_READ_CHUNK];
} reader_t;

static const char *read_chunk(lua_State *L, void *ud, size_t *size)
{
    reader_t *r = ud;
    *size = fread(r->buf, 1, sizeof(r->buf), r->f);
    return *size ? r->buf : NULL;
}

static int l_print(lua_State *L)
{
    luaL_Buffer b;
    int n = lua_gettop(L);
    luaL_buffinit(L, &b);
    for (int i = 1; i <= n; i++) {
        if (i > 1) {
            luaL_addchar(&b, '\t');
        }
        luaL_tolstring(L, i, NULL);
        luaL_addvalue(&b);
    }
    luaL_pushresult(&b);
    printf("[%s] %s\n", lh_get(L)->pkg.id, lua_tostring(L, -1));
    return 0;
}

static int l_collectgarbage(lua_State *L)
{
    const char *opt = luaL_optstring(L, 1, "collect");
    luaL_argcheck(L, !strcmp(opt, "count"), 1, "only \"count\" is available");
    lua_pushnumber(L, (lua_Number)lua_gc(L, LUA_GCCOUNT) + (lua_Number)lua_gc(L, LUA_GCCOUNTB) / 1024);
    return 1;
}

static int boot(lua_State *L)
{
    static const luaL_Reg libs[] = {
        {LUA_GNAME, luaopen_base}, {LUA_COLIBNAME, luaopen_coroutine}, {LUA_STRLIBNAME, luaopen_string},
        {LUA_TABLIBNAME, luaopen_table}, {LUA_MATHLIBNAME, luaopen_math}, {LUA_UTF8LIBNAME, luaopen_utf8},
    };
    for (size_t i = 0; i < sizeof(libs) / sizeof(libs[0]); i++) {
        luaL_requiref(L, libs[i].name, libs[i].func, 1);
        lua_pop(L, 1);
    }
    // LUA_API.md "Standard library": no loading code, no GC control, no dump.
    static const char *const drop[] = {"dofile", "loadfile", "load", "warn"};
    for (size_t i = 0; i < sizeof(drop) / sizeof(drop[0]); i++) {
        lua_pushnil(L);
        lua_setglobal(L, drop[i]);
    }
    lua_register(L, "collectgarbage", l_collectgarbage);
    lua_register(L, "print", l_print);
    lua_getglobal(L, LUA_STRLIBNAME);
    lua_pushnil(L);
    lua_setfield(L, -2, "dump");
    lua_pop(L, 1);
    lua_getglobal(L, LUA_MATHLIBNAME);
    lua_getfield(L, -1, "randomseed");
    lua_pushinteger(L, (lua_Integer)(esp_random() >> 1));
    lua_pushinteger(L, (lua_Integer)(esp_random() >> 1));
    lua_call(L, 2, 0);
    lua_pop(L, 1);

    lh_open_ui(L);
    lh_open_sys(L);
    lh_open_json(L);
    lh_open_net(L);
    lh_storage_load(lh_get(L));

    int rc = lua_load(L, read_chunk, lua_touserdata(L, 1), "=main.lua", "t");
    if (rc != LUA_OK) {
        lua_error(L);
    }
    lua_setfield(L, LUA_REGISTRYINDEX, "solana.main");
    // Drop the compiler's garbage now, then collect generationally: apps
    // churn short-lived strings and tables every tick, and young
    // collections keep the heap near the live data (mesh-wave peaked at
    // 31 KB instead of 45 KB with the incremental collector). The cap is a
    // ceiling; every KB a script holds is gone from the badge's heap, and
    // TLS needs ~30 KB of it for each server call.
    lua_gc(L, LUA_GCCOLLECT);
    lua_gc(L, LUA_GCGEN, 0, 0);
    return 0;
}

static int run_main(lua_State *L)
{
    lua_getfield(L, LUA_REGISTRYINDEX, "solana.main");
    lua_pushnil(L);
    lua_setfield(L, LUA_REGISTRYINDEX, "solana.main");
    lua_call(L, 0, 0);
    return 0;
}

// ---------------------------------------------------------------------------
// Events

static const char *const s_key_names[] = {"up", "down", "left", "right", "select", "back"};

static int key_cb(lua_State *L)
{
    lh_ev_t *ev = lua_touserdata(L, 1);
    lh_t *s = lh_get(L);
    if (lh_ui_key(L, s, ev->key, ev->repeat)) {
        return 0;
    }
    bool handled = false;
    lua_getglobal(L, "on_key");
    if (lua_isfunction(L, -1)) {
        lua_pushstring(L, s_key_names[ev->key]);
        lua_pushboolean(L, ev->repeat);
        lua_call(L, 2, 1);
        handled = lua_toboolean(L, -1);
    }
    if (!handled && ev->key == OS_KEY_BACK && !ev->repeat) {
        s->exit_req = true;
    }
    return 0;
}

static int tick_cb(lua_State *L)
{
    lua_getglobal(L, "on_tick");
    if (lua_isfunction(L, -1)) {
        lua_pushinteger(L, (lua_Integer)(intptr_t)lua_touserdata(L, 1));
        lua_call(L, 1, 0);
    }
    return 0;
}

// Takes the LVGL lock unless the OS is closing the app.
static bool lock(lh_t *s)
{
    while (!lvgl_port_lock(LH_LOCK_POLL_MS)) {
        if (s->quit) {
            return false;
        }
    }
    if (s->quit) {
        lvgl_port_unlock();
        return false;
    }
    return true;
}

static void run_events(lh_t *s, const lh_ev_t *ev)
{
    if (ev) {
        if (ev->kind == LH_EV_KEY && ev->key < sizeof(s_key_names) / sizeof(s_key_names[0])) {
            lh_callback(s, key_cb, (void *)ev);
        } else if (ev->kind == LH_EV_MESH) {
            lh_mesh_event(s, ev->data);
        }
    }
    lh_calls_run(s);
    int64_t now = lh_now();
    if (s->L && !s->failed && now >= s->next_tick) {
        int64_t ms = now - s->next_tick + LH_TICK_MS;
        s->next_tick = now + LH_TICK_MS;
        lh_callback(s, tick_cb, (void *)(intptr_t)ms);
    }
    if (s->L && !s->failed) {
        lh_timers_run(s);
    }
    if (s->exit_req && !s->failed) {
        s->exit_req = false;
        os_launch(&os_launcher);
    }
}

static TickType_t wait_ticks(lh_t *s)
{
    int64_t wake = s->next_tick, t = lh_timers_next(s), now = lh_now();
    if (t < wake) {
        wake = t;
    }
    if (s->store_pending && s->store_saved + LH_STORAGE_SAVE_MS < wake) {
        wake = s->store_saved + LH_STORAGE_SAVE_MS;
    }
    int64_t ms = wake - now;
    if (ms <= 0) {
        return 0;
    }
    if (ms > LH_MAX_WAIT_MS) {
        ms = LH_MAX_WAIT_MS;
    }
    return (ms + portTICK_PERIOD_MS - 1) / portTICK_PERIOD_MS;
}

// ---------------------------------------------------------------------------
// The task

static void task_end(lh_t *s)
{
    lh_stats = (lh_stats_t){
        .mem_peak = s->mem_peak, .mem_end = s->mem, .steps_peak = s->steps_peak,
        .stack_free = uxTaskGetStackHighWaterMark(NULL), .heap_before = s->heap_before,
    };
    strlcpy(lh_stats.id, s->pkg.id, sizeof(lh_stats.id));
    strlcpy(lh_stats.err, s->err, sizeof(lh_stats.err));
    free(s->store_pending);
    s->store_pending = NULL;
    xSemaphoreTake(lh_mx, portMAX_DELAY);
    s->finished = true;
    bool orphan = s->orphan;
    s_busy = false;
    xSemaphoreGive(lh_mx);
    if (orphan) {
        // The OS gave up waiting and left the session to us.
        lh_calls_free(s);
        free(s);
    } else {
        xSemaphoreGive(s_done);
    }
    vTaskDelete(NULL);
}

static void lua_task(void *arg)
{
    lh_t *s = arg;
    char rel[64], path[160];
    lua_pkg_rel(rel, sizeof(rel), s->pkg.id, "main.lua");
    os_data_path(path, sizeof(path), rel);
    reader_t *rd = malloc(sizeof(*rd));
    int rc = LUA_ERRMEM;
    s->L = rd ? lua_newstate(lh_alloc, s) : NULL;
    if (s->L) {
        *(lh_t **)lua_getextraspace(s->L) = s;
        lua_sethook(s->L, step_hook, LUA_MASKCOUNT, LH_HOOK_EVERY);
        rd->f = fopen(path, "r");
        if (rd->f) {
            lua_pushcfunction(s->L, boot);
            lua_pushlightuserdata(s->L, rd);
            rc = lua_pcall(s->L, 1, 0, 0);
            fclose(rd->f);
        } else {
            rc = LUA_ERRFILE;
            lua_pushliteral(s->L, "main.lua is missing");
        }
    }
    free(rd);
    ESP_LOGI(TAG, "%s: booted (%u bytes of Lua heap)", s->pkg.id, (unsigned)s->mem);

    if (lock(s)) {
        if (!s->L) {
            s->failed = true;
            show_error(s, "Out of memory", "Not enough memory to start the app.");
        } else if (rc != LUA_OK) {
            fail(s, rc);
        } else {
            s->next_tick = lh_now() + LH_TICK_MS;
            lh_callback(s, run_main, NULL);
            run_events(s, NULL);
        }
        lvgl_port_unlock();
    }

    while (s->L && !s->failed && !s->quit) {
        lh_ev_t ev;
        bool got = xQueueReceive(s_q, &ev, wait_ticks(s)) == pdTRUE;
        if (got && ev.gen != s->gen) {
            free(ev.data);
            continue;
        }
        if (got && ev.kind == LH_EV_QUIT) {
            break;
        }
        if (lock(s)) {
            run_events(s, got ? &ev : NULL);
            lvgl_port_unlock();
        }
        if (got) {
            free(ev.data);
        }
        if (s->L) {
            lh_storage_flush(s, false);
        }
    }
    close_state(s);
    if (s->led_used) {
        lh_led_off();
    }
    ESP_LOGI(TAG, "%s: closed (peak %u bytes, %u steps)", s->pkg.id, (unsigned)s->mem_peak,
             (unsigned)s->steps_peak);
    task_end(s);
}

// ---------------------------------------------------------------------------
// The host app (OS task)

static void host_enter(lv_obj_t *root)
{
    xSemaphoreTake(lh_mx, portMAX_DELAY);
    lua_pkg_t pkg = s_next;
    s_next.id[0] = 0;
    bool busy = s_busy;
    xSemaphoreGive(lh_mx);

    lv_obj_t *area = lv_obj_create(root);
    lv_obj_remove_style_all(area);
    lv_obj_set_size(area, OS_ROOT_W, OS_ROOT_H);
    lv_obj_set_style_bg_opa(area, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(area, ui_col(COL_BG), 0);
    lv_obj_set_scrollable(area, false);

    lh_t *s = pkg.id[0] && !busy ? calloc(1, sizeof(*s)) : NULL;
    if (!s) {
        lv_obj_t *l = ui_label(area, &sol_font_14, COL_DIM,
                               !pkg.id[0] ? "Open apps from the Apps library." : busy ? "The last app is still closing."
                               : "Not enough memory to start the app.");
        lv_obj_center(l);
        return;
    }
    s->pkg = pkg;
    s->gen = ++s_gen;
    s->t0 = lh_now();
    s->area = area;
    s->modal_ref = s->store_ref = LUA_NOREF;
    s->heap_before = heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
    xSemaphoreTake(s_done, 0);
    xSemaphoreTake(lh_mx, portMAX_DELAY);
    lh_cur = s;
    s_busy = true;
    xSemaphoreGive(lh_mx);
    if (pkg.perms & LUA_PERM_MESH) {
        lh_mesh_start(s);
    }
    if (xTaskCreate(lua_task, "lua", LH_STACK, s, LH_PRIO, &s->task) != pdPASS) {
        xSemaphoreTake(lh_mx, portMAX_DELAY);
        s_busy = false;
        s->finished = true;
        xSemaphoreGive(lh_mx);
        s->failed = true;
        show_error(s, "Out of memory", "Not enough memory to start the app.");
    }
}

static void host_exit(void)
{
    lh_t *s = lh_cur;
    if (!s) {
        return;
    }
    s->quit = true;
    lh_ev_t ev = { .kind = LH_EV_QUIT, .gen = s->gen };
    xQueueSendToFront(s_q, &ev, pdMS_TO_TICKS(50));
    bool done = s->finished || xSemaphoreTake(s_done, pdMS_TO_TICKS(LH_EXIT_WAIT_MS)) == pdTRUE;
    lh_net_stop(s);
    lh_ui_release(s);
    xSemaphoreTake(lh_mx, portMAX_DELAY);
    lh_cur = NULL;
    done = done || s->finished;
    if (!done) {
        s->orphan = true;
        ESP_LOGW(TAG, "%s: still running after %d ms; it will clean up when it stops", s->pkg.id, LH_EXIT_WAIT_MS);
    }
    xSemaphoreGive(lh_mx);
    if (done) {
        lh_calls_free(s);
        free(s);
    }
    lh_led_off();
}

static bool host_key(os_key_t key, bool repeat)
{
    lh_t *s = lh_cur;
    if (!s || s->failed || s->finished) {
        return key != OS_KEY_BACK;      // error screen: CANCEL exits
    }
    lh_ev_t ev = { .kind = LH_EV_KEY, .key = key, .repeat = repeat, .gen = s->gen };
    lh_post(&ev);
    return true;
}

const os_app_t lua_host_app = {
    .id = "lua",
    .name = s_title,
    .symbol = LV_SYMBOL_PLAY,
    .accent = COL_LPURPLE,
    .on_enter = host_enter,
    .on_exit = host_exit,
    .on_key = host_key,
};

bool lua_host_launch(const char *id)
{
    lua_pkg_t pkg;
    const char *why = NULL;
    if (!os_data_ready()) {
        why = "Insert a microSD card";
    } else if (!lua_pkg_load(id, &pkg)) {
        why = "That app isn't installed";
    } else if (lua_pkg_expired(&pkg)) {
        why = "This app has expired";
    }
    xSemaphoreTake(lh_mx, portMAX_DELAY);
    if (!why && s_busy && !lh_cur) {
        why = "The last app is still closing";
    }
    if (!why) {
        s_next = pkg;
        strlcpy(s_title, pkg.name, sizeof(s_title));
    }
    xSemaphoreGive(lh_mx);
    if (why) {
        os_toast("%s", why);
        printf("lua: %s: %s\n", id, why);
        return false;
    }
    if (os_current() == &lua_host_app) {
        os_launch(&os_launcher);        // re-entering the host needs a switch
    }
    os_launch(&lua_host_app);
    return true;
}

const char *lua_host_running(void)
{
    lh_t *s = lh_cur;
    return s && !s->failed ? s->pkg.id : NULL;
}

void lua_host_init(void)
{
    lh_mx = xSemaphoreCreateMutex();
    s_done = xSemaphoreCreateBinary();
    s_q = xQueueCreate(LH_QUEUE_LEN, sizeof(lh_ev_t));
    lh_cmd_register();
}
