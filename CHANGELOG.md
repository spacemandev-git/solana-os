# Changelog

Solana OS releases, newest first, from this repository's history. The version is
`CONFIG_APP_PROJECT_VER` in `sdkconfig.defaults`; a badge takes an over-the-air
update only to a newer one. Dates are commit dates. The contracts these builds
implement (`docs/LUA_API.md`, `docs/MESH.md`, `server/API.md`) and the server and
site changes that came with them live in the parent repository.

## 0.5.3 (2026-10-30)

- **Lua QR codes**: `ui.qr(x, y, size, text)` and `q:text()`, 25 to 218 px, text up to
  256 bytes, one at a time (its canvas sits on the system heap). Always black on white
  with a quiet zone, whatever the theme, since not every phone scanner reads
  light-on-dark codes. Text that doesn't fit at that size is a Lua error.
- The Lua API docs show a player's phone wallet signing a transaction the badge builds:
  the app posts the message to its own server, shows a Solana Pay transaction request
  as a QR code, and finds the sent transaction on chain by its reference.
- A UI method called on another kind of object (`bar.set(label, 50)`,
  `label.text(bar, "x")`) is now a Lua error. Before, it wrote one widget's fields into
  another's memory.

## 0.5.2 (2026-10-29)

- **Lua Solana RPC** (`rpc`, permission `rpc`): `rpc.call(method, [params], [opts], cb)`
  on mainnet, or devnet with `{cluster = "devnet"}`. RPC is sponsored by Triton One. The
  badge server holds the node URLs and makes the call (`POST /api/device/apps/:id/rpc`,
  as for `http`), allowing reads that fit a badge's 4 KB answer and `sendTransaction`
  of a transaction signed elsewhere; the callback gets the call's `result`. Whole
  numbers beyond 32 bits (lamports) arrive as exact decimal strings, because the
  badge's Lua integers are 32-bit. The docs warn never to create or store a keypair on
  the badge.
- The permission shows as "Read and send to Solana" in the App Store's details and the
  Apps library's info. The store refuses `rpc` apps to 0.5.0 and 0.5.1 badges, which
  have no `rpc` module.
- A server error shown to a script can be up to 127 characters (was 95), so an RPC
  node's error reads in full. A 429 that comes without an error text reads
  "rate limited" (was "server error 429").

## 0.5.1 (2026-10-29)

- **Lua WebSockets** (`ws`, permission `http`, to the app's `hosts`):
  `ws.open(url, [opts], cb)`, `sock:send`, `sock:close`, `sock:state`, with the events
  `open`, `message` and `close(reason, code)`. The socket goes straight from the badge
  to the host and takes the place of the badge's one TLS connection: the badge server
  checks the URL and signs a ticket (`badge_ticket` in the query, so the host learns the
  app, badge and player), then the cloud task closes its server connection, keeping the
  session ticket (`os_http_hangup`), and runs the socket (`core/os_socket.c`) until it
  closes. Meanwhile the app's other server calls fail with "socket open" and the badge's
  own syncs wait. One socket at a time, ≤ 4 KB per message, 8 waiting each way (a fast
  host is slowed down, not dropped); the handshake needs the same 56 KB free as the
  server connection's.
- `core/os_ws.c`: an RFC 6455 client over ESP-IDF's TLS transport, written here because
  the IDF WebSocket transport can stall on bytes it read along with the upgrade answer.
  It checks the host's addresses again before connecting (no private networks). The
  emulator uses the browser's WebSocket (`emulator/platform/os_ws_emu.c`).
- Fix: after a console upload (`lua put`, `lua recv`, so Run on badge too) to an app
  installed from the store, the badge kept running the store's compiled code from its
  flash slot. A slot now runs only while `code.bin` exists.

## 0.5.0 (2026-10-28)

- Lua 5.5.1 replaces 5.4.9, with the sandbox patches carried over. Apps need about 40 %
  less heap: snake peaks at 17.5 KB (30.9 KB on 0.4.5), magic-8 at 13.6 KB (22.8 KB).
- Store apps run compiled, in place from flash: the App Store compiles each version with
  this firmware's own Lua (`tools/luac/`), the badge installs the result as `code.bin`
  (checked by SHA-256) and `lua_xip.c` maps it from a 1 MB slot of the `storage`
  partition, so instructions, line info and long strings never enter the heap. Bytecode
  is loaded from nowhere else: console uploads and quest downloads delete `code.bin`,
  scripts can't load binary chunks, sideloaded apps compile from source.
- Lazy libraries (a function takes RAM once a script uses it), no `string.dump`, the
  string table at two strings per slot, strip level 2, source compiled before the
  libraries open, young collections at 10 % growth.
- For scripts: a `for` loop's variable is read-only, and `global` declares a global.
  Store apps need 0.5.0 from here on; older badges' App Store says how to update.

## 0.4.6 (2026-10-28)

- Lua `http` (`http.get`, `http.post`, `http.request`, permission `http`): the badge
  server makes the request (the badge has no heap for a second TLS connection while an
  app runs), only to the hosts the app lists, and signs who is asking. The App Store
  details and the Apps info screen show the hosts; `http` apps open the server
  connection before they start.

## 0.4.5 (2026-10-28)

- Bridges: badges that share no radio (an emulator and a badge, or two emulators) meet
  through the server: the code panel, scanning, a 1-3 s sync and a send queue
  (`core/os_bridge.c`). Bridged peers sit next to radio ones in the mesh, so X Dex,
  Trade and Lua `mesh` work over them; X Dex and Trade offer "Bridge a badge".
- The firmware became its own repository (this one), under the MIT license.

## 0.4.4 (2026-10-28)

- Emulated badges' ids start with a lowercase `e` (a badge's never starts with `E`), and
  the mesh accepts them.

