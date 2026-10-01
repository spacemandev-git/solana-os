// JSON <-> Lua values for json.*, storage.*, mesh and server answers.
//
// Not cJSON: decoding builds Lua tables directly, so the memory counts
// against the script's heap cap instead of a cJSON tree on the system heap.
// Encoding writes to a malloc'd buffer with a hard size limit and never
// raises a Lua error (callers may hold C memory), so it uses only raw,
// non-allocating Lua API calls.

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lua_priv.h"

// ---------------------------------------------------------------------------
// Encoder

static bool put(lh_jbuf_t *b, const char *s, size_t n)
{
    if (b->err) {
        return false;
    }
    if (b->len + n > b->max) {
        b->err = "too big";
        return false;
    }
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 128;
        while (cap < b->len + n + 1) {
            cap *= 2;
        }
        if (cap > b->max + 1) {
            cap = b->max + 1;
        }
        char *p = realloc(b->p, cap);
        if (!p) {
            b->err = "out of memory";
            return false;
        }
        b->p = p;
        b->cap = cap;
    }
    memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = 0;
    return true;
}

static bool puts_(lh_jbuf_t *b, const char *s)
{
    return put(b, s, strlen(s));
}

static void put_number(lua_State *L, int idx, lh_jbuf_t *b)
{
    char num[40];
    if (lua_isinteger(L, idx)) {
        snprintf(num, sizeof(num), LUA_INTEGER_FMT, (LUAI_UACINT)lua_tointeger(L, idx));
    } else {
        double v = (double)lua_tonumber(L, idx);
        if (!isfinite(v)) {
            b->err = "can't encode inf or nan";
            return;
        }
        // Whole numbers (e.g. timestamps that overflowed the 32-bit integers) print without ".0".
        snprintf(num, sizeof(num), v == floor(v) && fabs(v) < 9e15 ? "%.0f" : "%.7g", v);
    }
    puts_(b, num);
}

static void put_string(lh_jbuf_t *b, const char *s, size_t n)
{
    put(b, "\"", 1);
    size_t run = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = s[i];
        const char *esc = NULL;
        char u[8];
        switch (c) {
        case '"':  esc = "\\\""; break;
        case '\\': esc = "\\\\"; break;
        case '\n': esc = "\\n"; break;
        case '\r': esc = "\\r"; break;
        case '\t': esc = "\\t"; break;
        default:
            if (c < 0x20) {
                snprintf(u, sizeof(u), "\\u%04x", c);
                esc = u;
            }
        }
        if (esc) {
            put(b, s + i - run, run);
            puts_(b, esc);
            run = 0;
        } else {
            run++;
        }
    }
    put(b, s + n - run, run);
    put(b, "\"", 1);
}

static void put_key(lua_State *L, int idx, lh_jbuf_t *b)
{
    if (lua_type(L, idx) == LUA_TSTRING) {
        size_t n;
        const char *s = lua_tolstring(L, idx, &n);
        put_string(b, s, n);
    } else if (lua_type(L, idx) == LUA_TNUMBER) {
        put(b, "\"", 1);
        put_number(L, idx, b);
        put(b, "\"", 1);
    } else {
        b->err = "object keys must be strings or numbers";
    }
}

