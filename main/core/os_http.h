#pragma once

// Minimal blocking HTTP for os_cloud. The badge implements it with
// esp_http_client (core/os_http.c); the emulator with the browser's fetch.
// Only call it from a task that may block (never the OS/LVGL task).

#include <stddef.h>
#include "esp_err.h"

// method: "GET" or "POST". body: JSON or NULL. bearer: token or NULL.
// The response body is NUL-terminated into resp (truncated to cap - 1).
// Returns ESP_OK when a response arrived (check *status), an error otherwise.
esp_err_t os_http_request(const char *method, const char *url, const char *bearer, const char *body,
                          char *resp, size_t cap, int *status);
