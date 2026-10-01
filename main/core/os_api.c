// Device API client, see os_api.h and server/API.md.
// STUB: the contract compiles; firmware agent "fw-core" replaces this file
// with the real worker task + outbox.

#include "os_api.h"

esp_err_t os_api_call(const char *method, const char *path, const char *body, size_t resp_cap,
                      os_api_cb_t cb, void *ctx)
{
    return ESP_ERR_INVALID_STATE;
}

esp_err_t os_api_download(const char *path, const char *dest, size_t max_bytes, os_api_cb_t cb, void *ctx)
{
    return ESP_ERR_INVALID_STATE;
}

void os_api_cancel(void *ctx) {}
bool os_api_online(void) { return false; }
esp_err_t os_outbox_add(const char *path, const char *json) { return ESP_ERR_NOT_SUPPORTED; }
int os_outbox_count(void) { return 0; }
