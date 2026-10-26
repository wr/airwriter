// SPDX-License-Identifier: GPL-3.0-or-later
//
// ripjob's geometry from printer_caps and its output-stage hook (W-1082):
// a LaserWriter's page is ripped at 300 dpi into a stage that takes rows,
// while the ImageWriter's stream comes out byte for byte as before.
#include "test_util.h"
#include "ripjob.h"
#include "printer_caps.h"
#include "sw_model.h"
#include "apple_tables.h"
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

// ---- a StyleWriter's stage (W-1103): the sheet named by the page's width, the
// engine's printable area kept, four ink planes handed over, Apple's tone and
// screen. Pages here are a few rows tall; the width is what picks the sheet.

// A one-page 24-bit sRGB URF at 360 dpi, w x h; px gives each pixel's r, g, b.
static size_t make_urf_rgb(uint8_t *out, uint32_t w, uint32_t h,
                           void (*px)(uint32_t x, uint32_t y, uint8_t rgb[3]))
{
    size_t n = 0;
    memcpy(out, "UNIRAST\0", 8); n = 8;
    out[n++] = 0; out[n++] = 0; out[n++] = 0; out[n++] = 1;
    uint8_t hdr[32] = { 0 };
    hdr[0] = 24; hdr[1] = 1;
    hdr[12] = (uint8_t)(w >> 24); hdr[13] = (uint8_t)(w >> 16); hdr[14] = (uint8_t)(w >> 8); hdr[15] = (uint8_t)w;
    hdr[16] = (uint8_t)(h >> 24); hdr[17] = (uint8_t)(h >> 16); hdr[18] = (uint8_t)(h >> 8); hdr[19] = (uint8_t)h;
    hdr[22] = (uint8_t)(360 >> 8); hdr[23] = (uint8_t)(360 & 255);
    memcpy(out + n, hdr, 32); n += 32;
    for (uint32_t y = 0; y < h; y++) {
        out[n++] = 0;
        for (uint32_t x = 0; x < w;) {
            uint32_t k = w - x < 128 ? w - x : 128;
            if (k == 1) out[n++] = 0; else out[n++] = (uint8_t)(257 - k);
            for (uint32_t i = 0; i < k; i++, n += 3) px(x + i, y, out + n);
            x += k;
        }
    }
    return n;
}

typedef struct {
    int      begins, ends, rows, ink_rows;
    uint32_t w, h;
    uint8_t  first[400];                 // the first gray row
    uint8_t  plane[IW_INKS][400];        // the first row's four planes
    uint8_t  all[40][400];               // every gray row, up to 40
    uint8_t  kall[40][400];              // every row's black plane
    uint8_t  call[40][400], mall[40][400], yall[40][400];
} wrec_t;

static int w_begin(void *ctx, int index, uint32_t w, uint32_t h)
{
    (void)index;
    wrec_t *r = ctx;
    r->begins++;
    r->w = w;
    r->h = h;
    return 0;
}
static void w_row(void *ctx, const uint8_t *bits)
{
    wrec_t *r = ctx;
    size_t nb = (r->w + 7) / 8;
    if (r->rows == 0) memcpy(r->first, bits, nb);
    if (r->rows < 40) memcpy(r->all[r->rows], bits, nb);
    r->rows++;
}
static void w_ink_row(void *ctx, const uint8_t *const bits[IW_INKS])
{
    wrec_t *r = ctx;
    size_t nb = (r->w + 7) / 8;
    if (r->ink_rows == 0)
        for (int i = 0; i < IW_INKS; i++) memcpy(r->plane[i], bits[i], nb);
    if (r->ink_rows < 40) {
        memcpy(r->kall[r->ink_rows], bits[IW_BLACK], nb);
        memcpy(r->call[r->ink_rows], bits[IW_CYAN], nb);
        memcpy(r->mall[r->ink_rows], bits[IW_MAGENTA], nb);
        memcpy(r->yall[r->ink_rows], bits[IW_YELLOW], nb);
    }
    r->ink_rows++;
}
static int w_end(void *ctx) { ((wrec_t *)ctx)->ends++; return 0; }

