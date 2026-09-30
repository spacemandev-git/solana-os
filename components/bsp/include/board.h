#pragma once

// ESP32-C5 badge pin map.
// Recovered from the stock "c5_badge_hw_test" firmware (logs + disassembly) and
// confirmed by probing the board. See ../README.md for how each pin was verified.

#include "driver/spi_common.h"
#include "driver/i2c_types.h"

// Shared SPI bus (LCD + microSD). The SD card must be switched into SPI mode
// before any LCD traffic runs on this bus - see bsp_sd.c.
#define BOARD_SPI_HOST          SPI2_HOST
#define BOARD_SPI_SCK           9
#define BOARD_SPI_MOSI          10
#define BOARD_SPI_MISO          8

// ILI9341 320x240 TFT (landscape)
#define BOARD_LCD_CS            6
#define BOARD_LCD_DC            7
#define BOARD_LCD_RST           0       // as used by stock firmware
#define BOARD_LCD_BL            3       // active high, external pull-down
#define BOARD_LCD_H_RES         320
#define BOARD_LCD_V_RES         240
#define BOARD_LCD_PCLK_HZ       (20 * 1000 * 1000)

// microSD (SPI mode, no card-detect or write-protect lines)
#define BOARD_SD_CS             4

// I2C: TCA9534 I/O expander, P0..P5 = PB1..PB6 (active low), P6/P7 unconnected
#define BOARD_I2C_PORT          I2C_NUM_0
#define BOARD_I2C_SCL           23
#define BOARD_I2C_SDA           24
#define BOARD_TCA9534_ADDR      0x20
#define BOARD_TCA9534_INT       25      // open-drain, active low
#define BOARD_BTN_COUNT         6

// BOOT button (strapping pin, external pull-up, active low)
#define BOARD_BOOT_BTN          28

// 8x WS2812B RGB LEDs
#define BOARD_RGB_GPIO          27
#define BOARD_RGB_COUNT         8

// Battery sense: ADC1 on GPIO1 through a 1:1 divider
#define BOARD_BAT_ADC_GPIO      1
#define BOARD_BAT_DIVIDER       2

// UART0 console via CH340C
#define BOARD_UART_TX           11
#define BOARD_UART_RX           12
