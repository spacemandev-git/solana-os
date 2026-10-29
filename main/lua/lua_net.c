// mesh, lamps, items, profile and quest for Lua apps (docs/LUA_API.md).
//
// Server calls go through os_api with the app id fixed by the host. A sent
// call's os_api ctx is an os_api_token(); its answer finds the record by
// that, so an answer for a closed app finds nothing and touches no freed
// memory. Spending, transfers and deposits wait for os_confirm, which the OS draws on
// the top layer and answers from its own keys: a script can't press it.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "core/os_api.h"
#include "core/os_bridge.h"
#include "core/os_cloud.h"
#include "core/os_mesh.h"
#include "lua_priv.h"

typedef enum {
    OP_BALANCE, OP_SPEND, OP_TRANSFER, OP_DEPOSIT, OP_AWARD,
    OP_ITEMS, OP_GRANT, OP_UPDATE, OP_REMOVE,
    OP_ME, OP_PROFILE,
    OP_REPORT, OP_ACTIVE,
} op_t;

// Permission per op, and the field of the answer the callback gets (NULL = all of it).
static const struct {
    uint32_t perm;
    const char *field;
} s_ops[] = {
    [OP_BALANCE] = {LUA_PERM_LAMPS, "balance"}, [OP_SPEND] = {LUA_PERM_LAMPS, NULL},
    [OP_TRANSFER] = {LUA_PERM_LAMPS, NULL}, [OP_DEPOSIT] = {LUA_PERM_LAMPS, NULL},
    [OP_AWARD] = {LUA_PERM_LAMPS, NULL},
    [OP_ITEMS] = {LUA_PERM_ITEMS, "items"}, [OP_GRANT] = {LUA_PERM_ITEMS, NULL},
    [OP_UPDATE] = {LUA_PERM_ITEMS, NULL}, [OP_REMOVE] = {LUA_PERM_ITEMS, "ok"},
    [OP_ME] = {LUA_PERM_PROFILE, NULL}, [OP_PROFILE] = {LUA_PERM_PROFILE, NULL},
    [OP_REPORT] = {LUA_PERM_QUEST, NULL}, [OP_ACTIVE] = {LUA_PERM_QUEST, "items"},
};

static uint32_t s_token;

// ---------------------------------------------------------------------------
// Call records

static void wake(lh_t *s)
{
    lh_ev_t ev = { .kind = LH_EV_WAKE, .gen = s->gen };
    lh_post(&ev);
}

static void free_call(lh_call_t *c)
{
    free(c->body);
    free(c->method);
    free(c->path);
    free(c->req);
    free(c);
}

// os_api answer, on the OS task.
static void api_cb(int status, const char *body, void *ctx)
{
    xSemaphoreTake(lh_mx, portMAX_DELAY);
    lh_t *s = lh_cur;
    for (lh_call_t *c = s ? s->calls : NULL; c; c = c->next) {
        if (c->state == LH_CALL_SENT && c->ctx == ctx && !c->done) {
            c->status = status;
            c->body = body ? strdup(body) : NULL;
            c->done = true;
            wake(s);
            break;
        }
    }
    xSemaphoreGive(lh_mx);
}

static void finish(lh_call_t *c, const char *err)
{
    xSemaphoreTake(lh_mx, portMAX_DELAY);
    c->err = err;
    c->done = true;
    xSemaphoreGive(lh_mx);
}

static void send(lh_call_t *c)
{
    esp_err_t e = os_api_call(c->method, c->path, c->req, 0, api_cb, c->ctx);
    free(c->method);
    free(c->path);
    free(c->req);
    c->method = c->path = c->req = NULL;
    if (e != ESP_OK) {
        finish(c, e == ESP_ERR_INVALID_STATE ? "offline" : "busy");
    }
}

