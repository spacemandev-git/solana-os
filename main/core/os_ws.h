#pragma once

// A blocking WebSocket client, one connection at a time, for os_socket.c,
// which runs it on the cloud task in place of the server connection (the
// cloud task stays the only network worker, os_net.h). The badge implements
// it with ESP-IDF's WebSocket transport over TLS (core/os_ws.c), the emulator
// with the browser's WebSocket.

#include <stddef.h>
#include "esp_err.h"

// os_ws_recv results
#define OS_WS_MSG       1               // a whole message in *out
#define OS_WS_TIMEOUT   0               // nothing arrived in time
#define OS_WS_CLOSED    (-1)            // the host closed it: *code is its close code (1005: none given)
#define OS_WS_LOST      (-2)            // the connection dropped, or the host broke the protocol
#define OS_WS_TOO_BIG   (-3)            // a message over max (close with 1009)

// url: wss://<host>[/path][?query] on port 443. protocols: "a, b" or NULL.
// ESP_OK: open. ESP_ERR_NO_MEM: the heap can't hold a TLS handshake now.
// ESP_ERR_INVALID_ARG: a URL we don't open, or a host that resolves to a
// private address. ESP_FAIL: no connection (DNS, TCP, TLS, or the host
// refused the upgrade).
esp_err_t os_ws_connect(const char *url, const char *protocols);

// Waits up to timeout_ms for a message, joining fragments and answering
// pings. OS_WS_MSG: *out is malloc'd (NUL-terminated, the caller frees it)
// and *len its bytes. A binary message comes the same way.
int os_ws_recv(char **out, size_t *len, size_t max, int timeout_ms, int *code);

// One text message. text is masked in place while it goes out and restored
// after (a WebSocket client must mask), so it must be writable. ESP_FAIL: the
// connection is gone.
esp_err_t os_ws_send(char *text, size_t len);

// Sends a close frame with code (0: none, the connection is gone anyway) and
// frees everything.
void os_ws_close(int code);
