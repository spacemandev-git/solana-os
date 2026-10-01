#pragma once

// HH GtS background services: X Dex swaps (xdex.c), trades (trade.c) and
// quests (quests.c), plus what they share (docs/HHGTS.md, docs/MESH.md).
//
// Everything game-side runs on the OS task with the LVGL lock held: mesh
// handlers copy each message and hop over with os_post(), os_api callbacks
// already arrive there, and a 250 ms esp_timer posts the services' tick. So
// the services need no locks, and apps may read their state directly from
// their callbacks.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "cJSON.h"
#include "core/os_mesh.h"

#define GAME_TICK_MS        250
#define GAME_CARD_NAME      25          // MESH.md: owner label, ≤ 24 printable ASCII
#define GAME_CARD_X         17          // X handle

// Start the services, subscribe to the mesh and register console commands.
// After os_mesh_init(), before console_start().
void game_init(void);

int64_t game_now_ms(void);              // esp_timer ms

// This badge's id ("" until the first hello) and whether it can take part in
// signed mesh exchanges (claimed, has an id and a mesh key, radio ready).
const char *game_badge_id(void);
bool game_claimed(void);
bool game_can_sign(void);

// Card {"n","x","lv"} of this badge's owner (MESH.md §2), added to obj.
void game_add_card(cJSON *obj);

// cJSON accessors that never return NULL strings.
const char *game_jstr(const cJSON *obj, const char *key);
int game_jint(const cJSON *obj, const char *key, int def);
// Copy printable ASCII only (the fonts have no other glyphs), truncating.
void game_ascii(char *out, size_t n, const char *in);

// One line of list sub text (ui_row_t.sub wraps instead of cutting long
// text): at most GAME_SUB_CHARS characters, "..." when cut.
#define GAME_SUB_CHARS      42
void game_clip(char *out, size_t n, const char *in);

// Peer label for UI text: the owner label, else "badge <ID>".
void game_peer_label(const char *name, const char *id, char *out, size_t n);
// Signal bars from RSSI, e.g. "|||." (sol_mono_12 has no bar glyphs).
const char *game_signal(int rssi);

// Last good server answers in /sdcard/hh/cache/<name>.json, with the server
// time they were fetched at. get returns the parsed body (cJSON_Delete it) or
// NULL; *at_ms = unix ms, 0 if unknown.
bool game_cache_put(const char *name, const char *body);
cJSON *game_cache_get(const char *name, int64_t *at_ms);
// "synced 5 min ago" (or "synced just now", "saved copy").
void game_synced(int64_t at_ms, char *out, size_t n);

// LED effects on the whole strip, capped at OS_LED_MAX. flash: a short pulse
// of one colour (0xRRGGBB); sweep: the legendary purple -> green sweep.
void game_led_flash(uint32_t rgb);
void game_led_sweep(void);
uint32_t game_parse_hex(const char *hex, uint32_t def);

// Each service's half of game.c: init, tick and mesh dispatch.
void xdex_init(void);
void xdex_tick(int64_t now);
void xdex_on_mesh(const os_mesh_peer_t *from, cJSON *msg);
void trade_init(void);
void trade_tick(int64_t now);
void trade_on_mesh(const os_mesh_peer_t *from, cJSON *msg);
void quests_init(void);
void quests_tick(int64_t now);
void quests_on_mesh(const os_mesh_peer_t *from, cJSON *msg);
