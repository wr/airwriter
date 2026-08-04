# AirWriter M2 — host raster pipeline (`rip`)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a host binary that turns a captured URF or PWG raster file into the exact byte stream an ImageWriter II expects, plus a PNG preview, so the whole raster pipeline can be developed and regression-tested with no ESP32 and no printer.

**Architecture:** Four small C modules — decode, resample, halftone, output — each streaming one row at a time so nothing ever holds a page in RAM. The modules live under `firmware/components/` in ESP-IDF component layout and are compiled for the host by a Makefile in `tools/rip/`. Same source, two builds, no duplication. The host binary adds a file reader and a PNG writer that never ship to the device.

**Tech Stack:** C11, Apple clang, GNU make. No third-party libraries. Python 3 for the fixture-capture tool only.

**Status:** every code block in Tasks 1–8 was extracted, compiled under `-Wall -Wextra -Werror`
and run before this plan was committed. All 179 checks pass, `rip` builds, and the synthetic
fixture rips to a valid PNG and a 2673-byte stream that starts `1B 63 1B 70 1B 54 31 36` and
ends `0C`. Task 8 is the exception — it needs a phone and has not been run.

## Global Constraints

- **Language: C11.** Compile with `-std=c11 -Wall -Wextra -Werror`.
- **No dynamic allocation inside row loops.** Allocate in `*_init`, free in `*_free`. This code moves to an ESP32 with 2MB of PSRAM and a streaming HTTP body.
- **No third-party dependencies.** The PNG writer is written here, uncompressed, on purpose.
- **Licence: GPLv3.** Every new `.c` and `.h` gets the SPDX line `// SPDX-License-Identifier: GPL-3.0-or-later` as its first line.
- **Cite the source for anything reverse-engineered.** Format details carry a comment naming the file they came from, per `SPEC.md` §3.6.
- **Grayscale is luminance: 0 = black, 255 = white.** Ink is the inverse. Confirmed against `cups/raster-stream.c`.
- **Bit 0 of an ImageWriter graphics byte is the TOP pin.** Confirmed against the ImageWriter II Technical Reference p. 107.
- Target output: **144 dpi horizontal, 72 dpi vertical, 1152 dots wide.**

## File structure

| Path | Responsibility |
|---|---|
| `firmware/components/urf_decode/include/urf_decode.h` | Public decoder API |
| `firmware/components/urf_decode/urf_decode.c` | URF + PWG headers, PackBits-variant line RLE, row streaming |
| `firmware/components/resample/include/resample.h` | Public resampler API |
| `firmware/components/resample/resample.c` | Source raster grid → printer dot grid, box filter |
| `firmware/components/halftone/include/halftone.h` | Public halftone API |
| `firmware/components/halftone/halftone.c` | Threshold, Atkinson, Floyd–Steinberg, Bayer 8×8 |
| `firmware/components/imagewriter/include/imagewriter.h` | Public output-stage API |
| `firmware/components/imagewriter/imagewriter.c` | 8-row transpose, escape-code emission |
| `tools/rip/Makefile` | Host build and test runner |
| `tools/rip/main.c` | CLI, file I/O, wiring |
| `tools/rip/png_write.c`, `tools/rip/png_write.h` | Minimal uncompressed PNG writer, host only |
| `tools/rip/test/test_util.h` | Assert macros and a memory byte-source |
| `tools/rip/test/test_*.c` | One test binary per module |
| `tools/ippcap/ippcap.py` | Stub IPP server that captures a real job from an iPhone |
| `fixtures/` | Captured URF samples and their expected `stream.bin` |

Each module knows nothing about the others. `main.c` is the only file that includes more than one.

---

### Task 1: Build scaffold and test harness

**Files:**
- Create: `tools/rip/Makefile`
- Create: `tools/rip/test/test_util.h`
- Create: `tools/rip/test/test_smoke.c`

**Interfaces:**
- Consumes: nothing.
- Produces: `make -C tools/rip test` builds and runs every `tools/rip/test/test_*.c` as its own binary. `CHECK`, `CHECK_EQ_INT`, `CHECK_EQ_MEM`, `TEST_END` macros. `memsrc_t` / `memsrc_read` as a `urf_read_fn`-shaped byte source.

- [ ] **Step 1: Write the test harness header**

Create `tools/rip/test/test_util.h`:

```c
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
```

- [ ] **Step 2: Write the smoke test**

Create `tools/rip/test/test_smoke.c`:

```c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "test_util.h"

int main(void)
{
    const uint8_t data[] = { 0xDE, 0xAD, 0xBE, 0xEF };
    memsrc_t src = { data, sizeof data, 0 };
    uint8_t got[2];

    CHECK_EQ_INT(memsrc_read(&src, got, 2), 1);
    CHECK_EQ_INT(got[0], 0xDE);
    CHECK_EQ_INT(src.pos, 2);
    CHECK_EQ_INT(memsrc_read(&src, got, 99), 0);   /* short read fails */
    TEST_END;
}
```

- [ ] **Step 3: Write the Makefile**

Create `tools/rip/Makefile`:

```make
# SPDX-License-Identifier: GPL-3.0-or-later
CC      ?= cc
CFLAGS  := -std=c11 -Wall -Wextra -Werror -O2 -g
COMP    := ../../firmware/components
INCS    := -I$(COMP)/urf_decode/include \
           -I$(COMP)/resample/include \
           -I$(COMP)/halftone/include \
           -I$(COMP)/imagewriter/include \
           -I.
SRCS    := $(wildcard $(COMP)/urf_decode/*.c) \
           $(wildcard $(COMP)/resample/*.c) \
           $(wildcard $(COMP)/halftone/*.c) \
           $(wildcard $(COMP)/imagewriter/*.c)
TESTSRC := $(wildcard test/test_*.c)
TESTBIN := $(patsubst test/%.c,build/%,$(TESTSRC))

.PHONY: all test clean
all: build/rip

build:
	@mkdir -p build

build/rip: main.c png_write.c $(SRCS) | build
	$(CC) $(CFLAGS) $(INCS) -Itest -o $@ $^

build/%: test/%.c $(SRCS) | build
	$(CC) $(CFLAGS) $(INCS) -Itest -o $@ $^

test: $(TESTBIN)
	@fail=0; for t in $(TESTBIN); do ./$$t || fail=1; done; exit $$fail

clean:
	rm -rf build
```

- [ ] **Step 4: Run the tests and watch them pass**

Run: `make -C tools/rip test`
Expected: `ok  test/test_smoke.c  (4 checks)` and exit status 0.

Note: `build/rip` will not link until Task 8. `make test` does not build it.

- [ ] **Step 5: Commit**

```bash
git add tools/rip/Makefile tools/rip/test/test_util.h tools/rip/test/test_smoke.c
git commit -m "Add host build and test harness for the raster pipeline"
```

---

### Task 2: URF and PWG header parsing

**Files:**
- Create: `firmware/components/urf_decode/include/urf_decode.h`
- Create: `firmware/components/urf_decode/urf_decode.c`
- Create: `tools/rip/test/test_urf_header.c`

**Interfaces:**
- Consumes: `memsrc_read` from Task 1.
- Produces:
  - `typedef int (*urf_read_fn)(void *ctx, uint8_t *buf, size_t n)` — returns 1 on a full read, 0 on short read or error.
  - `urf_page_t` with fields `width`, `height`, `res_x`, `res_y`, `bits_per_pixel`, `colorspace`, `num_colors`, `bits_per_color`, `bpp_bytes`, `bytes_per_line`, `fill_byte`.
  - `urf_t` opaque-ish context.
  - `int urf_open(urf_t *u, urf_read_fn read, void *ctx)`
  - `int urf_next_page(urf_t *u)` → `URF_OK`, `URF_END`, or negative error.
  - Return codes `URF_OK 0`, `URF_END 1`, `URF_ERR_IO -1`, `URF_ERR_MAGIC -2`, `URF_ERR_UNSUPPORTED -3`.
  - `void urf_free(urf_t *u)`

- [ ] **Step 1: Write the failing test**

Create `tools/rip/test/test_urf_header.c`:

```c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "test_util.h"
#include "urf_decode.h"

/* A 12-byte URF file header followed by one 32-byte page header.
 * Page: 8bpp, colorspace 0 (sGray), simplex, 1024 x 768, 144 dpi. */
static const uint8_t hdr[] = {
    'U','N','I','R','A','S','T', 0x00,
    0x00, 0x00, 0x00, 0x01,                 /* page count = 1, big endian */

    0x08,                                   /* [0]  bits per pixel */
    0x00,                                   /* [1]  colorspace 0 = sGray */
    0x00,                                   /* [2]  duplex */
    0x04,                                   /* [3]  quality = normal */
    0x00,                                   /* [4]  media type */
    0x00,                                   /* [5]  media position */
    0,0,0,0,0,0,                            /* [6..11] unknown */
    0x00, 0x00, 0x04, 0x00,                 /* [12] width  = 1024 */
    0x00, 0x00, 0x03, 0x00,                 /* [16] height = 768 */
    0x00, 0x00, 0x00, 0x90,                 /* [20] resolution = 144 */
    0,0,0,0,0,0,0,0                         /* [24..31] unknown */
};

int main(void)
{
    memsrc_t src = { hdr, sizeof hdr, 0 };
    urf_t u;

    CHECK_EQ_INT(urf_open(&u, memsrc_read, &src), URF_OK);
    CHECK_EQ_INT(u.pages_total, 1);

    CHECK_EQ_INT(urf_next_page(&u), URF_OK);
    CHECK_EQ_INT(u.page.width, 1024);
    CHECK_EQ_INT(u.page.height, 768);
    CHECK_EQ_INT(u.page.res_x, 144);
    CHECK_EQ_INT(u.page.res_y, 144);      /* URF has one resolution for both axes */
    CHECK_EQ_INT(u.page.bits_per_pixel, 8);
    CHECK_EQ_INT(u.page.num_colors, 1);
    CHECK_EQ_INT(u.page.bits_per_color, 8);
    CHECK_EQ_INT(u.page.bpp_bytes, 1);
    CHECK_EQ_INT(u.page.bytes_per_line, 1024);
    CHECK_EQ_INT(u.page.fill_byte, 0xFF); /* additive space: blank is white */

    urf_free(&u);

    /* A bad magic must be rejected, not guessed at. */
    uint8_t bad[sizeof hdr];
    memcpy(bad, hdr, sizeof hdr);
    bad[0] = 'X';
    memsrc_t bsrc = { bad, sizeof bad, 0 };
    urf_t bu;
    CHECK_EQ_INT(urf_open(&bu, memsrc_read, &bsrc), URF_ERR_MAGIC);

    TEST_END;
}
```