static void put_value(lua_State *L, int idx, lh_jbuf_t *b, int depth)
{
    switch (lua_type(L, idx)) {
    case LUA_TNIL:
        puts_(b, "null");
        break;
    case LUA_TBOOLEAN:
        puts_(b, lua_toboolean(L, idx) ? "true" : "false");
        break;
    case LUA_TNUMBER:
        put_number(L, idx, b);
        break;
    case LUA_TSTRING: {
        size_t n;
        const char *s = lua_tolstring(L, idx, &n);
        put_string(b, s, n);
        break;
    }
    case LUA_TTABLE: {
        if (depth >= LH_JSON_DEPTH) {
            b->err = "nested too deeply (or a cycle)";
            break;
        }
        if (!lua_checkstack(L, 3)) {
            b->err = "out of memory";
            break;
        }
        // An array is a table whose keys are exactly 1..#t. Empty tables are objects.
        lua_Unsigned n = lua_rawlen(L, idx), keys = 0;
        lua_pushnil(L);
        while (lua_next(L, idx)) {
            keys++;
            lua_pop(L, 1);
        }
        if (n > 0 && keys == n) {
            put(b, "[", 1);
            for (lua_Unsigned i = 1; i <= n && !b->err; i++) {
                if (i > 1) {
                    put(b, ",", 1);
                }
                lua_rawgeti(L, idx, (lua_Integer)i);
                put_value(L, lua_gettop(L), b, depth + 1);
                lua_pop(L, 1);
            }
            put(b, "]", 1);
            break;
        }
        put(b, "{", 1);
        bool first = true;
        lua_pushnil(L);
        while (!b->err && lua_next(L, idx)) {
            if (!first) {
                put(b, ",", 1);
            }
            first = false;
            int top = lua_gettop(L);
            put_key(L, top - 1, b);
            put(b, ":", 1);
            put_value(L, top, b, depth + 1);
            lua_pop(L, 1);
        }
        if (b->err) {
            lua_pop(L, 1);      // the key lua_next left behind
        }
        put(b, "}", 1);
        break;
    }
    default:
        b->err = "can only encode tables, strings, numbers and booleans";
    }
}

bool lh_json_encode(lua_State *L, int idx, lh_jbuf_t *b)
{
    int top = lua_gettop(L);
    put_value(L, lua_absindex(L, idx), b, 0);
    lua_settop(L, top);
    if (!b->err && !b->p) {
        b->err = "out of memory";
    }
    return !b->err;
}

// ---------------------------------------------------------------------------
// Decoder

typedef struct {
    lua_State *L;
    const char *p, *end;
    const char *err;
    int depth;
} dec_t;

static void ws(dec_t *d)
{
    while (d->p < d->end && (*d->p == ' ' || *d->p == '\t' || *d->p == '\n' || *d->p == '\r')) {
        d->p++;
    }
}

static bool fail(dec_t *d, const char *err)
{
    if (!d->err) {
        d->err = err;
    }
    return false;
}

static bool lit(dec_t *d, const char *word)
{
    size_t n = strlen(word);
    if ((size_t)(d->end - d->p) < n || memcmp(d->p, word, n)) {
        return fail(d, "invalid JSON");
    }
    d->p += n;
    return true;
}

static int hex4(const char *p)
{
    int v = 0;
    for (int i = 0; i < 4; i++) {
        char c = p[i];
        v <<= 4;
        if (c >= '0' && c <= '9') {
            v |= c - '0';
        } else if (c >= 'a' && c <= 'f') {
            v |= c - 'a' + 10;
        } else if (c >= 'A' && c <= 'F') {
            v |= c - 'A' + 10;
        } else {
            return -1;
        }
    }
    return v;
}

static void add_utf8(luaL_Buffer *buf, unsigned cp)
{
    char u[4];
    int n;
    if (cp < 0x80) {
        u[0] = cp;
        n = 1;
    } else if (cp < 0x800) {
        u[0] = 0xC0 | (cp >> 6);
        u[1] = 0x80 | (cp & 0x3F);
        n = 2;
    } else if (cp < 0x10000) {
        u[0] = 0xE0 | (cp >> 12);
        u[1] = 0x80 | ((cp >> 6) & 0x3F);
        u[2] = 0x80 | (cp & 0x3F);
        n = 3;
    } else {
        u[0] = 0xF0 | (cp >> 18);
        u[1] = 0x80 | ((cp >> 12) & 0x3F);
        u[2] = 0x80 | ((cp >> 6) & 0x3F);
        u[3] = 0x80 | (cp & 0x3F);
        n = 4;
    }
    luaL_addlstring(buf, u, n);
}

