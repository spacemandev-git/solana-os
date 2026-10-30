# Solana OS Lua apps

Badge apps from the App Store are Lua 5.5 programs that run in a sandbox on Solana
OS: `main.lua`, plus any modules and data files the app brings along. This file is
the API reference. The web shows it at `/apps/docs`, Solana OS implements it
(`main/lua/`) and the server enforces the permissions.

## The package

An app is a folder. `main.lua` (UTF-8, ≤ 64 KB) runs first, and `app.json` is the
manifest, which you fill in when you submit the app on the web (a folder or .zip you
upload can bring one along to fill in the form). Every other file in the folder ships
with the app: Lua modules for `require`, and data of any kind (text, JSON, binary) for
`fs`. They stay on the microSD card until the app reads them, so a big app only spends
memory on what it has loaded.

```
trivia/
  main.lua              runs first
  app.json              the manifest
  lib/quiz.lua          require("lib.quiz")
  questions/space.txt   fs.lines("questions/space.txt")
```

Bundled files: at most 64 besides `main.lua` and `app.json`, each ≤ 256 KB, ≤ 1 MB
together. Paths use lowercase letters, digits, `_`, `-` and `.`, with `/` between
folders: ≤ 64 characters, ≤ 4 levels, and no name starts or ends with `.`. The App
Store installs an app into `/sdcard/hh/apps/<id>/`: `main.lua`, `app.json`, the app's
`storage` in `data.json`, and the bundled files under `files/`. An update downloads
only the files that changed, then swaps the whole folder in once everything is there,
keeping `data.json`.

Apps that quests hand out through a beacon are a `main.lua` only: beacons can't carry
bundled files.

### Compiled by the store

When you publish, the store compiles `main.lua` and every bundled file ending in `.lua`
with the badge's own Lua (the same sources, built for the server), and refuses a
version that doesn't compile, naming the file and line (`main.lua:12: 'end' expected
near <eof>`). Badges download that compiled code along with the source and run it
**in place from the badge's flash**: the instructions, line
numbers and long strings never enter the app's heap, there's no compiling at launch,
and `require` and `fs.load` of a compiled module read nothing from the card. On the
sample apps the heap peaks 15-25 % lower than when the badge compiles the same source
(and 30-45 % lower than on Solana OS 0.4: snake 31 KB then, 18 KB now).

The compiled code keeps file and line numbers in error messages but not the names of
local variables: an error reads `main.lua:12: attempt to index a nil value` rather than
`... (local 'player')`. Apps you run from the web editor (Try in emulator, Run on badge)
or copy over the console are compiled on the badge from their source, with full
messages.

**Apps need Solana OS 0.5.0 or later.** An older badge installs nothing: its App Store
says "Update to Solana OS 0.5.0 to install apps" for every app and tells how in the
app's details, buying and downloads are refused, a quest's app download stops with
"update Solana OS to 0.5.0 for apps", and Run on badge stops before sending anything.
Update it from the site's firmware flasher (`/stock`, a USB-C cable) or, from 0.4.2, in
Settings > Software update when it's offered. An app with the `rpc` permission needs
0.5.2: on 0.5.0 and 0.5.1 its details say so, and buying and downloads are refused with
"Update Solana OS to 0.5.2 or later to install this app".

The manifest, as the badge stores it:

```json
{
  "id": "snake",                    // 3-24 chars: a-z 0-9 -, unique in the store
  "name": "Snake",                  // ≤ 20 chars, shown in the launcher list
  "version": "1.0.0",
  "author": "@toly",
  "summary": "Eat, grow, don't bite yourself.",
  "icon": "play",                   // one of the icon names below
  "accent": "14F195",               // hex colour, no '#'
  "category": "games",              // games | tools | social | quests | demos
  "permissions": ["lamps", "items", "http"],  // led wifi mesh lamps items profile quest http rpc
  "hosts": ["api.example.com"],     // what `http` and `ws` may call (only with the `http` permission)
  "files": 2,                       // bundled files (the store fills it in)
  "filesSize": 5120,                // their bytes together
  "temp": false,                    // true for quest-granted apps
  "expires": null,                  // unix ms when a temporary app stops working
  "code": {                         // the store's compiled code, for Solana OS 0.5.0+
    "luac": "5.5.1-sol.1",          // the compiler; the badge runs it only if it's its own
    "sha256": "…", "size": 5692     // code.bin, checked when it's installed
  }
}
```

Icons: `play`, `star`, `heart`, `bell`, `gift`, `home`, `image`, `list`, `wifi`,
`shuffle`, `eye`, `edit`, `charge`, `gps`, `settings`, `audio`, `bulb` (the badge maps
them to LVGL symbols; the web to matching glyphs).

## Lifecycle

The host loads `main.lua`, runs the top-level chunk once (build your UI there), then
calls these globals if you define them:

```lua
function on_key(key, rep)    -- "up" "down" "left" "right" "select" "back"; rep = auto-repeat
  return true                -- handled. Returning false/nil for "back" exits the app.
