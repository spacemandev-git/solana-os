# Solana OS browser emulator

The badge firmware compiled to WebAssembly with Emscripten, running on an
emulated badge in the web page at `/emulator`. It is not a reimplementation:
the launcher, the OS core, the UI widgets, every app and the generated assets
are the badge's own C files, built from the same source list.

```sh
brew install emscripten          # once
bun run emulator                 # or firmware/emulator/build.sh
# -> web/static/emulator/solana-os.js + solana-os.wasm
```

Prerequisites: a configured badge project (`firmware/sdkconfig` and
`firmware/managed_components/`, produced by any `idf.py build` or
`idf.py reconfigure`) and ESP-IDF v5.5 installed (the build takes cJSON from
`$IDF_PATH/components/json`, falling back to `~/.espressif/v5.5*/esp-idf`).
The build dir is `firmware/emulator/build/` (`BUILD_DIR=... build.sh` to move it).
The generated files are checked in under `web/static/`, so working on the site
does not need Emscripten.

## How it fits together

```
firmware/main/sources.cmake
  SOLANA_OS_SRCS          -> compiled unchanged by both builds
  SOLANA_OS_DEVICE_SRCS   -> badge only; the emulator compiles platform/<name>_emu.c instead
firmware/components/bsp/*.c -> badge only; platform/bsp_<x>_emu.c

firmware/emulator/
  CMakeLists.txt          includes sources.cmake, builds LVGL from managed_components
  tools/gen_sdkconfig.py  sdkconfig.h from firmware/sdkconfig (+ sdkconfig.defaults)
  shim/                   ESP-IDF / FreeRTOS / driver headers (the API surface)
  platform/               their implementations + the device counterparts
```

| Piece | Badge | Emulator |
|---|---|---|
| `core/os.c`, `launcher.c`, `calibrate.c`, `ui.c`, `os_cloud.c`, `apps/*`, `assets/*` | same file | same file |
| LVGL 9.6 | managed component, Kconfig | same sources; `LV_CONF_SKIP` + `LV_CONF_KCONFIG_EXTERNAL_INCLUDE="sdkconfig.h"` generated from the badge's sdkconfig |
| `app_main.c` | boot order | `platform/app_main_emu.c` (same order, plus `os_cloud_set_server(location.origin)`) |
| `core/os_wifi.c` | ESP-IDF Wi-Fi STA | `platform/os_wifi_emu.c`: demo networks, joining always works, fake IP, same NVS keys |
| `core/os_http.c` | esp_http_client | `platform/os_http_emu.c`: `fetch()` against the page's origin |
| `core/console.c` | UART REPL | `platform/console_emu.c`: `emu_console("open wifi")` from the page |
| `bsp_display.c` | ILI9341 + esp_lvgl_port | `bsp_display_emu.c`: 2 x 320x24 partial buffers, RGBA framebuffer for a `<canvas>` |
| `bsp_buttons.c` | TCA9534 + BOOT, button task | `bsp_buttons_emu.c`: page sets the raw lines, same task/callback shape |
| `bsp_rgb.c` | WS2812 via RMT | `bsp_rgb_emu.c`: shown colours read by the page |
| `bsp_battery.c` | ADC | `bsp_battery_emu.c`: slowly draining fake reading |
| `bsp_sd.c` | SPI microSD + FatFs | `bsp_sd_emu.c`: 64 MB card in MEMFS (stdio/dirent work, `readdir` hides `.`/`..` like FatFs) |
| promiscuous Wi-Fi (`espnow_app.c`) | real radio | `esp_wifi_emu.c`: real 802.11 vendor action frames on the tuned channel |
| NVS | flash | `esp_emu.c`: localStorage (`solana-os-emu:nvs`) |

Values the firmware prints are honest: ESP-IDF version "browser emulator", a
random locally administered MAC (kept in localStorage), wasm heap figures.
`os_cloud.c` gets `-DOS_CLOUD_MODEL="emulator" -DOS_CLOUD_HW_PREFIX="emu-"`, so
the server sees model `emulator` and hw `emu-<mac>`.

### Tasks and timing

There are no threads (the site is static; no SharedArrayBuffer, no COOP/COEP).
Each `xTaskCreate` makes an Emscripten fiber (`-sASYNCIFY`). `main()` in
`emu_main.c` is the scheduler: it runs every runnable task until it blocks
(`vTaskDelay`, queue/semaphore/notification/event-group waits, the LVGL lock),
then runs `lv_timer_handler()` like esp_lvgl_port's task (skipped while a task
holds the LVGL lock), then `emscripten_sleep()`s until the next deadline (at
most 8 ms) so the browser stays responsive. Blocking calls therefore yield at
exactly the points where a badge task would block. Nothing preempts a task, so
critical sections are no-ops. `esp_timer` callbacks run on an "esp_timer" task.
Time is `performance.now()`, strictly increasing.

Asyncify instruments indirect calls too (`ASYNCIFY_IGNORE_INDIRECT` would save
~16 % gzipped but would corrupt memory the first time something blocks behind
a function pointer, e.g. an app callback).

Page API (all `EMSCRIPTEN_KEEPALIVE`): `emu_framebuffer/emu_frame`,
`emu_leds/emu_leds_version`, `emu_buttons_set(lines)`, `emu_button_line(key)`
(default wiring from `os_keys.h`), `emu_console(line)`, `emu_halt()`. The page
side is `web/src/lib/components/emulator/emulator.ts`.

## Keeping both builds in sync

- **New app or portable feature:** add it to `SOLANA_OS_SRCS` (and the
  registry). Nothing else; the emulator picks it up on the next build.
- **New ESP-IDF/FreeRTOS API in portable code:** add it to `shim/` +
  `platform/` (keep shims small and honest). If the API is big or truly
  hardware-bound, move that code into a badge-only file behind a small header.
- **New badge-only source** (in `SOLANA_OS_DEVICE_SRCS` or `components/bsp/`):
  add `platform/<name>_emu.c`. The build fails with a message until it exists.
- **Boot change in `app_main.c`:** mirror it in `platform/app_main_emu.c`. The
  build fails if `app_main.c` calls an `os_*`/`bsp_*`/`console_*` function the
  counterpart does not.
- **Config change:** `sdkconfig.defaults` as usual; the emulator reads it.
- Don't branch on `SOLANA_OS_EMULATOR` in firmware; fix the shim instead.

## Limitations

- Radios are simulated demo data (browsers
  can't sniff Wi-Fi). Wi-Fi joins any demo network with any password.
- The SD card is in memory and resets on reload. The battery is a fake.
- No real heap limit: the badge has ~125 KB free, the wasm heap has megabytes,
  so out-of-memory paths are not exercised. Check `heap` on the badge.
- Timing is cooperative: a task that spins without blocking would freeze the
  page (on the badge it would starve lower priorities).
- `esp_restart()` asks the page to boot a fresh instance.

## Gotchas found while building it

- Fiber stacks must be 16-byte aligned (`aligned_alloc`): LLVM folds frame
  offsets into ORs, so an 8-aligned stack silently corrupts locals.
- `EM_JS` bodies go through the C preprocessor: use `"..."` strings, not `''`.
