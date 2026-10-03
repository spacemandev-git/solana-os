loads = (loads or 0) + 1
return { add = function(x, y) return x + y end, args = {...} }
