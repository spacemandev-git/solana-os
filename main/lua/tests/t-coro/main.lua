-- Regression test (not shipped): the instruction budget must hold inside
-- coroutines. Each key starts a runaway loop that must end in
-- "Script took too long" (the error screen) within a fraction of a second.
-- SELECT: coroutine.wrap; UP: resumed in a loop that swallows the error;
-- DOWN: 5,000,000 iterations inside a coroutine, counted.
print("t-coro ready")
function on_key(k)
  if k == "select" then
    coroutine.wrap(function() while true do end end)()
  elseif k == "up" then
    while true do
      coroutine.resume(coroutine.create(function() while true do end end))
    end
  elseif k == "down" then
    local co = coroutine.create(function() local n = 0 for i = 1, 5000000 do n = n + i end return n end)
    print("t-coro FAIL ran to the end", coroutine.resume(co))
  end
  return k ~= "back"
end