// Lua task: hand queued calls to os_api, LH_CALLS_SENT at a time.
static void pump(lh_t *s)
{
    for (;;) {
        lh_call_t *next = NULL;
        int sent = 0;
        xSemaphoreTake(lh_mx, portMAX_DELAY);
        for (lh_call_t *c = s->calls; c; c = c->next) {
            if (c->done) {
                continue;
            }
            if (c->state == LH_CALL_SENT) {
                sent++;
            } else if (c->state == LH_CALL_QUEUED && !next) {
                next = c;
            }
        }
        if (next && sent < LH_CALLS_SENT) {
            next->state = LH_CALL_SENT;
            next->ctx = os_api_token();
        } else {
            next = NULL;
        }
        xSemaphoreGive(lh_mx);
        if (!next) {
            return;
        }
        send(next);
    }
}

// os_confirm answer (OS task): queue the call for the Lua task, or deny it.
static void confirm_done(bool yes, void *ctx)
{
    uint32_t token = (uint32_t)(uintptr_t)ctx;
    xSemaphoreTake(lh_mx, portMAX_DELAY);
    for (lh_call_t *c = lh_cur ? lh_cur->calls : NULL; c; c = c->next) {
        if (c->token == token && !c->done && c->state == LH_CALL_CONFIRM) {
            if (yes) {
                c->state = LH_CALL_QUEUED;
            } else {
                c->err = "denied";
                c->done = true;
            }
            wake(lh_cur);
            break;
        }
    }
    xSemaphoreGive(lh_mx);
}

// Starts a call. cb_idx: the callback. body_idx: table to send as JSON, or 0.
// confirm: the dialog question for spend/transfer/deposit, or NULL.
static int start(lua_State *L, op_t op, const char *method, const char *path, int body_idx, int cb_idx,
                 const char *confirm)
{
    lh_t *s = lh_get(L);
    luaL_checktype(L, cb_idx, LUA_TFUNCTION);
    if (s->ui_off) {
        return 0;       // closing: no callback can run any more
    }
    int n = 0;
    for (lh_call_t *c = s->calls; c; c = c->next) {
        n++;
    }
    lua_pushvalue(L, cb_idx);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    // No Lua errors from here on: C memory is in play.
    lh_jbuf_t b = { .max = OS_API_RESP_DEFAULT };
    bool body_ok = !body_idx || lh_json_encode(L, body_idx, &b);
    lh_call_t *c = calloc(1, sizeof(*c));
    if (!c) {
        free(b.p);
        luaL_unref(L, LUA_REGISTRYINDEX, ref);
        return luaL_error(L, "not enough memory");
    }
    c->cb_ref = ref;
    c->op = op;
    c->token = ++s_token;
    xSemaphoreTake(lh_mx, portMAX_DELAY);
    lh_call_t **pp = &s->calls;
    while (*pp) {
        pp = &(*pp)->next;
    }
    *pp = c;
    xSemaphoreGive(lh_mx);

    const char *err = !(s->pkg.perms & s_ops[op].perm) ? "denied"
                    : !body_ok ? (strcmp(b.err, "too big") ? b.err : "request too big")
                    : n >= LH_CALLS_MAX || (confirm && os_confirm_open()) ? "busy"
                    : !os_api_online() ? "offline" : NULL;
    if (!err) {
        c->method = strdup(method);
        c->path = strdup(path);
        c->req = b.p;
        b.p = NULL;
        err = !c->method || !c->path || (body_idx && !c->req) ? "busy" : NULL;
    }
    if (err) {
        finish(c, err);
    } else if (confirm) {
        c->state = LH_CALL_CONFIRM;
        os_confirm(confirm, confirm_done, (void *)(uintptr_t)c->token);
    } else {
        pump(s);
    }
    free(b.p);
    return 0;
}

// The answer's {"error": "..."} text, or a fallback.
static void push_error(lua_State *L, lh_call_t *c)
{
    const char *msg = NULL;
    if (c->err) {
        msg = c->err;
    } else if (c->status == 0) {
        msg = "offline";
    } else {
        cJSON *j = c->body ? cJSON_Parse(c->body) : NULL;
        const cJSON *e = cJSON_GetObjectItemCaseSensitive(j, "error");
        char buf[96];
        if (c->status == 403) {
            msg = cJSON_IsString(e) && strstr(e->valuestring, "not linked") ? "not linked" : "denied";
        } else if (cJSON_IsString(e)) {
            strlcpy(buf, e->valuestring, sizeof(buf));
            msg = buf;
        } else {
            snprintf(buf, sizeof(buf), "server error %d", c->status);
            msg = buf;
        }
        cJSON_Delete(j);
        lua_pushboolean(L, 0);
        lua_pushstring(L, msg);
        return;
    }
    lua_pushboolean(L, 0);
    lua_pushstring(L, msg);
}

