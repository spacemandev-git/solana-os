#pragma once

// Over-the-air updates of Solana OS (docs/HHGTS.md "Software updates").
//
// An admin offers a build; hello carries it (os_cloud_info_t.update). Once the
// owner confirms, os_ota downloads it to the card through os_api (the cloud
// task, like every request), checks size and SHA-256 against hello, then a
// worker task writes it into the OTA slot that isn't running
// (os_ota_flash.h, badge-only), switches slots and restarts. The running slot
// is never written, so a power cut before the switch boots the old build. The
// new build boots on trial (bootloader rollback) until os_ota_init() marks it
// good. Portable: the emulator's os_ota_flash says it can't.

#include <stdbool.h>
#include "esp_err.h"

#define OS_OTA_MIN_BATTERY  70          // percent, before downloading and again before writing

typedef enum {
    OS_OTA_IDLE,
    OS_OTA_DOWNLOADING,                 // pct: of the image
    OS_OTA_VERIFYING,                   // SHA-256 of the download
    OS_OTA_INSTALLING,                  // pct: written to the flash; nothing may stop it now
    OS_OTA_RESTARTING,                  // done: restarts in a moment
    OS_OTA_FAILED,                      // error says why; nothing changed, try again
} os_ota_phase_t;

typedef struct {
    os_ota_phase_t phase;
    int pct;
    char version[32];                   // the build being installed
    const char *error;                  // static text, OS_OTA_FAILED
} os_ota_info_t;

// After os_start(): deletes a leftover download, toasts how the last update
// went, and marks this build good (cancels the bootloader's rollback).
void os_ota_init(void);

// Battery for the 70 % rule: percent, 100 on USB power, -1 unreadable.
int os_ota_battery(void);

// Why an update of the offered build can't start now (static text), NULL if it
// can: an offer, Wi-Fi, a card, battery, no app install running. OS task.
const char *os_ota_blocker(void);

// Start the offered update (after the owner's confirm). OS task. Fails with
// ESP_ERR_INVALID_STATE when os_ota_blocker() says no or one is running.
esp_err_t os_ota_start(void);

// Stop a download (not an install). OS task.
void os_ota_cancel(void);

void os_ota_get(os_ota_info_t *out);    // any task
