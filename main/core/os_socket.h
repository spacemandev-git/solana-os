#pragma once

// The running Lua app's WebSocket (docs/LUA_API.md "ws"), one at a time.
//
// The badge holds one TLS connection, so the cloud task runs the socket in
// place of the server connection: it closes that (keeping its session
// ticket), connects to the URL the server signed, and stays on the socket
// until it ends; then it goes back to hellos and the device API, whose
// connection comes back with the next request. Meanwhile os_api_online() is
// false, so nothing else queues for the server.
//
// The app opens, sends and closes from its own task; events wait here until
// it takes them (os_socket_next), and wake() tells it there is one. While 8
// messages wait, the cloud task stops reading, so a fast host is slowed down
// instead of filling the heap. Portable: os_ws underneath.

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#define OS_SOCKET_MSG_MAX   4096        // bytes per message, either way
#define OS_SOCKET_URL_MAX   800         // a 512-character URL and the server's ticket

typedef enum { OS_SOCKET_EV_OPEN, OS_SOCKET_EV_MESSAGE, OS_SOCKET_EV_CLOSE } os_socket_ev_kind_t;

typedef struct {
    os_socket_ev_kind_t kind;
    char *data;                         // MESSAGE: malloc'd, NUL-terminated; the taker frees it
    size_t len;
    const char *reason;                 // CLOSE: "host closed", "lost", "offline", "unreachable",
                                        // "not enough memory", "host not allowed", "too big"
    int code;                           // CLOSE: the host's close code ("host closed"), else 0
} os_socket_ev_t;

// Any task, without locks held: an event is waiting for ctx.
typedef void (*os_socket_wake_t)(void *ctx);

// url: wss://, as the server answered it (the ticket included). protocols:
// "a, b" or NULL. ctx: the app's own, unique (an os_api_token()).
// ESP_ERR_INVALID_STATE: another socket hasn't ended yet.
esp_err_t os_socket_open(const char *url, const char *protocols, os_socket_wake_t wake, void *ctx);

// The next event for ctx, oldest first: OPEN, MESSAGEs, then CLOSE, after
// which the socket is gone and another may open. false: none waiting.
bool os_socket_next(void *ctx, os_socket_ev_t *ev);

// Queues one text message (copied). ESP_ERR_INVALID_STATE: not open,
// ESP_ERR_INVALID_SIZE: over OS_SOCKET_MSG_MAX, ESP_ERR_NO_MEM: 8 already
// waiting to go (or no memory).
esp_err_t os_socket_send(void *ctx, const char *text, size_t len);

// The app closes it, or is gone: no more events for ctx, and another socket
// may open at once. What it queued still goes out, then the connection
// closes (1000), before the next socket connects.
void os_socket_close(void *ctx);

// From os_socket_open until its CLOSE is taken: the server connection is off.
bool os_socket_active(void);
