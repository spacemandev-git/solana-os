// fs and require for Lua apps (docs/LUA_API.md "fs and require"): read-only
// access to the app's bundled files, apps/<id>/files/<path> on the card.
//
// What a script reads goes on its own Lua heap, read straight into the new
// string (lua_pushfilled, a vendor patch: a 16 KB read needs 16 KB, not a
// buffer and a copy), and costs instructions: LH_FS_OPEN_STEPS per file
// opened and one per LH_FS_BYTES_PER_STEP bytes read or compiled
// (lh_charge_steps), so a loop of reads can't hold the LVGL lock for long. require and fs.load
// compile straight from the card, LH_READ_CHUNK bytes at a time, so a
// module's text is never in RAM whole; only the compiled function is.
//
// The last file read stays open (lh_t.fs_file): paging through a file or
// iterating its lines seeks instead of reopening it. It's closed when the
// script reads another file and when the app stops (the card takes 5 open
// files in all).
//
// A collection may run the script's __gc finalizers, which may read other
// files through that cache. lua_pushfilled reads with no collection in
// between; compiling allocates all along, so it uses a file of its own; and
// nothing that can raise runs while a directory is open.

#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "core/os_data.h"
#include "lua_priv.h"

#define LOADED_KEY  "solana.loaded"     // registry: require's modules by name
#define LIST_MAX    64                  // fs.list entries

typedef char list_name_t[LUA_PKG_PATH_MAX + 2];     // a name and a '/' for folders

static char s_loading;                  // its address marks a module being loaded

static void full_path(const lh_t *s, const char *rel, char *out, size_t n)
{
    char r[24 + LUA_PKG_ID_MAX + LUA_PKG_PATH_MAX];
    snprintf(r, sizeof(r), "apps/%s/files%s%s", s->pkg.id, rel[0] ? "/" : "", rel);
    os_data_path(out, n, r);
}

// Argument idx as a bundled path into out. dir: none or "" means the top, and
// a trailing '/' is fine.
static void check_path(lua_State *L, int idx, char *out, bool dir)
{
    size_t len;
    const char *p = dir ? luaL_optlstring(L, idx, "", &len) : luaL_checklstring(L, idx, &len);
    if (dir && len && p[len - 1] == '/') {
        len--;
    }
    out[0] = 0;
    if (dir && !len) {
        return;
    }
    if (len <= LUA_PKG_PATH_MAX && strlen(p) >= len) {
        memcpy(out, p, len);
        out[len] = 0;
    }
    luaL_argcheck(L, lua_pkg_valid_path(out), idx, "paths are lowercase a-z 0-9 _ - . with / between folders");
}

static int not_found(lua_State *L)
{
    lua_pushnil(L);
    lua_pushliteral(L, "not found");
    return 2;
}

void lh_fs_close(lh_t *s)
{
    if (s->fs_file) {
        fclose(s->fs_file);
        s->fs_file = NULL;
    }
    s->fs_path[0] = 0;
}

// The cached file if it's rel, else rel opened (and cached). NULL: there's no
// such file.
static FILE *fs_open(lua_State *L, lh_t *s, const char *rel)
{
    if (s->fs_file && !strcmp(s->fs_path, rel)) {
        return s->fs_file;
    }
    lh_fs_close(s);
    lh_charge_steps(L, LH_FS_OPEN_STEPS);
    char path[160];
    struct stat st;
    full_path(s, rel, path, sizeof(path));
    FILE *f = stat(path, &st) == 0 && S_ISREG(st.st_mode) ? fopen(path, "rb") : NULL;
    if (f) {
        s->fs_file = f;
        s->fs_size = st.st_size;
        strlcpy(s->fs_path, rel, sizeof(s->fs_path));
    }
    return f;
}

typedef struct {
    FILE *f;
    long off;
    bool ok;
} fill_t;

static size_t fill_from(void *ud, char *buf, size_t len)
{
    fill_t *r = ud;
    size_t got = fseek(r->f, r->off, SEEK_SET) == 0 ? fread(buf, 1, len, r->f) : 0;
    r->ok = got == len;
    return got;
}

