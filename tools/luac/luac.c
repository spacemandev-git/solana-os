// The store's Lua compiler: this firmware's Lua (main/lua/vendor: 5.5,
// LUA_32BITS, the same parser limits) built to WebAssembly by build.sh, so a
// server turns an app's source into exactly the chunks a badge with the same
// label (LUA_SOLANA_LUAC) runs in place (main/lua/lua_code.c). Chunks are
// dumped with strip 2 (vendor/ldump.c): no local variable names, but line
// info, source names and upvalue names, so errors still say "main.lua:12:".
//
// The API, for the module's caller (the HH GtS server's luac.ts):
//   luac_label()                    the label of the chunks it makes
//   luac_compile(src, len, name)    chunk size, or -1 and luac_error()
//   luac_out()                      the last chunk's bytes

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lua.h"
#include "lauxlib.h"

// The badge host's hooks (vendor/luaconf.h): no task stack or instruction
// budget to guard while compiling.
int lh_cstack_check(void) { return 0; }
void lh_charge_steps(lua_State *L, unsigned int n) { (void)L; (void)n; }
void lh_thread_new(lua_State *L1) { (void)L1; }

static unsigned char *s_out;
static size_t s_len, s_cap;
static char s_err[512];

static int writer(lua_State *L, const void *p, size_t sz, void *ud)
{
    (void)L;
    (void)ud;
    if (!p || !sz) {
        return 0;                           // 5.5's last call: the end
    }
    if (s_len + sz > s_cap) {
        size_t cap = (s_len + sz) * 2;
        unsigned char *n = realloc(s_out, cap);
        if (!n) {
            return 1;
        }
        s_out = n;
        s_cap = cap;
    }
    memcpy(s_out + s_len, p, sz);
    s_len += sz;
    return 0;
}

const char *luac_label(void)
{
    return LUA_SOLANA_LUAC;
}

// src (len bytes) compiled as chunk "=<name>", as the badge names it.
int luac_compile(const char *src, int len, const char *name)
{
    char chunk[96];
    snprintf(chunk, sizeof(chunk), "=%s", name);
    s_len = 0;
    s_err[0] = 0;
    lua_State *L = luaL_newstate();
    if (!L) {
        strcpy(s_err, "not enough memory");
        return -1;
    }
    int rc = luaL_loadbufferx(L, src, (size_t)len, chunk, "t");
    if (rc != LUA_OK) {
        snprintf(s_err, sizeof(s_err), "%s", lua_tostring(L, -1));
    } else if ((rc = lua_dump(L, writer, NULL, 2)) != 0) {
        strcpy(s_err, "not enough memory");
    }
    lua_close(L);
    return rc == LUA_OK ? (int)s_len : -1;
}

const unsigned char *luac_out(void)
{
    return s_out;
}

const char *luac_error(void)
{
    return s_err;
}
