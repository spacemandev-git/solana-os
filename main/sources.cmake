# Source lists shared by the badge build (main/CMakeLists.txt) and the browser
# emulator (firmware/emulator/). Add new apps and portable core files to
# SOLANA_OS_SRCS so both builds pick them up; only code that talks to real
# hardware or ESP-IDF services belongs in SOLANA_OS_DEVICE_SRCS, and each of
# those needs an emulator counterpart.

set(SOLANA_OS_SRCS
    core/os.c core/launcher.c core/calibrate.c core/ui.c core/os_cloud.c core/os_settings.c
    apps/registry.c
    apps/wifi/wifi_app.c
    apps/badge/badge_app.c
    apps/espnow/espnow_app.c
    apps/debug/debug_app.c
    apps/settings/settings_app.c
    apps/about/about_app.c
    assets/sol_mono_12.c assets/sol_font_14.c assets/sol_font_20.c assets/sol_font_32.c
    assets/img_logomark_16.c assets/img_logomark_40.c assets/img_logomark_72.c
    assets/img_foundation.c assets/img_foundation_black.c
)

set(SOLANA_OS_DEVICE_SRCS
    app_main.c
    core/os_wifi.c
    core/os_http.c
    core/console.c
)
