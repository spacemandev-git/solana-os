#pragma once

// The one place that maps physical buttons to logical keys. Apps only ever
// see os_key_t. Front silkscreen: UP / DOWN / LEFT / RIGHT d-pad, SELECT and
// CANCEL; BOOT is on the back. OS_KEY_BACK is the CANCEL button.

#include "bsp.h"

typedef enum {
    OS_KEY_UP,
    OS_KEY_DOWN,
    OS_KEY_LEFT,
    OS_KEY_RIGHT,
    OS_KEY_SELECT,
    OS_KEY_BACK,
    OS_KEY_HOME,
    OS_KEY_COUNT,
    OS_KEY_NONE = -1,
} os_key_t;

// Default wiring, used until the buttons are calibrated (HW Debug > Input >
// SELECT, or console "open calib"). The calibrated map lives in NVS, so a
// board revision with different wiring needs no firmware change.
// Measured on the prototype with the calibration app.
#define OS_BTN_UP       BSP_BTN_PB(1)
#define OS_BTN_LEFT     BSP_BTN_PB(2)
#define OS_BTN_RIGHT    BSP_BTN_PB(3)
#define OS_BTN_DOWN     BSP_BTN_PB(4)
#define OS_BTN_SELECT   BSP_BTN_PB(5)
#define OS_BTN_BACK     BSP_BTN_PB(6)
#define OS_BTN_HOME     BSP_BTN_BOOT

// Front-panel keys that calibration assigns (HOME stays on BOOT).
#define OS_KEY_FRONT_COUNT  OS_KEY_HOME

os_key_t os_key_from_button(uint32_t bit);
uint32_t os_key_button(os_key_t k);
void os_keys_load(void);
void os_keys_save(const uint32_t map[OS_KEY_FRONT_COUNT]);

const char *os_key_name(os_key_t k);
os_key_t os_key_parse(const char *name);