- [ ] **Step 2: Run it and verify it fails**

Run: `make -C tools/rip test`
Expected: compile error, `urf_decode.h` not found.

- [ ] **Step 3: Write the header**

Create `firmware/components/urf_decode/include/urf_decode.h`:

```c
// SPDX-License-Identifier: GPL-3.0-or-later
//
// URF (Apple Raster) and PWG Raster decoder.
//
// Binary layout and the line encoding were read from OpenPrinting/cups
// cups/raster-stream.c (_cupsRasterReadHeader, _cupsRasterReadPixels) and
// PWG 5102.4-2012 section 4.4. Do not "fix" the arithmetic from memory:
// the three +1 conventions in the line encoding are deliberate.
#pragma once
#include <stdint.h>
#include <stddef.h>

#define URF_OK               0
#define URF_END              1
#define URF_ERR_IO          -1
#define URF_ERR_MAGIC       -2
#define URF_ERR_UNSUPPORTED -3
#define URF_ERR_MALFORMED   -4   // a run overran its line; the stream is corrupt

// Returns 1 when all n bytes were read, 0 on short read or error.
typedef int (*urf_read_fn)(void *ctx, uint8_t *buf, size_t n);

typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t res_x;           // dpi; URF carries one value, copied to both
    uint32_t res_y;
    uint8_t  bits_per_pixel;
    uint8_t  colorspace;      // raw code from the file
    uint8_t  num_colors;
    uint8_t  bits_per_color;
    uint8_t  bpp_bytes;       // width of one colour value, the RLE run unit
    uint8_t  fill_byte;       // what control byte 0x80 fills with
    size_t   bytes_per_line;
} urf_page_t;

typedef struct {
    urf_read_fn read;
    void       *ctx;
    int         is_pwg;
    uint32_t    pages_total;  // 0 means unknown / streaming
    uint32_t    pages_seen;
    urf_page_t  page;
    uint8_t    *line;         // decoded row scratch, bytes_per_line
    size_t      line_cap;
    unsigned    repeat_left;  // rows still owed from the line-repeat byte
    uint32_t    rows_emitted;
} urf_t;

int  urf_open(urf_t *u, urf_read_fn read, void *ctx);
int  urf_next_page(urf_t *u);
int  urf_next_row(urf_t *u, const uint8_t **row);
void urf_free(urf_t *u);
```

- [ ] **Step 4: Write the implementation**

Create `firmware/components/urf_decode/urf_decode.c`. `urf_next_row` is a stub here and gets written in Task 4 — leave it returning `URF_ERR_UNSUPPORTED` so the file links.

```c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "urf_decode.h"
#include <stdlib.h>
#include <string.h>

// cups/raster-stream.c, cups_raster_read_header(): rawcspace[] / rawnumcolors[]
static uint8_t colorspace_num_colors(uint8_t code)
{
    static const uint8_t n[] = { 1, 3, 3, 3, 1, 3, 4 };
    return code < sizeof n ? n[code] : 1;
}

// cups/raster-stream.c, _cupsRasterReadPixels(): control byte 0x80 fills 0xFF
// for additive spaces (W, RGB, SW, SRGB, RGBW, AdobeRGB) and 0x00 otherwise.
// URF codes: 0 SW, 1 SRGB, 3 AdobeRGB, 4 W, 5 RGB are additive; 2 CIELab and
// 6 CMYK are not. Getting this backwards prints solid black pages.
static uint8_t colorspace_fill(uint8_t code)
{
    switch (code) {
    case 0: case 1: case 3: case 4: case 5: return 0xFF;
    default:                                return 0x00;
    }
}

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}

int urf_open(urf_t *u, urf_read_fn read, void *ctx)
{
    memset(u, 0, sizeof *u);
    u->read = read;
    u->ctx  = ctx;

    uint8_t h[12];
    if (!read(ctx, h, 12)) return URF_ERR_IO;

    if (memcmp(h, "UNIRAST\0", 8) == 0) {
        u->is_pwg      = 0;
        u->pages_total = be32(h + 8);
        return URF_OK;
    }
    if (memcmp(h, "RaS2", 4) == 0) {
        // PWG: the sync word is 4 bytes, so 8 of the 12 read belong to the
        // first page header. Task 3 handles PWG; reject it for now rather
        // than silently mis-parsing.
        u->is_pwg = 1;
        return URF_ERR_UNSUPPORTED;
    }
    return URF_ERR_MAGIC;
}

int urf_next_page(urf_t *u)
{
    if (u->pages_total && u->pages_seen >= u->pages_total) return URF_END;

    uint8_t h[32];
    if (!u->read(u->ctx, h, 32)) {
        // A streaming file with pages_total == 0 ends by running out.
        return u->pages_total ? URF_ERR_IO : URF_END;
    }

    urf_page_t *p = &u->page;
    p->bits_per_pixel = h[0];
    p->colorspace     = h[1];
    p->width          = be32(h + 12);
    p->height         = be32(h + 16);
    p->res_x          = be32(h + 20);
    p->res_y          = p->res_x;          // URF carries a single scalar

    if (p->bits_per_pixel == 0 || p->width == 0 || p->height == 0)
        return URF_ERR_UNSUPPORTED;

    p->num_colors     = colorspace_num_colors(p->colorspace);
    p->bits_per_color = p->bits_per_pixel / p->num_colors;
    p->fill_byte      = colorspace_fill(p->colorspace);
    p->bpp_bytes      = (uint8_t)((p->bits_per_pixel + 7) / 8);
    if (p->bpp_bytes == 0) p->bpp_bytes = 1;

    // A malformed width must not wrap the multiply on a 32-bit target,
    // where size_t is 32 bits. 1152 dots is the printer's real limit;
    // 65535 is a generous ceiling that still cannot overflow.
    if (p->width > 65535 || p->height > 65535) return URF_ERR_UNSUPPORTED;

    // URF has no sub-byte depths, so this cannot lose a partial byte.
    p->bytes_per_line = (size_t)p->width * p->bits_per_pixel / 8;

    if (p->bytes_per_line > u->line_cap) {
        uint8_t *nl = realloc(u->line, p->bytes_per_line);
        if (!nl) return URF_ERR_IO;
        u->line     = nl;
        u->line_cap = p->bytes_per_line;
    }

    u->repeat_left  = 0;
    u->rows_emitted = 0;
    u->pages_seen++;
    return URF_OK;
}

int urf_next_row(urf_t *u, const uint8_t **row)
{
    (void)u; (void)row;
    return URF_ERR_UNSUPPORTED;   // Task 3
}

void urf_free(urf_t *u)
{
    free(u->line);
    u->line     = NULL;
    u->line_cap = 0;
}
```

- [ ] **Step 5: Run the tests and verify they pass**

Run: `make -C tools/rip test`
Expected: `ok  test/test_urf_header.c`.

- [ ] **Step 6: Commit**

```bash
git add firmware/components/urf_decode tools/rip/test/test_urf_header.c
git commit -m "Parse URF file and page headers

Offsets taken from cups/raster-stream.c rather than from memory, per
SPEC.md 3.6. URF carries one resolution scalar for both axes, which
matters for the ImageWriter's anisotropic 144x72 grid."
```

---
### Task 3: The line RLE decoder and row streaming

This is the part `SPEC.md` §3.6 warns about. Three separate `+1` conventions, and a control byte the PWG spec does not define.

**Files:**
- Modify: `firmware/components/urf_decode/urf_decode.c`
- Create: `tools/rip/test/test_urf_rle.c`

**Interfaces:**
- Consumes: `urf_t`, `urf_page_t` from Task 2.
- Produces: `static int decode_line(urf_t *u)` — internal, fills `u->line` with one fully decoded row. Not exported; Task 4 calls it.

- [ ] **Step 1: Write the failing test**

Create `tools/rip/test/test_urf_rle.c`. It drives the internal decoder through the public row API.

