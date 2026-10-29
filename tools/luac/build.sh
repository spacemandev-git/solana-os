#!/usr/bin/env bash
# The store's Lua compiler (luac.c): this firmware's Lua as one self-contained
# ES module (the wasm inlined), for a server running bun. Rebuild it with
# every firmware build that changes main/lua/vendor, so the store's chunks
# keep matching the badges' Lua (tools/build_web.sh does).
#   firmware/tools/luac/build.sh <out.js>      (needs: brew install emscripten)
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
out="${1:?usage: build.sh <out.js>}"
vendor="$here/../../main/lua/vendor"

# The core and lauxlib: compiling needs no standard library.
srcs=()
for f in lapi lauxlib lcode lctype ldebug ldo ldump lfunc lgc llex lmem lobject lopcodes lparser lstate \
         lstring ltable ltm lundump lvm lzio; do
    srcs+=("$vendor/$f.c")
done

mkdir -p "$(dirname "$out")"
emcc -O2 -I"$vendor" "${srcs[@]}" "$here/luac.c" -o "$out" \
    -sMODULARIZE=1 -sEXPORT_ES6=1 -sEXPORT_NAME=createLuac -sSINGLE_FILE=1 -sENVIRONMENT=node \
    -sALLOW_MEMORY_GROWTH=1 -sEXIT_RUNTIME=0 \
    -sEXPORTED_FUNCTIONS=_luac_label,_luac_compile,_luac_out,_luac_error,_malloc,_free \
    -sEXPORTED_RUNTIME_METHODS=HEAPU8,UTF8ToString
echo "luac: $out ($(wc -c < "$out" | tr -d ' ') bytes)"