// len bytes of rel from off, pushed as a string.
static void push_range(lua_State *L, lh_t *s, const char *rel, long off, long len)
{
    lh_charge_steps(L, (unsigned)(len / LH_FS_BYTES_PER_STEP));
    fill_t r = { .f = fs_open(L, s, rel), .off = off };
    if (r.f) {
        lua_pushfilled(L, (size_t)len, fill_from, &r);
    }
    if (!r.ok) {
        lh_fs_close(s);
        luaL_error(L, "fs: couldn't read %s from the microSD card", rel);
    }
}

// ---------------------------------------------------------------------------
// fs

static int l_exists(lua_State *L)
{
    char rel[LUA_PKG_PATH_MAX + 1], path[160];
    lh_t *s = lh_get(L);
    struct stat st;
    check_path(L, 1, rel, true);
    lh_charge_steps(L, LH_FS_OPEN_STEPS);
    full_path(s, rel, path, sizeof(path));
    lua_pushboolean(L, stat(path, &st) == 0);
    return 1;
}

static int l_size(lua_State *L)
{
    char rel[LUA_PKG_PATH_MAX + 1], path[160];
    lh_t *s = lh_get(L);
    struct stat st;
    check_path(L, 1, rel, false);
    if (s->fs_file && !strcmp(s->fs_path, rel)) {
        lua_pushinteger(L, (lua_Integer)s->fs_size);
        return 1;
    }
    lh_charge_steps(L, LH_FS_OPEN_STEPS);
    full_path(s, rel, path, sizeof(path));
    if (stat(path, &st) == 0 && S_ISREG(st.st_mode)) {
        lua_pushinteger(L, (lua_Integer)st.st_size);
    } else {
        lua_pushnil(L);
    }
    return 1;
}

static int by_name(const void *a, const void *b)
{
    return strcmp(a, b);
}

static int l_list(lua_State *L)
{
    char rel[LUA_PKG_PATH_MAX + 1], path[160];
    lh_t *s = lh_get(L);
    check_path(L, 1, rel, true);
    lh_charge_steps(L, LH_FS_OPEN_STEPS);
    full_path(s, rel, path, sizeof(path));
    // The names go into a buffer first: nothing may raise while the folder is open.
    list_name_t *names = lua_newuserdatauv(L, LIST_MAX * sizeof(list_name_t), 0);
    int n = 0;
    DIR *d = opendir(path);
    struct dirent *e;
    while (d && n < LIST_MAX && (e = readdir(d))) {
        size_t len = strlen(e->d_name);
        if (e->d_name[0] == '.' || len > LUA_PKG_PATH_MAX) {
            continue;                       // ".", ".." (nothing bundled starts with a dot)
        }
        memcpy(names[n], e->d_name, len);
        names[n][len] = e->d_type == DT_DIR ? '/' : 0;
        names[n][len + 1] = 0;
        n++;
    }
    if (d) {
        closedir(d);
    }
    qsort(names, n, sizeof(names[0]), by_name);
    lua_createtable(L, n, 0);
    for (int i = 0; i < n; i++) {
        lua_pushstring(L, names[i]);
        lua_rawseti(L, -2, i + 1);
    }
    return 1;
}

static int l_read(lua_State *L)
{
    char rel[LUA_PKG_PATH_MAX + 1];
    lh_t *s = lh_get(L);
    check_path(L, 1, rel, false);
    lua_Integer off = luaL_optinteger(L, 2, 0);
    luaL_argcheck(L, off >= 0, 2, "offsets count from 0");
    bool whole = lua_isnoneornil(L, 3);
    lua_Integer len = whole ? 0 : luaL_checkinteger(L, 3);
    luaL_argcheck(L, len >= 0 && len <= LH_FS_READ_MAX, 3, "at most 16 KB per read");
    if (!fs_open(L, s, rel)) {
        return not_found(L);
    }
    long left = off < s->fs_size ? s->fs_size - (long)off : 0;
    if (whole && left > LH_FS_READ_MAX) {
        return luaL_error(L, "fs.read: %s is %d bytes; read it in pieces of at most 16 KB: "
                          "fs.read(path, offset, length)", rel, (int)s->fs_size);
    }
    push_range(L, s, rel, (long)off, whole || len > left ? left : (long)len);
    return 1;
}

