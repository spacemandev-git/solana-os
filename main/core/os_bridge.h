#pragma once

// Bridges (docs/MESH.md "Bridges"): badges that share no radio, an emulator and
// a badge or two emulators, meet through the server. This badge shows a bridge
// code (os_bridge_show: a QR code that changes every 30 s), the other scans it
// (os_bridge_scan, the emulator page's camera or paste); after that each is a
// peer of the other in os_mesh (bridged = true), and XDEX, TRADE and APP
// messages between them go through POST /api/device/bridge/sync instead of the
// air. Apps don't need to know: os_mesh_send() and the handlers work the same.
//
// While a bridge is open or the code shows, the badge syncs every 1-3 s and
// keeps Wi-Fi joined (os_mesh_keep_wifi). Portable: the emulator runs it as is.

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "os_mesh.h"

#define OS_BRIDGE_MAX       6           // server/API.md: open bridges per badge
#define OS_BRIDGE_CODE_LEN  40          // "<id>.<window>.<tag>" + NUL, with room

typedef struct {
    char id[OS_MESH_ID_LEN + 1];
    char name[25];          // owner label ("" while unclaimed)
    int level;
    int left_s;             // seconds left, as of the last sync
} os_bridge_t;

void os_bridge_init(void);                  // after os_mesh_init()

// Open bridges as of the last sync (any task).
int os_bridge_count(void);
int os_bridge_list(os_bridge_t *out, int max);

// This badge's code for now, from its mesh key and the server's clock; false
// without an id, a key or the time (Wi-Fi was never on). url: what the QR
// code holds, "<site>/emulator?bridge=<code>". Any task.
bool os_bridge_code(char *code, size_t n);
bool os_bridge_url(char *url, size_t n);

// Show the code over the current app (os_overlay; CANCEL closes it), or close
// it. Any task.
void os_bridge_show(void);
void os_bridge_hide(void);
bool os_bridge_showing(void);

// Bridge with the badge that shows `code` (the code, or a link holding it).
// Offline, it waits up to 2 minutes for Wi-Fi. The outcome is a toast. Any task.
esp_err_t os_bridge_scan(const char *code);

void os_bridge_end_all(void);               // any task

// For os_mesh: queue a message for bridged badge `to`, or every bridge with to
// NULL. ESP_ERR_NOT_FOUND: no such bridge; ESP_ERR_NO_MEM: the queue is full.
// Any task.
esp_err_t os_bridge_send(os_mesh_type_t type, const char *to, const char *json);
