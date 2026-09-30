#!/usr/bin/env bash
# Build Solana OS once for everything the site ships, from the same tree:
#   1. the badge image (idf.py build), published for the /stock flasher (web/static/firmware/)
#   2. the browser emulator (web/static/emulator/)
# Commit both outputs together so the flasher and the emulator always run the same firmware.
#   bun run firmware
set -euo pipefail

root="$(cd "$(dirname "$0")/../.." && pwd)"
export IDF_TOOLS_PATH="${IDF_TOOLS_PATH:-$HOME/.espressif}"
if ! command -v idf.py >/dev/null; then
    # shellcheck disable=SC1090
    source "${IDF_EXPORT:-$IDF_TOOLS_PATH/v5.5.4/esp-idf/export.sh}" >/dev/null
fi

(cd "$root/firmware" && idf.py build)
bun "$root/firmware/tools/export_web.ts"
"$root/firmware/emulator/build.sh"