static int bit(const uint8_t *row, uint32_t x) { return !!(row[x >> 3] & (0x80 >> (x & 7))); }
static int popcount_row(const uint8_t *row, uint32_t w)
{
    int n = 0;
    for (uint32_t x = 0; x < w; x++) n += bit(row, x);
    return n;
}

static uint32_t g_white_cols[4];         // columns that are black in the test page
static int      g_n_black;
static uint8_t  g_flat = 255;

static uint8_t dots(uint32_t x, uint32_t y)
{
    (void)y;
    for (int i = 0; i < g_n_black; i++) if (x == g_white_cols[i]) return 0;
    return 255;
}
static uint8_t flat(uint32_t x, uint32_t y) { (void)x; (void)y; return g_flat; }

static uint8_t gray_page[1 << 17];          // 3060 dots a row, 20 rows at most
static uint8_t rgb_page[1 << 18];

// Rips a gray page of width w, 4 rows, black at the given source columns, for
// caps; returns the recorder's view.
static int rip_dots(const printer_caps_t *caps, uint32_t w, wrec_t *r, const uint32_t *black, int nb)
{
    g_n_black = nb;
    for (int i = 0; i < nb; i++) g_white_cols[i] = black[i];
    size_t n = make_urf(gray_page, w, 4, 360, dots);
    CHECK(n < sizeof gray_page);
    memset(r, 0, sizeof *r);
    rip_stage_t st = { w_begin, w_row, w_end, r, w_ink_row };
    ripjob_cfg_t cfg = { .dither = HT_THRESHOLD, .caps = caps, .stage = &st };
    return rip(gray_page, n, &cfg);
}

static void test_media_pick(void)
{
    wrec_t *r = calloc(1, sizeof *r);
    const printer_caps_t *ii = sw_caps(SW_II);

    // Letter's 3060: the engine's 72 dots of margin dropped on the left, 2880 kept.
    const uint32_t letter[] = { 71, 72, 2951, 2952 };
    CHECK_EQ_INT(rip_dots(ii, 3060, r, letter, 4), 0);
    CHECK_EQ_INT(r->w, 2880);
    CHECK_EQ_INT(r->h, 4);
    CHECK_EQ_INT(r->rows, 4);
    CHECK_EQ_INT(r->begins, 1);
    CHECK_EQ_INT(r->ends, 1);
    CHECK_EQ_INT(popcount_row(r->first, r->w), 2);
    CHECK(bit(r->first, 0));                      // source x 72
    CHECK(bit(r->first, 2879));                   // source x 2951

    // A4's 2976, and anything within 2 dots of it: 72 dropped, 2832 kept.
    const uint32_t a4[] = { 71, 72, 2903, 2904 };
    CHECK_EQ_INT(rip_dots(ii, 2976, r, a4, 4), 0);
    CHECK_EQ_INT(r->w, 2832);
    CHECK_EQ_INT(popcount_row(r->first, r->w), 2);
    CHECK(bit(r->first, 0) && bit(r->first, 2831));
    CHECK_EQ_INT(rip_dots(ii, 2977, r, a4, 0), 0);
    CHECK_EQ_INT(r->w, 2832);
    CHECK_EQ_INT(rip_dots(ii, 2974, r, a4, 0), 0);
    CHECK_EQ_INT(r->w, 2832);
    // Three dots away it is no sheet of the list's: the first (Letter) is used.
    CHECK_EQ_INT(rip_dots(ii, 2979, r, a4, 0), 0);
    CHECK_EQ_INT(r->w, 2880);

    // A 4 x 6 photo's 1440: no sheet, so Letter's; the page's own width ends the row.
    const uint32_t photo[] = { 71, 72, 1439 };
    CHECK_EQ_INT(rip_dots(ii, 1440, r, photo, 3), 0);
    CHECK_EQ_INT(r->w, 1368);
    CHECK_EQ_INT(popcount_row(r->first, r->w), 2);
    CHECK(bit(r->first, 0) && bit(r->first, 1367));

    // The Color StyleWriters print 2916 of Letter's 3060.
    const printer_caps_t *c25 = sw_caps(SW_C2500);
    const uint32_t cs[] = { 71, 72, 2987, 2988 };
    CHECK_EQ_INT(rip_dots(c25, 3060, r, cs, 4), 0);
    CHECK_EQ_INT(r->w, 2916);
    CHECK_EQ_INT(popcount_row(r->first, r->w), 2);
    CHECK(bit(r->first, 0) && bit(r->first, 2915));
    CHECK_EQ_INT(rip_dots(c25, 2976, r, a4, 0), 0);
    CHECK_EQ_INT(r->w, 2832);

    // A page narrower than the left margin has nothing to drop: it is kept whole.
    CHECK_EQ_INT(rip_dots(ii, 60, r, a4, 0), 0);
    CHECK_EQ_INT(r->w, 60);
    free(r);
}