```c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "test_util.h"
#include "urf_decode.h"

/* Build a one-page URF around a caller-supplied raster body. */
static size_t build(uint8_t *out, uint32_t w, uint32_t h, uint8_t bpp,
                    uint8_t cspace, const uint8_t *body, size_t body_len)
{
    static const uint8_t magic[12] = { 'U','N','I','R','A','S','T',0, 0,0,0,1 };
    memcpy(out, magic, 12);
    uint8_t *p = out + 12;
    memset(p, 0, 32);
    p[0] = bpp; p[1] = cspace;
    p[12] = (uint8_t)(w >> 24); p[13] = (uint8_t)(w >> 16);
    p[14] = (uint8_t)(w >> 8);  p[15] = (uint8_t)w;
    p[16] = (uint8_t)(h >> 24); p[17] = (uint8_t)(h >> 16);
    p[18] = (uint8_t)(h >> 8);  p[19] = (uint8_t)h;
    p[23] = 144;
    memcpy(out + 44, body, body_len);
    return 44 + body_len;
}

int main(void)
{
    uint8_t file[512];
    const uint8_t *row;
    urf_t u;

    /* -- repeat run: L=0, control 0x07 means 7+1 = 8 copies of 0x80 -- */
    {
        const uint8_t body[] = { 0x00, 0x07, 0x80 };
        memsrc_t s = { file, build(file, 8, 1, 8, 0, body, sizeof body), 0 };
        CHECK_EQ_INT(urf_open(&u, memsrc_read, &s), URF_OK);
        CHECK_EQ_INT(urf_next_page(&u), URF_OK);
        CHECK_EQ_INT(urf_next_row(&u, &row), URF_OK);
        const uint8_t want[8] = { 0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80 };
        CHECK_EQ_MEM(row, want, 8);
        urf_free(&u);
    }

    /* -- literal run: control 0xFE means 257-254 = 3 verbatim values -- */
    {
        const uint8_t body[] = { 0x00, 0xFE, 0x11, 0x22, 0x33 };
        memsrc_t s = { file, build(file, 3, 1, 8, 0, body, sizeof body), 0 };
        CHECK_EQ_INT(urf_open(&u, memsrc_read, &s), URF_OK);
        CHECK_EQ_INT(urf_next_page(&u), URF_OK);
        CHECK_EQ_INT(urf_next_row(&u, &row), URF_OK);
        const uint8_t want[3] = { 0x11, 0x22, 0x33 };
        CHECK_EQ_MEM(row, want, 3);
        urf_free(&u);
    }

    /* -- control 0x80 clears to end of line; sGray is additive so 0xFF -- */
    {
        const uint8_t body[] = { 0x00, 0x00, 0x42, 0x80 };
        memsrc_t s = { file, build(file, 4, 1, 8, 0, body, sizeof body), 0 };
        CHECK_EQ_INT(urf_open(&u, memsrc_read, &s), URF_OK);
        CHECK_EQ_INT(urf_next_page(&u), URF_OK);
        CHECK_EQ_INT(urf_next_row(&u, &row), URF_OK);
        const uint8_t want[4] = { 0x42, 0xFF, 0xFF, 0xFF };
        CHECK_EQ_MEM(row, want, 4);
        urf_free(&u);
    }

    /* -- same, but CMYK (code 6) is subtractive, so the fill is 0x00 -- */
    {
        const uint8_t body[] = { 0x00, 0x80 };
        memsrc_t s = { file, build(file, 2, 1, 32, 6, body, sizeof body), 0 };
        CHECK_EQ_INT(urf_open(&u, memsrc_read, &s), URF_OK);
        CHECK_EQ_INT(urf_next_page(&u), URF_OK);
        CHECK_EQ_INT(u.page.bpp_bytes, 4);
        CHECK_EQ_INT(urf_next_row(&u, &row), URF_OK);
        const uint8_t want[8] = { 0,0,0,0, 0,0,0,0 };
        CHECK_EQ_MEM(row, want, 8);
        urf_free(&u);
    }

    /* -- runs count colour VALUES, not bytes: 24bpp repeat of 2 pixels -- */
    {
        const uint8_t body[] = { 0x00, 0x01, 0xAA, 0xBB, 0xCC };
        memsrc_t s = { file, build(file, 2, 1, 24, 1, body, sizeof body), 0 };
        CHECK_EQ_INT(urf_open(&u, memsrc_read, &s), URF_OK);
        CHECK_EQ_INT(urf_next_page(&u), URF_OK);
        CHECK_EQ_INT(u.page.bpp_bytes, 3);
        CHECK_EQ_INT(u.page.bytes_per_line, 6);
        CHECK_EQ_INT(urf_next_row(&u, &row), URF_OK);
        const uint8_t want[6] = { 0xAA,0xBB,0xCC, 0xAA,0xBB,0xCC };
        CHECK_EQ_MEM(row, want, 6);
        urf_free(&u);
    }

    /* -- line repeat byte: L=2 delivers the same row three times -- */
    {
        const uint8_t body[] = { 0x02, 0x01, 0x55 };
        memsrc_t s = { file, build(file, 2, 3, 8, 0, body, sizeof body), 0 };
        CHECK_EQ_INT(urf_open(&u, memsrc_read, &s), URF_OK);
        CHECK_EQ_INT(urf_next_page(&u), URF_OK);
        const uint8_t want[2] = { 0x55, 0x55 };
        for (int i = 0; i < 3; i++) {
            CHECK_EQ_INT(urf_next_row(&u, &row), URF_OK);
            CHECK_EQ_MEM(row, want, 2);
        }
        CHECK_EQ_INT(urf_next_row(&u, &row), URF_END);  /* height reached */
        urf_free(&u);
    }

    TEST_END;
}
```

- [ ] **Step 2: Run it and verify it fails**

Run: `make -C tools/rip test`
Expected: `test_urf_rle` fails every check with `URF_ERR_UNSUPPORTED` (-3) from the stub.

- [ ] **Step 3: Write the line decoder**

In `firmware/components/urf_decode/urf_decode.c`, add above `urf_next_row`:

```c
// Decode one line into u->line. Sets *repeat to the number of times the
// caller should deliver it (the leading byte plus one).
//
// Ported from _cupsRasterReadPixels() in cups/raster-stream.c and cross-read
// against PWG 5102.4-2012 section 4.4. Note that this is PackBits with the
// ranges swapped: low control bytes repeat, high control bytes are literal.
//
// One deliberate divergence from CUPS: where CUPS clamps an over-long run to
// the remaining line, we reject the page. CUPS clamps the read as well as the
// write, so the run's unread bytes become the next line's control bytes and
// every later line is garbage. A printer should fail the job instead.
static int decode_line(urf_t *u, unsigned *repeat)
{
    const urf_page_t *p = &u->page;
    uint8_t  ctl;
    uint8_t *dst   = u->line;
    ssize_t  bytes = (ssize_t)p->bytes_per_line;

    if (!u->read(u->ctx, &ctl, 1)) return URF_ERR_IO;
    *repeat = (unsigned)ctl + 1;          // 0 means once, never zero times

    while (bytes > 0) {
        if (!u->read(u->ctx, &ctl, 1)) return URF_ERR_IO;

        if (ctl == 0x80) {
            // Undefined in PWG 5102.4; both CUPS and urftopdf treat it as
            // clear-to-end-of-line. The fill depends on the colour space.
            memset(dst, p->fill_byte, (size_t)bytes);
            dst  += bytes;
            bytes = 0;
        } else if (ctl & 0x80) {
            // Literal: 257 - ctl colour values, so 2..128. A literal run of
            // one is not encodable.
            size_t n = (size_t)(257 - ctl) * p->bpp_bytes;
            // A run never legitimately crosses a line boundary. Clamping here
            // would leave the run's remaining bytes unread and desynchronise
            // every later line, so treat it as corrupt input instead.
            if (n > (size_t)bytes) return URF_ERR_MALFORMED;
            if (!u->read(u->ctx, dst, n)) return URF_ERR_IO;
            dst   += n;
            bytes -= (ssize_t)n;
        } else {
            // Repeat: ctl + 1 copies of the next colour value.
            size_t n = ((size_t)ctl + 1) * p->bpp_bytes;
            if (n > (size_t)bytes) return URF_ERR_MALFORMED;
            bytes -= (ssize_t)n;
            if (!u->read(u->ctx, dst, p->bpp_bytes)) return URF_ERR_IO;
            dst += p->bpp_bytes;
            n   -= p->bpp_bytes;
            while (n > 0) {
                memcpy(dst, dst - p->bpp_bytes, p->bpp_bytes);
                dst += p->bpp_bytes;
                n   -= p->bpp_bytes;
            }
        }
    }

    return URF_OK;
}
```

- [ ] **Step 4: Write the row streamer**

In `firmware/components/urf_decode/urf_decode.c`, replace the whole `urf_next_row` stub from Task 2 with:

```c
int urf_next_row(urf_t *u, const uint8_t **row)
{
    // Bound by the declared height. The line-repeat byte lets a file claim
    // more rows than the header says; CUPS bounds this and urftopdf does
    // not, which is how urftopdf writes past the end of a page buffer.
    if (u->rows_emitted >= u->page.height) return URF_END;

    if (u->repeat_left == 0) {
        int rc = decode_line(u, &u->repeat_left);
        if (rc != URF_OK) return rc;
    }

    u->repeat_left--;
    u->rows_emitted++;
    *row = u->line;
    return URF_OK;
}
```

- [ ] **Step 5: Run the tests and verify they pass**

Run: `make -C tools/rip test`
Expected: every test binary reports `ok`, exit 0. The suite must be green before you commit.

- [ ] **Step 6: Commit**

```bash
git add firmware/components/urf_decode/urf_decode.c tools/rip/test/test_urf_rle.c
git commit -m "Decode raster lines and stream rows

The line encoding is ported from _cupsRasterReadPixels in
cups/raster-stream.c. Low control bytes repeat and high ones are
literal, inverted from real PackBits, and 0x80 clears to end of line
with a fill that depends on whether the colour space is additive.

Row delivery is bounded by the declared page height, because the
line-repeat byte can claim more rows than the header says."
```

---

### Task 4: Resample onto the printer's dot grid

The ImageWriter's grid is anisotropic — 144 dpi across, 72 dpi down — and iOS may send any resolution. This module maps source rows onto target rows by box filtering, streaming, with no full-page buffer. `SPEC.md` does not name this module; it is added here because §3.3 requires a downsample path and there was nowhere for it to live.

**Files:**
- Create: `firmware/components/resample/include/resample.h`
- Create: `firmware/components/resample/resample.c`
- Create: `tools/rip/test/test_resample.c`

**Interfaces:**
- Consumes: nothing.
- Produces:
  - `typedef void (*rs_row_fn)(void *ctx, const uint8_t *row, uint32_t width)`
  - `int rs_init(rs_t *r, uint32_t src_w, uint32_t src_h, uint32_t dst_w, uint32_t dst_h, rs_row_fn emit, void *ctx)`
  - `int rs_push(rs_t *r, const uint8_t *src_row)` — feed one source row, may emit zero, one, or several destination rows.
  - `void rs_free(rs_t *r)`
  - Grayscale only, one byte per pixel.

- [ ] **Step 1: Write the failing test**

Create `tools/rip/test/test_resample.c`:

