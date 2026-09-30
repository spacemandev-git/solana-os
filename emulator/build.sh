#!/usr/bin/env bash
# Build the Solana OS browser emulator into web/static/emulator/.
#   firmware/emulator/build.sh            # release build (-Os)
#   BUILD_DIR=/tmp/emu firmware/emulator/build.sh
# Needs Emscripten (brew install emscripten) and a configured badge project
# (firmware/sdkconfig + managed_components, from any idf.py build).
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
build="${BUILD_DIR:-$here/build}"
out="$(cd "$here/../.." && pwd)/web/static/emulator"

command -v emcmake >/dev/null || { echo "emcmake not found: brew install emscripten" >&2; exit 1; }
mkdir -p "$out"
emcmake cmake -S "$here" -B "$build" -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=MinSizeRel -DEMU_OUT_DIR="$out" >/dev/null
cmake --build "$build" -j "$(sysctl -n hw.ncpu 2>/dev/null || nproc)"
ls -l "$out"/solana-os.js "$out"/solana-os.wasm
