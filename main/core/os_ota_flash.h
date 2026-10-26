#pragma once

// The flash side of os_ota (badge-only: esp_ota_ops; the emulator's
// counterpart refuses). Called from os_ota's worker task, never the OS task.

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

// Write the image at path (size bytes, already checked against hello) into
// the OTA slot that isn't running, check it's a Solana OS build of `version`,
// let ESP-IDF validate it, and make it the boot slot. Blocking; *pct follows
// the write (0..100). On failure *why is a static, user-facing reason and the
// boot slot is unchanged.
esp_err_t os_ota_flash(const char *path, size_t size, const char *version, volatile int *pct, const char **why);

// The running build is fine: if it boots on trial after an update, keep it
// (cancel the rollback). Returns true if it was on trial.
bool os_ota_flash_confirm(void);
