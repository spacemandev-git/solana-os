-- Regression test (not shipped): label text is capped per label and in
-- total, because LVGL copies it into the badge's own heap.
local long = ("x"):rep(5000)
local l = ui.label(4, 4, long, {size = 12, w = 300})
print("t-text first label ok")
local ok, err = pcall(function()
  for i = 1, 63 do ui.label(4, 4, long, {size = 12}) end
end)
print(ok and "t-text FAIL no total cap" or ("t-text total cap: " .. err))
ui.clear()
ui.label(4, 40, "cleared, SELECT exits", {size = 14})
function on_key(k) if k == "select" then sys.exit() end return true end
