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

// Guide page categories and rarities (docs/HHGTS.md; web: --cat-*, --rarity-*).
#define COL_CAT_EVENTS      COL_PINK
#define COL_CAT_CULTURE     COL_ORANGE
#define COL_CAT_TECH        COL_BLUE
#define COL_CAT_RELEASES    COL_GREEN
#define COL_CAT_ECOSYSTEM   COL_AQUA
#define COL_CAT_HISTORY     COL_YELLOW
#define COL_CAT_PEOPLE      COL_LPURPLE
#define COL_CAT_LORE        COL_LIME
#define COL_RARITY_COMMON   COL_DIM
#define COL_RARITY_RARE     COL_BLUE
#define COL_RARITY_EPIC     COL_PURPLE
#define COL_RARITY_LEGEND   COL_GREEN    // plus the gradient bar / purple->green LED sweep
#define COL_LAMPS           COL_GREEN    // !LAMPs amounts

// Screen layout: status bar on top, app root below.
#define OS_STATUS_H     22
#define OS_ROOT_W       BOARD_LCD_H_RES
#define OS_ROOT_H       (BOARD_LCD_V_RES - OS_STATUS_H)

// WS2812s are blinding at full power; nothing should exceed this.
#define OS_LED_MAX      24
