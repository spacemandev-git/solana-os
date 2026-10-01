#pragma once

// Installed Lua apps on the microSD card (docs/LUA_API.md "The package"):
// /sdcard/hh/apps/<id>/{main.lua, app.json, data.json}. Shared by the host,
// the Apps library, the App Store and the console.
//
// Reading one manifest is a small file read (fine from app callbacks);
// scanning or removing directories blocks longer and belongs in a worker task.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LUA_PKG_ID_MAX      24
#define LUA_PKG_SRC_MAX     (64 * 1024)     // main.lua
#define LUA_PKG_JSON_MAX    2048            // app.json

typedef enum {
    LUA_PERM_LED = 1 << 0,
    LUA_PERM_WIFI = 1 << 1,
    LUA_PERM_MESH = 1 << 2,
    LUA_PERM_LAMPS = 1 << 3,
    LUA_PERM_ITEMS = 1 << 4,
    LUA_PERM_PROFILE = 1 << 5,
    LUA_PERM_QUEST = 1 << 6,
} lua_perm_t;

#define LUA_PERM_COUNT  7

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
    bool temp;
    int64_t expires;        // unix ms, 0 = never
} lua_pkg_t;

// ^[a-z0-9-]{3,24}$ (the store's rule); also keeps ids safe in paths.
bool lua_pkg_valid_id(const char *id);

// Parse a manifest. false if it isn't valid JSON or lacks a valid id. With
// want_id, the manifest's id must match it.
bool lua_pkg_parse(const char *json, const char *want_id, lua_pkg_t *out);

// Read apps/<id>/app.json. false if the card, the app or its main.lua is missing.
bool lua_pkg_load(const char *id, lua_pkg_t *out);

// Installed apps sorted by name; returns the count (≤ max), -1 without a card.
int lua_pkg_scan(lua_pkg_t *out, int max);

// Delete apps/<id>/ (blocking: directory walk).
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

// Permission names: "led" ... "quest", and a plain-words description.
const char *lua_perm_name(int bit_index);
const char *lua_perm_text(int bit_index);
uint32_t lua_perm_parse(const char *name);     // 0 if unknown

const char *lua_pkg_symbol(const char *icon);  // LV_SYMBOL_* (never NULL)
