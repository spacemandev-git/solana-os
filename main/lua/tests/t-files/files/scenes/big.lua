-- Many small functions: compiling it allocates all along.
local t = { total = 0 }
function t.f0(x) local s = "step 0" return x + 0, s end
function t.f1(x) local s = "step 1" return x + 1, s end
function t.f2(x) local s = "step 2" return x + 2, s end
function t.f3(x) local s = "step 3" return x + 3, s end
function t.f4(x) local s = "step 4" return x + 4, s end
function t.f5(x) local s = "step 5" return x + 5, s end
function t.f6(x) local s = "step 6" return x + 6, s end
function t.f7(x) local s = "step 7" return x + 7, s end
function t.f8(x) local s = "step 8" return x + 8, s end
function t.f9(x) local s = "step 9" return x + 9, s end
function t.f10(x) local s = "step 10" return x + 10, s end
function t.f11(x) local s = "step 11" return x + 11, s end
function t.f12(x) local s = "step 12" return x + 12, s end
function t.f13(x) local s = "step 13" return x + 13, s end
function t.f14(x) local s = "step 14" return x + 14, s end
function t.f15(x) local s = "step 15" return x + 15, s end
function t.f16(x) local s = "step 16" return x + 16, s end
function t.f17(x) local s = "step 17" return x + 17, s end
function t.f18(x) local s = "step 18" return x + 18, s end
function t.f19(x) local s = "step 19" return x + 19, s end
function t.f20(x) local s = "step 20" return x + 20, s end
function t.f21(x) local s = "step 21" return x + 21, s end
function t.f22(x) local s = "step 22" return x + 22, s end
function t.f23(x) local s = "step 23" return x + 23, s end
function t.f24(x) local s = "step 24" return x + 24, s end
function t.f25(x) local s = "step 25" return x + 25, s end
function t.f26(x) local s = "step 26" return x + 26, s end
function t.f27(x) local s = "step 27" return x + 27, s end
function t.f28(x) local s = "step 28" return x + 28, s end
function t.f29(x) local s = "step 29" return x + 29, s end
function t.f30(x) local s = "step 30" return x + 30, s end
function t.f31(x) local s = "step 31" return x + 31, s end
function t.f32(x) local s = "step 32" return x + 32, s end
function t.f33(x) local s = "step 33" return x + 33, s end
function t.f34(x) local s = "step 34" return x + 34, s end
function t.f35(x) local s = "step 35" return x + 35, s end
function t.f36(x) local s = "step 36" return x + 36, s end
function t.f37(x) local s = "step 37" return x + 37, s end
function t.f38(x) local s = "step 38" return x + 38, s end
function t.f39(x) local s = "step 39" return x + 39, s end
for i = 0, 39 do t.total = t.total + (t['f' .. i](0) == i and 1 or 0) end
return t
