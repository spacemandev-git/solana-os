# Solana OS

Firmware for the Solana Breakpoint 2026 conference badge (ESP32-C5, ILI9341
320x240, microSD, 6 buttons, 8 WS2812s): an app launcher, Wi-Fi, an ESP-NOW
badge-to-badge mesh, the *Hitchhiker's Guide to Solana* game apps and a
sandboxed Lua 5.5 app host. The same sources also build to WebAssembly for a
browser emulator (`emulator/`) and for the store's Lua compiler (`tools/luac/`).

Stack: ESP-IDF v5.5, C, LVGL 9, Lua 5.5.

## Lua apps and memory

The badge has no PSRAM, so a Lua app has to share ~90 KB of free heap with
Wi-Fi, TLS and the mesh. Since 0.5.0:

- Store apps arrive compiled: the App Store compiles them with this firmware's
  own Lua (`tools/luac/`, the same `main/lua/vendor/` built to wasm) and the
  badge runs the bytecode **in place from flash** (`main/lua/lua_xip.c`: slots
  in the otherwise unused `storage` partition, mapped through the flash cache).
  Instructions, line info and long strings never enter the heap and nothing is
  parsed at launch. Bytecode is only ever loaded from a store install whose
  SHA-256 matched; sideloaded apps compile from source.
- Libraries are lazy (`luaL_newlib`, a vendor patch): their tables start empty
  and a function costs RAM only once a script uses it.
- Source is compiled before the libraries are opened, the string table grows
  at two strings per slot, and the collector runs young collections at 10 %
  growth.

Measured on a badge (Lua heap peak, `lua mem`): snake 30.9 KB on 0.4.5, 17.5 KB
on 0.5.0; magic-8 22.8 KB, then 13.6 KB. While snake runs the badge keeps
~15 KB more heap free, and its largest free block doubled (15 KB to 33 KB).

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
| `main/lua/` | Lua host and the vendored Lua 5.5.1 (patches listed in `vendor/LICENSE`) |
| `emulator/` | WebAssembly build of the same firmware |
| `tools/` | Console driver, asset generators, web export, the store's Lua compiler (`luac/`) |

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
