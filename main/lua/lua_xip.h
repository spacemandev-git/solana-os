#pragma once

// Store apps' compiled code, run in place (docs/HHGTS.md "Badge storage").
// The host maps an app's code.bin for as long as it runs; Lua reads the
// instructions, line info and long strings straight from the mapping
// (vendor/lundump.c fixed buffers), so they cost no RAM.
//
// Badge: lua_xip.c copies the file into the internal flash's code slots
// (unless it's there already) and maps it through the flash cache.
// Emulator: platform/lua_xip_emu.c reads it into memory.
//
// Both block (SD reads, flash erases): call them from the app's own task.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    const uint8_t *data;    // NULL: not mapped
    size_t size;
    uint32_t handle;        // badge: esp_partition_mmap's handle
    int slot;               // badge: which code slot, -1 in the emulator
    bool copied;            // the file was copied in this time (not already there)
} lua_xip_t;

// Maps the file at path (a full path), which must be size bytes with this
// SHA-256. false (and nothing mapped) if it isn't, or there's no room.
bool lua_xip_map(const char *path, const uint8_t sha[32], size_t size, lua_xip_t *out);
void lua_xip_unmap(lua_xip_t *x);
