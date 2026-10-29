// ws for Lua apps (docs/LUA_API.md "ws"): one socket per app.
//
// ws.open first asks the server for a ticket (POST /api/device/apps/:id/ws,
// through os_api like any call), then hands the URL it signed to os_socket,
// which the cloud task connects in place of the server connection. The
// socket's events wait in os_socket until the Lua task takes them (wake()
// only says there are some). Sockets that never get that far (denied, busy,
// offline, closed by the app) close from a short list here, on the next
// round, so every socket ends with one "close" and never inside ws.open.
//
// The ticket's answer comes on the OS task and the wake on the cloud task:
// both find the session through lh_cur under lh_mx, by ctxs that are never
// pointers (os_api_token), so an answer for a closed app touches nothing.

#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "core/os_api.h"
#include "core/os_socket.h"
#include "lua_priv.h"

#define WS_META         "ws.socket"
#define WS_URL_MAX      512
#define WS_PROTOCOLS    4
#define WS_PROTOCOL_MAX 32

typedef struct {
    uint32_t id;
} ws_handle_t;

typedef struct {
    int ref;
    const char *event;
    const char *data;                   // a message or a reason, or NULL
    size_t len;
    int code;                           // the host's close code, 0 = none
} ws_cb_t;

static void wake(lh_t *s)
{
    lh_ev_t ev = { .kind = LH_EV_WAKE, .gen = s->gen };
    lh_post(&ev);
}

// os_socket: an event waits (cloud task).
static void sock_wake(void *ctx)
{
    xSemaphoreTake(lh_mx, portMAX_DELAY);
    if (lh_cur && lh_cur->ws.sock == ctx) {
        wake(lh_cur);
    }
    xSemaphoreGive(lh_mx);
}

// The ticket's answer (OS task).
static void ticket_cb(int status, const char *body, void *ctx)
{
    xSemaphoreTake(lh_mx, portMAX_DELAY);
    lh_t *s = lh_cur;
    if (s && s->ws.state == LH_WS_TICKET && s->ws.call == ctx && !s->ws.answered) {
        s->ws.status = status;
        s->ws.body = body ? strdup(body) : NULL;
        s->ws.answered = true;
        wake(s);
    }
    xSemaphoreGive(lh_mx);
}

// Protected: cb(event, data?, code?).
static int call_cb(lua_State *L)
{
    const ws_cb_t *a = lua_touserdata(L, 1);
    lua_rawgeti(L, LUA_REGISTRYINDEX, a->ref);
    lua_pushstring(L, a->event);
    int n = 1;
    if (a->data) {
        lua_pushlstring(L, a->data, a->len);
        n++;
        if (a->code) {
            lua_pushinteger(L, a->code);
            n++;
        }
    }
    lua_call(L, n, 0);
    return 0;
}

static void run_cb(lh_t *s, int ref, const char *event, const char *data, size_t len, int code)
{
    ws_cb_t a = { .ref = ref, .event = event, .data = data, .len = len, .code = code };
    lh_callback(s, call_cb, &a);
}

// A socket that ends before (or instead of) reaching os_socket: its close
// goes out on the next round. Takes the reference.
static void close_later(lua_State *L, lh_t *s, int ref, const char *reason)
{
    if (s->ws.nlate >= LH_WS_LATE) {
        luaL_unref(L, LUA_REGISTRYINDEX, ref);
        luaL_error(L, "one socket at a time");
    }
    s->ws.late[s->ws.nlate].ref = ref;
    s->ws.late[s->ws.nlate].reason = reason;
    s->ws.nlate++;
    wake(s);
}

