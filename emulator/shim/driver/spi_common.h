#pragma once

// Only what board.h needs: the emulator has no SPI bus.
typedef enum { SPI1_HOST, SPI2_HOST, SPI3_HOST } spi_host_device_t;
