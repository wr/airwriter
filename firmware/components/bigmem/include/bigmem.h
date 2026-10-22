// SPDX-License-Identifier: GPL-3.0-or-later
// Buffers a rip walks row by row, in PSRAM on the board: internal RAM is what
// Wi-Fi and lwIP buffer in, and a 300 dpi page's rows took nearly all of it
// (W-1082 bench, docs/traps.md). Internal RAM only when PSRAM is full; plain
// malloc on a host. free() frees them all.
#pragma once
#include <stdlib.h>

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#define BIGMEM_CAPS (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
static inline void *big_malloc(size_t n)
{
    void *p = heap_caps_malloc(n, BIGMEM_CAPS);
    return p ? p : malloc(n);
}
static inline void *big_calloc(size_t n, size_t size)
{
    void *p = heap_caps_calloc(n, size, BIGMEM_CAPS);
    return p ? p : calloc(n, size);
}
static inline void *big_realloc(void *old, size_t n)
{
    void *p = heap_caps_realloc(old, n, BIGMEM_CAPS);
    return p ? p : realloc(old, n);
}
#else
static inline void *big_malloc(size_t n) { return malloc(n); }
static inline void *big_calloc(size_t n, size_t size) { return calloc(n, size); }
static inline void *big_realloc(void *old, size_t n) { return realloc(old, n); }
#endif