// Protected: builds cb(ok, result) for a finished call and runs it.
static int deliver(lua_State *L)
{
    lh_call_t *c = lua_touserdata(L, 1);
    lua_rawgeti(L, LUA_REGISTRYINDEX, c->cb_ref);
    if (c->err || c->status < 200 || c->status > 299) {
        push_error(L, c);
    } else if (lh_json_decode(L, c->body ? c->body : "", c->body ? strlen(c->body) : 0)) {
        lua_pushboolean(L, 0);
        lua_pushliteral(L, "bad answer from the server");
    } else {
        const char *field = s_ops[c->op].field;
        if (field && lua_istable(L, -1)) {
            lua_getfield(L, -1, field);
            lua_remove(L, -2);
        }
        lua_pushboolean(L, 1);
        lua_insert(L, -2);
    }
    lua_call(L, 2, 0);
    return 0;
}

void lh_calls_run(lh_t *s)
{
    pump(s);
    while (s->L && !s->failed) {
        xSemaphoreTake(lh_mx, portMAX_DELAY);
        lh_call_t **pp = &s->calls, *c = NULL;
        for (; *pp; pp = &(*pp)->next) {
            if ((*pp)->done) {
                c = *pp;
                *pp = c->next;
                break;
            }
        }
        xSemaphoreGive(lh_mx);
        if (!c) {
            return;
        }
        if (c->op == OP_SPEND || c->op == OP_TRANSFER || c->op == OP_DEPOSIT || c->op == OP_AWARD) {
            // Keep My Badge's figure in step without waiting for the next hello.
            cJSON *j = c->status >= 200 && c->status < 300 && c->body ? cJSON_Parse(c->body) : NULL;
            const cJSON *bal = cJSON_GetObjectItemCaseSensitive(j, "balance");
            if (cJSON_IsNumber(bal)) {
                os_cloud_info_t in;
                os_cloud_get_info(&in);
                os_cloud_note_lamps(bal->valueint, in.level);
            }
            cJSON_Delete(j);
        }
        lh_callback(s, deliver, c);
        if (s->L) {
            luaL_unref(s->L, LUA_REGISTRYINDEX, c->cb_ref);
        }
        free_call(c);
        pump(s);
    }
}

void lh_net_stop(lh_t *s)
{
    xSemaphoreTake(lh_mx, portMAX_DELAY);
    for (lh_call_t *c = s->calls; c; c = c->next) {
        if (!c->done && c->state == LH_CALL_SENT) {
            os_api_cancel(c->ctx);
        }
    }
    xSemaphoreGive(lh_mx);
    lh_mesh_stop();
}

void lh_calls_free(lh_t *s)
{
    while (s->calls) {
        lh_call_t *c = s->calls;
        s->calls = c->next;
        free_call(c);
    }
}

// ---------------------------------------------------------------------------
// lamps, items, profile, quest

static void app_path(lua_State *L, char *out, size_t n, const char *tail)
{
    snprintf(out, n, "/api/device/apps/%s/%s", lh_get(L)->pkg.id, tail);
}

// "<App> (<author>)" for the OS confirm dialog. App-supplied text goes
// through lua_pkg_plain, and the dialog prints the amount and recipient on
// fixed lines of their own.
static void app_label(lua_State *L, char *out, size_t n)
{
    const lua_pkg_t *p = &lh_get(L)->pkg;
    char name[21], author[25];
    lua_pkg_plain(name, sizeof(name), p->name);
    lua_pkg_plain(author, sizeof(author), p->author);
    snprintf(out, n, author[0] ? "%s (%s)" : "%s", name, author);
}

// Ids that go into a URL path: item and badge ids are short and alphanumeric.
static const char *check_id(lua_State *L, int idx)
{
    const char *id = luaL_checkstring(L, idx);
    size_t n = strlen(id);
    bool ok = n > 0 && n <= 32;
    for (size_t i = 0; ok && i < n; i++) {
        char c = id[i];
        ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
    }
    luaL_argcheck(L, ok, idx, "not a valid id");
    return id;
}

