#pragma once

// User settings, persisted in NVS namespace "settings". Portable (the emulator
// keeps NVS in localStorage). Values are cached after the first read; call
// these from the OS task (app callbacks).

typedef enum {
    OS_SET_THEME,           // OS_THEME_DARK (default) or OS_THEME_LIGHT; see ui_col()
    OS_SET_LCD_INVERT,      // 1 (default) or 0: panel colour inversion, HW Debug > Display
    OS_SET_COUNT,
} os_setting_t;

enum { OS_THEME_DARK = 0, OS_THEME_LIGHT = 1 };

int os_setting(os_setting_t s);
void os_setting_set(os_setting_t s, int value);
