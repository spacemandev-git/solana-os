#pragma once

// Solana OS app manager.
//
// One app runs at a time. The OS owns the status bar and hands the app a root
// container. Every app callback runs on the OS task with the LVGL lock held,
// so apps may touch LVGL freely there, and must not block for long. Work that
// blocks (scans, SD I/O) belongs in the app's own task or driver callbacks;
// on_tick then renders the results.

#include <stdbool.h>
#include "lvgl.h"
#include "os_keys.h"
#include "theme.h"

#define OS_TICK_MS      100

typedef struct {
    const char *id;                 // short name for the console: "open wifi"
    const char *name;               // launcher / status bar title
    const char *symbol;             // LV_SYMBOL_* launcher icon, or NULL to use icon
    const lv_image_dsc_t *icon;
    uint32_t accent;
    void (*on_enter)(lv_obj_t *root);
    void (*on_exit)(void);          // stop tasks/radios/timers, free memory
    bool (*on_key)(os_key_t key, bool repeat);  // false = not handled (Back then exits)
    void (*on_tick)(void);          // every OS_TICK_MS
    void (*on_button)(uint32_t bit); // optional: raw presses instead of keys (calibration)
} os_app_t;

// apps/registry.c
extern const os_app_t *const os_apps[];
extern const int os_app_count;
extern const os_app_t os_launcher;
extern const os_app_t os_calibrate;     // hidden system app

void os_start(void);
void os_splash(void);

// Safe from any task. Takes effect after the current callback returns.
void os_launch(const os_app_t *app);
void os_launch_id(const char *id);
void os_inject_key(os_key_t key);

// Run fn(arg) on the OS task with the LVGL lock held, after the current
// callback. Safe from any task (mesh handlers, API callbacks, timers). Returns
// false if the queue is full; then fn is not called.
bool os_post(void (*fn)(void *arg), void *arg);
void os_toast(const char *fmt, ...);         // any task
const os_app_t *os_current(void);

// OS-drawn confirm dialog over the current app: SELECT = yes, CANCEL = no.
// The app (or a Lua script) never sees those keys, so it can't answer for
// the user: spending !LAMPs, transfers, factory reset. done(yes, ctx) runs on
// the OS task. Leaving the app (HOME, os_launch) answers no before the app's
// on_exit. If a confirm is already open, done(false) runs right away. Safe
// from any task (the text is copied).
typedef void (*os_confirm_cb_t)(bool yes, void *ctx);
void os_confirm(const char *text, os_confirm_cb_t done, void *ctx);
bool os_confirm_open(void);

// Rebuild the UI in the current theme (OS task, LVGL locked; the Settings app).
void os_theme_apply(void);

// Screenshot of the live screen over the console, see tools/badge.py.
void os_screenshot(void);
