#pragma once

// Solana OS data on the microSD card: /sdcard/hh/ (docs/HHGTS.md "Badge storage").
//
// Small files only through these helpers. Reads and writes up to ~16 KB are
// fine from app callbacks (a few ms on SPI). Anything bigger (downloads, wipes,
// directory walks) belongs in a worker task.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "bsp.h"

#define OS_DATA_DIR     BSP_SD_MOUNT "/hh"

bool os_data_ready(void);                   // card mounted and OS_DATA_DIR usable

// Path of `rel` (e.g. "apps/snake/main.lua") under OS_DATA_DIR.
void os_data_path(char *out, size_t n, const char *rel);

// mkdir -p of the parent directories of rel.
bool os_data_mkdirs(const char *rel);

// Whole-file helpers. read returns a malloc'd NUL-terminated buffer (free it)
// or NULL if missing or longer than max. write replaces atomically (tmp + rename).
char *os_data_read(const char *rel, size_t max, size_t *len_out);
bool os_data_write(const char *rel, const char *data, size_t len);
bool os_data_append(const char *rel, const char *line);    // adds '\n'
bool os_data_remove(const char *rel);                       // file or directory tree

// Delete everything under OS_DATA_DIR (factory reset, owner change). Blocking.
void os_data_wipe(void);

// Bumped by every os_data_wipe(). Services that keep SD data in RAM (X Dex
// cards, quest state) compare it to drop their copy after an owner change.
uint32_t os_data_epoch(void);
