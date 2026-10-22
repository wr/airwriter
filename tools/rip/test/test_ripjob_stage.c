// SPDX-License-Identifier: GPL-3.0-or-later
//
// ripjob's geometry from printer_caps and its output-stage hook (W-1082):
// a LaserWriter's page is ripped at 300 dpi into a stage that takes rows,
// while the ImageWriter's stream comes out byte for byte as before.
#include "test_util.h"
#include "ripjob.h"
#include "printer_caps.h"
#include <stdlib.h>

typedef struct {
    int      begins, ends, rows, index;
    uint32_t w, h, row_bytes_bad;
    uint8_t  first[320], last_row[320];
} rec_t;

static int rec_begin(void *ctx, int index, uint32_t w, uint32_t h)
{
    rec_t *r = ctx;
    r->begins++;
    r->index = index;
    r->w = w;
    r->h = h;
    return 0;
}

static void rec_row(void *ctx, const uint8_t *bits)
{
    rec_t *r = ctx;
    size_t nb = (r->w + 7) / 8;
    if (nb > sizeof r->first) { r->row_bytes_bad++; return; }
    if (r->rows == 0) memcpy(r->first, bits, nb);
    memcpy(r->last_row, bits, nb);
    r->rows++;
}

static int rec_end(void *ctx)
{
    ((rec_t *)ctx)->ends++;
    return 0;
}

static size_t out_n;
static void count(void *ctx, const uint8_t *b, size_t n) { (void)ctx; (void)b; out_n += n; }

static char logged[256];
static void keep_log(void *ctx, const char *msg)
{
    (void)ctx;
    strncpy(logged, msg, sizeof logged - 1);
}

static uint8_t *slurp(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    static uint8_t buf[1 << 20];
    *n = fread(buf, 1, sizeof buf, f);
    fclose(f);
    return buf;
}

static uint8_t stream[8192];
static size_t  stream_n;
static void collect(void *ctx, const uint8_t *b, size_t n)
{
    (void)ctx;
    if (stream_n + n <= sizeof stream) memcpy(stream + stream_n, b, n);
    stream_n += n;
}

static const printer_caps_t k_laser = {
    .make_model = "Apple LaserWriter II NT", .device_id = "MFG:Apple;MDL:LaserWriter II NT;CMD:URF;",
    .urf_rs = "RS300", .res_x = 300, .res_y = 300, .media = "na_letter_8.5x11in",
    .media_x = 21590, .media_y = 27940, .ppm = 4,
    .rip_dpi_x = 300, .rip_dpi_y = 300, .max_dots = 2550, .crop_margin = 0,
};

// A one-page 8-bit gray URF, w x h at res, each row as literal runs. px(x, y)
// gives the gray level.
static size_t make_urf(uint8_t *out, uint32_t w, uint32_t h, uint32_t res,
                       uint8_t (*px)(uint32_t x, uint32_t y))
{
    size_t n = 0;
    memcpy(out, "UNIRAST\0", 8); n = 8;
    out[n++] = 0; out[n++] = 0; out[n++] = 0; out[n++] = 1;
    uint8_t hdr[32] = { 0 };
    hdr[0] = 8;
    hdr[12] = (uint8_t)(w >> 24); hdr[13] = (uint8_t)(w >> 16); hdr[14] = (uint8_t)(w >> 8); hdr[15] = (uint8_t)w;
    hdr[16] = (uint8_t)(h >> 24); hdr[17] = (uint8_t)(h >> 16); hdr[18] = (uint8_t)(h >> 8); hdr[19] = (uint8_t)h;
    hdr[22] = (uint8_t)(res >> 8); hdr[23] = (uint8_t)res;
    memcpy(out + n, hdr, 32); n += 32;
    for (uint32_t y = 0; y < h; y++) {
        out[n++] = 0;
        for (uint32_t x = 0; x < w;) {
            uint32_t k = w - x < 128 ? w - x : 128;
            if (k == 1) { out[n++] = 0; out[n++] = px(x, y); x++; continue; }
            out[n++] = (uint8_t)(257 - k);
            for (uint32_t i = 0; i < k; i++) out[n++] = px(x + i, y);
            x += k;
        }
    }
    return n;
}

