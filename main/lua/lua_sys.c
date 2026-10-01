// led, timer, sys, badge, storage and wifi for Lua apps (docs/LUA_API.md).

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bsp.h"
#include "core/os_api.h"
#include "core/os_cloud.h"
#include "core/os_data.h"
#include "core/os_wifi.h"
#include "lua_priv.h"

// ---------------------------------------------------------------------------
// led: a shadow copy, shown (scaled to OS_LED_MAX) after each callback

static int channel(lua_State *L, int idx)
{
    lua_Integer v = luaL_checkinteger(L, idx);
    return v < 0 ? 0 : v > 255 ? 255 : (int)v;
}

static lh_t *led_session(lua_State *L)
{
    lh_need_perm(L, LUA_PERM_LED, "led");
    return lh_get(L);
}

static int led_index(lua_State *L)
{
    lua_Integer i = luaL_checkinteger(L, 1);
    luaL_argcheck(L, i >= 1 && i <= LH_LED_COUNT, 1, "LEDs are 1..8");
    return (int)i - 1;
}

static int l_led_set(lua_State *L)
{
    lh_t *s = led_session(L);
    int i = led_index(L);
    s->led[i] = channel(L, 2) << 16 | channel(L, 3) << 8 | channel(L, 4);
    s->led_dirty = true;
    return 0;
}

static int l_led_hex(lua_State *L)
{
    lh_t *s = led_session(L);
    int i = led_index(L);
    s->led[i] = (uint32_t)luaL_checkinteger(L, 2) & 0xFFFFFF;
    s->led_dirty = true;
    return 0;
}

static void led_fill(lh_t *s, uint32_t rgb)
{
    for (int i = 0; i < LH_LED_COUNT; i++) {
        s->led[i] = rgb;
    }
    s->led_dirty = true;
}

static int l_led_fill(lua_State *L)
{
    lh_t *s = led_session(L);
    led_fill(s, channel(L, 1) << 16 | channel(L, 2) << 8 | channel(L, 3));
    return 0;
}

static int l_led_off(lua_State *L)
{
    led_fill(led_session(L), 0);
    return 0;
}

void lh_led_flush(lh_t *s)
{
    if (!s->led_dirty) {
        return;
    }
    for (int i = 0; i < LH_LED_COUNT; i++) {
        uint32_t c = s->led[i];
        bsp_rgb_set(i, (c >> 16 & 0xFF) * OS_LED_MAX / 255, (c >> 8 & 0xFF) * OS_LED_MAX / 255,
                    (c & 0xFF) * OS_LED_MAX / 255);
    }
    bsp_rgb_show();
    s->led_dirty = false;
    s->led_used = true;
}

void lh_led_off(void)
{
    bsp_rgb_fill(0, 0, 0);
    bsp_rgb_show();
}

// ---------------------------------------------------------------------------
// timer