end
function on_tick(ms)         -- every 50 ms, ms = time since the last tick
end
function on_mesh(from, msg)  -- a message from the same app on another badge (needs "mesh")
end
function on_exit()           -- the app is closing (HOME, CANCEL, sys.exit, error)
end
```

HOME always returns to the launcher. The screen below the status bar is
**320 × 218** pixels (`ui.W`, `ui.H`), with (0, 0) at the top-left.

### Limits

| | |
|---|---|
| Memory | 48 KB of Lua heap (the app is stopped with "Out of memory" beyond that). A store app's compiled code runs from flash and mostly isn't in it; a library function takes a few bytes once the app first uses it |
| CPU | 200 000 VM instructions per callback, coroutines and pattern matching included (else "Script took too long") |
| UI objects | 64 at a time, one of them a QR code |
| Text | 200 bytes per label (longer text ends in "..."), 6 KB of label text on screen |
| Timers | 8 at a time, ≥ 20 ms |
| Storage | 16 KB per app (`storage.*`) |
| Files | 64 bundled files, ≤ 256 KB each, ≤ 1 MB together, read-only (`fs`, `require`) |
| Reads | ≤ 16 KB per `fs.read` and `fs.json`, lines ≤ 512 bytes. Card reads count toward the instruction budget: 500 per file opened or looked up (`fs.exists`, `fs.size`, `fs.list`), 1 per 4 bytes read or compiled |
| Mesh | 4 messages per second, ≤ 200 bytes each |
| HTTP | 60 requests a minute per badge, ≤ 2 KB sent, ≤ about 3.5 KB of answer, 6 s to answer |
| RPC | 30 calls a minute per badge (6 of them `sendTransaction`), ≤ 2 KB of params, ≤ about 3.5 KB of answer, 6 s to answer |
| WebSocket | one at a time, 20 opens a minute per badge, ≤ 4 KB per message either way, 8 waiting to go |
| Numbers | 32-bit integers (they wrap at ±2^31) and 32-bit floats (about 7 digits) |

A runtime error stops the app and shows the message with the line number. CANCEL
returns to the launcher. `print(...)` goes to the badge's serial console.

### Callbacks and network calls

Anything that talks to the server (including `http` and `rpc`, which go through it) is
asynchronous and takes a callback `cb(ok, result)`: `ok` is a boolean, and `result` is
the value or an error string (`"offline"` when Wi-Fi is off, `"not linked"` when the
badge has no owner, `"denied"` when the app lacks the permission or the user said no,
`"busy"` when 16 calls are already waiting, `"socket open"` while the app's `ws` socket
is open). Callbacks run like any other callback: between `on_tick`s, never
concurrently.

## Standard library

Lua 5.5 (Solana OS 0.5.0; earlier badges ran 5.4). Available: `assert error ipairs next
pairs pcall select tonumber tostring type xpcall print getmetatable setmetatable
rawequal rawget rawlen rawset _G _VERSION`, `string.*` (no `string.dump`), `table.*`
(with 5.5's `table.create`), `math.*` (with `math.random` seeded per launch), `utf8.*`,
`coroutine.*`, and `require` for the app's own modules (see `fs`). Removed: `io`, `os`,
`debug`, `package`, `load`/`loadfile`/`dofile` (`fs.load` compiles the app's own files),
`warn`, `collectgarbage` (except `collectgarbage("count")`).

What 5.5 changes for a 5.4 script: a `for` loop's variable is read-only (`for i = 1, 3
do i = i + 1 end` doesn't compile; copy it into a local), and `global x` declares a
global (`global` is still fine as a name elsewhere). The store refuses code that
doesn't compile, so you see these when you publish.

Library tables fill in as you use them: `string`, `table`, `math`, `utf8`, `coroutine`
and the badge's own modules (`ui`, `led`, `fs`, ...) keep their functions in flash, and
a function takes RAM only once the app uses it. Calling and indexing work as usual, and
`pairs(string)` lists everything; but `next` and `rawget` see only the functions used so
far, and `string.format = nil` doesn't remove a function (it comes back on the next
lookup). Adding your own functions to a library table (`function string.trim(s)`) works.

## Modules

### `ui`

```lua
ui.W, ui.H                          -- 320, 218
ui.bg(color)                        -- background of the app area
local l = ui.label(x, y, text, {size=14, color="text", mono=false, w=nil, align="left"})
--   size: 12 | 14 | 20 | 32 (12 is always mono), align: "left" | "center" | "right" (needs w)
l:text("hi")  l:pos(x, y)  l:color("green")  l:show(false)  l:remove()
local r = ui.rect(x, y, w, h, {color="panel", radius=0, border=nil})
r:pos(x, y)  r:size(w, h)  r:color(c)  r:show(b)  r:remove()
local b = ui.bar(x, y, w, h, value)     -- progress 0..100, Solana gradient fill
b:set(value)  b:remove()
local g = ui.gradient(x, y, w, h)       -- the Solana gradient
local list = ui.list(x, y, w, h, items, {on_select=function(i, item) end})
--   items: { "text", ... } or { {title=, sub=, right=}, ... }; the list takes up/down
--   rows are 36 px; long text ends in "..."
list:items(newItems)  list:selected() --> index  list:select(i)  list:remove()
local q = ui.qr(x, y, size, text)       -- a QR code, size x size px (25..218), text ≤ 256 bytes
q:text("solana:https://...")  q:pos(x, y)  q:show(b)  q:remove()
ui.hint("SELECT jump   CANCEL exit")    -- key hint pinned to the bottom (16 px)
ui.toast("Saved")
ui.confirm("Reset the score?", function(yes) end)      -- modal, SELECT = yes, CANCEL = no
ui.input("Your name", 16, function(text) end)          -- d-pad keyboard; text = nil if cancelled
ui.clear()                              -- remove every object
```

Colours are theme names: `"text" "dim" "bg" "panel" "border" "purple" "green" "pink"
"lpurple" "blue" "aqua" "lime" "yellow" "orange"`. They follow the Dark/Light theme.
A number `0xRRGGBB` is a fixed colour. While a list, `ui.confirm` or `ui.input` has
focus it consumes the keys it uses before `on_key` sees them.

`ui.qr` (Solana OS 0.5.3; `nil` on older badges, so check `if ui.qr then`) draws a QR
code a phone can scan: a link, an address, a [Solana Pay](#build-on-the-badge-sign-on-a-phone)
request. It's always black on white with a white margin, whatever the theme, because
not every scanner reads light-on-dark codes. One at a time: change what it shows with
`q:text()`. Text that doesn't fit at `size` is an error. Shorter text means bigger
squares that scan from farther away: up to about 130 bytes at 180 px gives 3 px per
square.

### `led` (permission `led`)

```lua
led.count                   -- 8 (1..6 across the top, 7..8 on the right edge)
led.set(i, r, g, b)         -- 0..255 each; the OS scales to its brightness cap
led.hex(i, 0x9945FF)
led.fill(r, g, b)
led.off()
```

Changes show at the end of the current callback. LEDs turn off when the app exits.

### `timer`

```lua
local id = timer.after(ms, fn)
local id = timer.every(ms, fn)
timer.cancel(id)
```

### `sys`, `badge`, `json`

```lua
sys.millis()                -- ms since the app started
sys.time()                  -- unix seconds from the server clock, or nil if never synced
sys.battery()               -- percent, or nil on USB / unknown
sys.exit()                  -- close the app
badge.id()                  -- "7K2Q9M", or nil before the first hello
badge.owner()               -- "@toly", or nil while unclaimed
badge.online()              -- Wi-Fi joined and registered with the server (false while a ws socket is open)
json.encode(value)  json.decode(text)   -- nil plus an error message on bad input
```

### `storage` (always allowed)

Private key/value storage on the SD card, kept across launches and removed with the app.

```lua
storage.get("best")         -- value or nil
storage.set("best", 42)     -- numbers, strings, booleans, tables (JSON-encodable)
storage.remove("best")
```

### `fs` and `require` (always allowed)

The app's bundled files (see "The package"), read-only. Paths are the ones in your
folder, like `"questions/space.txt"`: no leading `/` and no `..`. An app sees only its
own files; `main.lua`, `app.json` and `data.json` aren't among them.

```lua
local quiz = require("lib.quiz")      -- runs lib/quiz.lua once; later calls return the same value
fs.exists("questions")                -- true for a file or a folder
fs.size("questions/space.txt")        -- bytes, or nil if there's no such file
fs.list("questions")                  -- { "art.txt", "extra/", "space.txt" }: sorted, folders end in "/"
fs.list()                             -- the top of the app's files; {} for a folder that isn't there
fs.read("intro.txt")                  -- the whole file (≤ 16 KB) as a string, or nil, "not found"
fs.read("book.txt", 8192, 1024)       -- 1024 bytes from byte 8192 (counting from 0); "" past the end
for line in fs.lines("words.txt") do  -- each line without its "\n" or "\r\n"; ≤ 512 bytes,
end                                   -- longer lines come in pieces
fs.json("levels/1.json")              -- the decoded value (file ≤ 16 KB), or nil and an error
local scene = fs.load("scenes/intro.lua")  -- compiled, not run or kept: scene() runs it;
                                           -- nil and an error for a syntax error
```

`require("lib.quiz")` loads `lib/quiz.lua`: dots separate folders. The module runs
with the same globals as `main.lua`, and what it returns (usually a table, `true` if
nothing) is what every `require` of that name gets. A missing module or an error in
one stops the app like any other error; wrap it in `pcall` to handle it. A missing
file is an error for `fs.lines` too, while `fs.read`, `fs.json` and `fs.load` return
`nil` and a message. A bad path, or asking for more than 16 KB at once, is an error.

Everything you read lands in the app's 48 KB heap, so keep only what you use: read
big files in pieces (offsets or `fs.lines`), and load data when the app needs it.
`require` keeps a module until the app exits. For code you need only for a while (a
level, a scene), use `fs.load` and drop the function and what it returned once you're
done: the collector frees them. In a store app both load the store's compiled code of
the file from flash (see "Compiled by the store"), which costs far less than compiling
it; other files (an app copied over the console) compile from the card. Reading also counts toward the instruction budget
(see Limits): 100 KB costs 25 000 of the 200 000 a callback gets. The file read last
stays open, so reading on in it (pages, lines) doesn't pay to open it again.

### `wifi` (permission `wifi`)

```lua
wifi.status()               -- "off" | "connecting" | "on" | "failed"
wifi.ssid()                 -- saved network name or nil
wifi.on()                   -- join the saved network
wifi.off()
```

### `mesh` (permission `mesh`)

Opening an app with `mesh` puts the radio on the mesh channel while it runs.

```lua
mesh.nearby()               -- { {id="7K2Q9M", name="@toly", level=12, rssi=-48, kind="badge", bridged=false}, ... }
mesh.send(to, msg)          -- to = badge id or "*"; msg = table (≤ 200 bytes as JSON)
-- received by on_mesh(from_id, msg) on badges running the same app
mesh.bridge()               -- show this badge's bridge code over the app (CANCEL closes it)
```

**Bridged badges.** The emulator has no radio and the badge has no camera, so an
emulated badge meets a real one (or another emulator in a different browser) over a
**bridge** through the server (docs/MESH.md "Bridges"). One badge shows its bridge
code, the other scans it in the emulator page; after that each is in the other's
`mesh.nearby()` with `bridged=true` and `rssi=0`, and `mesh.send` and `on_mesh` work
exactly as over the air: a message to its id goes to it, and `"*"` reaches every badge
in range *and* every bridged badge. The same app has to run on both. Your app doesn't
need to know which is which, with two differences:

- **Latency.** A bridged message takes about 1 to 3 seconds (radio: a few ms), and
  messages to one badge arrive in order. Send state, not every key press: a turn-based
  game or a shared score works well, a fast action game doesn't.
- **Loss.** Like the radio, a bridge may drop a message (one waiting more than 60 s,
  or a backlog of more than 64). Repeat what matters until the other side answers.

`mesh.bridge()` opens the code on top of your app until the user presses CANCEL (keys
don't reach your app meanwhile); bridges that open show up in `mesh.nearby()` within a
few seconds. It needs Wi-Fi: without it the panel says so. In the emulator, the page's
**Bridge a badge** panel scans or pastes a code, so you can test a multiplayer app
with one badge and one emulator, or two emulator tabs.

```lua
-- Who's here, over the radio or a bridge?
local list = ui.list(0, 0, ui.W, ui.H - 16, {}, {})
ui.hint("SELECT ping   RIGHT bridge   CANCEL exit")

