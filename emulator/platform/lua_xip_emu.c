// Emulator counterpart of main/lua/lua_xip.c: there's no flash to map, so
// the compiled code is read into memory (checked against its SHA-256 like
// the badge does) and freed when the app stops. The wasm heap has room;
// what matters on the badge is that this memory is outside the Lua heap.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mbedtls/sha256.h"
#include "lua/lua_xip.h"

bool lua_xip_map(const char *path, const uint8_t sha[32], size_t size, lua_xip_t *out)
{
    memset(out, 0, sizeof(*out));
    out->slot = -1;
    FILE *f = fopen(path, "rb");
    // Aligned like the badge's flash slots: chunks start 16-aligned in the image.
    uint8_t *buf = f && size ? aligned_alloc(16, (size + 15) & ~(size_t)15) : NULL;
    bool ok = buf && fread(buf, 1, size, f) == size && fgetc(f) == EOF;
    if (f) {
        fclose(f);
    }
    uint8_t got[32];
    if (ok) {
        mbedtls_sha256_context c;
        mbedtls_sha256_init(&c);
        mbedtls_sha256_starts(&c, 0);
        mbedtls_sha256_update(&c, buf, size);
        mbedtls_sha256_finish(&c, got);
        mbedtls_sha256_free(&c);
        ok = !memcmp(got, sha, 32);
    }
    if (!ok) {
        free(buf);
        return false;
    }
    out->data = buf;
    out->size = size;
    out->copied = true;
    return true;
}

void lua_xip_unmap(lua_xip_t *x)
{
    free((void *)x->data);
    memset(x, 0, sizeof(*x));
    x->slot = -1;
}