// Black at the left edge, at x 2399 and at the right edge (2479); white elsewhere.
static uint8_t edges(uint32_t x, uint32_t y)
{
    (void)y;
    return (x == 0 || x == 2399 || x == 2479) ? 0 : 255;
}

static int rip(const uint8_t *urf, size_t len, ripjob_cfg_t *cfg)
{
    memsrc_t m = { urf, len, 0 };
    urf_t u;
    if (urf_open(&u, memsrc_read, &m) != URF_OK) return -100;
    int rc = ripjob_run(&u, cfg, NULL);
    urf_free(&u);
    return rc;
}

int main(void)
{
    CHECK_EQ_INT(printer_caps_imagewriter.max_dots, IW_MAX_DOTS);
    CHECK_EQ_INT(printer_caps_imagewriter.rip_dpi_x, RIPJOB_DPI_X);
    CHECK_EQ_INT(printer_caps_imagewriter.rip_dpi_y, RIPJOB_DPI_Y);

    size_t urf_n = 0, want_n = 0;
    uint8_t *urf = slurp("../../fixtures/checker.urf", &urf_n);
    CHECK(urf != NULL);
    if (!urf) TEST_END;
    uint8_t *checker = malloc(urf_n);
    memcpy(checker, urf, urf_n);

    // The checker (288 x 144 at 144 dpi) into a 300 dpi stage.
    rec_t r = { 0 };
    rip_stage_t st = { rec_begin, rec_row, rec_end, &r };
    ripjob_cfg_t cfg = { .out = count, .dither = HT_THRESHOLD, .caps = &k_laser, .stage = &st,
                         .top_margin = -144, .colour = 1 };
    out_n = 0;
    CHECK_EQ_INT(rip(checker, urf_n, &cfg), 0);
    CHECK_EQ_INT(r.begins, 1);
    CHECK_EQ_INT(r.index, 0);
    CHECK_EQ_INT(r.w, 600);
    CHECK_EQ_INT(r.h, 300);
    CHECK_EQ_INT(r.rows, 300);            // top_margin is the ImageWriter's alone
    CHECK_EQ_INT(r.row_bytes_bad, 0);
    CHECK_EQ_INT(r.ends, 1);
    CHECK_EQ_INT(out_n, 0);               // nothing reaches the ImageWriter stream
    CHECK_EQ_INT(r.first[0] & 0x80, 0);   // the checker's first square is white,
    CHECK(r.first[4] != 0);               // its second black (33 px at 300 dpi)

    // No caps, no stage: the ImageWriter, byte for byte as the fixture.
    uint8_t *want = slurp("../../fixtures/checker.expected.bin", &want_n);
    CHECK(want != NULL);
    uint8_t *expect = malloc(want_n);
    memcpy(expect, want, want_n);
    ripjob_cfg_t iw = { .out = collect, .dither = HT_ATKINSON };
    stream_n = 0;
    CHECK_EQ_INT(rip(checker, urf_n, &iw), 0);
    CHECK_EQ_INT(stream_n, want_n);
    if (stream_n == want_n) CHECK_EQ_MEM(stream, expect, want_n);

    // A page wider than the stage takes (A4's 2480 px into a 2400-dot
    // printer): clipped on the right, never scaled, and said in the log.
    static uint8_t wide[2480 * 4 + 4 * 40 + 64];
    size_t wide_n = make_urf(wide, 2480, 4, 300, edges);
    printer_caps_t narrow = k_laser;
    narrow.max_dots = 2400;
    rec_t c = { 0 };
    rip_stage_t cst = { rec_begin, rec_row, rec_end, &c };
    ripjob_cfg_t ccfg = { .dither = HT_THRESHOLD, .caps = &narrow, .stage = &cst,
                          .log = keep_log };
    logged[0] = '\0';
    CHECK_EQ_INT(rip(wide, wide_n, &ccfg), 0);
    CHECK_EQ_INT(c.w, 2400);
    CHECK_EQ_INT(c.rows, 4);
    CHECK(c.first[0] & 0x80);             // x 0 kept where it was
    CHECK(c.first[2399 / 8] & (0x80 >> (2399 % 8)));   // x 2399: the last column kept
    CHECK(strstr(logged, "clipped") != NULL);

    free(checker);
    free(expect);
    TEST_END;
}
