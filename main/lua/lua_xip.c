// Store apps' compiled code, run in place from the internal flash
// (lua_xip.h, docs/HHGTS.md "Badge storage").
//
// The "storage" partition (partitions.csv, ~7.9 MB, used raw: no file
// system) holds the code of the last few apps run, in SLOT_SIZE slots: a
// header sector, then the image (apps/<id>/code.bin). Opening an app maps
// its slot when the header has the same SHA-256; otherwise the file is
// copied into the oldest slot first: the slot is erased (header included),
// the image written, checked through the mapping, and only then the header
// written, so a power cut or a bad write leaves a slot that never matches.
// Relaunching an app writes nothing; a new one erases ~50 ms per 4 KB of
// its code. The slots take turns, which spreads the wear.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include "esp_log.h"
#include "esp_partition.h"
#include "mbedtls/sha256.h"
#include "lua_xip.h"

static const char *TAG = "lua_xip";

#define SLOT_SIZE   (1024 * 1024)   // image + header; code.bin is at most LUA_PKG_CODE_MAX
#define SLOTS_MAX   8
#define HDR_SIZE    4096            // a sector, so the image starts aligned
#define SLOT_MAGIC  0x584C4F53u     // "SOLX"
#define COPY_CHUNK  2048

typedef struct {
    uint32_t magic;
    uint32_t seq;                   // higher: written later
    uint32_t size;
    uint8_t sha[32];
} slot_hdr_t;

static const esp_partition_t *code_part(void)
{
    static const esp_partition_t *p;
    if (!p) {
        p = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_FAT, "storage");
    }
    return p;
}

static void sha256(const uint8_t *data, size_t n, uint8_t out[32])
{
    mbedtls_sha256_context c;
    mbedtls_sha256_init(&c);
    mbedtls_sha256_starts(&c, 0);
    mbedtls_sha256_update(&c, data, n);
    mbedtls_sha256_finish(&c, out);
    mbedtls_sha256_free(&c);
}

// The file into slot `off`'s image area, erased first along with the header.
static bool copy_in(const esp_partition_t *p, size_t off, const char *path, size_t size)
{
    if (esp_partition_erase_range(p, off, HDR_SIZE + ((size + 4095) & ~(size_t)4095)) != ESP_OK) {
        return false;
    }
    FILE *f = fopen(path, "rb");
    uint8_t *buf = f ? malloc(COPY_CHUNK) : NULL;
    bool ok = buf != NULL;
    for (size_t done = 0; ok && done < size;) {
        size_t n = fread(buf, 1, MIN((size_t)COPY_CHUNK, size - done), f);
        ok = n > 0 && esp_partition_write(p, off + HDR_SIZE + done, buf, n) == ESP_OK;
        done += n;
    }
    ok = ok && fgetc(f) == EOF;
    free(buf);
    if (f) {
        fclose(f);
    }
    return ok;
}

bool lua_xip_map(const char *path, const uint8_t sha[32], size_t size, lua_xip_t *out)
{
    memset(out, 0, sizeof(*out));
    out->slot = -1;
    const esp_partition_t *p = code_part();
    if (!p || !size || size > SLOT_SIZE - HDR_SIZE) {
        return false;
    }
    int slots = MIN((int)(p->size / SLOT_SIZE), SLOTS_MAX), use = -1, oldest = 0;
    uint32_t top = 0, low = UINT32_MAX;
    for (int i = 0; i < slots; i++) {
        slot_hdr_t h;
        if (esp_partition_read(p, (size_t)i * SLOT_SIZE, &h, sizeof(h)) != ESP_OK) {
            return false;
        }
        bool valid = h.magic == SLOT_MAGIC;
        if (valid && h.size == size && !memcmp(h.sha, sha, 32)) {
            use = i;
        }
        uint32_t seq = valid ? h.seq : 0;   // empty and broken slots go first
        top = MAX(top, seq);
        if (seq < low) {
            low = seq;
            oldest = i;
        }
    }
    bool copied = use < 0;
    if (copied) {
        use = oldest;
        if (!copy_in(p, (size_t)use * SLOT_SIZE, path, size)) {
            ESP_LOGW(TAG, "couldn't copy %s into slot %d", path, use);
            return false;
        }
    }
    const void *ptr;
    esp_partition_mmap_handle_t handle;
    if (esp_partition_mmap(p, (size_t)use * SLOT_SIZE + HDR_SIZE, size, ESP_PARTITION_MMAP_DATA, &ptr, &handle)
            != ESP_OK) {
        ESP_LOGW(TAG, "couldn't map slot %d", use);
        return false;
    }
    if (copied) {
        // Check what reached the flash (the SD read and the write both),
        // then mark the slot good.
        uint8_t got[32];
        sha256(ptr, size, got);
        slot_hdr_t h = { .magic = SLOT_MAGIC, .seq = top + 1, .size = (uint32_t)size };
        memcpy(h.sha, sha, 32);
        if (memcmp(got, sha, 32) || esp_partition_write(p, (size_t)use * SLOT_SIZE, &h, sizeof(h)) != ESP_OK) {
            ESP_LOGW(TAG, "%s: slot %d doesn't match the file's SHA-256", path, use);
            esp_partition_munmap(handle);
            return false;
        }
    }
    out->data = ptr;
    out->size = size;
    out->handle = handle;
    out->slot = use;
    out->copied = copied;
    return true;
}

void lua_xip_unmap(lua_xip_t *x)
{
    if (x->data) {
        esp_partition_munmap((esp_partition_mmap_handle_t)x->handle);
    }
    memset(x, 0, sizeof(*x));
    x->slot = -1;
}
