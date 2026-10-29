# Source lists shared by the badge build (main/CMakeLists.txt) and the browser
# emulator (firmware/emulator/). Add new apps and portable core files to
# SOLANA_OS_SRCS so both builds pick them up; only code that talks to real
# hardware or ESP-IDF services belongs in SOLANA_OS_DEVICE_SRCS, and each of
# those needs an emulator counterpart.

# Lua 5.4 for store apps (lua/vendor/LICENSE): the core and the libraries the
# sandbox offers, no io/os/package/debug.
set(SOLANA_OS_LUA_VENDOR_SRCS)
foreach(f lapi lauxlib lbaselib lcode lcorolib lctype ldebug ldo ldump lfunc lgc llex lmathlib lmem
          lobject lopcodes lparser lstate lstring lstrlib ltable ltablib ltm lundump lutf8lib lvm lzio)
    list(APPEND SOLANA_OS_LUA_VENDOR_SRCS lua/vendor/${f}.c)
endforeach()

set(SOLANA_OS_SRCS
    core/os.c core/launcher.c core/calibrate.c core/ui.c core/os_cloud.c core/os_settings.c core/os_wifi_boot.c
    core/os_mesh.c core/os_bridge.c core/os_api.c core/os_data.c core/os_cmd.c core/os_ota.c
    game/game.c game/xdex.c game/trade.c game/quests.c
    apps/registry.c
    apps/wifi/wifi_app.c
    apps/badge/badge_app.c
    apps/guide/guide_app.c apps/xdex/xdex_app.c apps/trade/trade_app.c apps/quests/quests_app.c
    apps/espnow/espnow_app.c
    apps/debug/debug_app.c
    apps/settings/settings_app.c
    apps/update/update_app.c
    apps/about/about_app.c
    apps/store/store_app.c
    apps/apps/apps_app.c
    lua/lua_host.c lua/lua_ui.c lua/lua_sys.c lua/lua_net.c lua/lua_json.c lua/lua_pkg.c lua/lua_cmd.c
    lua/lua_fs.c lua/lua_install.c
    ${SOLANA_OS_LUA_VENDOR_SRCS}
    assets/sol_mono_12.c assets/sol_font_14.c assets/sol_font_20.c assets/sol_font_32.c
    assets/img_logomark_16.c assets/img_logomark_40.c assets/img_logomark_72.c
    assets/img_foundation.c assets/img_foundation_black.c
)

set(SOLANA_OS_DEVICE_SRCS
    app_main.c
    core/os_wifi.c
    core/os_http.c
    core/os_ota_flash.c
    core/os_radio.c
    core/console.c
)
