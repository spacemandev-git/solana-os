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
// the queue is full.
esp_err_t os_api_call(const char *method, const char *path, const char *body, size_t resp_cap,
                      os_api_cb_t cb, void *ctx);

// GET path and stream the body into dest (a file path on the SD card), at most
// max_bytes. cb gets status and body = the X-App-Manifest header (decoded) or "".
esp_err_t os_api_download(const char *path, const char *dest, size_t max_bytes, os_api_cb_t cb, void *ctx);

void os_api_cancel(void *ctx);

// Wi-Fi joined, a server configured and this badge registered (has an id + secret).
bool os_api_online(void);

// Outbox: POSTs that must reach the server eventually (X Dex swaps, trades).
// Persisted in /sdcard/hh/outbox.jsonl (RAM without a card), sent in order
// whenever the badge is online, dropped on a 2xx or 4xx answer.
esp_err_t os_outbox_add(const char *path, const char *json);
int os_outbox_count(void);