// The live socket is over from here: nothing of it is left but the callback,
// which the caller runs (or hands to close_later) and frees. Lua task.
static int detach(lh_t *s)
{
    xSemaphoreTake(lh_mx, portMAX_DELAY);
    void *sock = s->ws.sock;
    int ref = s->ws.cb_ref;
    s->ws.state = LH_WS_NONE;
    s->ws.id = 0;
    s->ws.sock = s->ws.call = NULL;
    s->ws.cb_ref = LUA_NOREF;
    s->ws.answered = false;
    free(s->ws.body);
    s->ws.body = NULL;
    xSemaphoreGive(lh_mx);
    free(s->ws.protocols);
    s->ws.protocols = NULL;
    if (sock) {
        os_socket_close(sock);
    }
    return ref;
}

// The live socket ended: its "close" now.
static void end(lh_t *s, const char *reason, int code)
{
    int ref = detach(s);
    run_cb(s, ref, "close", reason, strlen(reason), code);
    if (s->L) {
        luaL_unref(s->L, LUA_REGISTRYINDEX, ref);
    }
}

// What a refused ticket means for the app, as push_error() words it for calls.
static void ticket_error(int status, const char *body, char *out, size_t n)
{
    if (status == 0) {
        strlcpy(out, "offline", n);
        return;
    }
    cJSON *j = body ? cJSON_Parse(body) : NULL;
    const char *e = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(j, "error"));
    if (status == 403) {
        strlcpy(out, e && strstr(e, "not linked") ? "not linked" : "denied", n);
    } else if (e) {
        strlcpy(out, e, n);
    } else {
        snprintf(out, n, "server error %d", status);
    }
    cJSON_Delete(j);
}

// The ticket came back: open the socket with it, or end it.
static void ticket_done(lh_t *s)
{
    xSemaphoreTake(lh_mx, portMAX_DELAY);
    bool answered = s->ws.answered;
    int status = s->ws.status;
    char *body = s->ws.body;
    s->ws.body = NULL;
    xSemaphoreGive(lh_mx);
    if (!answered) {
        return;
    }
    char why[48];
    cJSON *j = status == 200 && body ? cJSON_Parse(body) : NULL;
    const char *url = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(j, "url"));
    if (status == 200) {
        void *ctx = os_api_token();
        xSemaphoreTake(lh_mx, portMAX_DELAY);
        s->ws.sock = ctx;                   // before os_socket can wake us for it
        s->ws.state = LH_WS_CONNECTING;
        xSemaphoreGive(lh_mx);
        esp_err_t err = url ? os_socket_open(url, s->ws.protocols, sock_wake, ctx) : ESP_ERR_INVALID_ARG;
        if (err != ESP_OK) {
            xSemaphoreTake(lh_mx, portMAX_DELAY);
            s->ws.sock = NULL;
            xSemaphoreGive(lh_mx);
            strlcpy(why, err == ESP_ERR_INVALID_STATE ? "busy" : err == ESP_ERR_NO_MEM ? "not enough memory"
                         : "bad answer from the server", sizeof(why));
        }
        status = err == ESP_OK ? 200 : -1;
    } else {
        ticket_error(status, body, why, sizeof(why));
    }
    cJSON_Delete(j);
    free(body);
    if (status != 200) {
        end(s, why, 0);
    }
}

void lh_ws_run(lh_t *s)
{
    while (s->L && !s->failed && s->ws.nlate) {
        int ref = s->ws.late[0].ref;
        const char *reason = s->ws.late[0].reason;
        memmove(&s->ws.late[0], &s->ws.late[1], --s->ws.nlate * sizeof(s->ws.late[0]));
        run_cb(s, ref, "close", reason, strlen(reason), 0);
        if (s->L) {
            luaL_unref(s->L, LUA_REGISTRYINDEX, ref);
        }
    }
    if (s->L && !s->failed && s->ws.state == LH_WS_TICKET) {
        ticket_done(s);
    }
    os_socket_ev_t ev;
    while (s->L && !s->failed && s->ws.sock && os_socket_next(s->ws.sock, &ev)) {
        if (ev.kind == OS_SOCKET_EV_OPEN) {
            s->ws.state = LH_WS_OPEN;
            run_cb(s, s->ws.cb_ref, "open", NULL, 0, 0);
        } else if (ev.kind == OS_SOCKET_EV_MESSAGE) {
            run_cb(s, s->ws.cb_ref, "message", ev.data, ev.len, 0);
            free(ev.data);
        } else {
            xSemaphoreTake(lh_mx, portMAX_DELAY);
            s->ws.sock = NULL;              // os_socket has let it go
            xSemaphoreGive(lh_mx);
            end(s, ev.reason, ev.code);
        }
    }
}

