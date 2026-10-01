#pragma once

// Lua app host: runs one installed store app (docs/LUA_API.md) in a sandbox.
//
// The script gets a fresh lua_State per launch with a capped allocator, an
// instruction budget per callback and only the globals LUA_API.md lists. It
// runs on its own "lua" task, not the OS task: Lua and the LVGL calls under
// it need more stack than the OS task has, and a stuck script must not stall
// the OS. The task takes the LVGL lock around every callback, so callbacks
// never overlap with each other or with the OS.

#include <stdbool.h>
#include <stddef.h>
#include "core/os.h"

#define LUA_HOST_HEAP_MAX   (48 * 1024)     // Lua heap per app (LUA_API.md "Limits")
#define LUA_HOST_STEPS_MAX  200000          // VM instructions per callback

// Hidden system app; its title is the running script's name.
extern const os_app_t lua_host_app;

// Start an installed app (any task). When it can't start (no card, not
// installed, expired, the previous script still shutting down) it toasts
// why and returns false.
bool lua_host_launch(const char *id);

// Id of the running app, or NULL.
const char *lua_host_running(void);

// Registers the "lua" console command. Call before console_start().
void lua_host_init(void);