static int lines_next(lua_State *L)
{
    lh_t *s = lh_get(L);
    const char *rel = lua_tostring(L, lua_upvalueindex(1));
    lua_Integer off = lua_tointeger(L, lua_upvalueindex(2));
    if (!fs_open(L, s, rel)) {
        return luaL_error(L, "fs.lines: %s is gone", rel);
    }
    if (off >= s->fs_size) {
        return 0;
    }
    // Two more bytes than a line may have, to see the "\r\n" after a full one.
    char buf[LH_FS_LINE_MAX + 2];
    long want = s->fs_size - (long)off < (long)sizeof(buf) ? s->fs_size - (long)off : (long)sizeof(buf);
    size_t got = fseek(s->fs_file, (long)off, SEEK_SET) == 0 ? fread(buf, 1, want, s->fs_file) : 0;
    if (!got) {
        lh_fs_close(s);
        return luaL_error(L, "fs: couldn't read %s from the microSD card", rel);
    }
    const char *nl = memchr(buf, '\n', got);
    size_t len = nl ? (size_t)(nl - buf) : got, used;
    if (nl && len && buf[len - 1] == '\r') {
        len--;
    }
    if (nl && len <= LH_FS_LINE_MAX) {
        used = (size_t)(nl - buf) + 1;      // the whole line and its end
    } else {
        len = len < LH_FS_LINE_MAX ? len : LH_FS_LINE_MAX;
        used = len;                         // a piece of a longer line, or the last line
    }
    lh_charge_steps(L, (unsigned)(used / LH_FS_BYTES_PER_STEP));
    lua_pushinteger(L, off + (lua_Integer)used);
    lua_replace(L, lua_upvalueindex(2));
    lua_pushlstring(L, buf, len);
    return 1;
}

static int l_lines(lua_State *L)
{
    char rel[LUA_PKG_PATH_MAX + 1];
    check_path(L, 1, rel, false);
    if (!fs_open(L, lh_get(L), rel)) {
        return luaL_error(L, "fs.lines: there's no file %s", rel);
    }
    lua_pushstring(L, rel);
    lua_pushinteger(L, 0);
    lua_pushcclosure(L, lines_next, 2);
    return 1;
}

static int l_json(lua_State *L)
{
    char rel[LUA_PKG_PATH_MAX + 1];
    lh_t *s = lh_get(L);
    check_path(L, 1, rel, false);
    if (!fs_open(L, s, rel)) {
        return not_found(L);
    }
    if (s->fs_size > LH_FS_READ_MAX) {
        return luaL_error(L, "fs.json: %s is over 16 KB", rel);
    }
    push_range(L, s, rel, 0, s->fs_size);
    int top = lua_gettop(L);
    size_t len;
    const char *text = lua_tolstring(L, top, &len);
    const char *err = lh_json_decode(L, text, len);
    if (err) {
        lua_settop(L, top);
        lua_pushnil(L);
        lua_pushfstring(L, "%s: %s", rel, err);
        return 2;
    }
    return 1;
}

// ---------------------------------------------------------------------------
// fs.load, require

typedef struct {
    FILE *f;
    long left;
    char buf[LH_READ_CHUNK];
} reader_t;

static const char *read_piece(lua_State *L, void *ud, size_t *size)
{
    reader_t *r = ud;
    size_t want = r->left < (long)sizeof(r->buf) ? (size_t)r->left : sizeof(r->buf);
    *size = want ? fread(r->buf, 1, want, r->f) : 0;
    r->left -= *size;
    return *size ? r->buf : NULL;
}

enum { COMPILED, MISSING, BROKEN };