```c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "test_util.h"
#include "resample.h"

#define MAXROWS 16
#define MAXW    16

static uint8_t got[MAXROWS][MAXW];
static uint32_t got_n;

static void collect(void *ctx, const uint8_t *row, uint32_t w)
{
    (void)ctx;
    if (got_n < MAXROWS) memcpy(got[got_n], row, w);
    got_n++;
}

int main(void)
{
    /* -- 2:1 downscale in both axes, box averaged -- */
    {
        rs_t r;
        got_n = 0;
        CHECK_EQ_INT(rs_init(&r, 4, 4, 2, 2, collect, NULL), 0);

        const uint8_t a[4] = {   0,  20,  40,  60 };
        const uint8_t b[4] = { 100, 120, 140, 160 };
        CHECK_EQ_INT(rs_push(&r, a), 0);
        CHECK_EQ_INT(rs_push(&r, b), 0);
        CHECK_EQ_INT(rs_push(&r, a), 0);
        CHECK_EQ_INT(rs_push(&r, b), 0);

        CHECK_EQ_INT(got_n, 2);
        /* first dst row averages a and b over 2x2 blocks:
         * (0+20+100+120)/4 = 60,  (40+60+140+160)/4 = 100 */
        CHECK_EQ_INT(got[0][0], 60);
        CHECK_EQ_INT(got[0][1], 100);
        CHECK_EQ_INT(got[1][0], 60);
        CHECK_EQ_INT(got[1][1], 100);
        rs_free(&r);
    }

    /* -- 1:1 passes rows through untouched -- */
    {
        rs_t r;
        got_n = 0;
        CHECK_EQ_INT(rs_init(&r, 3, 2, 3, 2, collect, NULL), 0);
        const uint8_t a[3] = { 1, 2, 3 };
        const uint8_t b[3] = { 4, 5, 6 };
        CHECK_EQ_INT(rs_push(&r, a), 0);
        CHECK_EQ_INT(rs_push(&r, b), 0);
        CHECK_EQ_INT(got_n, 2);
        CHECK_EQ_MEM(got[0], a, 3);
        CHECK_EQ_MEM(got[1], b, 3);
        rs_free(&r);
    }

    /* -- upscale replicates: 2 source rows to 4 destination rows -- */
    {
        rs_t r;
        got_n = 0;
        CHECK_EQ_INT(rs_init(&r, 2, 2, 2, 4, collect, NULL), 0);
        const uint8_t a[2] = { 10, 20 };
        const uint8_t b[2] = { 30, 40 };
        CHECK_EQ_INT(rs_push(&r, a), 0);
        CHECK_EQ_INT(rs_push(&r, b), 0);
        CHECK_EQ_INT(got_n, 4);
        CHECK_EQ_INT(got[0][0], 10);
        CHECK_EQ_INT(got[1][0], 10);
        CHECK_EQ_INT(got[2][0], 30);
        CHECK_EQ_INT(got[3][0], 30);
        rs_free(&r);
    }

    TEST_END;
}
```

- [ ] **Step 2: Run it and verify it fails**

Run: `make -C tools/rip test`
Expected: compile error, `resample.h` not found.

- [ ] **Step 3: Write the header**

Create `firmware/components/resample/include/resample.h`:

```c
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Streaming grayscale resampler. Box filter when downscaling, replication
// when upscaling. Holds one destination row, never a page.
#pragma once
#include <stdint.h>
#include <stddef.h>

typedef void (*rs_row_fn)(void *ctx, const uint8_t *row, uint32_t width);

typedef struct {
    uint32_t  src_w, src_h, dst_w, dst_h;
    uint32_t *acc;         // dst_w wide accumulator
    uint32_t  acc_rows;    // source rows folded into acc
    uint32_t  src_y;       // source rows consumed
    uint32_t  dst_y;       // destination rows emitted
    uint8_t  *out;         // dst_w wide scratch
    rs_row_fn emit;
    void     *ctx;
} rs_t;

int  rs_init(rs_t *r, uint32_t src_w, uint32_t src_h,
             uint32_t dst_w, uint32_t dst_h, rs_row_fn emit, void *ctx);
int  rs_push(rs_t *r, const uint8_t *src_row);
void rs_free(rs_t *r);
```

- [ ] **Step 4: Write the implementation**

Create `firmware/components/resample/resample.c`:

```c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "resample.h"
#include <stdlib.h>
#include <string.h>

int rs_init(rs_t *r, uint32_t src_w, uint32_t src_h,
            uint32_t dst_w, uint32_t dst_h, rs_row_fn emit, void *ctx)
{
    memset(r, 0, sizeof *r);
    if (!src_w || !src_h || !dst_w || !dst_h) return -1;
    r->src_w = src_w; r->src_h = src_h;
    r->dst_w = dst_w; r->dst_h = dst_h;
    r->emit  = emit;  r->ctx   = ctx;
    r->acc = calloc(dst_w, sizeof *r->acc);
    r->out = calloc(dst_w, 1);
    if (!r->acc || !r->out) { rs_free(r); return -1; }
    return 0;
}

// Fold one source row into the accumulator, box averaging horizontally.
static void fold(rs_t *r, const uint8_t *src)
{
    for (uint32_t x = 0; x < r->dst_w; x++) {
        uint32_t x0 = (uint32_t)((uint64_t)x       * r->src_w / r->dst_w);
        uint32_t x1 = (uint32_t)((uint64_t)(x + 1) * r->src_w / r->dst_w);
        if (x1 <= x0) x1 = x0 + 1;            // upscaling: take one pixel
        if (x1 > r->src_w) x1 = r->src_w;
        uint32_t sum = 0;
        for (uint32_t sx = x0; sx < x1; sx++) sum += src[sx];
        r->acc[x] += sum / (x1 - x0);
    }
    r->acc_rows++;
}

int rs_push(rs_t *r, const uint8_t *src_row)
{
    if (r->src_y >= r->src_h) return -1;
    fold(r, src_row);
    r->src_y++;

    // Every destination row whose source range ends at or before src_y is
    // now complete. Upscaling makes this loop run more than once.
    uint32_t due = (uint32_t)((uint64_t)r->src_y * r->dst_h / r->src_h);
    if (r->dst_y < due) {
        while (r->dst_y < due) {
            for (uint32_t x = 0; x < r->dst_w; x++)
                r->out[x] = (uint8_t)(r->acc[x] / r->acc_rows);
            r->emit(r->ctx, r->out, r->dst_w);
            r->dst_y++;
        }
        // Only reset once something was emitted. Resetting unconditionally
        // throws away every source row that did not complete a target row,
        // which silently breaks all downscaling.
        memset(r->acc, 0, r->dst_w * sizeof *r->acc);
        r->acc_rows = 0;
    }
    return 0;
}

void rs_free(rs_t *r)
{
    free(r->acc); free(r->out);
    r->acc = NULL; r->out = NULL;
}
```

- [ ] **Step 5: Run the tests and verify they pass**

Run: `make -C tools/rip test`
Expected: `ok  test/test_resample.c`.

- [ ] **Step 6: Commit**

```bash
git add firmware/components/resample tools/rip/test/test_resample.c
git commit -m "Add a streaming resampler for the printer's dot grid

The ImageWriter is 144 dpi across and 72 dpi down, and iOS sends whatever
it likes, so the scale factors differ per axis. Box filter down, replicate
up, one destination row held at a time."
```

---

### Task 5: Halftoning

**Files:**
- Create: `firmware/components/halftone/include/halftone.h`
- Create: `firmware/components/halftone/halftone.c`
- Create: `tools/rip/test/test_halftone.c`

**Interfaces:**
- Consumes: nothing.
- Produces:
  - `typedef enum { HT_THRESHOLD, HT_ATKINSON, HT_FLOYD, HT_BAYER } ht_algo_t`
  - `int ht_init(ht_t *h, ht_algo_t algo, uint32_t width)`
  - `void ht_row(ht_t *h, const uint8_t *gray, uint8_t *bits)` — `gray` is `width` bytes of luminance, `bits` is `(width+7)/8` bytes with **bit 7 of byte 0 as the leftmost dot** and **a set bit meaning ink fires**.
  - `void ht_free(ht_t *h)`
  - `int ht_algo_from_name(const char *name, ht_algo_t *out)` — accepts `threshold`, `atkinson`, `floyd`, `bayer`.

- [ ] **Step 1: Write the failing test**

Create `tools/rip/test/test_halftone.c`:

```c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "test_util.h"
#include "halftone.h"

int main(void)
{
    /* -- threshold: < 128 is ink, >= 128 is paper -- */
    {
        ht_t h;
        CHECK_EQ_INT(ht_init(&h, HT_THRESHOLD, 8), 0);
        const uint8_t gray[8] = { 0, 127, 128, 255, 0, 0, 255, 255 };
        uint8_t bits[1] = { 0 };
        ht_row(&h, gray, bits);
        /* ink at x = 0,1,4,5 -> bits 7,6,3,2 -> 0b11001100 */
        CHECK_EQ_INT(bits[0], 0xCC);
        ht_free(&h);
    }

    /* -- solid black is all ink, solid white is none, for every algorithm -- */
    {
        const ht_algo_t algos[] = { HT_THRESHOLD, HT_ATKINSON, HT_FLOYD, HT_BAYER };
        for (unsigned i = 0; i < sizeof algos / sizeof algos[0]; i++) {
            ht_t h;
            CHECK_EQ_INT(ht_init(&h, algos[i], 16), 0);
            uint8_t gray[16], bits[2];

            memset(gray, 0x00, sizeof gray);
            for (int y = 0; y < 4; y++) {
                memset(bits, 0, sizeof bits);
                ht_row(&h, gray, bits);
                CHECK_EQ_INT(bits[0], 0xFF);
                CHECK_EQ_INT(bits[1], 0xFF);
            }
            ht_free(&h);

            CHECK_EQ_INT(ht_init(&h, algos[i], 16), 0);
            memset(gray, 0xFF, sizeof gray);
            for (int y = 0; y < 4; y++) {
                memset(bits, 0xFF, sizeof bits);
                ht_row(&h, gray, bits);
                CHECK_EQ_INT(bits[0], 0x00);
                CHECK_EQ_INT(bits[1], 0x00);
            }
            ht_free(&h);
        }
    }

    /* -- Atkinson pushes exactly err/8 downward --
     *
     * Width 1 on purpose. A wider row would have the same pixel's own
     * horizontal spread land back on later pixels of that row and feed into
     * their errors, so the expected values stop being hand-checkable. With
     * one column, only the two downward taps survive.
     *
     * v = 128, so adj = 128, which is >= 128 and prints white (out = 255).
     * error = 128 - 255 = -127, and C truncates toward zero: -127 / 8 = -15.
     * The taps at x+1, x+2 and x-1 are all clipped away, leaving r1[0] and
     * r2[0].
     *
     * ht_peek_err(h, 0, x) reads the row about to be processed next.
     */
    {
        ht_t h;
        CHECK_EQ_INT(ht_init(&h, HT_ATKINSON, 1), 0);
        const uint8_t row0[1] = { 128 };
        uint8_t bits[1] = { 0xFF };
        ht_row(&h, row0, bits);
        CHECK_EQ_INT(bits[0], 0x00);              /* nothing fired */
        CHECK_EQ_INT(ht_peek_err(&h, 0, 0), -15); /* next row */
        CHECK_EQ_INT(ht_peek_err(&h, 1, 0), -15); /* the row after that */
        CHECK_EQ_INT(ht_peek_err(&h, 2, 0), 0);   /* slot just cleared */
        ht_free(&h);
    }

    /* -- algorithm names round-trip -- */
    {
        ht_algo_t a;
        CHECK_EQ_INT(ht_algo_from_name("atkinson", &a), 0);
        CHECK_EQ_INT(a, HT_ATKINSON);
        CHECK_EQ_INT(ht_algo_from_name("bayer", &a), 0);
        CHECK_EQ_INT(a, HT_BAYER);
        CHECK_EQ_INT(ht_algo_from_name("nonsense", &a), -1);
    }

    TEST_END;
}
```

