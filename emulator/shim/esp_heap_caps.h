#pragma once

// Heap figures are the real WebAssembly heap (dlmalloc + unused linear
// memory), not the badge's SRAM.

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#define MALLOC_CAP_DEFAULT  (1 << 12)
#define MALLOC_CAP_8BIT     (1 << 2)
#define MALLOC_CAP_INTERNAL (1 << 11)
#define MALLOC_CAP_DMA      (1 << 3)
#define MALLOC_CAP_SPIRAM   (1 << 10)
#define MALLOC_CAP_32BIT    (1 << 1)

size_t heap_caps_get_free_size(uint32_t caps);
size_t heap_caps_get_minimum_free_size(uint32_t caps);
size_t heap_caps_get_largest_free_block(uint32_t caps);
size_t heap_caps_get_total_size(uint32_t caps);
#define heap_caps_malloc(n, caps)       malloc(n)
#define heap_caps_calloc(n, s, caps)    calloc(n, s)
#define heap_caps_realloc(p, n, caps)   realloc(p, n)
#define heap_caps_free(p)               free(p)
#define esp_get_free_heap_size()        ((uint32_t)heap_caps_get_free_size(MALLOC_CAP_DEFAULT))
#define esp_get_minimum_free_heap_size() ((uint32_t)heap_caps_get_minimum_free_size(MALLOC_CAP_DEFAULT))