static int check_amount(lua_State *L, int idx)
{
    lua_Integer a = luaL_checkinteger(L, idx);
    luaL_argcheck(L, a > 0 && a <= 1000000, idx, "amount must be 1..1000000");
    return (int)a;
}

static int l_balance(lua_State *L)
{
    char path[80];
    app_path(L, path, sizeof(path), "lamps");
    return start(L, OP_BALANCE, "GET", path, 0, 1, NULL);
}

static int l_spend(lua_State *L)
{
    int amount = check_amount(L, 1);
    const char *sku = luaL_checkstring(L, 2);
    luaL_argcheck(L, strlen(sku) <= 32, 2, "sku is at most 32 characters");
    lua_settop(L, 3);
    lua_createtable(L, 0, 2);
    lua_pushinteger(L, amount);
    lua_setfield(L, -2, "amount");
    lua_pushvalue(L, 2);
    lua_setfield(L, -2, "sku");
    char path[80], lamps[24], app[52], what[33], q[160];
    app_path(L, path, sizeof(path), "lamps/spend");
    app_label(L, app, sizeof(app));
    lua_pkg_plain(what, sizeof(what), sku);
    snprintf(q, sizeof(q), "Pay %s\nto %s\nfor: %s", ui_fmt_lamps(lamps, sizeof(lamps), amount), app, what);
    return start(L, OP_SPEND, "POST", path, 4, 3, q);
}

static int l_transfer(lua_State *L)
{
    const char *to = check_id(L, 1);
    int amount = check_amount(L, 2);
    const char *memo = luaL_optstring(L, 3, "");
    luaL_argcheck(L, strlen(memo) <= 64, 3, "memo is at most 64 characters");
    lua_settop(L, 4);
    lua_createtable(L, 0, 3);
    lua_pushvalue(L, 1);
    lua_setfield(L, -2, "to");
    lua_pushinteger(L, amount);
    lua_setfield(L, -2, "amount");
    lua_pushstring(L, memo);
    lua_setfield(L, -2, "memo");
    char path[80], lamps[24], app[52], note[65], q[224];
    app_path(L, path, sizeof(path), "lamps/transfer");
    app_label(L, app, sizeof(app));
    lua_pkg_plain(note, sizeof(note), memo);
    snprintf(q, sizeof(q), "Send %s\nto badge %s\nfrom %s%s%s", ui_fmt_lamps(lamps, sizeof(lamps), amount), to, app,
             note[0] ? "\nmemo: " : "", note);
    return start(L, OP_TRANSFER, "POST", path, 5, 4, q);
}

// Into an escrow (an "ESC-..." address) of the app's author: the server
// refuses anyone else's, so the dialog can name the app as the recipient.
static int l_deposit(lua_State *L)
{
    const char *to = check_id(L, 1);
    int amount = check_amount(L, 2);
    const char *memo = luaL_optstring(L, 3, "");
    luaL_argcheck(L, strlen(memo) <= 64, 3, "memo is at most 64 characters");
    lua_settop(L, 4);
    lua_createtable(L, 0, 3);
    lua_pushvalue(L, 1);
    lua_setfield(L, -2, "to");
    lua_pushinteger(L, amount);
    lua_setfield(L, -2, "amount");
    lua_pushstring(L, memo);
    lua_setfield(L, -2, "memo");
    char path[80], lamps[24], app[52], note[65], q[224];
    app_path(L, path, sizeof(path), "lamps/deposit");
    app_label(L, app, sizeof(app));
    lua_pkg_plain(note, sizeof(note), memo);
    snprintf(q, sizeof(q), "Pay %s\ninto escrow %s\nof %s%s%s", ui_fmt_lamps(lamps, sizeof(lamps), amount), to, app,
             note[0] ? "\nmemo: " : "", note);
    return start(L, OP_DEPOSIT, "POST", path, 5, 4, q);
}