// Compiles bundled file rel: the function pushed, or (MISSING, BROKEN) its
// error message. The parser allocates as it reads, so it gets its own file,
// out of the finalizers' reach.
static int compile(lua_State *L, lh_t *s, const char *rel)
{
    char path[160], name[LUA_PKG_PATH_MAX + 2];
    struct stat st;
    full_path(s, rel, path, sizeof(path));
    lh_charge_steps(L, LH_FS_OPEN_STEPS);
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
        lua_pushfstring(L, "%s: not found", rel);
        return MISSING;
    }
    lh_charge_steps(L, (unsigned)(st.st_size / LH_FS_BYTES_PER_STEP));
    reader_t r = { .f = fopen(path, "rb"), .left = st.st_size };
    if (!r.f) {
        lua_pushfstring(L, "%s: couldn't read it", rel);
        return BROKEN;
    }
    snprintf(name, sizeof(name), "=%s", rel);
    // Text only: a binary chunk could crash the VM. lua_load never raises.
    int rc = lua_load(L, read_piece, &r, name, "t");
    fclose(r.f);
    return rc == LUA_OK ? COMPILED : BROKEN;
}

static int l_load(lua_State *L)
{
    char rel[LUA_PKG_PATH_MAX + 1];
    check_path(L, 1, rel, false);
    int rc = compile(L, lh_get(L), rel);
    if (rc == COMPILED) {
        return 1;
    }
    if (rc == MISSING) {
        lua_pop(L, 1);
        return not_found(L);
    }
    lua_pushnil(L);
    lua_insert(L, -2);
    return 2;
}

static int l_require(lua_State *L)
{
    size_t len;
    const char *name = luaL_checklstring(L, 1, &len);
    char rel[LUA_PKG_PATH_MAX + 1] = "";
    // "lib.quiz" -> "lib/quiz.lua"
    bool ok = len && len + 4 <= LUA_PKG_PATH_MAX && strlen(name) == len;
    for (size_t i = 0; ok && i < len; i++) {
        rel[i] = name[i] == '.' ? '/' : name[i];
        ok = name[i] != '/';
    }
    if (ok) {
        memcpy(rel + len, ".lua", 5);
    }
    luaL_argcheck(L, ok && lua_pkg_valid_path(rel), 1,
                  "module names are lowercase a-z 0-9 _ - with dots between folders, like \"lib.quiz\"");
    lua_settop(L, 1);
    lua_getfield(L, LUA_REGISTRYINDEX, LOADED_KEY);     // 2: the modules so far
    lua_getfield(L, 2, name);
    if (lua_touserdata(L, 3) == &s_loading) {
        return luaL_error(L, "require: module \"%s\" requires itself", name);
    }
    if (!lua_isnil(L, 3)) {
        return 1;
    }
    lua_pop(L, 1);
    int rc = compile(L, lh_get(L), rel);
    if (rc == MISSING) {
        return luaL_error(L, "module \"%s\" not found: the app has no %s", name, rel);
    }
    if (rc == BROKEN) {
        return lua_error(L);                // "lib/quiz.lua:3: ..." as the parser said it
    }
    lua_pushlightuserdata(L, &s_loading);
    lua_setfield(L, 2, name);
    lua_pushvalue(L, 1);                    // like Lua's require: the name and where it was found
    lua_pushstring(L, rel);
    if (lua_pcall(L, 2, 1, 0) != LUA_OK) {
        lua_pushnil(L);
        lua_setfield(L, 2, name);           // a later require tries again
        return lua_error(L);
    }
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        lua_pushboolean(L, 1);
    }
    lua_pushvalue(L, -1);
    lua_setfield(L, 2, name);
    return 1;
}

void lh_open_fs(lua_State *L)
{
    static const luaL_Reg fs[] = {
        {"exists", l_exists}, {"size", l_size}, {"list", l_list}, {"read", l_read},
        {"lines", l_lines}, {"json", l_json}, {"load", l_load}, {NULL, NULL},
    };
    luaL_newlib(L, fs);
    lua_setglobal(L, "fs");
    lua_newtable(L);
    lua_setfield(L, LUA_REGISTRYINDEX, LOADED_KEY);
    lua_register(L, "require", l_require);
}