bool lh_ws_busy(const lh_t *s)
{
    return s->ws.state != LH_WS_NONE;
}

void lh_ws_stop(lh_t *s)
{
    xSemaphoreTake(lh_mx, portMAX_DELAY);
    void *sock = s->ws.sock;
    s->ws.sock = s->ws.call = NULL;
    s->ws.state = LH_WS_NONE;
    xSemaphoreGive(lh_mx);
    if (sock) {
        os_socket_close(sock);
    }
}

void lh_ws_free(lh_t *s)
{
    free(s->ws.body);
    free(s->ws.protocols);
    s->ws.body = s->ws.protocols = NULL;
}

// ---------------------------------------------------------------------------
// The module

static bool url_text(const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (s[i] < 0x21 || s[i] > 0x7E) {
            return false;
        }
    }
    return true;
}

// opts.protocols -> "a, b" in out (empty if none).
static void protocols_of(lua_State *L, int opts, char *out, size_t n)
{
    out[0] = 0;
    if (lua_isnoneornil(L, opts)) {
        return;
    }
    luaL_argcheck(L, lua_istable(L, opts), opts, "options are a table");
    lua_getfield(L, opts, "protocols");
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        return;
    }
    luaL_argcheck(L, lua_istable(L, -1), opts, "protocols is a list of names");
    int count = (int)lua_rawlen(L, -1);
    luaL_argcheck(L, count <= WS_PROTOCOLS, opts, "at most 4 protocols");
    for (int i = 1; i <= count; i++) {
        lua_rawgeti(L, -1, i);
        size_t len = 0;
        const char *p = lua_type(L, -1) == LUA_TSTRING ? lua_tolstring(L, -1, &len) : NULL;
        bool ok = p && len > 0 && len <= WS_PROTOCOL_MAX;
        for (size_t k = 0; ok && k < len; k++) {
            char c = p[k];
            ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-'
                 || c == '_';
        }
        luaL_argcheck(L, ok, opts, "protocols are letters, digits, . - and _, at most 32");
        if (out[0]) {
            strlcat(out, ", ", n);
        }
        strlcat(out, p, n);
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
}

// ws.open(url, [opts], cb) -> socket
static int l_open(lua_State *L)
{
    lh_t *s = lh_get(L);
    bool short_form = lua_isfunction(L, 2);
    int opts = short_form ? 0 : 2, cb = short_form ? 2 : 3;
    size_t n = 0;
    const char *url = lua_type(L, 1) == LUA_TSTRING ? lua_tolstring(L, 1, &n) : NULL;
    luaL_argcheck(L, url && n > 6 && n <= WS_URL_MAX && !strncmp(url, "wss://", 6) && url_text(url, n), 1,
                  "a wss:// URL without spaces, at most 512 characters");
    char protocols[WS_PROTOCOLS * (WS_PROTOCOL_MAX + 2) + 1];
    if (opts) {
        protocols_of(L, opts, protocols, sizeof(protocols));
    } else {
        protocols[0] = 0;
    }
    luaL_checktype(L, cb, LUA_TFUNCTION);

    ws_handle_t *h = lua_newuserdatauv(L, sizeof(*h), 0);
    h->id = ++s->ws.seq;
    luaL_setmetatable(L, WS_META);
    if (s->ui_off) {
        return 1;                           // closing: no callback can run any more
    }
    lua_pushvalue(L, cb);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    const char *fail = !(s->pkg.perms & LUA_PERM_HTTP) ? "denied" : lh_ws_busy(s) ? "busy"
                     : !os_api_online() ? "offline" : NULL;
    if (fail) {
        close_later(L, s, ref, fail);
        return 1;
    }

    // No Lua errors from here on: C memory is in play.
    char path[80];
    snprintf(path, sizeof(path), "/api/device/apps/%s/ws", s->pkg.id);
    cJSON *j = cJSON_CreateObject();
    char *body = j && cJSON_AddStringToObject(j, "url", url) ? cJSON_PrintUnformatted(j) : NULL;
    cJSON_Delete(j);
    s->ws.protocols = protocols[0] ? strdup(protocols) : NULL;
    void *call = os_api_token();
    xSemaphoreTake(lh_mx, portMAX_DELAY);
    s->ws.id = h->id;
    s->ws.cb_ref = ref;
    s->ws.state = LH_WS_TICKET;
    s->ws.call = call;
    s->ws.answered = false;
    xSemaphoreGive(lh_mx);
    esp_err_t err = body && (!protocols[0] || s->ws.protocols)
                  ? os_api_call("POST", path, body, 0, ticket_cb, call) : ESP_ERR_NO_MEM;
    cJSON_free(body);
    if (err != ESP_OK) {
        close_later(L, s, detach(s), err == ESP_ERR_INVALID_STATE ? "offline" : "busy");
    }
    return 1;
}