## 0.4.3 (2026-10-26)

- Production panels show true colours: inversion is off for good, and the 0.4.1
  setting and its HW Debug toggle are gone.
- Wi-Fi joins the saved network at boot when it's in range, else turns off after 30 s
  (`core/os_wifi_boot.c`).
- My Badge's "Link X at <site>/profile" uses the address hello's `site` gives (the site
  moved to solana.com/breakpoint/badge).

## 0.4.2 (2026-10-25)

- Fix: every over-the-air download crashed (an SD command started while a display
  flush's DMA was still running on the shared SPI bus). A flush now holds the bus from
  its start to its done callback, and every SD command and probe waits for it
  (`components/bsp/bsp_bus.h`). Badges on 0.4.0 or 0.4.1 can't update over the air
  because of this: update them over USB (`/stock` on the site).
- The `ota` console command (the console now starts after every command is registered).

## 0.4.1 (2026-10-25)

- Colour inversion became a setting (some panels showed RED on cyan with it on), applied
  at boot and flipped in HW Debug > Display.

## 0.4.0 (2026-10-25)

- Over-the-air updates: Settings > Software update installs the build an admin offers
  (hello `update`). It needs Wi-Fi, a card and 70 % battery or USB; it downloads to the
  card, verifies, writes the other OTA slot and restarts, and a build that doesn't
  reach the launcher rolls back. 0.4.0 and 0.4.1 come from the same commit.

## 0.3.1 (2026-10-06)

- `lamps.deposit`: Lua apps pay into their author's escrows (`ESC-…` addresses), always
  through the OS confirm and under the badge's daily cap.

## 0.3.0 (2026-10-02)

- Apps are folders: `main.lua`, `app.json` and up to 64 bundled files (≤ 256 KB each,
  1 MB together) that stay on the card until the app reads them. Lua `require` for the
  app's modules and a read-only `fs` (`exists`, `size`, `list`, `read`, `lines`,
  `json`, `load`) on the app's heap and instruction budget; a vendor patch
  (`lua_pushfilled`) reads straight into the string.
- Installs (`lua/lua_install.c`, for the App Store and `lua install`): only changed
  files download, into `apps/<id>.new/`; every SHA-256 is checked, `data.json` is kept,
  the folders swap, and a power cut mid-swap is recovered. The store shows sizes and
  file progress.
- Console: `lua recv` (a base64 stream acknowledged block by block), `lua sums`,
  `lua rm <id> <file>` and `lua put` for bundled paths, which the site's Run on badge
  uses. The REPL task has 5 KB of stack.

## 0.2.2 (2026-10-02)

- Selection highlights are solid Solana Green.
- ESP-NOW app: a Mesh tab sweeps every channel for beacons and badges and moves the
  badge's mesh channel there (console `mesh channel <n|auto>`); the Frames tab keeps the
  sniffer.

## 0.2.1 (2026-10-01)

- The production server is the build default: a new badge needs no `server` command.
- My Badge shows the claim code (`ABCD-2345`) next to the badge ID and QR code, to claim
  without scanning.
- Pack reveals burst into confetti (`ui_confetti`), more for rarer pages.
- Skate Monkey is no longer a built-in app.
- Fixes: stale trade confirms close themselves, X Dex learns a queued upload's result
  once the outbox sends it, the console's quest log prints, character colours are
  readable in Light.

## 0.2.0 (2026-10-01): the Hitchhiker's Guide release

The game layer, *The Hitchhiker's Guide to Solana*:

- Apps: My Badge (owner, !LAMPs, Level, pages and packs, offline from the card), Guide
  (collection, page reader, pack reveal, shop), X Dex (badge swaps over the air, the
  yearbook), Trade (!LAMPs, items and pages; both sides lock, the inviter signs on a
  deliberate confirm), Quests (beacons nearby, the quest log), App Store and Apps.
- The mesh (`core/os_mesh.c`, docs/MESH.md) over ESP-NOW: fragments, HELLO presence
  and a peer table, HMAC tags, MAC binding. Mesh input is hostile: nested JSON is
  dropped before parsing, each message type is pre-filtered, only HELLOs add peers.
- The server link (`core/os_cloud.c`, `core/os_api.c`): hello, the device API and an
  outbox for swaps and trades, all on the cloud task, over one kept-alive TLS
  connection with session tickets (a new handshake only when the heap can hold it). A
  new owner wipes the card's game data; factory reset (`reset yes`).
- The Lua host: Lua 5.4.9 in a sandbox (48 KB heap, an instruction budget per callback,
  a C stack guard) with `ui`, `led`, `timer`, `sys`, `badge`, `json`, `storage`,
  `wifi`, `mesh`, `lamps`, `items`, `profile` and `quest`. Spending and transfers go
  through an OS confirm a script can't press; server calls queue per app.
- Wi-Fi is off at boot with the radio parked on the mesh channel; joining prefers the
  SSID's access point on the mesh channel; mesh apps keep the radio awake.
- Shared UI: the offline panel, amounts, category and rarity colours, `os_confirm`,
  toasts from any task; console commands from portable code (`os_cmd`).
- Skate Monkey, a Flappy Bird game, joined the built-in apps.

## 0.1.0 (2026-09-30)

- First release: the launcher and the Wi-Fi, My Badge (claim QR code), ESP-NOW
  sniffer, HW Debug, Settings and About apps; the board drivers from the reference
  investigation (display, microSD on the shared SPI bus, buttons with calibration,
  LEDs, battery); the UART console with screenshots (`shot`, `tools/badge.py`); and the
  browser emulator built from the same sources.