- [ ] **Step 2: Run it and verify it fails**

Run: `make -C tools/rip test`
Expected: compile error, `halftone.h` not found.

- [ ] **Step 3: Write the header**

Create `firmware/components/halftone/include/halftone.h`:

```c
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Grayscale to 1bpp. Input is luminance, 0 = black. Output bit set = ink.
#pragma once
#include <stdint.h>
#include <stddef.h>

typedef enum { HT_THRESHOLD, HT_ATKINSON, HT_FLOYD, HT_BAYER } ht_algo_t;

typedef struct {
    ht_algo_t algo;
    uint32_t  width;
    int16_t  *err;      // 3 rows of width, used as a ring by row index % 3
    uint32_t  y;
} ht_t;

int  ht_init(ht_t *h, ht_algo_t algo, uint32_t width);
void ht_row(ht_t *h, const uint8_t *gray, uint8_t *bits);
void ht_free(ht_t *h);
int  ht_algo_from_name(const char *name, ht_algo_t *out);

// Test hook: error carried for a row `ahead` rows from now, at column x.
int16_t ht_peek_err(const ht_t *h, uint32_t ahead, uint32_t x);
```

- [ ] **Step 4: Write the implementation**

Create `firmware/components/halftone/halftone.c`:

```c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "halftone.h"
#include <stdlib.h>
#include <string.h>

// Bill Atkinson's kernel, written for the Macintosh in the same years the
// ImageWriter shipped. It spreads only 6/8 of the error on purpose, which is
// why it holds contrast on a bilevel device with no ink density control.
static const uint8_t bayer8[8][8] = {
    {  0, 32,  8, 40,  2, 34, 10, 42 },
    { 48, 16, 56, 24, 50, 18, 58, 26 },
    { 12, 44,  4, 36, 14, 46,  6, 38 },
    { 60, 28, 52, 20, 62, 30, 54, 22 },
    {  3, 35, 11, 43,  1, 33,  9, 41 },
    { 51, 19, 59, 27, 49, 17, 57, 25 },
    { 15, 47,  7, 39, 13, 45,  5, 37 },
    { 63, 31, 55, 23, 61, 29, 53, 21 },
};

int ht_init(ht_t *h, ht_algo_t algo, uint32_t width)
{
    memset(h, 0, sizeof *h);
    if (!width) return -1;
    h->algo  = algo;
    h->width = width;
    h->err   = calloc((size_t)width * 3, sizeof *h->err);
    return h->err ? 0 : -1;
}

void ht_free(ht_t *h) { free(h->err); h->err = NULL; }

int ht_algo_from_name(const char *name, ht_algo_t *out)
{
    if (!name) return -1;
    if (!strcmp(name, "threshold")) { *out = HT_THRESHOLD; return 0; }
    if (!strcmp(name, "atkinson"))  { *out = HT_ATKINSON;  return 0; }
    if (!strcmp(name, "floyd"))     { *out = HT_FLOYD;     return 0; }
    if (!strcmp(name, "bayer"))     { *out = HT_BAYER;     return 0; }
    return -1;
}

static int16_t *errrow(ht_t *h, uint32_t y) { return h->err + (size_t)(y % 3) * h->width; }

int16_t ht_peek_err(const ht_t *h, uint32_t ahead, uint32_t x)
{
    return h->err[(size_t)((h->y + ahead) % 3) * h->width + x];
}

static void spread(ht_t *h, uint32_t x, int err)
{
    int16_t *r0 = errrow(h, h->y);
    int16_t *r1 = errrow(h, h->y + 1);
    int16_t *r2 = errrow(h, h->y + 2);
    uint32_t w  = h->width;

    if (h->algo == HT_ATKINSON) {
        int e = err / 8;                    // six neighbours, one eighth each
        if (x + 1 < w) { r0[x + 1] += (int16_t)e; }
        if (x + 2 < w) { r0[x + 2] += (int16_t)e; }
        if (x > 0)     { r1[x - 1] += (int16_t)e; }
        r1[x] += (int16_t)e;
        if (x + 1 < w) { r1[x + 1] += (int16_t)e; }
        r2[x] += (int16_t)e;
    } else {                                // Floyd-Steinberg
        if (x + 1 < w) { r0[x + 1] += (int16_t)(err * 7 / 16); }
        if (x > 0)     { r1[x - 1] += (int16_t)(err * 3 / 16); }
        r1[x] += (int16_t)(err * 5 / 16);
        if (x + 1 < w) { r1[x + 1] += (int16_t)(err * 1 / 16); }
    }
}

void ht_row(ht_t *h, const uint8_t *gray, uint8_t *bits)
{
    uint32_t w      = h->width;
    size_t   nbytes = (w + 7) / 8;
    memset(bits, 0, nbytes);

    int16_t *cur = errrow(h, h->y);

    for (uint32_t x = 0; x < w; x++) {
        int v = gray[x];
        int ink;

        switch (h->algo) {
        case HT_THRESHOLD:
            ink = v < 128;
            break;
        case HT_BAYER: {
            int t = (bayer8[h->y % 8][x % 8] * 255) / 64;
            ink = v <= t;
            break;
        }
        default: {                              // HT_ATKINSON, HT_FLOYD
            int adj = v + cur[x];
            if (adj < 0)   adj = 0;
            if (adj > 255) adj = 255;
            int out = adj < 128 ? 0 : 255;
            ink = (out == 0);
            spread(h, x, adj - out);
            break;
        }
        }

        if (ink) bits[x >> 3] |= (uint8_t)(0x80 >> (x & 7));
    }

    // This row's error slot is finished with; clear it for reuse two rows on.
    memset(cur, 0, (size_t)w * sizeof *cur);
    h->y++;
}
```

- [ ] **Step 5: Run the tests and verify they pass**

Run: `make -C tools/rip test`
Expected: `ok  test/test_halftone.c`.

If the Atkinson error checks fail by one, confirm C integer division is truncating toward zero as the test comment assumes: `-127 / 8 == -15`, not `-16`.

- [ ] **Step 6: Commit**

```bash
git add firmware/components/halftone tools/rip/test/test_halftone.c
git commit -m "Add halftoning with Atkinson, Floyd-Steinberg, Bayer and threshold

Atkinson is the default. It spreads six eighths of the error rather than
all of it, which is what holds contrast on a device with no ink density
control."
```

---

### Task 6: The ImageWriter output stage

**Files:**
- Create: `firmware/components/imagewriter/include/imagewriter.h`
- Create: `firmware/components/imagewriter/imagewriter.c`
- Create: `tools/rip/test/test_imagewriter.c`

**Interfaces:**
- Consumes: 1bpp rows in the layout Task 6 produces — bit 7 of byte 0 is the leftmost dot, a set bit is ink.
- Produces:
  - `typedef void (*iw_out_fn)(void *ctx, const uint8_t *buf, size_t n)`
  - `void iw_init(iw_t *w, uint32_t width_dots, iw_out_fn out, void *ctx)`
  - `void iw_begin_page(iw_t *w)`
  - `void iw_row(iw_t *w, const uint8_t *bits)` — feed one 1bpp row; every eighth row flushes a strip.
  - `void iw_end_page(iw_t *w)` — pads a partial strip, flushes, emits the form feed.
  - `void iw_transpose(const uint8_t *rows[8], uint8_t *cols, uint32_t width_dots)` — exposed for testing. A `NULL` row entry means blank. Note the parameter is `const uint8_t **`, not `const uint8_t *const *`: C does not implicitly convert between those two, and `-Werror` turns that into a build failure.

- [ ] **Step 1: Write the failing test**

Create `tools/rip/test/test_imagewriter.c`:

```c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "test_util.h"
#include "imagewriter.h"

static uint8_t sink[4096];
static size_t  sink_n;
static void collect(void *ctx, const uint8_t *b, size_t n)
{
    (void)ctx;
    if (sink_n + n <= sizeof sink) memcpy(sink + sink_n, b, n);
    sink_n += n;
}

int main(void)
{
    /* -- transpose: bit 0 of a column byte is the TOP pin -- */
    {
        uint8_t r[8][1];
        const uint8_t *rows[8];
        for (int i = 0; i < 8; i++) { r[i][0] = 0x00; rows[i] = r[i]; }

        r[0][0] = 0x80;                 /* top row, leftmost dot */
        uint8_t cols[8] = { 0 };
        iw_transpose(rows, cols, 8);
        CHECK_EQ_INT(cols[0], 0x01);    /* column 0, bit 0 = top pin */
        CHECK_EQ_INT(cols[1], 0x00);

        for (int i = 0; i < 8; i++) r[i][0] = 0x00;
        r[7][0] = 0x80;                 /* bottom row, leftmost dot */
        memset(cols, 0, sizeof cols);
        iw_transpose(rows, cols, 8);
        CHECK_EQ_INT(cols[0], 0x80);    /* bit 7 = bottom pin */

        for (int i = 0; i < 8; i++) r[i][0] = 0xFF;
        memset(cols, 0, sizeof cols);
        iw_transpose(rows, cols, 8);
        for (int x = 0; x < 8; x++) CHECK_EQ_INT(cols[x], 0xFF);
    }

    /* -- page framing and the four-ASCII-digit byte count -- */
    {
        iw_t w;
        sink_n = 0;
        iw_init(&w, 8, collect, NULL);
        iw_begin_page(&w);

        /* ESC c, ESC p, ESC T 1 6 */
        const uint8_t want_begin[] = { 0x1B,'c', 0x1B,'p', 0x1B,'T','1','6' };
        CHECK_EQ_INT(sink_n, sizeof want_begin);
        CHECK_EQ_MEM(sink, want_begin, sizeof want_begin);

        sink_n = 0;
        uint8_t row[1] = { 0x80 };
        for (int i = 0; i < 8; i++) iw_row(&w, row);   /* one full strip */

        /* ESC G 0 0 0 8, then 8 column bytes, then CR LF */
        const uint8_t want_strip[] = {
            0x1B,'G','0','0','0','8',
            0xFF, 0, 0, 0, 0, 0, 0, 0,
            '\r', '\n'
        };
        CHECK_EQ_INT(sink_n, sizeof want_strip);
        CHECK_EQ_MEM(sink, want_strip, sizeof want_strip);

        sink_n = 0;
        iw_end_page(&w);
        CHECK_EQ_INT(sink_n, 1);
        CHECK_EQ_INT(sink[0], 0x0C);      /* form feed, or it waits for Select */
    }

    /* -- a partial strip is padded with blank rows, not dropped -- */
    {
        iw_t w;
        sink_n = 0;
        iw_init(&w, 8, collect, NULL);
        iw_begin_page(&w);
        sink_n = 0;

        uint8_t row[1] = { 0x80 };
        iw_row(&w, row);                  /* one row only */
        CHECK_EQ_INT(sink_n, 0);          /* nothing emitted yet */

        iw_end_page(&w);
        /* strip header + 8 data + CR LF + FF = 6 + 8 + 2 + 1 */
        CHECK_EQ_INT(sink_n, 17);
        CHECK_EQ_INT(sink[6], 0x01);      /* only the top pin fires */
        CHECK_EQ_INT(sink[16], 0x0C);
    }

    TEST_END;
}
```

