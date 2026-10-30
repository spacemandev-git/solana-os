-- Regression test (not shipped): ui.qr's limits, and that a method called on
-- another kind of object fails in Lua instead of reaching LVGL.
local function try(name, f)
  local ok, err = pcall(f)
  print(("t-qr %s: %s"):format(name, ok and "FAIL no error" or err))
end

local q = ui.qr(10, 9, 200, "solana:https://example.com/tx/abc123")
print("t-qr made, heap " .. collectgarbage("count"))
try("second", function() ui.qr(0, 0, 100, "x") end)
try("tiny", function() ui.qr(0, 0, 10, "x") end)
try("huge", function() ui.qr(0, 0, 300, "x") end)
try("long text", function() q:text(("x"):rep(257)) end)
q:text(("x"):rep(256))
print("t-qr 256 bytes ok")
q:remove()
try("too dense", function() ui.qr(0, 0, 25, ("x"):rep(100)) end)

local l = ui.label(220, 20, "label", {size = 12})
local b = ui.bar(220, 40, 90, 8, 10)
try("bar.set on label", function() b.set(l, 50) end)
try("label.text on bar", function() l.text(b, "x") end)

q = ui.qr(10, 9, 200, "solana:https://pay.example.com/tx/7K2Q9M-1a2b3c4d")
l:text("t-qr done")
ui.hint("SELECT exits")
print("t-qr done")
function on_key(k) if k == "select" then sys.exit() end return true end