static int add_timer(lua_State *L, bool every)
{
    lh_t *s = lh_get(L);
    lua_Integer ms = luaL_checkinteger(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    if (s->ui_off) {
        return 0;       // closing: nothing will fire
    }
    if (ms < LH_TIMER_MIN_MS) {
        ms = LH_TIMER_MIN_MS;
    }
    for (int i = 0; i < LH_TIMER_MAX; i++) {
        lh_timer_t *t = &s->timers[i];
        if (t->id) {
            continue;
        }
        lua_pushvalue(L, 2);
        t->fn_ref = luaL_ref(L, LUA_REGISTRYINDEX);
        t->id = ++s->timer_seq;
        t->period = every ? (uint32_t)ms : 0;
        t->due = lh_now() + ms;
        lua_pushinteger(L, t->id);
        return 1;
    }
    return luaL_error(L, "too many timers (%d)", LH_TIMER_MAX);
}

static int l_after(lua_State *L)
{
    return add_timer(L, false);
}

static int l_every(lua_State *L)
{
    return add_timer(L, true);
}

static int l_cancel(lua_State *L)
{
    lh_t *s = lh_get(L);
    lua_Integer id = luaL_checkinteger(L, 1);
    for (int i = 0; id && i < LH_TIMER_MAX; i++) {
        lh_timer_t *t = &s->timers[i];
        if (t->id == id) {
            luaL_unref(L, LUA_REGISTRYINDEX, t->fn_ref);
            t->id = 0;
        }
    }
    return 0;
}

int64_t lh_timers_next(lh_t *s)
{
    int64_t next = INT64_MAX;
    for (int i = 0; i < LH_TIMER_MAX; i++) {
        if (s->timers[i].id && s->timers[i].due < next) {
            next = s->timers[i].due;
        }
    }
    return next;
}

typedef struct {
    int ref;
    bool once;
} fire_t;

static int timer_cb(lua_State *L)
{
    const fire_t *f = lua_touserdata(L, 1);
    lua_rawgeti(L, LUA_REGISTRYINDEX, f->ref);
    if (f->once) {
        luaL_unref(L, LUA_REGISTRYINDEX, f->ref);
    }
    lua_call(L, 0, 0);
    return 0;
}

void lh_timers_run(lh_t *s)
{
    // Bounded, so a timer slower than its period can't loop forever.
    for (int n = 0; n < LH_TIMER_MAX && s->L && !s->failed && !s->exit_req; n++) {
        int64_t now = lh_now();
        lh_timer_t *due = NULL;
        for (int i = 0; i < LH_TIMER_MAX; i++) {
            lh_timer_t *t = &s->timers[i];
            if (t->id && t->due <= now && (!due || t->due < due->due)) {
                due = t;
            }
        }
        if (!due) {
            return;
        }
        fire_t f = { .ref = due->fn_ref, .once = !due->period };
        if (due->period) {
            due->due += due->period;
            if (due->due <= now) {
                due->due = now + due->period;
            }
        } else {
            due->id = 0;
        }
        lh_callback(s, timer_cb, &f);
    }
}

// ---------------------------------------------------------------------------
// sys, badge

static int l_millis(lua_State *L)
{
    lua_pushinteger(L, (lua_Integer)(lh_now() - lh_get(L)->t0));
    return 1;
}

static int l_time(lua_State *L)
{
    int64_t ms = os_cloud_time_ms();
    if (ms) {
        lua_pushinteger(L, (lua_Integer)(ms / 1000));
    } else {
        lua_pushnil(L);
    }
    return 1;
}

static int l_battery(lua_State *L)
{
    int mv = bsp_battery_mv();
    if (mv < 0 || mv > 4300) {      // unknown, or on USB power
        lua_pushnil(L);
    } else {
        lua_pushinteger(L, bsp_battery_percent(mv));
    }
    return 1;
}

static int l_exit(lua_State *L)
{
    lh_get(L)->exit_req = true;
    return 0;
}

static int l_badge_id(lua_State *L)
{
    os_cloud_info_t in;
    os_cloud_get_info(&in);
    if (in.id[0]) {
        lua_pushstring(L, in.id);
    } else {
        lua_pushnil(L);
    }
    return 1;
}

static int l_badge_owner(lua_State *L)
{
    os_cloud_info_t in;
    os_cloud_get_info(&in);
    if (in.state == OS_CLOUD_CLAIMED && in.owner[0]) {
        lua_pushstring(L, in.owner);
    } else {
        lua_pushnil(L);
    }
    return 1;
}

static int l_badge_online(lua_State *L)
{
    lua_pushboolean(L, os_api_online());
    return 1;
}

// ---------------------------------------------------------------------------
// storage: data.json, kept as a table; set() re-encodes it (≤ 16 KB) and the
// write happens at most once a second, and on exit.

static int store_table(lua_State *L, lh_t *s)
{
    lua_rawgeti(L, LUA_REGISTRYINDEX, s->store_ref);
    return lua_gettop(L);
}

static int l_get(lua_State *L)
{
    lh_t *s = lh_get(L);
    luaL_checkstring(L, 1);
    int t = store_table(L, s);
    lua_pushvalue(L, 1);
    lua_rawget(L, t);
    return 1;
}

static int l_set(lua_State *L)
{
    lh_t *s = lh_get(L);
    luaL_checkstring(L, 1);
    int type = lua_type(L, 2);
    luaL_argcheck(L, type == LUA_TNIL || type == LUA_TNONE || type == LUA_TNUMBER || type == LUA_TSTRING
                  || type == LUA_TBOOLEAN || type == LUA_TTABLE, 2, "storage takes numbers, strings, booleans and tables");
    lua_settop(L, 2);
    int t = store_table(L, s);
    lua_pushvalue(L, 1);
    lua_rawget(L, t);               // old value, kept to undo
    lua_pushvalue(L, 1);
    lua_pushvalue(L, 2);
    lua_rawset(L, t);
    lh_jbuf_t b = { .max = LH_STORAGE_MAX };
    if (!lh_json_encode(L, t, &b)) {
        free(b.p);
        lua_pushvalue(L, 1);
        lua_pushvalue(L, -2);
        lua_rawset(L, t);
        return luaL_error(L, "storage: %s", !strcmp(b.err, "too big") ? "full (16 KB per app)" : b.err);
    }
    free(s->store_pending);
    s->store_pending = b.p;
    s->store_len = b.len;
    return 0;
}

static int l_remove(lua_State *L)
{
    lua_settop(L, 1);
    lua_pushnil(L);
    return l_set(L);
}

bool lh_storage_load(lh_t *s)
{
    lua_State *L = s->L;
    char rel[64];
    lua_pkg_rel(rel, sizeof(rel), s->pkg.id, "data.json");
    size_t len = 0;
    int top = lua_gettop(L);
    char *json = os_data_read(rel, LH_STORAGE_MAX, &len);
    const char *err = json ? lh_json_decode(L, json, len) : "missing";
    free(json);
    if (err || !lua_istable(L, -1)) {
        lua_settop(L, top);
        lua_newtable(L);
    }
    s->store_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    return true;
}

void lh_storage_flush(lh_t *s, bool force)
{
    if (!s->store_pending || (!force && lh_now() - s->store_saved < LH_STORAGE_SAVE_MS)) {
        return;
    }
    char rel[64];
    lua_pkg_rel(rel, sizeof(rel), s->pkg.id, "data.json");
    if (!os_data_write(rel, s->store_pending, s->store_len)) {
        printf("[lua:%s] storage: write failed\n", s->pkg.id);
    }
    free(s->store_pending);
    s->store_pending = NULL;
    s->store_saved = lh_now();
}

// ---------------------------------------------------------------------------
// wifi

static int l_wifi_status(lua_State *L)
{
    lh_need_perm(L, LUA_PERM_WIFI, "wifi");
    const char *st = "off";
    switch (os_wifi_state()) {
    case OS_WIFI_CONNECTED:  st = "on"; break;
    case OS_WIFI_CONNECTING: st = "connecting"; break;
    case OS_WIFI_FAILED:     st = "failed"; break;
    default: break;
    }
    lua_pushstring(L, st);
    return 1;
}

static int l_wifi_ssid(lua_State *L)
{
    lh_need_perm(L, LUA_PERM_WIFI, "wifi");
    char ssid[33];
    if (os_wifi_saved_ssid(ssid, sizeof(ssid)) && ssid[0]) {
        lua_pushstring(L, ssid);
    } else {
        lua_pushnil(L);
    }
    return 1;
}

static int l_wifi_on(lua_State *L)
{
    lh_need_perm(L, LUA_PERM_WIFI, "wifi");
    os_wifi_set_enabled(true);
    return 0;
}

static int l_wifi_off(lua_State *L)
{
    lh_need_perm(L, LUA_PERM_WIFI, "wifi");
    os_wifi_set_enabled(false);
    return 0;
}

// ---------------------------------------------------------------------------

void lh_open_sys(lua_State *L)
{
    static const luaL_Reg led[] = {
        {"set", l_led_set}, {"hex", l_led_hex}, {"fill", l_led_fill}, {"off", l_led_off}, {NULL, NULL},
    };
    static const luaL_Reg timer[] = {{"after", l_after}, {"every", l_every}, {"cancel", l_cancel}, {NULL, NULL}};
    static const luaL_Reg sys[] = {
        {"millis", l_millis}, {"time", l_time}, {"battery", l_battery}, {"exit", l_exit}, {NULL, NULL},
    };
    static const luaL_Reg badge[] = {
        {"id", l_badge_id}, {"owner", l_badge_owner}, {"online", l_badge_online}, {NULL, NULL},
    };
    static const luaL_Reg storage[] = {{"get", l_get}, {"set", l_set}, {"remove", l_remove}, {NULL, NULL}};
    static const luaL_Reg wifi[] = {
        {"status", l_wifi_status}, {"ssid", l_wifi_ssid}, {"on", l_wifi_on}, {"off", l_wifi_off}, {NULL, NULL},
    };
    luaL_newlib(L, led);
    lua_pushinteger(L, LH_LED_COUNT);
    lua_setfield(L, -2, "count");
    lua_setglobal(L, "led");
    luaL_newlib(L, timer);
    lua_setglobal(L, "timer");
    luaL_newlib(L, sys);
    lua_setglobal(L, "sys");
    luaL_newlib(L, badge);
    lua_setglobal(L, "badge");
    luaL_newlib(L, storage);
    lua_setglobal(L, "storage");
    luaL_newlib(L, wifi);
    lua_setglobal(L, "wifi");
}
