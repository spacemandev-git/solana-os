#!/bin/sh
# Regenerate firmware/main/assets from the brand pack in web/static/brand.
set -e
cd "$(dirname "$0")/.."
B=../web/static/brand
F=$B/Fonts
font() { # file size name fallback
    bunx --bun lv_font_conv@1.5.3 --no-compress --no-prefilter --bpp 4 --size "$2" --font "$1" \
        --range 0x20-0x7E,0xB0,0x2022 --format lvgl --lv-include lvgl.h ${4:+--lv-fallback $4} \
        --force-fast-kern-format -o "main/assets/$3.c"
}
font "$F/ABC Diatype Semi-Mono/ABCDiatypeSemi-Mono-Regular.otf" 12 sol_mono_12 lv_font_montserrat_12
font "$F/ABC Diatype/ABCDiatype-Regular.otf" 14 sol_font_14 lv_font_montserrat_14
font "$F/ABC Diatype/ABCDiatype-Medium.otf" 20 sol_font_20 lv_font_montserrat_20
font "$F/ABC Diatype/ABCDiatype-Bold.otf" 32 sol_font_32
M="$B/Logos/Solana Logomark/2x"
python3 tools/img2lvgl.py "$M/Size=18, Color=color@2x.png" img_logomark_16 main/assets/img_logomark_16.c 16
python3 tools/img2lvgl.py "$M/Size=36, Color=color@2x.png" img_logomark_40 main/assets/img_logomark_40.c 40
python3 tools/img2lvgl.py "$M/Size=72, Color=color@2x.png" img_logomark_72 main/assets/img_logomark_72.c 72
python3 tools/img2lvgl.py "$B/Logos/Solana Foundation Logotype/2x/Size=18, Color=white@2x.png" img_foundation main/assets/img_foundation.c 20
python3 tools/img2lvgl.py "$B/Logos/Solana Foundation Logotype/2x/Size=18, Color=black@2x.png" img_foundation_black main/assets/img_foundation_black.c 20
