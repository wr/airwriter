// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

static int g_checks = 0;
static int g_failed = 0;

#define CHECK(cond) do { g_checks++; if (!(cond)) { g_failed++; \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

#define CHECK_EQ_INT(a, b) do { g_checks++; long _a = (long)(a), _b = (long)(b); \
    if (_a != _b) { g_failed++; \
    fprintf(stderr, "FAIL %s:%d: %s == %s (got %ld, want %ld)\n", \
            __FILE__, __LINE__, #a, #b, _a, _b); } } while (0)

#define CHECK_EQ_MEM(a, b, n) do { g_checks++; \
    if (memcmp((a), (b), (n)) != 0) { g_failed++; \
    fprintf(stderr, "FAIL %s:%d: %s != %s over %zu bytes\n", \
            __FILE__, __LINE__, #a, #b, (size_t)(n)); \
    for (size_t _i = 0; _i < (size_t)(n); _i++) \
        fprintf(stderr, "   [%zu] got %02X want %02X%s\n", _i, \
                ((const uint8_t *)(a))[_i], ((const uint8_t *)(b))[_i], \
                ((const uint8_t *)(a))[_i] != ((const uint8_t *)(b))[_i] ? "  <--" : ""); \
    } } while (0)

#define TEST_END do { \
    if (g_failed) { fprintf(stderr, "%d/%d checks FAILED\n", g_failed, g_checks); return 1; } \
    printf("ok  %s  (%d checks)\n", __FILE__, g_checks); return 0; } while (0)

/* A byte source over a fixed buffer, shaped like urf_read_fn. */
typedef struct { const uint8_t *buf; size_t len; size_t pos; } memsrc_t;

static inline int memsrc_read(void *ctx, uint8_t *out, size_t n)
{
    memsrc_t *m = (memsrc_t *)ctx;
    if (m->pos + n > m->len) return 0;
    memcpy(out, m->buf + m->pos, n);
    m->pos += n;
    return 1;
}
