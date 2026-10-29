#pragma once

// Badge-to-badge and badge-to-beacon messaging over ESP-NOW (docs/MESH.md).
//
// Portable: framing, fragmentation, presence (HELLO) and the peer table live in
// os_mesh.c; the raw radio is os_radio (esp_now on the badge, a
// BroadcastChannel between browser tabs in the emulator). A "mesh" task owns
// the radio. It broadcasts HELLO every ~2 s while the radio is on the mesh
// channel and reassembles incoming messages.
//
// Badges that share no radio talk over a bridge through the server instead
// (os_bridge.h, docs/MESH.md "Bridges"). A bridged badge is a peer like any
// other (bridged = true): os_mesh_send() to its id, or to everyone, goes over
// the bridge, and what it sends arrives through the same handlers.
//
// Handlers run on the mesh task, NOT the OS task: copy what you need into your
// own state (under your own lock) and render it from on_tick, or hop over with
// os_post(). Keep them short: the mesh task has ~2 KB of stack to spare, so
// no SD I/O or HTTP there, and never take the LVGL lock (os_toast() and
// os_post() are fine). After os_mesh_unsubscribe() returns, the handler is
// not running and won't be called again.

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define OS_MESH_ID_LEN      6
#define OS_MESH_MSG_MAX     1600        // assembled JSON message, bytes
#define OS_MESH_PEER_TTL_MS 30000       // peers vanish 30 s after their last HELLO
#define OS_MESH_TAG_LEN     16          // hex chars of an HMAC tag (8 bytes)

typedef enum {
    OS_MESH_HELLO = 1,
    OS_MESH_XDEX = 2,
    OS_MESH_TRADE = 3,
    OS_MESH_RELAY = 4,
    OS_MESH_RELAY_RESP = 5,
    OS_MESH_APP = 6,
    OS_MESH_TYPE_MAX,
} os_mesh_type_t;

typedef enum {
    OS_PEER_BADGE,
    OS_PEER_BEACON,
} os_peer_kind_t;

typedef struct {
    char id[OS_MESH_ID_LEN + 1];
    uint8_t mac[6];
    os_peer_kind_t kind;
    char name[25];          // badge: owner label ("" while unclaimed); beacon: character name
    char title[33];         // beacon: character title
    char character[17];     // beacon: character id
    uint32_t color;         // beacon: character accent 0xRRGGBB
    char nonce[17];         // beacon: current relay nonce
    int level;              // badge: Level
    int8_t rssi;            // 0 when bridged
    int64_t last_ms;        // esp_timer ms of the last frame heard (bridged: of the last sync)
    bool bridged;           // over a bridge (no MAC), not the radio
} os_mesh_peer_t;

void os_mesh_init(void);                    // after os_wifi_init() and os_cloud_init()

// Radio on the mesh channel or a bridge open, and this badge has an id and a mesh key.
bool os_mesh_ready(void);
int os_mesh_channel(void);                  // the channel peers are expected on

// Use `channel` for the mesh instead of the one the server announced, for
// beacons whose access point is on another channel (the ESP-NOW app's Mesh
// tab finds them). It lasts until reboot, or until the server announces a
// different channel; 0 goes back to the server's. Applied within a second.
void os_mesh_set_channel(int channel);
int os_mesh_server_channel(void);           // what the server announced, 0 before the first hello
bool os_mesh_channel_overridden(void);

// Peers heard in the last OS_MESH_PEER_TTL_MS, strongest first.
int os_mesh_peers(os_mesh_peer_t *out, int max);
bool os_mesh_peer(const char *id, os_mesh_peer_t *out);
// Keep a peer in the table whatever else is heard (a trade partner, an
// accepted X Dex peer); unpin when done. False if the id isn't in the table.
// Only HELLOs add peers and set their MAC: other messages from an unknown id
// or another MAC are dropped before any handler sees them.
bool os_mesh_pin(const char *id, bool pin);

// Send a JSON object (NUL-terminated, ≤ OS_MESH_MSG_MAX) to a peer id, or to
// everyone with to_id == NULL. Fragments as needed. Never blocks for long.
esp_err_t os_mesh_send(os_mesh_type_t type, const char *to_id, const char *json);

typedef void (*os_mesh_handler_t)(os_mesh_type_t type, const os_mesh_peer_t *from, const char *json, void *ctx);
int os_mesh_subscribe(os_mesh_type_t type, os_mesh_handler_t cb, void *ctx);   // handle, or -1
void os_mesh_unsubscribe(int handle);

// tag(meshKey, msg) as 16 lowercase hex + NUL (docs/MESH.md "Tags").
// false when the badge has no mesh key yet.
bool os_mesh_sign(const char *msg, char out[OS_MESH_TAG_LEN + 1]);
void os_mesh_nonce(char out[17]);           // 16 random lowercase hex + NUL

// Apps that talk over the mesh call acquire in on_enter and release in
// on_exit. If Wi-Fi is joined on a channel other than the mesh channel, the
// first acquire suspends Wi-Fi and the last release resumes it, except while
// os_mesh_keep_wifi() holds it.
void os_mesh_acquire(void);
void os_mesh_release(void);

// For os_bridge. Bridges need the network: while kept, mesh apps don't suspend
// Wi-Fi (a suspension of ours ends at once; it comes back if a mesh app is
// still open when the hold ends). Any task.
void os_mesh_keep_wifi(bool keep);

typedef struct {
    char id[OS_MESH_ID_LEN + 1];
    char name[25];          // owner label, as a HELLO's "n"
    int level;
} os_mesh_bridged_t;
// The open bridges after a sync (OS task): listed as live badges with
// bridged = true, never evicted; ids missing from the list stop being peers.
// While bridged, radio frames claiming that id are dropped.
void os_mesh_set_bridged(const os_mesh_bridged_t *list, int n);
// A message the server carried from bridged badge `from`: checked and
// delivered on the mesh task like radio input. Any task; json is copied.
void os_mesh_bridge_rx(os_mesh_type_t type, const char *from, const char *json);
