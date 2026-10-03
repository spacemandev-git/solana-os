#pragma once

// Store installs and updates of Lua apps: main.lua, app.json and the bundled
// files, swapped in together once all of them are on the card. See
// lua_install.c.

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "lua_pkg.h"

typedef enum {
    LUA_INSTALL_IDLE,
    LUA_INSTALL_RUNNING,
    LUA_INSTALL_DONE,               // the last install worked
    LUA_INSTALL_FAILED,
} lua_install_state_t;

typedef struct {
    lua_install_state_t state;
    char id[LUA_PKG_ID_MAX + 1];
    char version[17];               // DONE: the version on the card now
    int files_done, files_total;    // bundled files handled so far, in the version
    uint32_t seq;                   // bumped whenever an install ends
    char error[48];                 // FAILED: why, as toasted
} lua_install_info_t;

// Installs app <id> from the store, or updates it (any task). name is for the
// toasts (NULL: the id). ESP_ERR_INVALID_STATE while another install runs, ESP_ERR_NOT_FOUND
// without a card, ESP_ERR_NO_MEM when the worker can't start. The outcome is
// toasted; lua_install_get() follows it.
esp_err_t lua_install_start(const char *id, const char *name);
void lua_install_get(lua_install_info_t *out);

// Is <id> being installed (any app, with NULL)? Meanwhile the host doesn't
// start it and lua_pkg_remove() leaves it alone. Any task.
bool lua_install_busy(const char *id);

// Puts back an app whose update was cut off between the two renames of the
// swap (apps/<id>.old without apps/<id>). Blocking (a directory walk): the
// Apps scans call it from their workers.
void lua_install_recover(void);

void lua_install_init(void);                // lua_host_init()