- [ ] **Step 2: Run it and verify it fails**

Run: `make -C tools/rip test`
Expected: compile error, `imagewriter.h` not found.

- [ ] **Step 3: Write the header**

Create `firmware/components/imagewriter/include/imagewriter.h`:

```c
// SPDX-License-Identifier: GPL-3.0-or-later
//
// ImageWriter II output stage. 144 dpi horizontal, 72 dpi vertical, one pass.
//
// Escape codes from the Apple ImageWriter II Technical Reference:
//   ESC c        reset                              (Table A-15, p. 144)
//   ESC p        144 dpi, 1152 dots max             (Table 8-2,  p. 106)
//   ESC T nn     line feed of nn/144 inch, 2 digits (Table A-15, p. 144)
//   ESC G nnnn   nnnn bytes of graphics, 4 digits   (Table 8-1,  p. 105)
// Bit 0 of a graphics byte is the TOP pin (p. 107).
// ESC T16 is 16/144 = 8/72 inch, which exactly abuts successive strips.
#pragma once
#include <stdint.h>
#include <stddef.h>

#define IW_MAX_DOTS 1152

typedef void (*iw_out_fn)(void *ctx, const uint8_t *buf, size_t n);

typedef struct {
    uint32_t  width_dots;
    iw_out_fn out;
    void     *ctx;
    uint8_t   rows[8][(IW_MAX_DOTS + 7) / 8];
    uint32_t  nrows;
    uint8_t   cols[IW_MAX_DOTS];
} iw_t;

void iw_init(iw_t *w, uint32_t width_dots, iw_out_fn out, void *ctx);
void iw_begin_page(iw_t *w);
void iw_row(iw_t *w, const uint8_t *bits);
void iw_end_page(iw_t *w);
void iw_transpose(const uint8_t *rows[8], uint8_t *cols, uint32_t width_dots);
```

- [ ] **Step 4: Write the implementation**

Create `firmware/components/imagewriter/imagewriter.c`:

```c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "imagewriter.h"
#include <string.h>

void iw_transpose(const uint8_t *rows[8], uint8_t *cols, uint32_t width_dots)
{
    memset(cols, 0, width_dots);
    for (uint32_t r = 0; r < 8; r++) {
        const uint8_t *src = rows[r];
        if (!src) continue;
        for (uint32_t x = 0; x < width_dots; x++)
            if (src[x >> 3] & (0x80 >> (x & 7)))
                cols[x] |= (uint8_t)(1u << r);   // r == 0 is the top pin
    }
}

void iw_init(iw_t *w, uint32_t width_dots, iw_out_fn out, void *ctx)
{
    memset(w, 0, sizeof *w);
    w->width_dots = width_dots > IW_MAX_DOTS ? IW_MAX_DOTS : width_dots;
    w->out = out;
    w->ctx = ctx;
}

void iw_begin_page(iw_t *w)
{
    static const uint8_t init[] = { 0x1B, 'c', 0x1B, 'p', 0x1B, 'T', '1', '6' };
    w->out(w->ctx, init, sizeof init);
    w->nrows = 0;
}

static void flush(iw_t *w)
{
    const uint8_t *rows[8];
    for (uint32_t i = 0; i < 8; i++)
        rows[i] = i < w->nrows ? w->rows[i] : NULL;   // pad short strips blank

    iw_transpose(rows, w->cols, w->width_dots);

    uint8_t hdr[6] = { 0x1B, 'G', 0, 0, 0, 0 };
    uint32_t n = w->width_dots;                        // one byte per column
    hdr[2] = (uint8_t)('0' + (n / 1000) % 10);
    hdr[3] = (uint8_t)('0' + (n / 100)  % 10);
    hdr[4] = (uint8_t)('0' + (n / 10)   % 10);
    hdr[5] = (uint8_t)('0' +  n         % 10);
    w->out(w->ctx, hdr, sizeof hdr);
    w->out(w->ctx, w->cols, n);

    static const uint8_t eol[] = { '\r', '\n' };
    w->out(w->ctx, eol, sizeof eol);

    w->nrows = 0;
}

void iw_row(iw_t *w, const uint8_t *bits)
{
    memcpy(w->rows[w->nrows], bits, (w->width_dots + 7) / 8);
    if (++w->nrows == 8) flush(w);
}

void iw_end_page(iw_t *w)
{
    if (w->nrows) flush(w);
    // The widely reported "you have to press Select to make it print" is
    // almost always a missing form feed, not a hardware fault.
    static const uint8_t ff[] = { 0x0C };
    w->out(w->ctx, ff, sizeof ff);
}
```

- [ ] **Step 5: Run the tests and verify they pass**

Run: `make -C tools/rip test`
Expected: `ok  test/test_imagewriter.c`.

- [ ] **Step 6: Commit**

```bash
git add firmware/components/imagewriter tools/rip/test/test_imagewriter.c
git commit -m "Add the ImageWriter output stage

Bit 0 is the top pin, ESC G takes four ASCII digits, and every page ends
with a form feed. All three are cited to the technical reference in the
header, because all three are wrong in most circulating documentation."
```

---

### Task 7: The `rip` CLI

**Files:**
- Create: `tools/rip/png_write.h`
- Create: `tools/rip/png_write.c`
- Create: `tools/rip/main.c`

**Interfaces:**
- Consumes: every module above.
- Produces: `rip --in FILE [--out-stream FILE] [--out-preview FILE] [--dither NAME]`, exit 0 on success.

- [ ] **Step 1: Write the PNG writer**

Uncompressed on purpose — a deflate implementation is not worth carrying for a debug preview, and stored blocks are valid PNG that every viewer opens.

Create `tools/rip/png_write.h`:

```c
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdint.h>
#include <stdio.h>

// 8-bit grayscale, one byte per pixel, row-major. Returns 0 on success.
int png_write_gray(const char *path, const uint8_t *pix, uint32_t w, uint32_t h);
```

Create `tools/rip/png_write.c`:

```c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "png_write.h"
#include <stdlib.h>
#include <string.h>

static uint32_t crc_table[256];
static void crc_init(void)
{
    for (uint32_t n = 0; n < 256; n++) {
        uint32_t c = n;
        for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crc_table[n] = c;
    }
}
static uint32_t crc(const uint8_t *b, size_t n)
{
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) c = crc_table[(c ^ b[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}
static void be32w(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}
static int chunk(FILE *f, const char *tag, const uint8_t *data, size_t n)
{
    uint8_t hdr[8];
    be32w(hdr, (uint32_t)n);
    memcpy(hdr + 4, tag, 4);
    if (fwrite(hdr, 1, 8, f) != 8) return -1;
    if (n && fwrite(data, 1, n, f) != n) return -1;

    uint8_t *tmp = malloc(n + 4);
    if (!tmp) return -1;
    memcpy(tmp, tag, 4);
    if (n) memcpy(tmp + 4, data, n);
    uint8_t c[4];
    be32w(c, crc(tmp, n + 4));
    free(tmp);
    return fwrite(c, 1, 4, f) == 4 ? 0 : -1;
}

int png_write_gray(const char *path, const uint8_t *pix, uint32_t w, uint32_t h)
{
    crc_init();
    FILE *f = fopen(path, "wb");
    if (!f) return -1;

    static const uint8_t sig[8] = { 137,'P','N','G','\r','\n',26,'\n' };
    if (fwrite(sig, 1, 8, f) != 8) { fclose(f); return -1; }

    uint8_t ihdr[13];
    be32w(ihdr, w); be32w(ihdr + 4, h);
    ihdr[8] = 8;                        // bit depth
    ihdr[9] = 0;                        // colour type 0 = grayscale
    ihdr[10] = ihdr[11] = ihdr[12] = 0; // deflate, no filter, no interlace
    if (chunk(f, "IHDR", ihdr, sizeof ihdr)) { fclose(f); return -1; }

    // Raw stream: one filter byte (0) per row, then the row.
    size_t raw_len = (size_t)h * (w + 1);
    uint8_t *raw = malloc(raw_len);
    if (!raw) { fclose(f); return -1; }
    for (uint32_t y = 0; y < h; y++) {
        raw[(size_t)y * (w + 1)] = 0;
        memcpy(raw + (size_t)y * (w + 1) + 1, pix + (size_t)y * w, w);
    }

    // zlib wrapper around stored deflate blocks.
    size_t nblocks = (raw_len + 65534) / 65535;
    size_t z_len   = 2 + nblocks * 5 + raw_len + 4;
    uint8_t *z = malloc(z_len);
    if (!z) { free(raw); fclose(f); return -1; }
    size_t zi = 0;
    z[zi++] = 0x78; z[zi++] = 0x01;
    size_t off = 0;
    while (off < raw_len) {
        size_t n = raw_len - off > 65535 ? 65535 : raw_len - off;
        z[zi++] = (off + n >= raw_len) ? 1 : 0;
        z[zi++] = (uint8_t)(n & 0xFF);        z[zi++] = (uint8_t)(n >> 8);
        z[zi++] = (uint8_t)(~n & 0xFF);       z[zi++] = (uint8_t)((~n >> 8) & 0xFF);
        memcpy(z + zi, raw + off, n);
        zi += n; off += n;
    }
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < raw_len; i++) { a = (a + raw[i]) % 65521; b = (b + a) % 65521; }
    be32w(z + zi, (b << 16) | a);
    zi += 4;

    int rc = chunk(f, "IDAT", z, zi);
    free(z); free(raw);
    if (rc == 0) rc = chunk(f, "IEND", NULL, 0);
    fclose(f);
    return rc;
}
```

