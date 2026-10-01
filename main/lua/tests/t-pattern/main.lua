-- Regression test (not shipped): pattern matching is charged to the
-- instruction budget. Each key runs a backtracking pattern that would take
-- seconds to minutes in C; it must stop with "Script took too long".
-- Normal patterns still work: the chunk checks a few on load.
assert(("hello world"):find("o w") == 5)
assert(("key=value"):match("(%w+)=(%w+)") == "key")
assert(select(2, ("a,b,,c"):gsub(",", ";")) == 3)
local n = 0
for w in ("one two three"):gmatch("%a+") do n = n + 1 end
assert(n == 3)
print("t-pattern ready")
function on_key(k)
  if k == "select" then
    print("t-pattern FAIL find returned", string.find(("a"):rep(16), ("a-"):rep(16) .. "b"))
  elseif k == "up" then
    print("t-pattern FAIL gsub returned", (("a"):rep(24)):gsub(("a-"):rep(24) .. "b", "x"))
  elseif k == "down" then
    -- pcall must not keep a runaway match alive either
    for i = 1, 100 do pcall(string.match, ("a"):rep(20), ("a-"):rep(20) .. "b") end
    print("t-pattern FAIL pcall loop finished")
  elseif k == "right" then
    for m in (("a"):rep(30)):gmatch(("a-"):rep(12) .. "b") do end
    print("t-pattern FAIL gmatch finished")
  end
  return k ~= "back"
end
