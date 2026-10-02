#pragma once

// Emulator internals shared by the platform files. Firmware never includes this.

#include <stdbool.h>
#include <stdint.h>

// ---- Time ----
int64_t emu_now_us(void);           // monotonic, strictly increasing

// ---- Scheduler (freertos_emu.c) ----
// Runs every runnable task until each one blocks. Called from the main loop
// only (never from a task).
void emu_sched_run(void);
// Earliest wake-up among blocked tasks, INT64_MAX if none.
int64_t emu_sched_next_wake_us(void);
bool emu_in_task(void);
void emu_sched_init(void);
// Block the current task until `ready(obj)` or the deadline. Returns true if
// ready. Outside a task (main loop / JS callbacks) it only polls once.
typedef bool (*emu_ready_fn)(void *obj);
bool emu_block_until(emu_ready_fn ready, void *obj, int64_t deadline_us);
int64_t emu_ticks_to_deadline(uint32_t ticks);

// ---- LVGL port (bsp_display_emu.c) ----
bool emu_lvgl_locked(void);
void emu_display_pump(void);        // lv_timer_handler when the lock is free
int emu_display_next_ms(void);

// ---- Radio (esp_wifi_emu.c) ----
uint8_t emu_radio_channel(void);
bool emu_radio_promiscuous(void);
// While promiscuous: hand a mesh frame heard from another tab (os_radio_emu.c)
// to the sniffer as the ESP-NOW action frame it would be on air.
void emu_promisc_espnow(const uint8_t mac[6], const uint8_t *data, int len, int rssi);

// ---- Identity (esp_emu.c) ----
const uint8_t *emu_mac(void);       // persistent random locally-administered MAC

// ---- Boot ----
void app_main(void);                // platform/app_main_emu.c