- [ ] **Step 2: Write the CLI**

Create `tools/rip/main.c`:

```c
// SPDX-License-Identifier: GPL-3.0-or-later
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "urf_decode.h"
#include "resample.h"
#include "halftone.h"
#include "imagewriter.h"
#include "png_write.h"

#define DPI_X 144
#define DPI_Y  72

static int file_read(void *ctx, uint8_t *buf, size_t n)
{
    return fread(buf, 1, n, (FILE *)ctx) == n;
}

typedef struct {
    ht_t     ht;
    iw_t     iw;
    FILE    *stream;
    uint8_t *bits;
    uint8_t *preview;      // dst_w * dst_h, grown as rows arrive
    uint32_t dst_w, dst_h, prev_y;
} pipe_t;

static void stream_out(void *ctx, const uint8_t *b, size_t n)
{
    pipe_t *p = (pipe_t *)ctx;
    if (p->stream) fwrite(b, 1, n, p->stream);
}

/* One resampled row: halftone it, feed the printer, record the preview. */
static void on_row(void *ctx, const uint8_t *row, uint32_t w)
{
    pipe_t *p = (pipe_t *)ctx;
    ht_row(&p->ht, row, p->bits);
    iw_row(&p->iw, p->bits);

    if (p->preview && p->prev_y < p->dst_h) {
        uint8_t *dst = p->preview + (size_t)p->prev_y * w;
        for (uint32_t x = 0; x < w; x++)
            dst[x] = (p->bits[x >> 3] & (0x80 >> (x & 7))) ? 0 : 255;
        p->prev_y++;
    }
}

static void usage(void)
{
    fputs("usage: rip --in FILE [--out-stream FILE] [--out-preview FILE]\n"
          "           [--dither threshold|atkinson|floyd|bayer]\n", stderr);
}

int main(int argc, char **argv)
{
    const char *in = NULL, *out_stream = NULL, *out_preview = NULL;
    const char *dither = "atkinson";

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--in") && i + 1 < argc)               in = argv[++i];
        else if (!strcmp(argv[i], "--out-stream") && i + 1 < argc)  out_stream = argv[++i];
        else if (!strcmp(argv[i], "--out-preview") && i + 1 < argc) out_preview = argv[++i];
        else if (!strcmp(argv[i], "--dither") && i + 1 < argc)      dither = argv[++i];
        else { usage(); return 2; }
    }
    if (!in) { usage(); return 2; }

    ht_algo_t algo;
    if (ht_algo_from_name(dither, &algo)) {
        fprintf(stderr, "rip: unknown dither '%s'\n", dither);
        return 2;
    }

    FILE *f = fopen(in, "rb");
    if (!f) { perror(in); return 1; }

    urf_t u;
    int rc = urf_open(&u, file_read, f);
    if (rc != URF_OK) {
        fprintf(stderr, "rip: %s is not a URF file (%d)\n", in, rc);
        fclose(f);
        return 1;
    }

    pipe_t p;
    memset(&p, 0, sizeof p);
    p.stream = out_stream ? fopen(out_stream, "wb") : NULL;
    if (out_stream && !p.stream) { perror(out_stream); fclose(f); return 1; }

    int pages = 0;
    while ((rc = urf_next_page(&u)) == URF_OK) {
        if (u.page.bits_per_pixel != 8 || u.page.num_colors != 1) {
            fprintf(stderr, "rip: page %d is %u bpp / %u colours; only 8-bit "
                            "grayscale is supported\n",
                    pages + 1, u.page.bits_per_pixel, u.page.num_colors);
            rc = -1;
            break;
        }

        uint32_t res = u.page.res_x ? u.page.res_x : DPI_X;
        p.dst_w = (uint32_t)((uint64_t)u.page.width  * DPI_X / res);
        p.dst_h = (uint32_t)((uint64_t)u.page.height * DPI_Y / res);
        if (p.dst_w > IW_MAX_DOTS) p.dst_w = IW_MAX_DOTS;
        if (!p.dst_w || !p.dst_h) { fprintf(stderr, "rip: empty page\n"); rc = -1; break; }

        p.bits   = calloc((p.dst_w + 7) / 8, 1);
        p.prev_y = 0;
        p.preview = out_preview ? calloc((size_t)p.dst_w * p.dst_h, 1) : NULL;
        if (!p.bits || (out_preview && !p.preview)) { rc = -1; break; }

        if (ht_init(&p.ht, algo, p.dst_w)) { rc = -1; break; }
        iw_init(&p.iw, p.dst_w, stream_out, &p);
        iw_begin_page(&p.iw);

        rs_t rs;
        if (rs_init(&rs, u.page.width, u.page.height, p.dst_w, p.dst_h, on_row, &p)) {
            rc = -1; break;
        }

        const uint8_t *row;
        int r;
        while ((r = urf_next_row(&u, &row)) == URF_OK) rs_push(&rs, row);
        if (r < 0) { fprintf(stderr, "rip: decode failed on page %d (%d)\n", pages + 1, r); rc = r; }

        iw_end_page(&p.iw);
        rs_free(&rs);
        ht_free(&p.ht);

        if (p.preview && pages == 0) {
            if (png_write_gray(out_preview, p.preview, p.dst_w, p.dst_h))
                fprintf(stderr, "rip: could not write %s\n", out_preview);
        }
        free(p.preview); p.preview = NULL;
        free(p.bits);    p.bits    = NULL;

        pages++;
        if (rc < 0) break;
    }

    if (p.stream) fclose(p.stream);
    urf_free(&u);
    fclose(f);

    if (rc < 0) return 1;
    fprintf(stderr, "rip: %d page%s\n", pages, pages == 1 ? "" : "s");
    return 0;
}
```

- [ ] **Step 3: Build it**

Run: `make -C tools/rip`
Expected: `build/rip` links with no warnings.

- [ ] **Step 4: Prove it end to end on a synthetic file**

Create `tools/rip/test/make_fixture.py` and run it to generate a checkerboard URF, then rip it:

```python
#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Write a small 8-bit grayscale URF with an uncompressed body."""
import struct, sys

W, H, RES = 288, 144, 144          # 2 inches at 144 dpi
out = bytearray(b"UNIRAST\0" + struct.pack(">I", 1))
hdr = bytearray(32)
hdr[0] = 8                          # bits per pixel
hdr[1] = 0                          # colorspace 0 = sGray
struct.pack_into(">I", hdr, 12, W)
struct.pack_into(">I", hdr, 16, H)
struct.pack_into(">I", hdr, 20, RES)
out += hdr

for y in range(H):
    row = bytes(0 if ((x // 16) + (y // 16)) % 2 else 255 for x in range(W))
    out.append(0)                   # line repeat: emit once
    x = 0
    while x < W:                    # encode as literal runs of up to 128
        n = min(128, W - x)
        if n == 1:                  # a literal run of one is not encodable
            out.append(0x00); out.append(row[x])
        else:
            out.append(257 - n); out += row[x:x + n]
        x += n

open(sys.argv[1], "wb").write(bytes(out))
print(f"wrote {sys.argv[1]}: {W}x{H} @ {RES}dpi, {len(out)} bytes")
```

Run:
```bash
python3 tools/rip/test/make_fixture.py fixtures/checker.urf
tools/rip/build/rip --in fixtures/checker.urf \
    --out-stream /tmp/stream.bin --out-preview /tmp/preview.png
```

Expected: `rip: 1 page`. Then verify:
```bash
head -c 8 /tmp/stream.bin | xxd     # 1b 63 1b 70 1b 54 31 36  = ESC c ESC p ESC T16
tail -c 1 /tmp/stream.bin | xxd     # 0c                       = form feed
open /tmp/preview.png               # a checkerboard, 288 wide, 72 tall
```

The preview is 288 x 72 because the vertical axis is 72 dpi against 144 horizontal. That squashed aspect is correct and is what the printer will produce.

- [ ] **Step 5: Lock in the regression**

```bash
mkdir -p fixtures
cp /tmp/stream.bin fixtures/checker.expected.bin
```

Add to `tools/rip/Makefile`:

```make
.PHONY: regress
regress: build/rip
	@python3 test/make_fixture.py /tmp/checker.urf >/dev/null
	@./build/rip --in /tmp/checker.urf --out-stream /tmp/checker.bin 2>/dev/null
	@cmp /tmp/checker.bin ../../fixtures/checker.expected.bin \
	  && echo "ok  regression: stream.bin unchanged" \
	  || (echo "FAIL regression: stream.bin changed"; exit 1)
```

Run: `make -C tools/rip regress`
Expected: `ok  regression: stream.bin unchanged`.

- [ ] **Step 6: Commit**

```bash
git add tools/rip/main.c tools/rip/png_write.c tools/rip/png_write.h \
        tools/rip/test/make_fixture.py tools/rip/Makefile \
        fixtures/checker.urf fixtures/checker.expected.bin
git commit -m "Wire the raster pipeline into a rip binary

Decode, resample onto the 144x72 grid, halftone, transpose, emit. The
byte-for-byte regression against a committed stream is the point of the
whole tool."
```

---

### Task 8: Capture a real URF from an iPhone

Everything above runs on a synthetic file. This task replaces the guess with the real thing, and answers a question the spec flags in §3.3: what resolution and colour space iOS actually sends when told the printer can only do 72 and 144 dpi.

**Files:**
- Create: `tools/ippcap/ippcap.py`
- Create: `fixtures/README.md`

**Interfaces:**
- Consumes: nothing from earlier tasks. Standalone.
- Produces: a captured `.urf` under `fixtures/`, and a recorded note of the header values iOS chose.

- [ ] **Step 1: Write the capture server**

Create `tools/ippcap/ippcap.py`:

