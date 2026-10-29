# Solana OS

Firmware for the Solana Breakpoint 2026 conference badge (ESP32-C5, ILI9341
320x240, microSD, 6 buttons, 8 WS2812s): an app launcher, Wi-Fi, an ESP-NOW
badge-to-badge mesh, the *Hitchhiker's Guide to Solana* game apps and a
sandboxed Lua 5.4 app host. The same sources also build to WebAssembly for a
browser emulator (`emulator/`).

Stack: ESP-IDF v5.5, C, LVGL 9, Lua 5.4.

## Build and flash

```sh
source ~/.espressif/v5.5.4/esp-idf/export.sh
idf.py build
idf.py -p /dev/cu.usbserial-110 -b 460800 flash
```

`tools/badge.py` drives the badge's serial console from the host and saves
`shot` screenshots as PNGs (`python3 tools/badge.py --reset "shot"`).

## Layout

| Path | What |
|---|---|
| `components/bsp/` | Board drivers: display, microSD, buttons, RGB LEDs, battery |
| `main/core/` | The OS: app manager, Wi-Fi, mesh, server link, OTA, UI kit |
| `main/apps/` | Built-in apps (one folder each, registered in `apps/registry.c`) |
| `main/game/` | Game services (X Dex, trades, quests) |
| `main/lua/` | Lua host and the vendored Lua 5.4.9 |
| `emulator/` | WebAssembly build of the same firmware |
| `tools/` | Console driver, asset generators, web export |

## Part of solana-bp26-badges

This repo is a git submodule (at `firmware/`) of
[solana-bp26-badges](https://github.com/spacemandev-git/solana-bp26-badges),
which holds the server, website, beacon firmware and the protocol contracts
(`docs/MESH.md`, `docs/LUA_API.md`, `server/API.md`). A few tools assume that
layout: `tools/export_web.ts` / `tools/build_web.sh`, `tools/gen_assets.sh`
(brand fonts) and the emulator build (writes to `../web/static/emulator`).
The firmware itself builds standalone.

`sdkconfig.defaults` points the badge at the production server
(`CONFIG_SOLANA_OS_SERVER_URL`); override it there or on the console with
`server <url>`.

## License

MIT (see `LICENSE`). The bundled fonts in `main/assets/` are rendered from
ABC Diatype and the logos are Solana brand assets; both remain subject to
their owners' terms.
