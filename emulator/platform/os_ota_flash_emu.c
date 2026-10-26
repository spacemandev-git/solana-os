// os_ota_flash for the browser emulator: there's no flash to write. The page
// always runs the site's Solana OS, so the server never offers it an update.

#include "core/os_ota_flash.h"

esp_err_t os_ota_flash(const char *path, size_t size, const char *version, volatile int *pct, const char **why)
{
    *why = "The emulator runs the site's Solana OS: reload the page instead.";
    return ESP_ERR_NOT_SUPPORTED;
}

bool os_ota_flash_confirm(void)
{
    return false;
}
