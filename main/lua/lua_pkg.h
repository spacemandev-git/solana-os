#pragma once

// Installed Lua apps on the microSD card (docs/LUA_API.md "The package"):
// /sdcard/hh/apps/<id>/{main.lua, app.json, data.json}, the bundled files
// under files/ and, for store installs, code.bin: the store's compiled code
// (server/API.md "Compiled code"), which the host runs in place from flash. Shared by the host, the installer, the Apps library, the App
// Store and the console.
//
// Reading one manifest is a small file read (fine from app callbacks);
// scanning or removing directories blocks longer and belongs in a worker task.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LUA_PKG_ID_MAX      24
#define LUA_PKG_SRC_MAX     (64 * 1024)     // main.lua
#define LUA_PKG_JSON_MAX    2048            // app.json
// Bundled files: the store's limits (server/API.md), checked again here.
#define LUA_PKG_FILES_MAX   64
#define LUA_PKG_FILE_MAX    (256 * 1024)
#define LUA_PKG_FILES_TOTAL (1024 * 1024)
#define LUA_PKG_PATH_MAX    64              // "lib/quiz.lua"
#define LUA_PKG_PATH_DEPTH  4
#define LUA_PKG_CODE_MAX    (1536 * 1024)   // code.bin

typedef enum {
    LUA_PERM_LED = 1 << 0,
    LUA_PERM_WIFI = 1 << 1,
    LUA_PERM_MESH = 1 << 2,
    LUA_PERM_LAMPS = 1 << 3,
    LUA_PERM_ITEMS = 1 << 4,
    LUA_PERM_PROFILE = 1 << 5,
    LUA_PERM_QUEST = 1 << 6,
    LUA_PERM_HTTP = 1 << 7,
    LUA_PERM_RPC = 1 << 8,
} lua_perm_t;

#define LUA_PERM_COUNT  9

typedef struct {
    char id[LUA_PKG_ID_MAX + 1];
    char name[21];
    char version[17];
    char author[25];
    char summary[81];
    char category[8];
    const char *symbol;     // LV_SYMBOL_* for the manifest's icon
    uint32_t accent;        // 0xRRGGBB
    uint32_t perms;         // lua_perm_t bits
    int files;              // bundled files, as the manifest says
    int32_t files_size;     // their bytes
    bool temp;
    int64_t expires;        // unix ms, 0 = never
    // The manifest's "code", when its compiler label is this badge's own
    // (LUA_SOLANA_LUAC): code.bin's size and SHA-256. Otherwise false, and
    // the host compiles main.lua.
    bool code;
    int32_t code_size;
    uint8_t code_sha[32];
} lua_pkg_t;

// ^[a-z0-9-]{3,24}$ (the store's rule); also keeps ids safe in paths.
bool lua_pkg_valid_id(const char *id);

// A bundled file's path (LUA_API.md "The package"): lowercase segments of
// [a-z0-9_-] with single dots inside, '/' between them, at most
// LUA_PKG_PATH_MAX characters and LUA_PKG_PATH_DEPTH levels. Nothing else can
// leave the app's files/ folder.
bool lua_pkg_valid_path(const char *path);

// A file of app <id> as the console and the installer name it: "main.lua"
// and "app.json" are the app's own, any other valid path is a bundled file
// (apps/<id>/files/<path>). Fills `rel` (relative to OS_DATA_DIR) and the
// size limit; false for an invalid name.
bool lua_pkg_file_rel(char *rel, size_t n, const char *id, const char *file, size_t *max);

// 64 hex digits into 32 bytes. false unless that's exactly what s is.
bool lua_pkg_unhex(const char *s, uint8_t out[32]);

// SHA-256 of a file (a full path), and its size. false if it can't be read.
bool lua_pkg_sha256_file(const char *path, uint8_t out[32], long *size);
void lua_pkg_hex(char *out, const uint8_t *bin, size_t n);     // out: 2n + 1 bytes

// Parse a manifest. false if it isn't valid JSON or lacks a valid id. With
// want_id, the manifest's id must match it.
bool lua_pkg_parse(const char *json, const char *want_id, lua_pkg_t *out);

// Read apps/<id>/app.json. false if the card, the app or its main.lua is missing.
bool lua_pkg_load(const char *id, lua_pkg_t *out);

// Installed apps sorted by name; returns the count (≤ max), -1 without a card.
int lua_pkg_scan(lua_pkg_t *out, int max);

// Delete apps/<id>/ (blocking: directory walk). false while the store
// installs that app.
bool lua_pkg_remove(const char *id);

// Temporary app past its expiry by the server clock. Unknown time = not expired.
bool lua_pkg_expired(const lua_pkg_t *p);

// Applies a GET /api/device/library answer to the installed temporary apps:
// the server extends a quest app's grant while its quest is active, so the
// expiry in app.json moves forward. Returns how many manifests changed.
// Small SD writes: fine from app callbacks.
int lua_pkg_apply_library(const char *body);

// "apps/<id>/<file>" relative to OS_DATA_DIR.
void lua_pkg_rel(char *out, size_t n, const char *id, const char *file);

// What `http` may call, for the info screens: the manifest's "hosts" (a
// cJSON array) as one "a.example.com, *.b.example.com" line of printable
// ASCII, cut with "..." to fit. "" without any.
struct cJSON;
void lua_pkg_hosts_line(const struct cJSON *hosts, char *out, size_t n);
// The same from apps/<id>/app.json (a small file read). false without any.
bool lua_pkg_hosts(const char *id, char *out, size_t n);

// Permission names: "led" ... "rpc", and a plain-words description.
const char *lua_perm_name(int bit_index);
const char *lua_perm_text(int bit_index);
uint32_t lua_perm_parse(const char *name);     // 0 if unknown

const char *lua_pkg_symbol(const char *icon);  // LV_SYMBOL_* (never NULL)

// App-supplied text for an OS confirm dialog: printable ASCII only, cut to
// fit, so it can't add lines or restructure the question.
void lua_pkg_plain(char *out, size_t n, const char *in);