static void px_inks(uint32_t x, uint32_t y, uint8_t rgb[3])
{
    (void)y;
    uint8_t v[3] = { 255, 255, 255 };
    switch (x) {
    case 72: v[0] = 0; break;                                  // cyan: red taken out
    case 73: v[0] = 0; v[1] = 0; v[2] = 0; break;              // black
    case 74: v[1] = 0; v[2] = 0; break;                        // red: magenta and yellow
    case 75: v[0] = 0; v[2] = 0; break;                        // green: cyan and yellow
    default: break;
    }
    memcpy(rgb, v, 3);
}

static void test_ink_rows(void)
{
    wrec_t *r = calloc(1, sizeof *r);
    const printer_caps_t *c25 = sw_caps(SW_C2500);
    size_t n = make_urf_rgb(rgb_page, 3060, 4, px_inks);
    CHECK(n < sizeof rgb_page);

    // A stage that takes four planes gets them, and no gray row and no ImageWriter bytes.
    rip_stage_t st = { w_begin, w_row, w_end, r, w_ink_row };
    ripjob_cfg_t cfg = { .out = count, .dither = HT_THRESHOLD, .caps = c25, .stage = &st, .colour = 1 };
    out_n = 0;
    CHECK_EQ_INT(rip(rgb_page, n, &cfg), 0);
    CHECK_EQ_INT(r->w, 2916);
    CHECK_EQ_INT(r->h, 4);
    CHECK_EQ_INT(r->ink_rows, 4);
    CHECK_EQ_INT(r->rows, 0);
    CHECK_EQ_INT(r->ends, 1);
    CHECK_EQ_INT(out_n, 0);
    // Source x 72 is column 0: cyan; 73 black; 74 red is magenta and yellow; 75 green is cyan and yellow.
    CHECK_EQ_INT(popcount_row(r->plane[IW_CYAN], r->w), 2);
    CHECK(bit(r->plane[IW_CYAN], 0) && bit(r->plane[IW_CYAN], 3));
    CHECK_EQ_INT(popcount_row(r->plane[IW_BLACK], r->w), 1);
    CHECK(bit(r->plane[IW_BLACK], 1));
    CHECK_EQ_INT(popcount_row(r->plane[IW_MAGENTA], r->w), 1);
    CHECK(bit(r->plane[IW_MAGENTA], 2));
    CHECK_EQ_INT(popcount_row(r->plane[IW_YELLOW], r->w), 2);
    CHECK(bit(r->plane[IW_YELLOW], 2) && bit(r->plane[IW_YELLOW], 3));

    // Colour off: the page is gray, as a black cartridge prints it.
    memset(r, 0, sizeof *r);
    cfg.colour = 0;
    CHECK_EQ_INT(rip(rgb_page, n, &cfg), 0);
    CHECK_EQ_INT(r->ink_rows, 0);
    CHECK_EQ_INT(r->rows, 4);

    // A stage with no four-plane hook (a LaserWriter's) gets gray, whatever colour says.
    memset(r, 0, sizeof *r);
    rip_stage_t gray_only = { w_begin, w_row, w_end, r, NULL };
    cfg.colour = 1;
    cfg.stage = &gray_only;
    CHECK_EQ_INT(rip(rgb_page, n, &cfg), 0);
    CHECK_EQ_INT(r->ink_rows, 0);
    CHECK_EQ_INT(r->rows, 4);
    free(r);
}