local function refresh()
  local names = {}
  for _, p in ipairs(mesh.nearby()) do
    if p.kind == "badge" then
      names[#names + 1] = (p.name ~= "" and p.name or p.id) .. (p.bridged and "  (bridged)" or "")
    end
  end
  list:items(names)
end
timer.every(1000, refresh)

function on_key(k)
  if k == "right" then mesh.bridge() return true end
  if k == "select" then mesh.send("*", {ping = true}) return true end
  return false
end

function on_mesh(from, msg)
  if msg.ping then ui.toast(from .. " says hi") end
end
```

### `lamps` (permission `lamps`)

```lua
lamps.balance(cb)                       -- cb(true, 1250)
lamps.spend(amount, sku, cb)            -- in-app purchase paid to the app's author.
                                        -- The OS asks "Pay 50 !LAMPs to <App> for <sku>?"
                                        -- cb(true, {balance=1200}) | cb(false, "denied")
lamps.transfer(badgeId, amount, memo, cb) -- pay another badge's owner (OS confirm dialog too)
lamps.deposit(address, amount, memo, cb) -- pay into one of the author's escrows (OS confirm too):
                                        -- "Pay 50 !LAMPs into escrow ESC-7K2Q9M3X of <App>?"
                                        -- cb(true, {balance=1150, id="4821"})  (Solana OS 0.3.1)
                                        -- spend + transfer + deposit: at most 500 !LAMPs a day per user
lamps.award(amount, reason, cb)         -- earn: only within the app's daily award cap,
                                        -- cb(true, {awarded=10, balance=1260})
                                        -- only owners who linked X earn (the cap counts
                                        -- per X account); otherwise
                                        -- cb(true, {awarded=0, balance=1250, needsX=true})
```

#### Escrows

`lamps.spend` pays the author's own balance. To hold !LAMPs for players instead (a
wager's pot, buy-ins, a jackpot, a bank the app pays winners from), create an escrow
at `/apps/escrows`: a payment address like `ESC-7K2Q9M3X` with a balance of its own,
apart from yours. Put the address in your app and pay into it with `lamps.deposit`;
it only accepts escrows that belong to the app's author. `memo` (≤ 64 characters) is
how your app says what a deposit is for (`"round 42 red"`).

Paying out is up to your own server, with the escrow's key (shown once when you make
the escrow; you can rotate it): it polls the escrow's ledger for new deposits (who
paid, from which badge and app, the memo), decides, and pays players back, winners,
or you, by badge id or user id. The badge never holds the key, so nobody can pay
themselves out of your escrow by tampering with a badge. The endpoints are in the
server API (`/api/escrow`, `/api/escrow/ledger`, `/api/escrow/pay`):

```sh
curl -H "Authorization: Bearer $ESCROW_KEY" "https://<server>/api/escrow/ledger?after=0"
curl -H "Authorization: Bearer $ESCROW_KEY" -H "content-type: application/json" \
  -d '{"to":"7K2Q9M","amount":100,"memo":"won round 42","key":"round-42"}' https://<server>/api/escrow/pay
```

Payouts show in the player's wallet as "Escrow payout" with your memo. `key` makes a
payout safe to retry. Your app can also talk to your server with `http` (below), and
the signed headers tell the server which badge and player is asking: join a round,
then pay into it with the round in the memo.

### `items` (permission `items`)

Items are JSON objects in the owner's inventory. An app can create and change only
its own items, and it can read everything. An app granted by a quest may also update
the items that quest gave while the grant is live (e.g. Virus Lab mutating the quest's
virus), and `items.list` includes them. Lists hold the newest 50.

```lua
items.list(cb)              -- the items this app may update: { {id=, kind=, name=, data={...}, tradable=true}, ... }
items.all(cb)               -- every item the owner has (read-only for other apps' items)
items.grant(kind, name, data, cb)        -- new item; data ≤ 1 KB as JSON; cb(true, item)
items.update(id, data, cb)               -- replace data of one of this app's items
items.remove(id, cb)
```

The server rate-limits grants (20 per user per app per hour) and caps an app at
200 live items per user.

### `profile` (permission `profile`)

```lua
profile.me(cb)              -- {name=, x=, level=, lamps=, pages=, badge=}
profile.get(badgeId, cb)    -- public profile of that badge's owner: {name=, x=, level=}
```

### `quest` (permission `quest`)

For apps that quests hand out.

```lua
quest.report(event, data, cb)   -- tell the quest engine something happened, e.g.
                                -- quest.report("mutated", {strain="omega"})
quest.active(cb)                -- the owner's active quests: { {id=, title=, step=}, ... }
```

### `http` (permission `http`, Solana OS 0.4.6)

Requests to web servers: your own (a game server, the server behind your escrow) or a
public API. The badge can't open a second secure connection while an app runs, so the
badge server makes the request for it and hands back the answer. A request needs
Wi-Fi like every server call, and works the same in the emulator.

```lua
http.get(url, cb)                       -- cb(true, res) or cb(false, "offline")
http.get(url, headers, cb)              -- headers: {["Accept"] = "text/plain"}
http.post(url, body, cb)                -- body: a string, or a table (sent as JSON)
http.post(url, body, headers, cb)
http.request({method = "PUT", url = url, headers = {...}, body = body}, cb)
-- res = {status = 200, body = "...", type = "application/json", truncated = false}
```

**Hosts.** An app calls only the hosts it lists in `hosts` when you publish it: up to
8 host names, each a name like `api.example.com` or `*.example.com` (every subdomain of
example.com, not example.com itself), with no IP addresses, ports or paths. The store
shows them next to the permission, so people see where the app sends data, and the
badge server refuses any other host with `"host not allowed"`. Change them in a new
version.

**Requests.**

- `url`: `https://` only (port 443), ≤ 512 characters, no `user:password@`.
- `method`: `GET`, `POST`, `PUT`, `PATCH`, `DELETE` or `HEAD`; `http.get` and
  `http.post` set it, `http.request` defaults to `GET`, or `POST` when there's a body.
- `headers`: a table of up to 8 name = value pairs. Names are letters, digits and `-`;
  values printable ASCII, ≤ 256 characters. The badge server sets `Host`,
  `User-Agent`, `Content-Length`, the connection headers and the `X-Badge-*` headers
  itself, so those are refused.
- `body`: ≤ 2 KB. A table is sent as JSON (with `Content-Type: application/json`
  unless you set one), a string as it is (`text/plain; charset=utf-8` unless you set
  one).

**Answers.** Any answer from the server counts: `ok` is `true` and `res.status` says
what it was, so check it (a 404 is `ok` too). `res.body` is its text, up to about
3.5 KB; a longer answer is cut there with `res.truncated = true`. Ask your server for
just what the app needs (a 20 KB weather report won't fit, a 300-byte summary will).
Bodies are text: bytes that aren't UTF-8 come out as `"�"`. `res.type` is the
`Content-Type`. Redirects aren't followed: a 3xx comes back as it is, with
`res.location`. `json.decode(res.body)` turns a JSON answer into a table.

`ok` is `false` when there was no answer: `"offline"`, `"not linked"`, `"denied"` (the
app lacks the permission, or isn't in the store: see below), `"busy"`, `"host not
allowed"`, `"too fast"` (over 60 a minute), `"timeout"` (no answer within 6 s),
`"unreachable"` (no such host, no connection, or a bad certificate), `"bad url"`,
`"bad header: <name>"` (one the badge server sets itself) or `"request too big"`. A
URL that doesn't start with `https://` or has spaces in it, and a malformed header,
are Lua errors, like a bad argument anywhere else.

**Publish before you test.** The badge server checks each request against the app as
it is in the store: the permission, the hosts, and that the badge's owner may run it.
An app that was never published gets `"denied"`, even from Run on badge or Try in
emulator. Publish a first version, then keep trying changes with Run on badge; the
hosts in effect are the published version's.

**What your server receives.** Each request carries headers that say who is calling,
signed by the badge server so nobody can fake them:

```
User-Agent: SolanaOS-Badge/1
X-Badge-App: snake                     the app's id
X-Badge-Id: 7K2Q9M                     the badge (an emulator's id starts with "e")
X-Badge-User: 2f1c...-...              the owner's user id, as in escrow ledgers
X-Badge-Time: 1790000000000            unix ms
X-Badge-Request: 4c1f0e9a7b2d6e83      random, different for every request
X-Badge-Key: 9d2b7e41c0a8f356          which signing key (see below)
X-Badge-Signature: 3q2+7w...==         Ed25519 signature, base64
```

They are ids, never a name or email (an app that needs the name has `profile`). The
signature covers these nine lines joined with `"\n"` (no newline at the end):

```
badge-http-v1
POST                                   the method
https://api.example.com/round?id=42    the URL as sent (without a #fragment)
9f86d081...                            SHA-256 of the body as sent, hex (of "" for none)
snake                                  X-Badge-App
7K2Q9M                                 X-Badge-Id
2f1c...-...                            X-Badge-User
1790000000000                          X-Badge-Time
4c1f0e9a7b2d6e83                       X-Badge-Request
```

The public key is at `GET <site>/api/http/key`, with `<site>` as in the address bar
of the store (`https://solana.com/breakpoint/badge`):
`{"id": "9d2b7e41c0a8f356", "alg": "Ed25519", "key": "<32 bytes, base64>", "pem":
"-----BEGIN PUBLIC KEY-----..."}`. Keep it by `id`, and fetch it again when
`X-Badge-Key` names one you don't have (the key may be replaced). To trust a request,
check the signature, that `X-Badge-App` is your app, that `X-Badge-Time` is within a
minute of your clock, and that you haven't seen its `X-Badge-Request` in that minute
(a replay). In TypeScript (Bun, Node 20+, Deno, Cloudflare Workers):

```ts
const SITE = 'https://solana.com/breakpoint/badge';
const keys = new Map<string, CryptoKey>();
const seen = new Map<string, number>(); // X-Badge-Request -> time
let fetchedAt = 0;

async function badgeKey(id: string): Promise<CryptoKey | null> {
  if (!keys.has(id) && Date.now() - fetchedAt > 60_000) { // at most once a minute
    fetchedAt = Date.now();
    const k = await (await fetch(`${SITE}/api/http/key`)).json();
    const raw = Uint8Array.from(atob(k.key), (c) => c.charCodeAt(0));
    keys.set(k.id, await crypto.subtle.importKey('raw', raw, 'Ed25519', false, ['verify']));
  }
  return keys.get(id) ?? null;
}

/** The badge and player behind a request from your app, or null. `body`: the raw body. */
export async function fromBadge(req: Request, body: string, app: string) {
  const h = (n: string) => req.headers.get(n) ?? '';
  const time = Number(h('x-badge-time'));
  const id = h('x-badge-request');
  if (h('x-badge-app') !== app || !(Math.abs(Date.now() - time) < 60_000) || !id || seen.has(id)) return null;
  const key = await badgeKey(h('x-badge-key'));
  if (!key) return null;
  const hash = new Uint8Array(await crypto.subtle.digest('SHA-256', new TextEncoder().encode(body)));
  const signed = ['badge-http-v1', req.method, req.url, [...hash].map((b) => b.toString(16).padStart(2, '0')).join(''),
    app, h('x-badge-id'), h('x-badge-user'), h('x-badge-time'), id].join('\n');
  const sig = Uint8Array.from(atob(h('x-badge-signature')), (c) => c.charCodeAt(0));
  if (!(await crypto.subtle.verify('Ed25519', key, sig, new TextEncoder().encode(signed)))) return null;
  seen.set(id, time);
  for (const [k, t] of seen) if (Date.now() - t > 60_000) seen.delete(k);
  return { badge: h('x-badge-id'), user: h('x-badge-user') };
}
```

`req.url` has to be the URL the badge asked for. Behind a proxy or load balancer that
changes it (to `http://`, or another host), put your public `https://` address and the
path with its query together yourself.

The signature proves which badge and account sent a request through your app's id,
not that your exact code sent it: anyone can run their own script under your app's id
on their own badge. Decide on your server what a player may do, as you would for any
client. And since anyone who has your app can read `main.lua`, an API key in it is no
secret: keep keys on your server and let the app call that.

```lua
-- A shared high-score table on your own server (hosts = {"scores.example.com"}).
local list = ui.list(0, 0, ui.W, ui.H - 16, {"Loading..."}, {})
ui.hint("SELECT submit 100   CANCEL exit")

local function show(ok, res)
  if not ok then list:items({"No scores: " .. res}) return end
  if res.status ~= 200 then list:items({"Server said " .. res.status}) return end
  local rows = {}
  for i, s in ipairs(json.decode(res.body) or {}) do
    rows[#rows + 1] = {title = s.name, right = tostring(s.score)}
  end
  list:items(rows)
end

http.get("https://scores.example.com/top?n=10", show)

function on_key(k)
  if k ~= "select" then return false end
  -- The server learns the player from the signed headers, so the body only says what happened.
  http.post("https://scores.example.com/score", {score = 100}, function(ok, res)
    ui.toast(ok and res.status == 200 and "Sent" or "Not sent")
    http.get("https://scores.example.com/top?n=10", show)
  end)
  return true
end
```

### `ws` (permission `http`, Solana OS 0.5.1)

A live connection (a WebSocket) to one of the app's hosts, for messages both ways as
they happen: a multiplayer game, a chat, live scores. Unlike `http` it goes straight
from the badge to the host, so messages arrive as the host sends them. The badge holds
one secure connection at a time, so while the socket is open it takes the place of the
badge's own connection to the badge server (see "While a socket is open").

```lua
local sock = ws.open(url, cb)                         -- url: wss://, to one of the app's hosts
local sock = ws.open(url, {protocols = {"chat.v1"}}, cb)
-- cb("open")                       connected
-- cb("message", text)              a message from the host
-- cb("close", reason, code)        the end, always the last event (reasons below)
sock:send(text)                     -- or a table, sent as JSON: true, or nil and why not
sock:close()                        -- cb("close", "closed") follows
sock:state()                        -- "connecting", "open" or "closed"
```

**Opening.** `url` is `wss://` on port 443 (no other port, no `user:password@`), ≤ 512
characters, to a host in the app's `hosts` (the same list as `http`), with no
`badge_ticket` in its query. The badge first asks the badge server for a ticket (see
"What the host receives"), which checks the permission, the hosts and that the owner
may run the app, as for `http`: publish before you test. Then it closes its connection
to the badge server and connects. `protocols` (optional): up to 4 subprotocols to
offer, each letters, digits, `.`, `-` and `_`, ≤ 32 characters; the host picks one or
none. One socket at a time: while one is connecting or open, `ws.open` returns a
socket that closes at once with `"busy"`. A URL that doesn't start with `wss://` or has
spaces in it is a Lua error.

**Messages.** ≤ 4 KB each way, in order. `sock:send` sends a text message (a table as
JSON) and returns `true`, or `nil` and `"not open"` (still connecting, or closed), `"too
big"` or `"too fast"` (8 messages already waiting to go out). A message from the host
arrives as a string (a binary message too, byte for byte); one over 4 KB closes the
socket with `"too big"`. Messages are delivered between callbacks like everything else,
and the badge stops reading from the host while 8 wait for the app, so a fast host is
slowed down, not dropped.

**Closing.** Every socket ends with one `cb("close", reason, code)`; `code` is the
host's close code when the host closed it (1000 normal, 1001 going away, ...), else
nil. The socket also closes when the app exits (no callback then).

| `reason` | |
|---|---|
| `"closed"` | the app called `sock:close()` |
| `"host closed"` | the host closed it (see `code`) |
| `"lost"` | the connection dropped without a close |
| `"offline"` | no Wi-Fi, or it went |
| `"unreachable"` | no connection: no such host, a TLS error, or the host refused the WebSocket |
| `"not enough memory"` | no room for a secure connection beside the app (see "Memory") |
| `"too big"` | a message over 4 KB came in |
| `"busy"` | another socket is connecting or open |
| `"too fast"` | over 20 opens a minute |
| `"not linked"`, `"denied"`, `"host not allowed"`, `"bad url"` | as for `http` |

To reconnect, call `ws.open` again from the `"close"` callback, after a few seconds if
it failed (opens count toward 20 a minute).

**While a socket is open** (from `ws.open` until its `"close"`), the badge isn't
connected to the badge server. The app's other server calls (`lamps`, `items`,
`profile`, `quest`, `http`, `rpc`) fail at once with `"socket open"`, `badge.online()` is
false, and the badge's own syncs (its owner, bridges, X Dex and trade uploads) wait.
Badges nearby over the air (`mesh`) keep working; bridged ones pause. So call the
server first, then open the socket, and close it when you need the server again: the
connection to the badge server comes back by itself once the socket's `"close"` has
run.

**Memory.** Starting a secure connection takes up to about 33 KB for a moment (22 KB
to `echo.websocket.org`), and an open one keeps about 5 KB. The badge closes its server
connection first, then starts the socket only with 56 KB free, the same rule as for
its own connection, so other things don't run out meanwhile. With Wi-Fi joined, an app
whose Lua heap is around 10 KB when it calls `ws.open` has that room, one at 20 KB
doesn't (`lua mem` shows the heap). So open the socket first and load big data after
it's up; without the room it closes with `"not enough memory"`. In the emulator the
socket is the browser's: everything above holds, except that memory is never short and
a failed connection is always `"unreachable"` (browsers don't say why).

**What the host receives.** The badge adds a query parameter to the URL, last:
`badge_ticket=<ticket>`. It tells your server the app, badge and player, as `http`'s
signed headers do (a browser can't set headers on a WebSocket, so the emulator couldn't
send those). The ticket is seven fields joined with `.`:

```
<app>.<badge>.<user>.<time>.<request>.<key>.<signature>
snake.7K2Q9M.2f1c...-....1790000000000.4c1f0e9a7b2d6e83.9d2b7e41c0a8f356.Lw3n...
```

The app id, the badge id (an emulator's starts with `e`), the owner's user id, unix
ms, 16 random hex, the signing key's id, and an Ed25519 signature (base64url, no
padding) by the same key as `http`'s (`GET <site>/api/http/key`) over these lines
joined with `"\n"`:

```
badge-ws-v1
api.example.com                        the host the badge connects to
snake                                  the app
7K2Q9M                                 the badge
2f1c...-...                            the owner's user id
1790000000000                          the time
4c1f0e9a7b2d6e83                       the request
```

Check it when the connection opens, as for a request: the signature, that it names
your app, that the time is within a minute of your clock and that you haven't seen the
request in that minute. With `badgeKey()` and `seen` from the `http` example:

```ts
/** The badge and player opening a socket to your app, or null. host: your wss host name. */
export async function fromBadgeSocket(req: Request, app: string, host: string) {
  const ticket = new URL(req.url).searchParams.get('badge_ticket') ?? '';
  const [a, badge, user, time, request, keyId, sig, extra] = ticket.split('.');
  if (extra !== undefined || a !== app || !sig || !request || seen.has(request) ||
      !(Math.abs(Date.now() - Number(time)) < 60_000)) return null;
  const key = await badgeKey(keyId);
  if (!key) return null;
  const signed = ['badge-ws-v1', host, app, badge, user, time, request].join('\n');
  const raw = Uint8Array.from(atob(sig.replace(/-/g, '+').replace(/_/g, '/')), (c) => c.charCodeAt(0));
  if (!(await crypto.subtle.verify('Ed25519', key, raw, new TextEncoder().encode(signed)))) return null;
  seen.set(request, Number(time));
  return { badge, user };
}

// Bun: Bun.serve({ async fetch(req, server) { const who = await fromBadgeSocket(req, 'snake', 'api.example.com');
//   return who && server.upgrade(req, { data: who }) ? undefined : new Response('no', { status: 403 }); }, websocket: {...} })
```

As with `http`, the ticket proves which badge and account opened the socket through
your app's id, not that your exact code did, and anything in `main.lua` is public.

```lua
-- A chat room on your own server (hosts = {"chat.example.com"}).
local list = ui.list(0, 0, ui.W, ui.H - 16, {"Connecting..."}, {})
ui.hint("SELECT wave   CANCEL exit")
local lines, sock = {}, nil

local function add(text)
  table.insert(lines, 1, text)
  lines[9] = nil
  list:items(lines)
end

local function connect()
  sock = ws.open("wss://chat.example.com/rooms/lobby", function(event, data)
    if event == "open" then
      add("Connected")
    elseif event == "message" then
      local m = json.decode(data)
      if m then add(m.from .. ": " .. m.text) end
    else
      add("Closed: " .. data)
      timer.after(5000, connect)               -- and again in 5 s
    end
  end)
end
connect()

function on_key(k)
  if k ~= "select" then return false end
  -- The server learns the player from the ticket, so the message only says what happened.
  local ok, err = sock:send({text = "wave"})
  if not ok then ui.toast(err) end
  return true
end
```

![Triton One](img/triton-on-dark.svg#gh-dark-mode-only)
![Triton One](img/triton-on-light.svg#gh-light-mode-only)

### `rpc` (permission `rpc`, Solana OS 0.5.2)

Solana's JSON-RPC API on mainnet and devnet, so apps can read the chain and send
transactions. **The badges' RPC is sponsored by [Triton One](https://triton.one)**: their
nodes answer every call. The badge server holds the node addresses and makes each call
for the app, as with `http`, so it needs Wi-Fi like every server call, works the same in
the emulator, and needs no `hosts`.

> **Never create or store a keypair on the badge.** A badge is not a wallet. It has no secure storage: the microSD card comes out and reads on any computer, an app's files and `storage` are plain files that the console lists and copies, and badges get lent, lost and passed around. A secret key in a script, a bundled file, `storage`, a message or a QR code is as good as published, and so is everything it holds. Sign transactions where keys are kept safe (a wallet, or your own server with its own key) and give the badge only the signed transaction to send.

```lua
rpc.call(method, params, cb)                        -- mainnet
rpc.call(method, params, {cluster = "devnet"}, cb)  -- "mainnet" (the default) or "devnet"
-- cb(true, result) or cb(false, "offline")
rpc.call("getBalance", {"Gh9ZwEmdLJ8DscKNTkTqPbNwLNNBjuSzaG9Vp2KGtKJr"}, function(ok, r)
  -- r = {context = {slot = 371234567}, value = "5000000000"}
end)
```

- `method`: one of the methods below, spelled as Solana spells it.
- `params`: a table, sent as the call's params array (`{address, {commitment =
  "confirmed"}}`), or nil for none. ≤ 2 KB as JSON.
- `result` is the call's `result`, decoded: a table, string, number, boolean, or nil
  (JSON's null, e.g. `getAccountInfo`'s `value` for an account that doesn't exist, or
  `getTransaction` for a signature the node doesn't know).

**Big numbers come back as strings.** A badge's Lua has 32-bit integers, and
`json.decode` would turn a bigger whole number into a float good to about 7 digits:
1.5 SOL is 1,500,000,000 lamports, but 5 SOL doesn't fit. So the badge server turns
every whole number beyond ±2,147,483,647 into its exact decimal string. `getBalance`
of an account with 5 SOL answers `value = "5000000000"`, with 1 SOL `value =
1000000000` (a number). Slots and block heights still fit and stay numbers. Fractions
stay numbers (32-bit floats): for token amounts use `uiAmountString`. `tostring()`
gives the digits either way, so string functions turn lamports into SOL exactly:

```lua
local function sol(lamports)                 -- "5000000000" or 1500000000 -> "5" or "1.5"
  local s = string.rep("0", 10) .. tostring(lamports)
  local whole = s:sub(1, -10):gsub("^0+", "")
  local frac = s:sub(-9):gsub("0+$", "")
  return (whole == "" and "0" or whole) .. (frac ~= "" and "." .. frac or "")
end
```

**Methods.** Reads, and `sendTransaction`:

| | |
|---|---|
| Accounts | `getAccountInfo`, `getBalance`, `getMultipleAccounts` (≤ 5 addresses), `getMinimumBalanceForRentExemption` |
| Tokens | `getTokenAccountBalance`, `getTokenAccountsByOwner`, `getTokenLargestAccounts`, `getTokenSupply` |
| Transactions | `getSignaturesForAddress` (`limit` ≤ 10, and 10 if you don't set one), `getSignatureStatuses` (≤ 10), `getTransaction` (`maxSupportedTransactionVersion` is 0 unless you set it), `getFeeForMessage`, `simulateTransaction`, `sendTransaction` |
| The chain | `getSlot`, `getBlockHeight`, `getBlockTime`, `getEpochInfo`, `getLatestBlockhash`, `isBlockhashValid`, `getRecentPrioritizationFees`, `getHealth`, `getVersion`, `getGenesisHash` |

Anything else fails with `"method not allowed"`: the rest either can't answer in a
badge-sized reply (`getBlock`, `getProgramAccounts`, `getLargestAccounts`) or isn't
for apps (`requestAirdrop`: use a faucet).

**Answers** are ≤ about 3.5 KB. A bigger one fails with `"too big"` instead of arriving
cut, because half a JSON answer is of no use. So ask for what fits: `dataSlice =
{offset = 0, length = 64}` with `encoding = "base64"` for an account's data,
`encoding = "jsonParsed"` for token accounts, a smaller `limit`.

**Sending transactions.** `sendTransaction` takes one signed, serialized transaction
as base64 (≤ 1,232 bytes, so ≤ 1,644 characters) and answers its signature, a string.
The badge server sends it with `encoding = "base64"` (it refuses any other encoding);
the rest of the config (`skipPreflight`, `preflightCommitment`, `maxRetries`,
`minContextSlot`) goes as you set it. `simulateTransaction` takes the same and answers
what the node would do (`err`, `logs`, `unitsConsumed`), which can be over 3.5 KB with
many logs. The signatures must already be in the transaction: build and sign it where
the key lives (your server, through `http`, or a wallet), never on the badge (see the
warning above). A player's phone wallet can sign what the badge builds: see [Build on
the badge, sign on a phone](#build-on-the-badge-sign-on-a-phone). A sent transaction isn't confirmed yet: check it with
`getSignatureStatuses`. A transaction the node refuses fails with the node's error and,
in brackets, why: `"rpc -32002: Transaction simulation failed (AccountNotFound)"`, or
`(BlockhashNotFound)`, or `({"InstructionError":[0,{"Custom":1}]})` for a program's error.

`ok` is `false` with `"offline"`, `"not linked"`, `"denied"` (the app lacks the
permission, or isn't in the store: publish before you test, as for `http`), `"busy"`,
`"socket open"`, `"too fast"` (over 30 calls a minute, or 6 `sendTransaction`s), `"rate
limited"` (the RPC node is limiting the badge server: wait a few seconds and try again),
`"timeout"` (no answer within 6 s), `"unavailable"` (this badge server has no node for
that cluster, or the node didn't answer), `"method not allowed"`, `"bad params"`,
`"request too big"`, `"too big"`, or the node's own error as `"rpc <code>:
<message> (<why>)"`, cut to about 120 characters. A method that isn't a string, params that
aren't a table and an unknown cluster are Lua errors.

```lua
-- Watch a wallet: its SOL, and its latest transactions.
local ADDR = "Gh9ZwEmdLJ8DscKNTkTqPbNwLNNBjuSzaG9Vp2KGtKJr"
local bal = ui.label(0, 8, "...", {size=20, w=ui.W, align="center"})
local list = ui.list(0, 44, ui.W, ui.H - 60, {"Loading..."}, {})
ui.hint("SELECT refresh   CANCEL exit")

local function refresh()
  rpc.call("getBalance", {ADDR}, function(ok, r)
    bal:text(ok and sol(r.value) .. " SOL" or r)          -- sol() from above
  end)
  rpc.call("getSignaturesForAddress", {ADDR, {limit = 5}}, function(ok, r)
    if not ok then list:items({r}) return end
    local rows = {}
    for _, s in ipairs(r) do
      rows[#rows + 1] = {title = s.signature:sub(1, 12) .. "...", right = s.err and "failed" or "ok"}
    end
    list:items(#rows > 0 and rows or {"No transactions"})
  end)
end
refresh()

function on_key(k)
  if k == "select" then refresh() return true end
  return false
end
```

### Build on the badge, sign on a phone

A player's phone wallet can sign and send a transaction the badge builds. Phantom and
Solflare scan [Solana Pay](https://github.com/solana-foundation/pay/blob/main/typescript/packages/solana-pay/spec/SPEC.md)
*transaction requests*: a QR code holding a link to a server (`solana:https://…`),
which hands the wallet a transaction for its account. The player sees what it does,
approves it, and the wallet signs and sends it. The transaction itself doesn't fit in
the QR code, so your server sits in between:

1. The app builds the transaction's message on the badge. It leaves a placeholder
   where the wallet's address goes (it can't know which wallet will scan) and puts a
   random *reference* address among the accounts.
2. It posts the message to your server with `http`. The server keeps it and answers
   with a `solana:` link.
3. The app shows the link with `ui.qr`, and the player scans it in their wallet app.
4. The wallet asks your server for the transaction, naming its account. The server
   writes that account over the placeholder and a fresh blockhash into the empty slot,
   and hands the transaction over.
5. The wallet signs and sends it. Nothing comes back to the badge: it finds the
   transaction on chain by its reference (`getSignaturesForAddress`).

No key is on the badge or on your server: the only signature is the wallet's.

`lib/solana.lua`, addresses and messages (copy it into your app):

```lua
-- lib/solana.lua: Solana addresses and transaction messages, built on the badge.
local M = {}
local B58 = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz"

function M.decode(s)                 -- base58 address -> its 32 bytes
  local b = {}                       -- little-endian while it builds
  for i = 1, #s do
    local carry = B58:find(s:sub(i, i), 1, true) - 1
    for j = 1, #b do
      carry = carry + b[j] * 58
      b[j] = carry & 0xFF
      carry = carry >> 8
    end
    while carry > 0 do b[#b + 1] = carry & 0xFF; carry = carry >> 8 end
  end
  for i = 1, #s do                   -- each leading "1" is a zero byte
    if s:sub(i, i) ~= "1" then break end
    b[#b + 1] = 0
  end
  local out = {}
  for i = #b, 1, -1 do out[#out + 1] = string.char(b[i]) end
  return table.concat(out)
end

function M.encode(bytes)             -- 32 bytes -> base58
  local d = {}
  for i = 1, #bytes do
    local carry = bytes:byte(i)
    for j = 1, #d do
      carry = carry + d[j] * 256
      d[j] = carry % 58
      carry = carry // 58
    end
    while carry > 0 do d[#d + 1] = carry % 58; carry = carry // 58 end
  end
  local out = {}
  for i = 1, #bytes do
    if bytes:byte(i) ~= 0 then break end
    out[#out + 1] = "1"
  end
  for i = #d, 1, -1 do out[#out + 1] = B58:sub(d[i] + 1, d[i] + 1) end
  return table.concat(out)
end

function M.hex(s) return ("%02x"):rep(#s):format(s:byte(1, -1)) end

-- 32 random bytes: an address to find a transaction by, never a key pair.
function M.random_address()
  local b = {}
  for i = 1, 32 do b[i] = math.random(0, 255) end
  return string.char(table.unpack(b))
end

-- Solana's compact length: 7 bits a byte, low bits first.
local function len(n)
  local s = ""
  repeat
    local b = n & 0x7F
    n = n >> 7
    s = s .. string.char(n > 0 and b | 0x80 or b)
  until n == 0
  return s
end

M.SYSTEM = ("\0"):rep(32)            -- 11111111111111111111111111111111
M.MEMO = M.decode("MemoSq4gqABAXKb96qnH8TysNcWxMyWCqXgDLGmfcHr")
M.WALLET = ("\1"):rep(32)            -- stands in for the wallet; the server fills it in

-- Send SOL from the wallet. Extra addresses ride along read-only (a reference).
function M.transfer(to, lamports, ...)
  local accounts = {{M.WALLET, w = true}, {to, w = true}}
  for _, a in ipairs({...}) do accounts[#accounts + 1] = {a} end
  return {program = M.SYSTEM, accounts = accounts, data = string.pack("<I4i8", 2, lamports)}
end

function M.memo(text) return {program = M.MEMO, accounts = {}, data = text} end

-- A legacy message whose only signer, and fee payer, is the wallet. Instructions
-- are {program = address, accounts = {{address, w = true}, {address}}, data = bytes},
-- with w on the accounts it changes. The blockhash is left for the server.
function M.message(instructions)
  local keys, at, w = {M.WALLET}, {[M.WALLET] = 0}, {}
  local function add(k)
    if not at[k] then keys[#keys + 1] = k; at[k] = #keys - 1 end
  end
  for _, ix in ipairs(instructions) do
    for _, a in ipairs(ix.accounts) do if a.w then w[a[1]] = true; add(a[1]) end end
  end
  local writable = #keys
  for _, ix in ipairs(instructions) do
    for _, a in ipairs(ix.accounts) do add(a[1]) end
    add(ix.program)
  end
  local out = {string.char(1, 0, #keys - writable), len(#keys), table.concat(keys), ("\0"):rep(32),
               len(#instructions)}
  for _, ix in ipairs(instructions) do
    local idx = {}
    for i, a in ipairs(ix.accounts) do idx[i] = string.char(at[a[1]]) end
    out[#out + 1] = string.char(at[ix.program]) .. len(#idx) .. table.concat(idx) .. len(#ix.data) .. ix.data
  end
  return table.concat(out)
end

return M
```

`main.lua`:

```lua
-- Pay 0.01 SOL on devnet from the player's phone wallet. The badge builds the
-- transaction, your server hands it to the wallet, the wallet signs and sends it.
-- app.json: "permissions": ["http", "rpc"], "hosts": ["pay.example.com"]
local sol = require("lib.solana")
local SERVER = "https://pay.example.com"
local TO = sol.decode("Gh9ZwEmdLJ8DscKNTkTqPbNwLNNBjuSzaG9Vp2KGtKJr")

local info = ui.label(198, 16, "Preparing...", {w = 114})
ui.hint("CANCEL exit")
if not ui.qr then info:text("Needs Solana OS 0.5.3") return end

local ref = sol.random_address()            -- the payment's reference: the badge finds it by this
local REF = sol.encode(ref)
local msg = sol.message({
  sol.transfer(TO, 10000000, ref),          -- 0.01 SOL; at most 2147483647 lamports (32-bit integers)
  sol.memo("Paid from badge " .. (badge.id() or "?")),
})
local qr, poll

local function stop(text)
  timer.cancel(poll)
  poll = nil
  info:text(text)
end

local function watch()                      -- every 3 s: has the reference shown up on chain?
  if sys.millis() > 300000 then             -- the server forgets the transaction after 5 minutes
    return stop("Timed out. Open the app again to retry.")
  end
  rpc.call("getSignaturesForAddress", {REF, {limit = 1, commitment = "confirmed"}},
    {cluster = "devnet"}, function(ok, r)
      if not (ok and poll and r[1]) then return end      -- not yet: ask again next round
      stop(r[1].err and "The payment failed." or "Paid! Scan to see it on the explorer.")
      qr:text("https://explorer.solana.com/tx/" .. r[1].signature .. "?cluster=devnet")
    end)
end

http.post(SERVER .. "/tx", {message = sol.hex(msg)}, function(ok, res)
  local link = ok and res.status == 200 and (json.decode(res.body) or {}).url
  if not link then
    info:text("The server said no: " .. (ok and res.status or res))
    return
  end
  qr = ui.qr(8, 11, 180, link)
  info:text("Pay 0.01 SOL: scan this in Phantom or Solflare.")
  poll = timer.every(3000, watch)
end)
```

Your server (Bun, `bun add bs58`), at `pay.example.com` in this example:

```ts
// pay.example.com: keeps each badge's transaction for 5 minutes and hands it to
// the wallet that scans it, as a Solana Pay transaction request. Bun; bun add bs58.
import bs58 from 'bs58';
import { fromBadge } from './badge'; // the check from "What your server receives" (http)

const APP = 'phone-pay';
const ORIGIN = 'https://pay.example.com';
const RPC = 'https://api.devnet.solana.com';
const PROGRAMS = new Set(['11111111111111111111111111111111', 'MemoSq4gqABAXKb96qnH8TysNcWxMyWCqXgDLGmfcHr']);
const pending = new Map<string, { message: Uint8Array; badge: string; at: number }>();

/** A legacy message with the wallet as its one signer, calling only PROGRAMS; else null. */
function read(m: Uint8Array) {
  let i = 0;
  const byte = () => { if (i >= m.length) throw 0; return m[i++]; };
  const len = () => { // compact length
    let n = 0;
    for (let s = 0; s < 21; s += 7) { const b = byte(); n |= (b & 0x7f) << s; if (b < 0x80) return n; }
    throw 0;
  };
  const skip = () => { const n = len(); i += n; };
  try {
    if (byte() !== 1 || byte() !== 0) return null; // 1 signer, writable (a v0 message starts with 0x80)
    byte();
    const keys: Uint8Array[] = [];
    for (let n = len(); n > 0; n--, i += 32) keys.push(m.subarray(i, i + 32));
    const blockhash = i;
    i += 32;
    for (let n = len(); n > 0; n--) {
      if (!PROGRAMS.has(bs58.encode(keys[byte()]))) return null;
      skip(); // account indexes
      skip(); // data
    }
    return i === m.length && keys.every((k) => k.length === 32) ? { keys, blockhash } : null;
  } catch {
    return null;
  }
}

async function latestBlockhash() {
  const res = await fetch(RPC, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ jsonrpc: '2.0', id: 1, method: 'getLatestBlockhash', params: [{ commitment: 'confirmed' }] }),
  });
  return bs58.decode((await res.json()).result.value.blockhash);
}

Bun.serve({
  routes: {
    // The badge (through the badge server's http): keep its message, answer the link to show.
    '/tx': {
      POST: async (req) => {
        const body = await req.text();
        const from = await fromBadge(req, body, APP);
        if (!from) return new Response('who are you?', { status: 403 });
        let hex = '';
        try {
          hex = String(JSON.parse(body).message);
        } catch {}
        const message = Uint8Array.from(Buffer.from(hex, 'hex'));
        if (message.length > 1100 || message.length * 2 !== hex.length || !read(message)) {
          return Response.json({ error: 'not a transaction this server serves' }, { status: 400 });
        }
        for (const [id, p] of pending) if (Date.now() - p.at > 300_000) pending.delete(id);
        const id = Buffer.from(crypto.getRandomValues(new Uint8Array(9))).toString('base64url');
        pending.set(id, { message, badge: from.badge, at: Date.now() });
        return Response.json({ url: `solana:${ORIGIN}/tx/${id}` });
      },
    },
    // The wallet (Solana Pay): first who's asking, then the transaction for its account.
    '/tx/:id': {
      GET: (req) =>
        pending.has(req.params.id)
          ? Response.json({ label: 'Badge payment', icon: `${ORIGIN}/icon.png` })
          : Response.json({ error: 'expired' }, { status: 404 }),
      POST: async (req) => {
        const p = pending.get(req.params.id);
        if (!p) return Response.json({ error: 'expired' }, { status: 404 });
        let wallet: Uint8Array;
        try {
          wallet = bs58.decode(String((await req.json()).account));
        } catch {
          wallet = new Uint8Array();
        }
        const m = p.message.slice(); // a copy: the wallet may ask again
        const { keys, blockhash } = read(m)!;
        if (wallet.length !== 32 || keys.some((k) => Buffer.compare(k, wallet) === 0)) {
          return Response.json({ error: 'this wallet cannot pay this' }, { status: 400 });
        }
        keys[0].set(wallet); // the wallet signs and pays the fee
        m.set(await latestBlockhash(), blockhash);
        const tx = new Uint8Array(1 + 64 + m.length); // 1 signature, still empty, then the message
        tx[0] = 1;
        tx.set(m, 65);
        return Response.json({ transaction: Buffer.from(tx).toString('base64'), message: `From badge ${p.badge}` });
      },
    },
  },
});
```

- **Your server signs nothing, but it answers in your name.** The wallet shows the
  player your domain, the label and its own simulation of what the transaction does.
  Anyone can run their own script under your app's id and post any message, so serve
  only what you mean to: here, `read()` lets through only a legacy message whose one
  signer is the wallet and which calls only the System and Memo programs. The
  `X-Badge-*` headers tell you which badge posted it.
- **Devnet first.** The wallet sends the transaction on the network it's set to, so
  switch it to devnet before you scan (Phantom: testnet mode in its developer settings;
  Solflare: its network setting). The app's `rpc` cluster and the server's `RPC` must
  match it.
- **Scan in the wallet app**, with its own scan button. A phone's camera app may not
  open `solana:` links, or may open them in whichever wallet was installed first.
- **Memory:** while it waits, the example holds about 25 KB of Lua heap. Compiled on
  the badge (Run on badge) it peaks at 36 KB of the 48 KB; installed from the store it
  runs from flash and needs less.
- The message travels as hex, which keeps the Lua short: messages up to about 1,000
  bytes fit in `http`'s 2 KB. A transfer is limited to 2,147,483,647 lamports, because
  the badge's integers are 32-bit.
- **A payment alone needs no server.** A Solana Pay *transfer request* in the QR code,
  `solana:<recipient>?amount=0.01&reference=<address>&label=Badge`, has the wallet
  build the transfer itself. Watch for the reference in the same way.

## Example

```lua
-- Tap counter that pays 1 !LAMP per 100 taps (needs an award cap from an admin).
local taps = storage.get("taps") or 0
local title = ui.label(0, 60, "Taps", {size=20, w=ui.W, align="center"})
local count = ui.label(0, 100, tostring(taps), {size=32, w=ui.W, align="center", color="green"})
ui.hint("SELECT tap   CANCEL exit")

function on_key(k)
  if k ~= "select" then return false end
  taps = taps + 1
  count:text(tostring(taps))
  led.fill(0x14, 0xF1, 0x95)
  timer.after(80, led.off)
  if taps % 100 == 0 then
    lamps.award(1, "100 taps", function(ok, r)
      if ok and r.awarded > 0 then ui.toast("+1 !LAMP")
      elseif ok and r.needsX then ui.toast("Link X to earn !LAMPs") end
    end)
  end
  return true
end

function on_exit() storage.set("taps", taps) end
```

And one with files: flashcards, one deck per file, loaded when you pick it.

```lua
-- decks/*.txt: "question|answer" per line. lib/cards.lua: function M.parse(line) -> q, a
local cards = require("lib.cards")
local names = {}
for _, f in ipairs(fs.list("decks")) do
  if f:sub(-4) == ".txt" then names[#names + 1] = f:sub(1, -5) end
end
local card = ui.label(10, 80, "", {size=20, w=300, align="center"})
local deck, i, flipped
local menu                                      -- declared first: its callback uses it

menu = ui.list(0, 0, ui.W, ui.H - 16, names, {on_select=function(_, name)
  deck, i, flipped = {}, 1, false
  for line in fs.lines("decks/" .. name .. ".txt") do
    local q, a = cards.parse(line)
    if q then deck[#deck + 1] = {q, a} end
  end
  menu:show(false)
  card:text(deck[1] and deck[1][1] or "Empty deck")
  ui.hint("SELECT flip   RIGHT next   CANCEL decks")
end})
ui.hint("SELECT open   CANCEL exit")

function on_key(k)
  if not deck then return false end            -- on the deck list, CANCEL exits
  if k == "back" then
    deck = nil                                  -- the collector frees the cards
    card:text("")
    menu:show(true)
    ui.hint("SELECT open   CANCEL exit")
  elseif k == "select" and deck[i] then
    flipped = not flipped
    card:text(deck[i][flipped and 2 or 1])
  elseif k == "right" and deck[i + 1] then
    i, flipped = i + 1, false
    card:text(deck[i][1])
  end
  return true
end
```

## Develop and publish

- Write the app in the editor on `/apps/submit`, or drop a folder or .zip on it:
  `main.lua`, `app.json` if you have one (it fills in the form) and the files it
  needs. New text files can be made right there.
- **Try in emulator** runs it on an emulated badge in the page, with every file on the
  emulated card. `print()` and errors show below it.
- **Run on badge** copies it onto a real badge over USB (desktop Chrome or Edge, Web
  Serial), sending only the files that changed, starts it and shows the badge's
  console, `print()` included. The badge needs a microSD card and Solana OS 0.5.0 or
  later (it checks `info` first). It replaces an installed app with the same id.
- **Publish** puts it in the store, once every Lua file compiles with the badge's Lua
  (see "Compiled by the store") and an automatic check has read the code: it looks for
  apps that harm players (taking !LAMPs by deception, asking for passwords, sending
  data away, hiding what they do), usually within a minute. What it flags waits for an
  organiser; until then only you see the app, or the store keeps your previous version.
  Each update is a new version; the badge offers it as an update and downloads only
  the files that changed. An app that uses `http` or
  `ws` gets answers and sockets only once it's published (the store holds its hosts),
  so publish a first version early.

## Console (development)

`lua list`, `lua run <id>`, `lua rm <id> [file]` on the badge console (and in the
emulator via `emu_console`). The web submit page uses `lua run` to test an app in the
emulator. `lua mem` shows the last run's peak heap and instruction count, and whether
its code was the store's (run in place) or compiled on the badge; `lua
install <id>` installs or updates an app from the App Store, as the store app does.
For bridges: `bridge` lists them, `bridge show` opens the code, `bridge code` prints it,
`bridge <code or link>` bridges with the badge that shows it, `bridge end` closes them.

To copy an app over the serial console, `<file>` is `main.lua`, `app.json` or a
bundled path like `lib/quiz.lua`:

- `lua put <id> <file> begin|<base64>|end` writes a file a line at a time (at most 128
  base64 characters per line), easy to type or script.
- `lua recv <id> <file> <bytes> <sha256>` is the fast way, for tools. The badge answers
  `recv: ready <n>`: send the next `n` characters of the file's base64 (the rest, at the
  end) and wait for `recv: more <n>` before sending more, so the card is never written
  while data is arriving. It ends with `recv: ok <bytes>` once the size and SHA-256
  match, or `recv: error <why>` (5 s without data is a timeout), keeping the old file.
- `lua sums <id>` prints `<sha256> <bytes> <file>` for each file of an installed app,
  then `sums: <n> files` (0 if it isn't installed), so a tool can skip what's already
  there. Run on badge works this way.

These refuse while the app runs (`open home` closes it) and check `app.json` like an
install does. Writing or removing any file of an app deletes its compiled code from
the store (`code.bin`): from then on it runs from its source until the store installs
it again.
