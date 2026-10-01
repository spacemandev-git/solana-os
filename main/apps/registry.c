// Every installed app, in launcher order. To add an app: create
// apps/<name>/<name>_app.c exporting an os_app_t, add it to SOLANA_OS_SRCS in
// main/sources.cmake (the badge and the emulator both build from it) and list
// it here.

#include "core/os.h"

extern const os_app_t app_wifi, app_badge, app_espnow, app_skate, app_debug, app_settings, app_about;
extern const os_app_t app_guide, app_xdex, app_trade, app_quests;

const os_app_t *const os_apps[] = {
    &app_wifi,
    &app_badge,
    &app_guide,
    &app_xdex,
    &app_trade,
    &app_quests,
    &app_espnow,
    &app_skate,
    &app_debug,
    &app_settings,
    &app_about,
};

const int os_app_count = sizeof(os_apps) / sizeof(os_apps[0]);