static void px_flat_rgb(uint32_t x, uint32_t y, uint8_t rgb[3])
{
    (void)x; (void)y;
    rgb[0] = rgb[1] = rgb[2] = g_flat;
}

static uint8_t g_rgb[3];
static void px_const_rgb(uint32_t x, uint32_t y, uint8_t rgb[3])
{
    (void)x; (void)y;
    memcpy(rgb, g_rgb, 3);
}

static uint8_t k_zero[256];

static void test_tone(void)
{
    wrec_t *r = calloc(1, sizeof *r);
    const printer_caps_t *ii = sw_caps(SW_II);
    g_flat = 250;                                  // nearly paper: no ink on a threshold
    size_t n = make_urf(gray_page, 3060, 4, 360, flat);
    CHECK(n < sizeof gray_page);
    rip_stage_t st = { w_begin, w_row, w_end, r, w_ink_row };
    ripjob_cfg_t cfg = { .dither = HT_THRESHOLD, .caps = ii, .stage = &st };
    CHECK_EQ_INT(rip(gray_page, n, &cfg), 0);
    CHECK_EQ_INT(popcount_row(r->first, r->w), 0);

    // A curve that turns every gray black puts ink everywhere.
    memset(r, 0, sizeof *r);
    cfg.tone = k_zero;
    CHECK_EQ_INT(rip(gray_page, n, &cfg), 0);
    CHECK_EQ_INT(popcount_row(r->first, r->w), (int)r->w);

    // The dot gain curve goes on top of a tone curve and a tone curve of none changes nothing.
    memset(r, 0, sizeof *r);
    cfg.tone = NULL;
    cfg.gain = 20;
    CHECK_EQ_INT(rip(gray_page, n, &cfg), 0);
    CHECK_EQ_INT(popcount_row(r->first, r->w), 0);

    // A colour page: the curve is put on R, G and B before the split.
    size_t m = make_urf_rgb(rgb_page, 3060, 4, px_flat_rgb);
    CHECK(m < sizeof rgb_page);
    rip_stage_t cst = { w_begin, w_row, w_end, r, w_ink_row };
    ripjob_cfg_t ccfg = { .dither = HT_THRESHOLD, .caps = sw_caps(SW_C2500), .stage = &cst, .colour = 1 };
    memset(r, 0, sizeof *r);
    CHECK_EQ_INT(rip(rgb_page, m, &ccfg), 0);
    CHECK_EQ_INT(popcount_row(r->plane[IW_BLACK], r->w), 0);
    memset(r, 0, sizeof *r);
    ccfg.tone = k_zero;
    CHECK_EQ_INT(rip(rgb_page, m, &ccfg), 0);
    CHECK_EQ_INT(popcount_row(r->plane[IW_BLACK], r->w), (int)r->w);
    CHECK_EQ_INT(popcount_row(r->plane[IW_CYAN], r->w), 0);
    CHECK_EQ_INT(popcount_row(r->plane[IW_MAGENTA], r->w), 0);
    CHECK_EQ_INT(popcount_row(r->plane[IW_YELLOW], r->w), 0);
    free(r);
}