static int l_award(lua_State *L)
{
    int amount = check_amount(L, 1);
    luaL_checkstring(L, 2);
    lua_settop(L, 3);
    lua_createtable(L, 0, 2);
    lua_pushinteger(L, amount);
    lua_setfield(L, -2, "amount");
    lua_pushvalue(L, 2);
    lua_setfield(L, -2, "reason");
    char path[80];
    app_path(L, path, sizeof(path), "lamps/award");
    return start(L, OP_AWARD, "POST", path, 4, 3, NULL);
}

static int items_get(lua_State *L, bool all)
{
    char path[80];
    app_path(L, path, sizeof(path), all ? "items?all=1" : "items?all=0");
    return start(L, OP_ITEMS, "GET", path, 0, 1, NULL);
}

static int l_items_list(lua_State *L)
{
    return items_get(L, false);
}

static int l_items_all(lua_State *L)
{
    return items_get(L, true);
}

static int l_grant(lua_State *L)
{
    luaL_checkstring(L, 1);
    luaL_checkstring(L, 2);
    luaL_checktype(L, 3, LUA_TTABLE);
    lua_settop(L, 4);
    lua_createtable(L, 0, 3);
    lua_pushvalue(L, 1);
    lua_setfield(L, -2, "kind");
    lua_pushvalue(L, 2);
    lua_setfield(L, -2, "name");
    lua_pushvalue(L, 3);
    lua_setfield(L, -2, "data");
    char path[80];
    app_path(L, path, sizeof(path), "items");
    return start(L, OP_GRANT, "POST", path, 5, 4, NULL);
}

static int l_update(lua_State *L)
{
    const char *id = check_id(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE);
    lua_settop(L, 3);
    lua_createtable(L, 0, 1);
    lua_pushvalue(L, 2);
    lua_setfield(L, -2, "data");
    char tail[48], path[96];
    snprintf(tail, sizeof(tail), "items/%s", id);
    app_path(L, path, sizeof(path), tail);
    return start(L, OP_UPDATE, "PATCH", path, 4, 3, NULL);
}

static int l_items_remove(lua_State *L)
{
    const char *id = check_id(L, 1);
    char tail[48], path[96];
    snprintf(tail, sizeof(tail), "items/%s", id);
    app_path(L, path, sizeof(path), tail);
    return start(L, OP_REMOVE, "DELETE", path, 0, 2, NULL);
}

static int l_me(lua_State *L)
{
    char path[80];
    app_path(L, path, sizeof(path), "profile");
    return start(L, OP_ME, "GET", path, 0, 1, NULL);
}

static int l_profile_get(lua_State *L)
{
    const char *id = check_id(L, 1);
    char tail[48], path[96];
    snprintf(tail, sizeof(tail), "profile/%s", id);
    app_path(L, path, sizeof(path), tail);
    return start(L, OP_PROFILE, "GET", path, 0, 2, NULL);
}

static int l_report(lua_State *L)
{
    luaL_checkstring(L, 1);
    if (lua_isfunction(L, 2)) {         // quest.report(event, cb): no data
        lua_pushnil(L);
        lua_insert(L, 2);
    }
    lua_settop(L, 3);
    lua_createtable(L, 0, 2);
    lua_pushvalue(L, 1);
    lua_setfield(L, -2, "event");
    if (!lua_isnil(L, 2)) {
        luaL_checktype(L, 2, LUA_TTABLE);
        lua_pushvalue(L, 2);
        lua_setfield(L, -2, "data");
    }
    char path[80];
    app_path(L, path, sizeof(path), "quest");
    return start(L, OP_REPORT, "POST", path, 4, 3, NULL);
}

static int l_active(lua_State *L)
{
    char path[80];
    app_path(L, path, sizeof(path), "quest");
    return start(L, OP_ACTIVE, "GET", path, 0, 1, NULL);
}

// ---------------------------------------------------------------------------
// mesh: APP messages {"app":id,"to":to,"d":msg} (docs/MESH.md), only between
// badges running the same app.

static int s_mesh_sub = -1;
static bool s_mesh_acquired;
static uint32_t s_mesh_gen;
static char s_mesh_app[LUA_PKG_ID_MAX + 1];     // "" = queue nothing
static char s_mesh_me[OS_MESH_ID_LEN + 1];

