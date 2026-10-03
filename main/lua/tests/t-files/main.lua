-- Regression test (not shipped): fs and require over bundled files. Copy
-- files/ along with it (the bundled files: lua put, or the emulator's FS).
-- The chunk checks the API on load and prints "t-files ready"; each key runs
-- a case that must stop the app:
--   SELECT  reading in a loop runs out of budget ("Script took too long")
--   DOWN    a module that fails at run time shows its file and line
local function fails(f, ...)
  local ok, err = pcall(f, ...)
  assert(not ok, "expected an error")
  return tostring(err)
end

-- require: cached, nested, args, loops, missing and broken modules, names
local u = require("lib.util")
assert(require("lib.util") == u and loads == 1, "require caches")
assert(u.args[1] == "lib.util" and u.args[2] == "lib/util.lua", "module args")
assert(require("lib.nested").two == 2)
assert(require("lib.nothing") == true)
assert(fails(require, "lib.missing"):find("not found"), "missing module")
assert(fails(require, "lib.loop"):find("requires itself"))
assert(fails(require, "lib.loop"):find("requires itself"), "a failed module loads again")
assert(fails(require, "lib.bad"):find("lib/bad.lua:1:"))
assert(fails(require, "Lib.Util"):find("lowercase"))
assert(fails(require, "lib/util"):find("lowercase"))
assert(fails(require, "..x"))

-- exists, size, list
assert(fs.exists("data") and fs.exists("data/ok.json") and not fs.exists("nope"))
assert(fs.exists() and fs.exists("") and fs.exists("data/"))
assert(fs.size("data/big.txt") == 20000 and fs.size("data") == nil and fs.size("nope") == nil)
assert(fs.size("empty.txt") == 0)
assert(table.concat(fs.list(), ",") == "data/,empty.txt,lib/,scenes/", table.concat(fs.list(), ","))
assert(#fs.list("nope") == 0)
assert(table.concat(fs.list("lib/"), ",") == "bad.lua,boom.lua,loop.lua,nested.lua,nothing.lua,util.lua")

-- read: whole, ranges, past the end, limits, bad paths, switching files
assert(fs.read("empty.txt") == "")
assert(fs.read("data/lines.txt") == "one\r\ntwo\n\nfour")
assert(fs.read("data/lines.txt", 5, 3) == "two")
assert(fs.read("data/lines.txt", 100, 5) == "")
assert(#fs.read("data/big.txt", 16000, 16384) == 4000)
local r, e = fs.read("nope.txt")
assert(r == nil and e == "not found")
assert(fails(fs.read, "data/big.txt"):find("in pieces"))
assert(fails(fs.read, "data/big.txt", 0, 16385):find("16 KB"))
assert(fails(fs.read, "../main.lua"))
assert(fails(fs.read, "/data/ok.json"))
assert(fails(fs.read, "Data/ok.json"))
assert(fails(fs.read, "data/lines.txt", -1, 2))
assert(fs.read("data/ok.json", 0, 1) == "{" and fs.read("data/lines.txt", 0, 3) == "one"
       and fs.read("data/ok.json", 1, 1) == '"')

-- lines: \r\n, empty lines, no final newline, long lines in pieces, two at once
local got = {}
for l in fs.lines("data/lines.txt") do got[#got + 1] = l end
assert(table.concat(got, "|") == "one|two||four", table.concat(got, "|"))
got = {}
for l in fs.lines("data/long.txt") do got[#got + 1] = #l end
assert(table.concat(got, ",") == "512,512,176,3", table.concat(got, ","))
got = {}
for l in fs.lines("data/edge.txt") do got[#got + 1] = #l end
assert(table.concat(got, ",") == "512,512,1,1", "a full line's \\r\\n, then a long line: " .. table.concat(got, ","))
for _ in fs.lines("empty.txt") do error("an empty file has no lines") end
assert(fails(fs.lines, "nope.txt"):find("no file"))
local a, b = fs.lines("data/lines.txt"), fs.lines("data/long.txt")
assert(a() == "one" and #b() == 512 and a() == "two")

-- json
local j = fs.json("data/ok.json")
assert(j.n == 3 and #j.list == 3 and j.s == "hi")
local v, err = fs.json("data/bad.json")
assert(v == nil and err:find("data/bad.json"))
assert(select(2, fs.json("nope.json")) == "not found")

-- load: compiled each time, not cached; errors come back
local f = fs.load("scenes/one.lua")
assert(type(f) == "function" and f().id == 1 and fs.load("scenes/one.lua") ~= f, "fs.load doesn't cache")
local g, gerr = fs.load("lib/bad.lua")
assert(g == nil and gerr:find("lib/bad.lua:1:"))
assert(select(2, fs.load("nope.lua")) == "not found")

-- Finalizers that read files while a module compiles or a read allocates
-- must not disturb either (compiling has its own file; reads allocate first).
local hits = 0
local mt = {__gc = function() hits = hits + 1; fs.read("data/ok.json", 0, 4) end}
for i = 1, 10 do
  for k = 1, 40 do setmetatable({}, mt) end
  local big = assert(fs.load("scenes/big.lua"))
  assert(big().total == 40)
  big = nil
  assert(#fs.read("data/big.txt", 100, 8000) == 8000)
end
print("t-files finalizers ran", hits)
print("t-files ready", collectgarbage("count") // 1, "KB")

function on_key(k)
  if k == "select" then
    while true do fs.read("data/big.txt", 0, 16000) end
  elseif k == "down" then
    require("lib.boom")
    print("t-files FAIL lib.boom loaded")
  end
  return k ~= "back"
end