static bool dec_string(dec_t *d)
{
    luaL_Buffer buf;
    d->p++;     // opening quote
    luaL_buffinit(d->L, &buf);
    for (;;) {
        const char *run = d->p;
        while (d->p < d->end && *d->p != '"' && *d->p != '\\' && (unsigned char)*d->p >= 0x20) {
            d->p++;
        }
        luaL_addlstring(&buf, run, d->p - run);
        if (d->p >= d->end || (unsigned char)*d->p < 0x20) {
            luaL_pushresult(&buf);
            lua_pop(d->L, 1);
            return fail(d, "unterminated string");
        }
        if (*d->p == '"') {
            d->p++;
            luaL_pushresult(&buf);
            return true;
        }
        if (d->end - d->p < 2) {
            break;
        }
        char c = d->p[1];
        d->p += 2;
        switch (c) {
        case '"': case '\\': case '/':
            luaL_addchar(&buf, c);
            break;
        case 'b': luaL_addchar(&buf, '\b'); break;
        case 'f': luaL_addchar(&buf, '\f'); break;
        case 'n': luaL_addchar(&buf, '\n'); break;
        case 'r': luaL_addchar(&buf, '\r'); break;
        case 't': luaL_addchar(&buf, '\t'); break;
        case 'u': {
            int cp = d->end - d->p >= 4 ? hex4(d->p) : -1;
            if (cp < 0) {
                goto bad;
            }
            d->p += 4;
            // A high surrogate must pair with a low one.
            if (cp >= 0xD800 && cp < 0xDC00) {
                int lo = d->end - d->p >= 6 && d->p[0] == '\\' && d->p[1] == 'u' ? hex4(d->p + 2) : -1;
                if (lo < 0xDC00 || lo > 0xDFFF) {
                    goto bad;
                }
                d->p += 6;
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
            } else if (cp >= 0xDC00 && cp < 0xE000) {
                goto bad;
            }
            add_utf8(&buf, cp);
            break;
        }
        default:
            goto bad;
        }
    }
bad:
    luaL_pushresult(&buf);
    lua_pop(d->L, 1);
    return fail(d, "bad escape in string");
}

static bool dec_number(dec_t *d)
{
    const char *s = d->p;
    bool is_float = false;
    if (d->p < d->end && *d->p == '-') {
        d->p++;
    }
    if (d->p >= d->end || *d->p < '0' || *d->p > '9') {
        return fail(d, "invalid JSON");
    }
    while (d->p < d->end) {
        char c = *d->p;
        if (c == '.' || c == 'e' || c == 'E' || c == '+' || (c == '-' && d->p > s)) {
            is_float = true;
        } else if (c < '0' || c > '9') {
            break;
        }
        d->p++;
    }
    char num[40];
    size_t n = d->p - s;
    if (n >= sizeof(num)) {
        return fail(d, "number too long");
    }
    memcpy(num, s, n);
    num[n] = 0;
    char *endp;
    if (!is_float) {
        // 32-bit Lua integers: bigger whole numbers (ms timestamps) become floats.
        long long v = strtoll(num, &endp, 10);
        if (*endp == 0 && v >= LUA_MININTEGER && v <= LUA_MAXINTEGER) {
            lua_pushinteger(d->L, (lua_Integer)v);
            return true;
        }
    }
    double v = strtod(num, &endp);
    if (*endp) {
        return fail(d, "invalid number");
    }
    lua_pushnumber(d->L, (lua_Number)v);
    return true;
}

static bool dec_value(dec_t *d);

