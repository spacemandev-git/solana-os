#pragma once

// Authenticated calls to the server's device API (server/API.md "Device API").
//
// Requests are queued to one "api" worker task, which makes the blocking
// os_http call with the badge's bearer. The callback is then posted to the OS
// task (LVGL lock held), so apps can update their UI directly. Call
// os_api_cancel(ctx) in on_exit: callbacks still queued for that ctx are
// dropped instead of touching a dead UI.
//
// Portable (os_http underneath; the emulator's os_http uses fetch).

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#define OS_API_RESP_DEFAULT 4096        // API.md: device responses are ≤ 4 KB

// status: HTTP status, 0 = no response (offline, network error, out of
// memory). body: NUL-terminated response body (JSON), or NULL when status is 0.
// The body is freed after the callback returns.
typedef void (*os_api_cb_t)(int status, const char *body, void *ctx);

// path: "/api/device/..." (the server origin comes from os_cloud). method:
// "GET" | "POST" | "PATCH" | "DELETE". body: JSON or NULL. resp_cap 0 = default.
// ESP_ERR_INVALID_STATE when offline (cb is not called), ESP_ERR_NO_MEM when
// the queue is full (8 waiting). Requests run one at a time on the cloud task,
// between hellos. Going offline answers what is still queued with status 0.
// Any task may call it; cb always runs on the OS task.
esp_err_t os_api_call(const char *method, const char *path, const char *body, size_t resp_cap,
                      os_api_cb_t cb, void *ctx);

// GET path and stream the body into dest (a full path on the SD card, e.g.
// from os_data_path(); missing directories under OS_DATA_DIR are created), at
// most max_bytes. dest is replaced only by a complete 2xx body (written to
// dest.part first). cb gets status and body = the X-App-Manifest header
// (decoded) or "". No card: status 0.
esp_err_t os_api_download(const char *path, const char *dest, size_t max_bytes, os_api_cb_t cb, void *ctx);

// OS task (on_exit): no callback for ctx runs after this. A NULL ctx is never
// cancelled (os_api_cancel(NULL) does nothing), so a module that cancels must
// pass a ctx of its own: its state's address, or a unique os_api_token().
void os_api_cancel(void *ctx);
void *os_api_token(void);                   // unique non-NULL ctx, never a real address

// The badge keeps one connection to the server open (a TLS handshake needs
// ~35 KB, much of it in one block; requests on an open connection need a few
// KB). Call this when an app that talks to the server starts, BEFORE it
// allocates: if online and no connection is open, the cloud task says hello
// now, which opens one. Cheap when the connection is already up.
void os_api_prewarm(void);

// Wi-Fi joined, a server configured and this badge registered (has an id + secret).
bool os_api_online(void);

// Outbox: POSTs that must reach the server eventually (X Dex swaps, trades).
// Persisted in /sdcard/hh/outbox.jsonl (RAM without a card), sent in order
// whenever the badge is online, dropped on a 2xx or 4xx answer (except 401,
// 408 and 429, which are retried). path: "/api/device/..."; json <= 4 KB.
// Appends to the SD card: call it from the OS task or your own task, not from
// a mesh handler (os_post() over first). Answers that carry "lamps" and
// "level" update os_cloud's figures.
esp_err_t os_outbox_add(const char *path, const char *json);
int os_outbox_count(void);