// Apple's screen as the drivers read it (apple_tables.h): a gray page prints on the
// black ink's placement, its tile turned over, and every page starts on the tile's
// second row. Written out here from the table, not from the halftoner.
static void test_apple(void)
{
    wrec_t *r = calloc(1, sizeof *r);
    g_flat = 128;
    size_t n = make_urf(gray_page, 3060, 20, 360, flat);
    CHECK(n < sizeof gray_page);
    rip_stage_t st = { w_begin, w_row, w_end, r, w_ink_row };
    ripjob_cfg_t cfg = { .dither = HT_APPLE, .caps = sw_caps(SW_II), .stage = &st };
    CHECK_EQ_INT(rip(gray_page, n, &cfg), 0);
    CHECK_EQ_INT(r->rows, 20);
    int bad = 0, ink = 0;
    for (uint32_t y = 0; y < 20; y++)
        for (uint32_t x = 0; x < r->w; x++) {
            int t = 254 - apple_screen16[(((y + 1) & 15) << 4) | (x & 15)];
            int want = 255 - 128 > t;
            ink += want;
            bad += bit(r->all[y], x) != want;
        }
    CHECK_EQ_INT(bad, 0);
    CHECK(ink > 20 * (int)r->w / 4);               // about half the cells, not none or all

    // A colour page: yellow reads the tile as it is, black turned over, from the second row.
    static const struct { uint8_t rgb[3]; iw_ink_t ink; int inverted; } k[] = {
        { { 255, 255, 127 }, IW_YELLOW, 0 },
        { { 127, 255, 255 }, IW_CYAN, 0 },
        { { 255, 127, 255 }, IW_MAGENTA, 0 },
        { { 127, 127, 127 }, IW_BLACK, 1 },
    };
    ripjob_cfg_t ccfg = { .dither = HT_APPLE, .caps = sw_caps(SW_C2500), .stage = &st, .colour = 1 };
    for (size_t i = 0; i < sizeof k / sizeof k[0]; i++) {
        memcpy(g_rgb, k[i].rgb, 3);
        size_t m = make_urf_rgb(rgb_page, 3060, 20, px_const_rgb);
        CHECK(m < sizeof rgb_page);
        memset(r, 0, sizeof *r);
        CHECK_EQ_INT(rip(rgb_page, m, &ccfg), 0);
        CHECK_EQ_INT(r->ink_rows, 20);
        const uint8_t (*rows)[400] = k[i].ink == IW_BLACK ? r->kall : k[i].ink == IW_CYAN ? r->call
                                    : k[i].ink == IW_MAGENTA ? r->mall : r->yall;
        int bad2 = 0, on = 0;
        for (uint32_t y = 0; y < 20; y++)
            for (uint32_t x = 0; x < r->w; x++) {
                int t = apple_screen16[(((y + 1) & 15) << 4) | (x & 15)];
                if (k[i].inverted) t = 254 - t;
                int want = 128 > t;
                on += want;
                bad2 += bit(rows[y], x) != want;
            }
        CHECK_EQ_INT(bad2, 0);
        CHECK(on > 0);
    }
    free(r);
}

// The other screens read their tiles from the page's first row, as they always have.
static void test_other_screens_unmoved(void)
{
    wrec_t *r = calloc(1, sizeof *r);
    static const ht_algo_t algos[] = { HT_BLUENOISE, HT_BAYER, HT_ATKINSON, HT_FLOYD, HT_THRESHOLD };
    g_flat = 100;
    size_t n = make_urf(gray_page, 3060, 12, 360, flat);
    CHECK(n < sizeof gray_page);
    for (size_t a = 0; a < sizeof algos / sizeof algos[0]; a++) {
        rip_stage_t st = { w_begin, w_row, w_end, r, w_ink_row };
        ripjob_cfg_t cfg = { .dither = algos[a], .caps = sw_caps(SW_II), .stage = &st };
        memset(r, 0, sizeof *r);
        CHECK_EQ_INT(rip(gray_page, n, &cfg), 0);
        ht_t h;
        CHECK_EQ_INT(ht_init(&h, algos[a], 2880), 0);
        uint8_t src[2880], want[360];
        memset(src, 100, sizeof src);
        int bad = 0;
        for (uint32_t y = 0; y < 12; y++) {
            ht_row(&h, src, want);
            bad += memcmp(want, r->all[y], 360) != 0;
        }
        ht_free(&h);
        CHECK_EQ_INT(bad, 0);
    }
    free(r);
}

int main(void)
{
    test_media_pick();
    test_ink_rows();
    test_tone();
    test_apple();
    test_other_screens_unmoved();

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
    rip_stage_t st = { rec_begin, rec_row, rec_end, &r, NULL };
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
    rip_stage_t cst = { rec_begin, rec_row, rec_end, &c, NULL };
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