static bool dec_container(dec_t *d, bool object)
{
    lua_State *L = d->L;
    if (++d->depth > LH_JSON_DEPTH) {
        return fail(d, "nested too deeply");
    }
    if (!lua_checkstack(L, 4)) {
        return fail(d, "out of memory");
    }
    d->p++;
    lua_newtable(L);
    ws(d);
    if (d->p < d->end && *d->p == (object ? '}' : ']')) {
        d->p++;
        d->depth--;
        return true;
    }
    for (lua_Integer i = 1;; i++) {
        ws(d);
        if (object) {
            if (d->p >= d->end || *d->p != '"' || !dec_string(d)) {
                return fail(d, "expected a key");
            }
            ws(d);
            if (d->p >= d->end || *d->p != ':') {
                lua_pop(L, 1);
                return fail(d, "expected ':'");
            }
            d->p++;
        }
        if (!dec_value(d)) {
            if (object) {
                lua_pop(L, 1);
            }
            return false;
        }
        if (object) {
            lua_rawset(L, -3);
        } else {
            lua_rawseti(L, -2, i);
        }
        ws(d);
        if (d->p < d->end && *d->p == ',') {
            d->p++;
            continue;
        }
        if (d->p < d->end && *d->p == (object ? '}' : ']')) {
            d->p++;
            d->depth--;
            return true;
        }
        return fail(d, object ? "expected ',' or '}'" : "expected ',' or ']'");
    }
}

static bool dec_value(dec_t *d)
{
    ws(d);
    if (d->p >= d->end) {
        return fail(d, "unexpected end");
    }
    bool ok;
    int top = lua_gettop(d->L);
    switch (*d->p) {
    case '{': ok = dec_container(d, true); break;
    case '[': ok = dec_container(d, false); break;
    case '"': ok = dec_string(d); break;
    case 't': ok = lit(d, "true") && (lua_pushboolean(d->L, 1), true); break;
    case 'f': ok = lit(d, "false") && (lua_pushboolean(d->L, 0), true); break;
    case 'n': ok = lit(d, "null") && (lua_pushnil(d->L), true); break;
    default:  ok = dec_number(d); break;
    }
    if (!ok) {
        lua_settop(d->L, top);
    }
    return ok;
}

const char *lh_json_decode(lua_State *L, const char *s, size_t len)
{
    dec_t d = { .L = L, .p = s, .end = s + len };
    if (!dec_value(&d)) {
        return d.err ? d.err : "invalid JSON";
    }
    ws(&d);
    if (d.p != d.end) {
        lua_pop(L, 1);
        return "unexpected data after the value";
    }
    return NULL;
}

// ---------------------------------------------------------------------------
// json.encode / json.decode

static int push_buf(lua_State *L)
{
    lua_pushlstring(L, lua_touserdata(L, 1), (size_t)lua_tointeger(L, 2));
    return 1;
}

void lh_push_jbuf(lua_State *L, lh_jbuf_t *b)
{
    // Copy into Lua under pcall, so the C buffer is freed even when the copy
    // runs out of memory; then rethrow.
    lua_pushcfunction(L, push_buf);
    lua_pushlightuserdata(L, b->p);
    lua_pushinteger(L, (lua_Integer)b->len);
    int rc = lua_pcall(L, 2, 1, 0);
    free(b->p);
    b->p = NULL;
    if (rc != LUA_OK) {
        lua_error(L);
    }
}

static int l_encode(lua_State *L)
{
    luaL_checkany(L, 1);
    lh_jbuf_t b = { .max = LH_STORAGE_MAX };
    if (!lh_json_encode(L, 1, &b)) {
        free(b.p);
        lua_pushnil(L);
        lua_pushstring(L, b.err);
        return 2;
    }
    lh_push_jbuf(L, &b);
    return 1;
}

static int l_decode(lua_State *L)
{
    size_t n;
    const char *s = luaL_checklstring(L, 1, &n);
    const char *err = lh_json_decode(L, s, n);
    if (err) {
        lua_pushnil(L);
        lua_pushstring(L, err);
        return 2;
    }
    return 1;
}

void lh_open_json(lua_State *L)
{
    static const luaL_Reg fns[] = {
        {"encode", l_encode},
        {"decode", l_decode},
        {NULL, NULL},
    };
    luaL_newlib(L, fns);
    lua_setglobal(L, "json");
}
