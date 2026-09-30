#pragma once

// The badge's FAT VFS never lists "." and ".."; Emscripten's MEMFS does.
// Wrap readdir so firmware sees FAT behaviour on the emulated SD card.

#include_next <dirent.h>

struct dirent *emu_readdir(DIR *d);
#define readdir emu_readdir
