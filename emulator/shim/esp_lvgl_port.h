#pragma once

// esp_lvgl_port for the emulator: LVGL runs on the scheduler loop instead of
// its own task, and skips a frame while a firmware task holds the lock, just
// as the port's LVGL task would wait for it.

#include <stdbool.h>
#include <stdint.h>
#include "lvgl.h"

bool lvgl_port_lock(uint32_t timeout_ms);
void lvgl_port_unlock(void);