```python
#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""A stub IPP printer that exists only to capture what iOS sends.

It answers Get-Printer-Attributes and Validate-Job with the minimum iOS
needs, then writes any Print-Job body straight to disk. It is not a printer
and makes no attempt to be correct beyond getting the job out of the phone.

Run it, then in another terminal advertise it over Bonjour:

  dns-sd -R AirWriterCap _ipp._tcp,_universal local 6631 \\
      txtvers=1 rp=ipp/print ty="AirWriter capture" \\
      pdl=image/urf URF=V1.4,CP1,RS72-144,W8,DM1,IS1,MT1,FN3 \\
      Color=F Duplex=F

Leave both running, then print from the iPhone to "AirWriterCap".
"""
import http.server, struct, sys, pathlib, datetime

OUT = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else "fixtures")
PORT = 6631

# IPP tags
TAG_OPERATION, TAG_JOB, TAG_PRINTER, TAG_END = 0x01, 0x02, 0x04, 0x03
INTEGER, BOOLEAN, ENUM = 0x21, 0x22, 0x23
TEXT, NAME, KEYWORD, URI, CHARSET, LANGUAGE, MIMETYPE = 0x41, 0x42, 0x44, 0x45, 0x47, 0x48, 0x49
RESOLUTION = 0x32


def attr(tag, name, value):
    out = bytes([tag]) + struct.pack(">H", len(name)) + name.encode()
    return out + struct.pack(">H", len(value)) + value


def txt(tag, name, s):
    return attr(tag, name, s.encode())


def num(name, v):
    return attr(INTEGER, name, struct.pack(">i", v))


def res(name, x, y):
    return attr(RESOLUTION, name, struct.pack(">iiB", x, y, 3))  # 3 = dpi


def printer_attrs(req_id, host):
    b = bytearray(struct.pack(">HHI", 0x0200, 0x0000, req_id))  # version, ok, id
    b.append(TAG_OPERATION)
    b += txt(CHARSET, "attributes-charset", "utf-8")
    b += txt(LANGUAGE, "attributes-natural-language", "en-us")
    b.append(TAG_PRINTER)
    b += txt(URI, "printer-uri-supported", f"ipp://{host}/ipp/print")
    b += txt(KEYWORD, "uri-authentication-supported", "none")
    b += txt(KEYWORD, "uri-security-supported", "none")
    b += txt(NAME, "printer-name", "AirWriter capture")
    b += attr(ENUM, "printer-state", struct.pack(">i", 3))       # idle
    b += txt(KEYWORD, "printer-state-reasons", "none")
    b += txt(KEYWORD, "ipp-versions-supported", "1.1")
    # A 1setOf is encoded as repeated values where every value after the
    # first carries a zero-length name.
    for i, op in enumerate((0x0002, 0x0004, 0x0008, 0x0009, 0x000A, 0x000B)):
        b += attr(ENUM, "operations-supported" if i == 0 else "",
                  struct.pack(">i", op))
    b += txt(CHARSET, "charset-configured", "utf-8")
    b += txt(CHARSET, "charset-supported", "utf-8")
    b += txt(LANGUAGE, "natural-language-configured", "en-us")
    b += txt(LANGUAGE, "generated-natural-language-supported", "en-us")
    b += txt(MIMETYPE, "document-format-default", "image/urf")
    b += txt(MIMETYPE, "document-format-supported", "image/urf")
    b += attr(BOOLEAN, "printer-is-accepting-jobs", b"\x01")
    b += num("queued-job-count", 0)
    b += txt(KEYWORD, "pdl-override-supported", "attempted")
    b += num("printer-up-time", 1)
    b += txt(KEYWORD, "compression-supported", "none")
    b += txt(KEYWORD, "media-default", "na_letter_8.5x11in")
    b += txt(KEYWORD, "media-supported", "na_letter_8.5x11in")
    b += res("printer-resolution-default", 144, 72)
    b += res("printer-resolution-supported", 144, 72)
    b += txt(KEYWORD, "print-color-mode-default", "monochrome")
    b += txt(KEYWORD, "print-color-mode-supported", "monochrome")
    for i, v in enumerate(("V1.4", "CP1", "RS72-144", "W8",
                           "DM1", "IS1", "MT1", "FN3")):
        b += txt(KEYWORD, "urf-supported" if i == 0 else "", v)
    b.append(TAG_END)
    return bytes(b)


def job_attrs(req_id, job_id, host):
    b = bytearray(struct.pack(">HHI", 0x0200, 0x0000, req_id))
    b.append(TAG_OPERATION)
    b += txt(CHARSET, "attributes-charset", "utf-8")
    b += txt(LANGUAGE, "attributes-natural-language", "en-us")
    b.append(TAG_JOB)
    b += num("job-id", job_id)
    b += txt(URI, "job-uri", f"ipp://{host}/ipp/print/{job_id}")
    b += attr(ENUM, "job-state", struct.pack(">i", 9))           # completed
    b += txt(KEYWORD, "job-state-reasons", "job-completed-successfully")
    b.append(TAG_END)
    return bytes(b)


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    job_id = 1

    def log_message(self, fmt, *a):
        sys.stderr.write("  " + fmt % a + "\n")

    def do_POST(self):
        n = int(self.headers.get("Content-Length", 0))
        if self.headers.get("Transfer-Encoding", "").lower() == "chunked":
            body = bytearray()
            while True:
                size = int(self.rfile.readline().strip() or b"0", 16)
                if size == 0:
                    self.rfile.readline()
                    break
                body += self.rfile.read(size)
                self.rfile.readline()
            body = bytes(body)
        else:
            body = self.rfile.read(n)

        op = struct.unpack(">H", body[2:4])[0]
        rid = struct.unpack(">I", body[4:8])[0]
        host = self.headers.get("Host", "localhost")
        print(f"op 0x{op:04X}  {len(body)} bytes")

        if op == 0x000B:                                  # Get-Printer-Attributes
            resp = printer_attrs(rid, host)
        elif op == 0x0004:                                # Validate-Job
            resp = job_attrs(rid, Handler.job_id, host)
        elif op in (0x0002, 0x0009, 0x000A):              # Print-Job / Get-Job(s)
            if op == 0x0002:
                # The document follows the end-of-attributes tag.
                i = body.index(bytes([TAG_END])) + 1
                doc = body[i:]
                OUT.mkdir(parents=True, exist_ok=True)
                stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
                ext = "urf" if doc[:7] == b"UNIRAST" else \
                      "pwg" if doc[:4] == b"RaS2" else "bin"
                path = OUT / f"ios-{stamp}.{ext}"
                path.write_bytes(doc)
                print(f"  captured {len(doc)} bytes -> {path}")
                if ext == "urf" and len(doc) >= 44:
                    bpp, cs = doc[12], doc[13]
                    w, h, r = struct.unpack(">III", doc[24:36])
                    print(f"  URF: {w}x{h} @ {r}dpi, {bpp}bpp, colorspace {cs}")
                Handler.job_id += 1
            resp = job_attrs(rid, Handler.job_id - 1, host)
        else:
            resp = struct.pack(">HHI", 0x0200, 0x0501, rid) + bytes([TAG_END])

        self.send_response(200)
        self.send_header("Content-Type", "application/ipp")
        self.send_header("Content-Length", str(len(resp)))
        self.end_headers()
        self.wfile.write(resp)


if __name__ == "__main__":
    print(f"ippcap on :{PORT}, writing to {OUT}/")
    http.server.ThreadingHTTPServer(("", PORT), Handler).serve_forever()
```

- [ ] **Step 2: Run it and print from the phone**

Terminal 1:
```bash
python3 tools/ippcap/ippcap.py fixtures
```

Terminal 2:
```bash
dns-sd -R AirWriterCap _ipp._tcp,_universal local 6631 \
    txtvers=1 rp=ipp/print ty="AirWriter capture" \
    pdl=image/urf URF=V1.4,CP1,RS72-144,W8,DM1,IS1,MT1,FN3 Color=F Duplex=F
```

On the iPhone, open a photo or a page in Safari, tap share, tap Print, pick **AirWriterCap**, print.

Expected: terminal 1 logs `op 0x000B` then `op 0x0002`, and prints the captured size, path, and the URF geometry line.

If the printer does not appear in the print sheet, the subtype is the usual cause — `_universal._sub._ipp._tcp` is what makes iOS list it, and `dns-sd` expresses that as `_ipp._tcp,_universal`. If it appears but the job never arrives, watch for a `Validate-Job` in the log; a phone that validates and then stops did not like the attribute set.

- [ ] **Step 3: Record what iOS actually chose**

Create `fixtures/README.md`:

```markdown
# Fixtures

`checker.urf` / `checker.expected.bin` — synthetic, generated by
`tools/rip/test/make_fixture.py`. The byte-for-byte regression target.
Regenerate the expected stream only when the output is meant to change,
and say why in the commit message.

`ios-*.urf` — captured from a real iPhone through `tools/ippcap/ippcap.py`.

## What iOS sent

Fill this in from the ippcap log after the first capture. It answers the
question SPEC.md 3.3 leaves open: whether a client honours RS72-144 or
ignores it and sends 300 dpi anyway.

| Capture | Device / iOS | Width | Height | dpi | bpp | Colorspace |
|---|---|---|---|---|---|---|
| | | | | | | |
```

- [ ] **Step 4: Run the real capture through the pipeline**

```bash
tools/rip/build/rip --in fixtures/ios-*.urf \
    --out-stream /tmp/ios.bin --out-preview /tmp/ios.png
open /tmp/ios.png
```

Expected: a recognisable, dithered version of whatever was printed. If it comes out inverted, the luminance-to-ink polarity is wrong somewhere — grayscale 0 is black, and a set output bit means ink fires.

- [ ] **Step 5: Commit**

```bash
git add tools/ippcap/ippcap.py fixtures/README.md fixtures/ios-*.urf
git commit -m "Capture a real URF job from iOS

The stub IPP server exists only to get a document out of the phone and
onto disk. What it captures settles whether a client honours RS72-144 or
sends 300 dpi regardless."
```

---

## Done when

- `make -C tools/rip test` passes with no warnings.
- `make -C tools/rip regress` passes.
- A real iOS capture rips to a preview that looks like the page that was printed.
- `fixtures/README.md` records the geometry iOS chose.

## Deliberately not in this plan

- **PWG raster input.** `urf_open` detects `RaS2` and returns `URF_ERR_UNSUPPORTED` rather than mis-parsing it. iOS sends URF; PWG is a later compatibility item and the 1796-byte header parser is its own task.
- **144 dpi vertical.** `SPEC.md` §3.8 defers the two-pass mode to v2, and this plan holds that line.
- **Colour.** The 4-colour ribbon is single-source and currently out of stock.
- **Anything on the ESP32.** ESP-IDF is not installed and no boards are ordered. The components are laid out in ESP-IDF component form so they compile there unchanged, but this plan never builds them for the device.
