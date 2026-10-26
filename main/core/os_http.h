#pragma once

// Minimal blocking HTTP for os_cloud and os_api. The badge implements it with
// esp_http_client (core/os_http.c) over one kept-alive connection to the
// server, so the TLS handshake (and its heap peak) happens once, not per
// request; the emulator uses the browser's fetch. Only the cloud task calls
// in here (never the OS/LVGL task).

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

// method: "GET", "POST", "PATCH" or "DELETE". body: JSON or NULL. bearer:
// token or NULL. The response body is NUL-terminated into resp (truncated to
// cap - 1). Returns ESP_OK when a response arrived (check *status), an error
// otherwise (ESP_ERR_NO_MEM: not enough heap for TLS right now).
esp_err_t os_http_request(const char *method, const char *url, const char *bearer, const char *body,
                          char *resp, size_t cap, int *status);

// GET url and stream a 2xx body into the file at path (created or truncated),
// at most max bytes: a longer body fails with ESP_ERR_INVALID_SIZE (the file
// is then incomplete; the caller removes it). Other statuses leave the file
// alone. header (or NULL): name of one response header to copy into hdr_out
// ("" if absent).
esp_err_t os_http_download(const char *url, const char *bearer, const char *path, size_t max,
                           const char *header, char *hdr_out, size_t hdr_cap, int *status);

// Bytes of the download in flight so far and its Content-Length (0 if
// unknown); both 0 when none is running. Any task: a progress bar.
void os_http_progress(size_t *done, size_t *total);

// The kept-alive connection: closed after 5.5 min without requests (call
// tick from the cloud task's loop) or at once with close (Wi-Fi gone).
void os_http_tick(void);
void os_http_close(void);
bool os_http_connected(void);               // a connection is open now
void os_http_stats(uint32_t *requests, uint32_t *connects);