// On the mesh task: filter, copy, queue for the Lua task.
static void mesh_handler(os_mesh_type_t type, const os_mesh_peer_t *from, const char *json, void *ctx)
{
    cJSON *m = cJSON_Parse(json);
    const cJSON *app = cJSON_GetObjectItemCaseSensitive(m, "app");
    const cJSON *to = cJSON_GetObjectItemCaseSensitive(m, "to");
    const cJSON *d = cJSON_GetObjectItemCaseSensitive(m, "d");
    char *text = NULL;
    xSemaphoreTake(lh_mx, portMAX_DELAY);
    bool mine = s_mesh_app[0] && cJSON_IsString(app) && !strcmp(app->valuestring, s_mesh_app)
                && cJSON_IsString(to) && (!strcmp(to->valuestring, "*") || !s_mesh_me[0]
                                          || !strcmp(to->valuestring, s_mesh_me));
    uint32_t gen = s_mesh_gen;
    xSemaphoreGive(lh_mx);
    if (mine && cJSON_IsObject(d) && (text = cJSON_PrintUnformatted(d))) {
        size_t idn = strlen(from->id), n = strlen(text);
        char *data = malloc(idn + 1 + n + 1);
        if (data) {
            memcpy(data, from->id, idn + 1);
            memcpy(data + idn + 1, text, n + 1);
            lh_ev_t ev = { .kind = LH_EV_MESH, .gen = gen, .data = data };
            if (!lh_post(&ev)) {
                free(data);
            }
        }
        cJSON_free(text);
    }
    cJSON_Delete(m);
}

void lh_mesh_start(lh_t *s)
{
    os_cloud_info_t in;
    os_cloud_get_info(&in);
    xSemaphoreTake(lh_mx, portMAX_DELAY);
    strlcpy(s_mesh_app, s->pkg.id, sizeof(s_mesh_app));
    strlcpy(s_mesh_me, in.id, sizeof(s_mesh_me));
    s_mesh_gen = s->gen;
    xSemaphoreGive(lh_mx);
    os_mesh_acquire();
    s_mesh_acquired = true;
    s_mesh_sub = os_mesh_subscribe(OS_MESH_APP, mesh_handler, NULL);
}

void lh_mesh_mute(void)
{
    xSemaphoreTake(lh_mx, portMAX_DELAY);
    s_mesh_app[0] = 0;
    xSemaphoreGive(lh_mx);
}

void lh_mesh_stop(void)
{
    lh_mesh_mute();
    if (s_mesh_sub >= 0) {
        os_mesh_unsubscribe(s_mesh_sub);    // waits for a handler that is running
        s_mesh_sub = -1;
    }
    if (s_mesh_acquired) {
        os_mesh_release();
        s_mesh_acquired = false;
    }
}

// Protected: on_mesh(from, msg).
static int mesh_deliver(lua_State *L)
{
    const char *data = lua_touserdata(L, 1);
    const char *json = data + strlen(data) + 1;
    lua_getglobal(L, "on_mesh");
    if (!lua_isfunction(L, -1)) {
        return 0;
    }
    lua_pushstring(L, data);
    if (lh_json_decode(L, json, strlen(json))) {
        return 0;
    }
    lua_call(L, 2, 0);
    return 0;
}

void lh_mesh_event(lh_t *s, char *data)
{
    lh_callback(s, mesh_deliver, data);
}

static int l_mesh_nearby(lua_State *L)
{
    lh_need_perm(L, LUA_PERM_MESH, "mesh");
    enum { MAX_PEERS = 16 };
    // Scratch space from the script's heap, so an error below can't leak it.
    os_mesh_peer_t *peers = lua_newuserdatauv(L, MAX_PEERS * sizeof(os_mesh_peer_t), 0);
    int n = os_mesh_peers(peers, MAX_PEERS);
    lua_createtable(L, n, 0);
    for (int i = 0; i < n; i++) {
        lua_createtable(L, 0, 6);
        lua_pushstring(L, peers[i].id);
        lua_setfield(L, -2, "id");
        lua_pushstring(L, peers[i].name);
        lua_setfield(L, -2, "name");
        lua_pushinteger(L, peers[i].level);
        lua_setfield(L, -2, "level");
        lua_pushinteger(L, peers[i].rssi);
        lua_setfield(L, -2, "rssi");
        lua_pushstring(L, peers[i].kind == OS_PEER_BEACON ? "beacon" : "badge");
        lua_setfield(L, -2, "kind");
        lua_pushboolean(L, peers[i].bridged);
        lua_setfield(L, -2, "bridged");
        lua_rawseti(L, -2, i + 1);
    }
    return 1;
}