// The socket behind argument 1, or NULL when it isn't the live one.
static lh_t *live(lua_State *L)
{
    ws_handle_t *h = luaL_checkudata(L, 1, WS_META);
    lh_t *s = lh_get(L);
    return h->id && h->id == s->ws.id && s->ws.state != LH_WS_NONE ? s : NULL;
}

// sock:send(text or table) -> true | nil, why
static int m_send(lua_State *L)
{
    lh_t *s = live(L);
    luaL_argcheck(L, lua_type(L, 2) == LUA_TSTRING || lua_istable(L, 2), 2, "a string or a table");
    const char *why = NULL;
    if (!s || s->ws.state != LH_WS_OPEN) {
        why = "not open";
    } else if (lua_istable(L, 2)) {
        lh_jbuf_t b = { .max = OS_SOCKET_MSG_MAX };
        if (!lh_json_encode(L, 2, &b)) {
            why = b.err;                    // "too big", or what can't be JSON
        } else {
            esp_err_t err = os_socket_send(s->ws.sock, b.p, b.len);
            why = err == ESP_OK ? NULL : err == ESP_ERR_NO_MEM ? "too fast" : "not open";
        }
        free(b.p);
    } else {
        size_t n;
        const char *text = lua_tolstring(L, 2, &n);
        esp_err_t err = os_socket_send(s->ws.sock, text, n);
        why = err == ESP_OK ? NULL : err == ESP_ERR_INVALID_SIZE ? "too big" : err == ESP_ERR_NO_MEM ? "too fast"
            : "not open";
    }
    if (why) {
        lua_pushnil(L);
        lua_pushstring(L, why);
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

// sock:close(): cb("close", "closed") on the next round
static int m_close(lua_State *L)
{
    lh_t *s = live(L);
    if (s) {
        close_later(L, s, detach(s), "closed");
    }
    return 0;
}

static int m_state(lua_State *L)
{
    lh_t *s = live(L);
    lua_pushstring(L, !s ? "closed" : s->ws.state == LH_WS_OPEN ? "open" : "connecting");
    return 1;
}

void lh_open_ws(lua_State *L)
{
    static const luaL_Reg methods[] = {{"send", m_send}, {"close", m_close}, {"state", m_state}, {NULL, NULL}};
    static const luaL_Reg fns[] = {{"open", l_open}, {NULL, NULL}};
    luaL_newmetatable(L, WS_META);
    luaL_newlib(L, methods);
    lua_setfield(L, -2, "__index");
    lua_pushliteral(L, "socket");
    lua_setfield(L, -2, "__metatable");     // getmetatable() can't reach the methods
    lua_pop(L, 1);
    luaL_newlib(L, fns);
    lua_setglobal(L, "ws");
}
