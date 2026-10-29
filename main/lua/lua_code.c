// The store's compiled code for an app (server/API.md "Compiled code"),
// run in place: apps/<id>/code.bin, mapped by lua_xip for as long as the
// app runs. Every Lua file of the version (main.lua and the bundled .lua
// files) is a binary chunk in it, compiled by the store with this build's
// Lua (tools/luac) and loaded with lua_load's mode "B": instructions, line
// info and long strings stay in the mapping, so only the functions' headers,
// constants and short strings take RAM (about a third of what compiling the
// source on the badge keeps, and no parser peak at launch).
//
// Only the installer writes code.bin, after checking its SHA-256 against the
// manifest the store sent; console uploads delete it (lua_cmd.c). A binary
// chunk can crash the VM, so nothing else is ever loaded as one: without
// code.bin, or with a label that isn't this build's, main.lua is compiled.
//
// code.bin, little-endian:
//   0   "SOLC"
//   4   char[16]  compiler label, NUL-padded: LUA_SOLANA_LUAC
//   20  u32       chunk count
//   24  count x { char path[64] (NUL-padded), u32 offset, u32 size }
//       chunks, each at a 16-aligned offset

#include <string.h>
#include "core/os_data.h"
#include "lua_priv.h"

#define HDR         24
#define ENTRY       72
#define PATH_LEN    64
#define CHUNKS_MAX  (1 + LUA_PKG_FILES_MAX)

static uint32_t u32_at(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static bool valid(const uint8_t *d, size_t n)
{
    if (n < HDR || memcmp(d, "SOLC", 4) || strncmp((const char *)d + 4, LUA_SOLANA_LUAC, 16)) {
        return false;
    }
    uint32_t count = u32_at(d + 20);
    if (count == 0 || count > CHUNKS_MAX || HDR + (size_t)count * ENTRY > n
            || strncmp((const char *)d + HDR, "main.lua", PATH_LEN)) {     // main.lua comes first
        return false;
    }
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *e = d + HDR + i * ENTRY;
        uint32_t off = u32_at(e + PATH_LEN), size = u32_at(e + PATH_LEN + 4);
        if (off % 16 || off < HDR + count * ENTRY || size == 0 || off > n || size > n - off) {
            return false;
        }
    }
    return true;
}

bool lh_code_open(lh_t *s)
{
    if (!s->pkg.code) {
        return false;
    }
    char rel[64], path[160];
    lua_pkg_rel(rel, sizeof(rel), s->pkg.id, "code.bin");
    os_data_path(path, sizeof(path), rel);
    if (!lua_xip_map(path, s->pkg.code_sha, (size_t)s->pkg.code_size, &s->code)) {
        printf("[lua:%s] no compiled code (%s): compiling main.lua\n", s->pkg.id, rel);
        return false;
    }
    if (!valid(s->code.data, s->code.size)) {
        printf("[lua:%s] code.bin isn't for this badge: compiling main.lua\n", s->pkg.id);
        lua_xip_unmap(&s->code);
        return false;
    }
    return true;
}

void lh_code_close(lh_t *s)
{
    lua_xip_unmap(&s->code);
}

typedef struct {
    const char *p;
    size_t n;
} whole_t;

// The whole chunk in one piece: lua_load's fixed mode needs it contiguous.
static const char *read_whole(lua_State *L, void *ud, size_t *size)
{
    whole_t *w = ud;
    *size = w->n;
    w->n = 0;
    return *size ? w->p : NULL;
}

int lh_code_load(lua_State *L, lh_t *s, const char *rel)
{
    const uint8_t *d = s->code.data;
    size_t len = strlen(rel);
    if (!d || len > PATH_LEN) {
        return LH_CODE_NONE;
    }
    uint32_t count = u32_at(d + 20);
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *e = d + HDR + i * ENTRY;
        if (!strncmp((const char *)e, rel, PATH_LEN) && (len == PATH_LEN || !e[len])) {
            char name[PATH_LEN + 2];
            snprintf(name, sizeof(name), "=%s", rel);
            whole_t w = { (const char *)d + u32_at(e + PATH_LEN), u32_at(e + PATH_LEN + 4) };
            return lua_load(L, read_whole, &w, name, "B");
        }
    }
    return LH_CODE_NONE;
}