static int l_mesh_send(lua_State *L)
{
    lh_need_perm(L, LUA_PERM_MESH, "mesh");
    lh_t *s = lh_get(L);
    const char *to = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE);
    bool bcast = !strcmp(to, "*");
    luaL_argcheck(L, bcast || strlen(to) == OS_MESH_ID_LEN, 1, "a badge id or \"*\"");
    int64_t now = lh_now();
    int oldest = 0;
    for (int i = 1; i < LH_MESH_PER_S; i++) {
        if (s->mesh_sent[i] < s->mesh_sent[oldest]) {
            oldest = i;
        }
    }
    if (s->mesh_sent[oldest] && now - s->mesh_sent[oldest] < 1000) {
        lua_pushnil(L);
        lua_pushliteral(L, "too fast: 4 messages per second");
        return 2;
    }
    lh_jbuf_t d = { .max = LH_MESH_MAX };
    if (!lh_json_encode(L, 2, &d)) {
        free(d.p);
        lua_pushnil(L);
        lua_pushstring(L, strcmp(d.err, "too big") ? d.err : "message over 200 bytes");
        return 2;
    }
    char msg[LH_MESH_MAX + 64];
    snprintf(msg, sizeof(msg), "{\"app\":\"%s\",\"to\":\"%s\",\"d\":%s}", s->pkg.id, to, d.p);
    free(d.p);
    esp_err_t e = os_mesh_send(OS_MESH_APP, bcast ? NULL : to, msg);
    if (e != ESP_OK) {
        lua_pushnil(L);
        if (e == ESP_ERR_NO_MEM) {
            lua_pushliteral(L, "too fast: the bridge is busy");
        } else if (e == ESP_ERR_NOT_FOUND) {
            lua_pushliteral(L, "not nearby");
        } else {
            lua_pushliteral(L, "the mesh is off");
        }
        return 2;
    }
    s->mesh_sent[oldest] = now;
    lua_pushboolean(L, 1);
    return 1;
}

// mesh.bridge(): the bridge code over the app (core/os_bridge.h); the OS
// panel takes the keys until CANCEL, so the script can't press anything there.
static int l_mesh_bridge(lua_State *L)
{
    lh_need_perm(L, LUA_PERM_MESH, "mesh");
    os_bridge_show();
    return 0;
}

void lh_open_net(lua_State *L)
{
    static const luaL_Reg lamps[] = {
        {"balance", l_balance}, {"spend", l_spend}, {"transfer", l_transfer}, {"deposit", l_deposit},
        {"award", l_award}, {NULL, NULL},
    };
    static const luaL_Reg items[] = {
        {"list", l_items_list}, {"all", l_items_all}, {"grant", l_grant}, {"update", l_update},
        {"remove", l_items_remove}, {NULL, NULL},
    };
    static const luaL_Reg profile[] = {{"me", l_me}, {"get", l_profile_get}, {NULL, NULL}};
    static const luaL_Reg quest[] = {{"report", l_report}, {"active", l_active}, {NULL, NULL}};
    static const luaL_Reg mesh[] = {{"nearby", l_mesh_nearby}, {"send", l_mesh_send}, {"bridge", l_mesh_bridge},
                                    {NULL, NULL}};
    luaL_newlib(L, lamps);
    lua_setglobal(L, "lamps");
    luaL_newlib(L, items);
    lua_setglobal(L, "items");
    luaL_newlib(L, profile);
    lua_setglobal(L, "profile");
    luaL_newlib(L, quest);
    lua_setglobal(L, "quest");
    luaL_newlib(L, mesh);
    lua_setglobal(L, "mesh");
}
