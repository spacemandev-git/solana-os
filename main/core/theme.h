#pragma once

// Solana brand palette (solana.com/brand, web/static/brand/Color Palettes).
#define COL_PURPLE      0x9945FF    // Solana Purple
#define COL_GREEN       0x14F195    // Solana Green
#define COL_PINK        0xEB54BC
#define COL_LPURPLE     0xB6B0FF
#define COL_BLUE        0x64A8F2
#define COL_AQUA        0x80ECFF
#define COL_LIME        0xACFF9B
#define COL_YELLOW      0xE8F180
#define COL_ORANGE      0xFF754A

// UI surfaces on the black brand background
#define COL_BG          0x000000
#define COL_PANEL       0x111116
#define COL_PANEL_HI    0x1C1B26
#define COL_BORDER      0x2A2935
#define COL_TEXT        0xFFFFFF
#define COL_DIM         0x8E8C9E

#define COL_OK          COL_GREEN
#define COL_WARN        COL_YELLOW
#define COL_ERR         COL_ORANGE

// Screen layout: status bar on top, app root below.
#define OS_STATUS_H     22
#define OS_ROOT_W       BOARD_LCD_H_RES
#define OS_ROOT_H       (BOARD_LCD_V_RES - OS_STATUS_H)

// WS2812s are blinding at full power; nothing should exceed this.
#define OS_LED_MAX      24
