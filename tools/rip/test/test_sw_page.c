// SPDX-License-Identifier: GPL-3.0-or-later
//
// sw_page (W-1103): the StyleWriter's page stage. Rows are fed to it as ripjob
// would, then the frames it wrote are decoded by a reader written here from the
// design's wire rules (sw_decode_row for the rows themselves) and held to the
// rows that went in. Expected rects and margins are written out from the design's
// table, not read back from the model table. A band starts at the first row with
// ink after the last band ended and runs the band height from there, clamped to
// the printable area's last row (the stage holds no page: R15).
#include "test_util.h"
#include "sw_page.h"
#include "sw_code.h"
#include "printer_caps.h"
#include <stdlib.h>

// ---- what the stage writes

static uint8_t *g_buf;
static size_t   g_n, g_cap;
static int      g_frames, g_malformed;

static void sink(void *ctx, const uint8_t *f, size_t n)
{
    (void)ctx;
    // One whole frame a call: type, u16 LE length, payload.
    if (n < 3 || f[0] < SWF_PAGE || f[0] > SWF_PAGE_END || n != 3u + (f[1] | (f[2] << 8))) g_malformed++;
    if (g_n + n > g_cap) { g_cap = (g_n + n) * 2; g_buf = realloc(g_buf, g_cap); }
    memcpy(g_buf + g_n, f, n);
    g_n += n;
    g_frames++;
}

static void sink_reset(void) { g_n = 0; g_frames = 0; g_malformed = 0; }

// ---- the reader

enum { MAX_RECTS = 64, MAX_BLOCKS = 4096, MAX_PAGES = 4 };

typedef struct {
    int      pages, page_ends, rects, blocks;
    int      page_flag[MAX_PAGES];
    struct { uint8_t kind; uint16_t l, t, r, b; int blocks; } rect[MAX_RECTS];
    uint32_t block_len[MAX_BLOCKS];
    int      block_rect[MAX_BLOCKS];
    size_t   block_off[MAX_BLOCKS];          // where in g_buf a block's data starts
    int      bad;                            // a stream that does not parse, or rows that do not decode
    int      mid_row_ends;                   // a block that did not end on a whole row
    uint8_t *img[MAX_PAGES][4];              // C, M, Y, K: nrows x rowbytes each
    uint32_t nrows, rowbytes;
} dec_t;

static void dec_free(dec_t *d)
{
    for (int p = 0; p < MAX_PAGES; p++)
        for (int k = 0; k < 4; k++) free(d->img[p][k]);
    memset(d, 0, sizeof *d);
}

// Reads g_buf for `model`; the printable page is nrows x rowbytes, white until a rect puts ink in.
static void decode(dec_t *d, sw_model_t model, uint32_t nrows, uint32_t rowbytes)
{
    const sw_spec_t *s = sw_spec(model);
    memset(d, 0, sizeof *d);
    d->nrows = nrows;
    d->rowbytes = rowbytes;
    int cur = -1;
    // The rect being filled.
    int in_rect = 0, planes = 0, plane = 0;
    uint32_t row = 0, t = 0, rb = 0;
    static const int ink_of[4] = { 0, 1, 2, 3 };       // C, M, Y, K: the wire order
    uint8_t *tmp = malloc(rowbytes + 8);
    for (size_t pos = 0; pos + 3 <= g_n;) {
        uint8_t type = g_buf[pos];
        size_t len = g_buf[pos + 1] | (g_buf[pos + 2] << 8);
        const uint8_t *pl = g_buf + pos + 3;
        if (pos + 3 + len > g_n) { d->bad++; break; }
        switch (type) {
        case SWF_PAGE:
            if (d->pages < MAX_PAGES) {
                d->page_flag[d->pages] = pl[0];
                for (int k = 0; k < 4; k++) d->img[d->pages][k] = calloc(nrows, rowbytes);
            }
            cur = d->pages++;
            in_rect = 0;
            break;
        case SWF_RECT: {
            if (len != 9 || cur < 0 || d->rects >= MAX_RECTS) { d->bad++; break; }
            int i = d->rects++;
            d->rect[i].kind = pl[0];
            d->rect[i].l = (uint16_t)(pl[1] | (pl[2] << 8));
            d->rect[i].t = (uint16_t)(pl[3] | (pl[4] << 8));
            d->rect[i].r = (uint16_t)(pl[5] | (pl[6] << 8));
            d->rect[i].b = (uint16_t)(pl[7] | (pl[8] << 8));
            if (pl[0] != 'R' && pl[0] != 'c') d->bad++;
            planes = pl[0] == 'c' ? 4 : 1;
            in_rect = 1;
            row = 0;
            plane = 0;
            t = d->rect[i].t;
            rb = ((uint32_t)d->rect[i].r - d->rect[i].l + 1 + 7) / 8;
            break;
        }
        case SWF_BLOCK: {
            if (!in_rect || d->blocks >= MAX_BLOCKS) { d->bad++; break; }
            int bi = d->blocks++;
            d->block_len[bi] = (uint32_t)len;
            d->block_rect[bi] = d->rects - 1;
            d->block_off[bi] = pos + 3;
            d->rect[d->rects - 1].blocks++;
            size_t used = 0;
            while (used < len) {
                uint32_t y = t + row;
                if (cur >= MAX_PAGES || y >= nrows || rb > rowbytes) { d->bad++; break; }
                int ink = planes == 4 ? ink_of[plane] : 3;
                uint8_t *dst = d->img[cur][ink] + (size_t)y * rowbytes;
                const uint8_t *prev = (s->delta && row > 0) ? dst - rowbytes : NULL;
                size_t u = sw_decode_row(pl + used, len - used, prev, rb, tmp);
                if (!u) { d->bad++; break; }
                memcpy(dst, tmp, rb);
                used += u;
                if (++plane == planes) { plane = 0; row++; }
            }
            if (plane != 0) d->mid_row_ends++;
            break;
        }
        case SWF_PAGE_END:
            d->page_ends++;
            in_rect = 0;
            break;
        default:
            d->bad++;
        }
        pos += 3 + len;
    }
    free(tmp);
}

// ---- feeding rows

typedef void (*fill_fn)(uint32_t y, int ink, uint8_t *row, uint32_t rowbytes);

static void fill_blank(uint32_t y, int ink, uint8_t *row, uint32_t rb)
{
    (void)y; (void)ink;
    memset(row, 0, rb);
}

static uint32_t lcg(uint32_t *v) { *v = *v * 1103515245u + 12345u; return *v >> 16; }

static void fill_noise(uint32_t y, int ink, uint8_t *row, uint32_t rb)
{
    uint32_t v = y * 31u + (uint32_t)ink * 7919u + 1u;
    for (uint32_t i = 0; i < rb; i++) row[i] = (uint8_t)lcg(&v);
}

// The same noise on every row (and in every plane): the rows after the first are
// all-white deltas on a model that XORs, and a full row each on the original.
static void fill_same_noise(uint32_t y, int ink, uint8_t *row, uint32_t rb)
{
    (void)y; (void)ink;
    uint32_t v = 99;
    for (uint32_t i = 0; i < rb; i++) row[i] = (uint8_t)lcg(&v);
}

static void set_bit(uint8_t *row, uint32_t x) { row[x >> 3] |= (uint8_t)(0x80 >> (x & 7)); }

// A few dots, each at a source row (the stage's, margins counted) and column.
typedef struct { uint32_t y, x; int ink; } dot_t;
static const dot_t *g_dots;
static int g_ndots;

static void fill_dots(uint32_t y, int ink, uint8_t *row, uint32_t rb)
{
    memset(row, 0, rb);
    for (int i = 0; i < g_ndots; i++)
        if (g_dots[i].y == y && g_dots[i].ink == ink && g_dots[i].x < rb * 8) set_bit(row, g_dots[i].x);
}

// Pushes a page of w x h rows through the stage the way ripjob does: page_begin, a
// row call per row, page_end. `ink` feeds four planes a row (iw_ink_t order, as ripjob).
static void feed(rip_stage_t *st, int index, uint32_t w, uint32_t h, int ink, fill_fn fill)
{
    uint32_t rb = (w + 7) / 8;
    uint8_t *planes[IW_INKS];
    for (int k = 0; k < IW_INKS; k++) planes[k] = malloc(rb);
    CHECK_EQ_INT(st->page_begin(st->ctx, index, w, h), 0);
    for (uint32_t y = 0; y < h; y++) {
        for (int k = 0; k < IW_INKS; k++) {
            fill(y, k, planes[k], rb);
            // A row's last byte carries no bits past the width, as the halftoner leaves it.
            if (w & 7) planes[k][rb - 1] &= (uint8_t)(0xFF << (8 - (w & 7)));
        }
        if (ink) st->ink_row(st->ctx, (const uint8_t *const *)planes);
        else     st->row(st->ctx, planes[IW_BLACK]);
    }
    CHECK_EQ_INT(st->page_end(st->ctx), 0);
    for (int k = 0; k < IW_INKS; k++) free(planes[k]);
}

// The expected printable image of a page fed with `fill`: source rows top .. h - bottom,
// the first pw columns. planes: 1 for K only, 4 for C, M, Y, K (wire order).
static uint8_t *expect(uint32_t w, uint32_t h, uint32_t top, uint32_t bottom, uint32_t pw,
                       fill_fn fill, int k_wire, uint32_t rowbytes)
{
    uint32_t nrows = h > top + bottom ? h - top - bottom : 0;
    uint8_t *img = calloc(nrows ? nrows : 1, rowbytes);
    uint32_t rb = (w + 7) / 8;
    uint8_t *row = malloc(rb);
    static const int ink_of_wire[4] = { IW_CYAN, IW_MAGENTA, IW_YELLOW, IW_BLACK };
    for (uint32_t y = 0; y < nrows; y++) {
        fill(top + y, ink_of_wire[k_wire], row, rb);
        for (uint32_t x = 0; x < pw; x++)
            if (row[x >> 3] & (0x80 >> (x & 7))) set_bit(img + (size_t)y * rowbytes, x);
    }
    free(row);
    return img;
}

static void stage_for(sw_page_t **pp, rip_stage_t *st, sw_model_t m, int colour_cart)
{
    sw_page_cfg_t cfg = { .model = m, .colour_cart = colour_cart, .out = sink };
    *pp = sw_page_new(&cfg);
    CHECK(*pp != NULL);
    if (*pp) sw_page_stage(*pp, st);
}

// ---- tests

static void test_letter_ii(void)
{
    // Letter 3060 x 3960 at 360 dpi, the II's 2880 across: 72 rows above, 114 below.
    sw_page_t *p;
    rip_stage_t st;
    stage_for(&p, &st, SW_II, 0);
    if (!p) return;
    static const dot_t dots[] = {
        { 71, 100, IW_BLACK },        // the margin above: dropped
        { 72, 100, IW_BLACK },        // printable row 0
        { 900, 2879, IW_BLACK },      // row 828: first ink after the first band, last column
        { 1700, 0, IW_BLACK },        // row 1628: first ink after the second band
        { 3845, 5, IW_BLACK },        // row 3773: the last printable row
        { 3846, 5, IW_BLACK },        // the margin below: dropped
    };
    g_dots = dots; g_ndots = 6;
    sink_reset();
    feed(&st, 0, 2880, 3960, 0, fill_dots);
    CHECK_EQ_INT(g_malformed, 0);

    dec_t d;
    decode(&d, SW_II, 3774, 360);
    CHECK_EQ_INT(d.bad, 0);
    CHECK_EQ_INT(d.mid_row_ends, 0);
    CHECK_EQ_INT(d.pages, 1);
    CHECK_EQ_INT(d.page_ends, 1);
    CHECK_EQ_INT(d.page_flag[0], 0);
    // A band starts at its first row with ink and is 800 rows from there, or as far as the
    // printable area goes: the rows between bands send nothing, and the last band is one row.
    CHECK_EQ_INT(d.rects, 4);
    static const struct { uint16_t t, b; } want[4] = { { 0, 799 }, { 828, 1627 }, { 1628, 2427 }, { 3773, 3773 } };
    for (int i = 0; i < d.rects && i < 4; i++) {
        CHECK_EQ_INT(d.rect[i].kind, 'R');
        CHECK_EQ_INT(d.rect[i].l, 16);                 // the II's rects start 16 in
        CHECK_EQ_INT(d.rect[i].r, 16 + 2879);
        CHECK_EQ_INT(d.rect[i].t, want[i].t);
        CHECK_EQ_INT(d.rect[i].b, want[i].b);
        CHECK_EQ_INT(d.rect[i].blocks, 1);
    }
    CHECK_EQ_INT(g_frames, 1 + 4 * 2 + 1);             // page, four rects with a block each, end
    uint8_t *want_k = expect(2880, 3960, 72, 114, 2880, fill_dots, 3, 360);
    CHECK_EQ_MEM(d.img[0][3], want_k, 3774 * 360);
    for (int k = 0; k < 3; k++)
        for (size_t i = 0; i < 3774u * 360; i++)
            if (d.img[0][k][i]) { CHECK(0); break; }
    free(want_k);
    dec_free(&d);
    sw_page_free(p);
}

static void test_blank_and_one_dot(void)
{
    sw_page_t *p;
    rip_stage_t st;
    stage_for(&p, &st, SW_II, 0);
    if (!p) return;

    // A white page: its start and its end, nothing between.
    sink_reset();
    feed(&st, 0, 2880, 3960, 0, fill_blank);
    CHECK_EQ_INT(g_frames, 2);
    CHECK_EQ_INT(g_buf[0], SWF_PAGE);
    CHECK_EQ_INT(g_buf[3], 0);
    CHECK_EQ_INT(g_buf[4], SWF_PAGE_END);
    CHECK_EQ_INT(g_n, 4 + 3);

    // One black dot: one rect and one block.
    static const dot_t dot[] = { { 600, 1234, IW_BLACK } };       // printable row 528
    g_dots = dot; g_ndots = 1;
    sink_reset();
    feed(&st, 1, 2880, 3960, 0, fill_dots);
    dec_t d;
    decode(&d, SW_II, 3774, 360);
    CHECK_EQ_INT(d.bad, 0);
    CHECK_EQ_INT(d.rects, 1);
    CHECK_EQ_INT(d.blocks, 1);
    CHECK_EQ_INT(d.rect[0].t, 528);                    // the rect starts at the dot's row
    CHECK_EQ_INT(d.rect[0].b, 528 + 799);
    CHECK_EQ_INT(d.img[0][3][(600 - 72) * 360 + 1234 / 8], 0x80 >> (1234 % 8));
    // The frames, byte for byte: page, rect, block, end.
    CHECK_EQ_INT(g_frames, 4);
    CHECK_EQ_INT(g_buf[0], SWF_PAGE);
    CHECK_EQ_INT(g_buf[1], 1);
    CHECK_EQ_INT(g_buf[2], 0);
    CHECK_EQ_INT(g_buf[3], 0);
    // R, left 16, top 528 = 0x0210, right 16 + 2879 = 0x0B4F, bottom 1327 = 0x052F, little-endian.
    static const uint8_t rect[] = { SWF_RECT, 9, 0, 'R', 0x10, 0x00, 0x10, 0x02, 0x4F, 0x0B, 0x2F, 0x05 };
    CHECK_EQ_MEM(g_buf + 4, rect, sizeof rect);
    // The band's 800 rows are all coded, the blank ones that end it too: the dot's row raw, the
    // row after it back to white as a delta, and then a lone 80 for each of the 798 left.
    {
        const uint8_t *blk = g_buf + d.block_off[0];
        size_t used = 0;
        int rows = 0;
        uint8_t tmp[360];
        while (used < d.block_len[0]) {
            size_t u = sw_decode_row(blk + used, d.block_len[0] - used, NULL, 360, tmp);
            if (!u) break;
            used += u;
            rows++;
        }
        CHECK_EQ_INT(rows, 800);
        CHECK_EQ_INT(used, d.block_len[0]);
        int ends_white = 1;
        for (size_t i = d.block_len[0] - 798; i < d.block_len[0]; i++) ends_white &= blk[i] == 0x80;
        CHECK(ends_white);
        CHECK(blk[d.block_len[0] - 799] != 0x80);       // the row before them is the delta that clears the dot
    }
    dec_free(&d);
    sw_page_free(p);
}

static void test_splits_blocks(void)
{
    // Noise in two bands: a band's coded size passes the block cap, so it splits into
    // blocks of whole rows, each within sw_block_cap, the delta carried across them.
    sw_page_t *p;
    rip_stage_t st;
    stage_for(&p, &st, SW_II, 0);
    if (!p) return;
    sink_reset();
    feed(&st, 0, 2880, 1000, 0, fill_noise);           // 1000 rows: 814 printable
    dec_t d;
    decode(&d, SW_II, 814, 360);
    CHECK_EQ_INT(d.bad, 0);
    CHECK_EQ_INT(d.mid_row_ends, 0);
    CHECK_EQ_INT(d.rects, 2);                          // 800 + 14 rows
    CHECK(d.rect[0].blocks > 5);
    CHECK_EQ_INT(d.rect[0].b, 799);
    uint32_t cap = sw_block_cap(SW_II, 1);
    uint32_t row_max = (uint32_t)sw_code_max(360);
    for (int i = 0; i < d.blocks; i++) {
        CHECK(d.block_len[i] <= cap);
        // Cut only when the next row might not fit: a block is nearly full unless it is a rect's last.
        int last_of_rect = i + 1 == d.blocks || d.block_rect[i + 1] != d.block_rect[i];
        if (!last_of_rect) CHECK(d.block_len[i] + row_max > cap);
    }
    uint8_t *want_k = expect(2880, 1000, 72, 114, 2880, fill_noise, 3, 360);
    CHECK_EQ_MEM(d.img[0][3], want_k, 814 * 360);

    // The first row of the rect's second block is coded against the row before it: it
    // decodes to the fed row with that row, and to something else without it.
    int second = -1;
    for (int i = 0; i < d.blocks; i++)
        if (d.block_rect[i] == 0) { second = i + 1; break; }
    CHECK(second > 0 && second < d.blocks && d.block_rect[second] == 0);
    if (second > 0 && second < d.blocks) {
        uint32_t first_row = 0;
        for (int i = 0; i < second; i++) {
            if (d.block_rect[i] != 0) continue;
            // rows in the earlier blocks of this rect
            size_t used = 0;
            const uint8_t *blk = g_buf + d.block_off[i];
            uint8_t tmp[360];
            while (used < d.block_len[i]) {
                size_t u = sw_decode_row(blk + used, d.block_len[i] - used, NULL, 360, tmp);
                used += u;
                first_row++;
            }
        }
        const uint8_t *fed_prev = want_k + (size_t)(first_row - 1) * 360;
        const uint8_t *fed_this = want_k + (size_t)first_row * 360;
        uint8_t got[360];
        const uint8_t *blk = g_buf + d.block_off[second];
        CHECK(sw_decode_row(blk, d.block_len[second], fed_prev, 360, got) > 0);
        CHECK_EQ_MEM(got, fed_this, 360);
        CHECK(sw_decode_row(blk, d.block_len[second], NULL, 360, got) > 0);
        CHECK(memcmp(got, fed_this, 360) != 0);
    }
    free(want_k);
    dec_free(&d);
    sw_page_free(p);
}

static void test_colour_c2500(void)
{
    // Colour: the 2500 prints 2916 across with 45 rows above and 99 below, 400-row bands.
    sw_page_t *p;
    rip_stage_t st;
    stage_for(&p, &st, SW_C2500, 1);
    if (!p) return;
    static const dot_t dots[] = {
        { 55, 10, IW_CYAN }, { 55, 20, IW_MAGENTA }, { 55, 30, IW_YELLOW }, { 55, 40, IW_BLACK },
        { 550, 2915, IW_YELLOW },            // printable row 505: after the first band, only yellow
        { 3860, 7, IW_BLACK },               // printable row 3815: the last
        { 3861, 8, IW_BLACK },               // the 99 rows below: dropped
    };
    g_dots = dots; g_ndots = 7;
    sink_reset();
    feed(&st, 0, 2916, 3960, 1, fill_dots);
    CHECK_EQ_INT(g_malformed, 0);
    dec_t d;
    decode(&d, SW_C2500, 3816, 365);
    CHECK_EQ_INT(d.bad, 0);
    CHECK_EQ_INT(d.mid_row_ends, 0);
    CHECK_EQ_INT(d.page_flag[0], 1);
    CHECK_EQ_INT(d.rects, 3);
    static const struct { uint16_t t, b; } want[3] = { { 10, 409 }, { 505, 904 }, { 3815, 3815 } };
    for (int i = 0; i < d.rects && i < 3; i++) {
        CHECK_EQ_INT(d.rect[i].kind, 'c');
        CHECK_EQ_INT(d.rect[i].l, 0);                  // the 2500 has no offset
        CHECK_EQ_INT(d.rect[i].r, 2915);
        CHECK_EQ_INT(d.rect[i].t, want[i].t);
        CHECK_EQ_INT(d.rect[i].b, want[i].b);
    }
    // C, M, Y, K on the wire: the band starts at row 10 (source 55), whose four planes come
    // first and raw, in that order: C has a dot at 10, M at 20, Y at 30, K at 40.
    const uint8_t *blk = g_buf + d.block_off[0];
    uint8_t tmp[365];
    size_t used = 0, u;
    static const int col[4] = { 10, 20, 30, 40 };
    for (int k = 0; k < 4; k++) {
        u = sw_decode_row(blk + used, d.block_len[0] - used, NULL, 365, tmp);
        CHECK(u > 1);
        for (int x = 0; x < 365 * 8; x++)
            CHECK_EQ_INT(!!(tmp[x >> 3] & (0x80 >> (x & 7))), x == col[k] ? 1 : 0);
        used += u;
    }
    uint8_t *w[4];
    for (int k = 0; k < 4; k++) {
        w[k] = expect(2916, 3960, 45, 99, 2916, fill_dots, k, 365);
        CHECK_EQ_MEM(d.img[0][k], w[k], 3816 * 365);
        free(w[k]);
    }
    dec_free(&d);

    // The same rows with a black cartridge: R rects, the K plane only; a band whose
    // only ink is cyan, magenta or yellow has nothing to print.
    sw_page_free(p);
    stage_for(&p, &st, SW_C2500, 0);
    if (!p) return;
    sink_reset();
    feed(&st, 0, 2916, 3960, 1, fill_dots);
    decode(&d, SW_C2500, 3816, 365);
    CHECK_EQ_INT(d.bad, 0);
    CHECK_EQ_INT(d.page_flag[0], 0);
    CHECK_EQ_INT(d.rects, 2);                          // the yellow-only row has no K ink to start a band
    CHECK_EQ_INT(d.rect[0].kind, 'R');
    CHECK_EQ_INT(d.rect[0].t, 10);
    CHECK_EQ_INT(d.rect[0].b, 409);
    CHECK_EQ_INT(d.rect[1].t, 3815);
    CHECK_EQ_INT(d.rect[1].b, 3815);
    uint8_t *wk = expect(2916, 3960, 45, 99, 2916, fill_dots, 3, 365);
    CHECK_EQ_MEM(d.img[0][3], wk, 3816 * 365);
    free(wk);
    for (int k = 0; k < 3; k++)
        for (size_t i = 0; i < 3816u * 365; i++)
            if (d.img[0][k][i]) { CHECK(0); break; }
    dec_free(&d);

    // A colour cartridge and a gray page (row(), not ink_row()): black rects, K only.
    sw_page_free(p);
    stage_for(&p, &st, SW_C2500, 1);
    if (!p) return;
    sink_reset();
    feed(&st, 0, 2916, 3960, 0, fill_dots);
    decode(&d, SW_C2500, 3816, 365);
    CHECK_EQ_INT(d.bad, 0);
    CHECK_EQ_INT(d.page_flag[0], 0);
    CHECK_EQ_INT(d.rects, 2);
    CHECK_EQ_INT(d.rect[0].kind, 'R');
    dec_free(&d);

    // A model with no colour cartridge ignores colour_cart.
    sw_page_free(p);
    stage_for(&p, &st, SW_II, 1);
    if (!p) return;
    sink_reset();
    feed(&st, 0, 2880, 3960, 1, fill_dots);
    decode(&d, SW_II, 3774, 360);
    CHECK_EQ_INT(d.page_flag[0], 0);
    for (int i = 0; i < d.rects; i++) CHECK_EQ_INT(d.rect[i].kind, 'R');
    dec_free(&d);
    sw_page_free(p);
}

static void test_original_raw(void)
{
    // The original takes no delta: every row is coded on its own, and its rects are not offset.
    sw_page_t *p;
    rip_stage_t st;
    stage_for(&p, &st, SW_ORIGINAL, 0);
    if (!p) return;
    sink_reset();
    feed(&st, 0, 2880, 3960, 0, fill_same_noise);
    dec_t d;
    decode(&d, SW_ORIGINAL, 3774, 360);
    CHECK_EQ_INT(d.bad, 0);
    CHECK_EQ_INT(d.rects, 5);
    CHECK_EQ_INT(d.rect[0].l, 0);
    CHECK_EQ_INT(d.rect[0].r, 2879);
    uint32_t cap = sw_block_cap(SW_ORIGINAL, 1);
    CHECK(d.blocks > 100);                             // 3774 dense rows at 8 KB a block
    uint32_t total = 0;
    for (int i = 0; i < d.blocks; i++) { CHECK(d.block_len[i] <= cap); total += d.block_len[i]; }
    CHECK(total > 3774u * 300);                        // no row shrank to a delta
    // Every row decodes with no row before it.
    uint8_t one[360];
    fill_same_noise(0, IW_BLACK, one, 360);
    uint8_t tmp[360];
    int bad = 0;
    for (int i = 0; i < d.blocks; i++) {
        size_t used = 0;
        const uint8_t *blk = g_buf + d.block_off[i];
        while (used < d.block_len[i]) {
            size_t u = sw_decode_row(blk + used, d.block_len[i] - used, NULL, 360, tmp);
            if (!u || memcmp(tmp, one, 360)) { bad++; break; }
            used += u;
        }
    }
    CHECK_EQ_INT(bad, 0);
    dec_free(&d);
    sw_page_free(p);

    // The II, fed the same rows, codes all but the first of each band as white deltas.
    stage_for(&p, &st, SW_II, 0);
    if (!p) return;
    sink_reset();
    feed(&st, 0, 2880, 3960, 0, fill_same_noise);
    decode(&d, SW_II, 3774, 360);
    CHECK_EQ_INT(d.bad, 0);
    CHECK_EQ_INT(d.rects, 5);
    CHECK_EQ_INT(d.blocks, 5);
    total = 0;
    for (int i = 0; i < d.blocks; i++) total += d.block_len[i];
    CHECK(total < 5u * 800 + 5u * 400);                // a row each plus an 80 per further row
    dec_free(&d);
    sw_page_free(p);
}

// A dense colour page on the 1500: every plane noise, so the band splits into many blocks
// within the four-plane cap, and every row comes back.
static void test_dense_1500_colour(void)
{
    sw_page_t *p;
    rip_stage_t st;
    stage_for(&p, &st, SW_C1500, 1);
    if (!p) return;
    sink_reset();
    feed(&st, 0, 2880, 1000, 1, fill_noise);           // 1000 rows: 814 printable
    CHECK_EQ_INT(g_malformed, 0);
    dec_t d;
    decode(&d, SW_C1500, 814, 360);
    CHECK_EQ_INT(d.bad, 0);
    CHECK_EQ_INT(d.mid_row_ends, 0);
    CHECK_EQ_INT(d.page_flag[0], 1);
    CHECK_EQ_INT(d.rects, 2);                          // 800 rows, then 14
    CHECK_EQ_INT(d.rect[0].kind, 'c');
    CHECK(d.rect[0].blocks > 20);                      // ~1.4 KB a row, a 29 KB block
    uint32_t cap = sw_block_cap(SW_C1500, 4);
    for (int i = 0; i < d.blocks; i++) CHECK(d.block_len[i] <= cap);
    for (int k = 0; k < 4; k++) {
        uint8_t *w = expect(2880, 1000, 72, 114, 2880, fill_noise, k, 360);
        CHECK_EQ_MEM(d.img[0][k], w, 814 * 360);
        free(w);
    }
    dec_free(&d);
    sw_page_free(p);
}

// A page that ends with its band unfinished (the stage was fed short) is finished with white
// rows: the rect has said how many there are.
static void test_short_feed(void)
{
    sw_page_t *p;
    rip_stage_t st;
    stage_for(&p, &st, SW_II, 0);
    if (!p) return;
    static const dot_t dot[] = { { 72 + 3, 9, IW_BLACK } };
    g_dots = dot; g_ndots = 1;
    sink_reset();
    uint8_t row[360];
    CHECK_EQ_INT(st.page_begin(st.ctx, 0, 2880, 3960), 0);
    for (uint32_t y = 0; y < 72 + 10; y++) {           // the margin and ten printable rows
        fill_dots(y, IW_BLACK, row, 360);
        st.row(st.ctx, row);
    }
    CHECK_EQ_INT(st.page_end(st.ctx), 0);
    dec_t d;
    decode(&d, SW_II, 3774, 360);
    CHECK_EQ_INT(d.bad, 0);
    CHECK_EQ_INT(d.rects, 1);
    CHECK_EQ_INT(d.rect[0].t, 3);
    CHECK_EQ_INT(d.rect[0].b, 3 + 799);
    CHECK_EQ_INT(d.page_ends, 1);
    CHECK_EQ_INT(d.img[0][3][3 * 360 + 1], 0x40);
    int ink = 0;
    for (size_t i = 0; i < 3774u * 360; i++) ink += d.img[0][3][i] != 0;
    CHECK_EQ_INT(ink, 1);
    dec_free(&d);
    sw_page_free(p);
}

static void test_pages_and_misc(void)
{
    // Two pages in a row, a short one and a tall one: state does not leak across.
    sw_page_t *p;
    rip_stage_t st;
    stage_for(&p, &st, SW_C2200, 0);
    if (!p) return;
    static const dot_t first[] = { { 100, 3, IW_BLACK } };
    static const dot_t second[] = { { 4900, 9, IW_BLACK }, { 4941, 9, IW_BLACK } };   // the 99 rows below: dropped
    sink_reset();
    g_dots = first; g_ndots = 1;
    feed(&st, 0, 2916, 400, 0, fill_dots);
    g_dots = second; g_ndots = 2;
    feed(&st, 1, 2916, 5040, 0, fill_dots);
    dec_t d;
    decode(&d, SW_C2200, 5040 - 45 - 99, 365);
    CHECK_EQ_INT(d.bad, 0);
    CHECK_EQ_INT(d.pages, 2);
    CHECK_EQ_INT(d.page_ends, 2);
    CHECK_EQ_INT(d.rects, 2);
    CHECK(d.img[0][3][(100 - 45) * 365] == 0x10);
    CHECK(d.img[1][3][(4900 - 45) * 365 + 1] == 0x40);
    CHECK(d.img[1][3][(100 - 45) * 365] == 0);             // the first page's dot is not on the second
    CHECK(d.img[0][3][(4900 - 45) * 365 + 1] == 0);
    dec_free(&d);

    // A page shorter than its margins has no printable rows: begun and ended, no rects.
    sink_reset();
    feed(&st, 2, 2916, 100, 0, fill_dots);
    CHECK_EQ_INT(g_frames, 2);
    CHECK_EQ_INT(g_buf[0], SWF_PAGE);
    sw_page_free(p);

    // A row wider than any sheet's printable width is cut to it, never sent past it.
    static const dot_t wide[] = { { 100, 2879, IW_BLACK }, { 100, 2880, IW_BLACK }, { 101, 3000, IW_BLACK } };
    for (int m = 0; m < 2; m++) {
        sw_model_t model = m ? SW_C2500 : SW_II;
        uint32_t cols = m ? 2916 : 2880, dx = m ? 0 : 16;
        stage_for(&p, &st, model, 0);
        if (!p) return;
        g_dots = wide; g_ndots = 3;
        sink_reset();
        feed(&st, 0, 3060, 400, 0, fill_dots);
        decode(&d, model, 400 - (m ? 144 : 186), 365);
        CHECK_EQ_INT(d.bad, 0);
        CHECK_EQ_INT(d.rects, 1);
        CHECK_EQ_INT(d.rect[0].l, dx);
        CHECK_EQ_INT(d.rect[0].r, dx + cols - 1);
        if (m) CHECK_EQ_INT(d.img[0][3][(100 - 45) * 365 + 360], 0x80);   // 2880 is inside the 2500's 2916
        else   CHECK_EQ_INT(d.img[0][3][(100 - 72) * 365 + 359], 0x01);   // 2879 is the II's last
        dec_free(&d);
        sw_page_free(p);
    }

    // What does not make a stage.
    sw_page_cfg_t bad = { .model = SW_NONE, .out = sink };
    CHECK(sw_page_new(&bad) == NULL);
    sw_page_cfg_t noout = { .model = SW_II };
    CHECK(sw_page_new(&noout) == NULL);
    sw_page_free(NULL);
}

// ---- pages that are not the paper they claim (Review Focus 2), through the rip

static uint8_t *make_urf(size_t *len, uint32_t w, uint32_t h, const dot_t *dots, int n)
{
    // 8-bit gray at 360 dpi, white with black dots; rows that match the one before share it.
    size_t cap = (size_t)h * 40 + (size_t)(h / 64 + 1) * (w / 128 * 2 + 64) + (size_t)n * (w + w / 64 + 64) + 4096;
    uint8_t *out = malloc(cap);
    size_t o = 0;
    memcpy(out, "UNIRAST\0", 8); o = 8;
    out[o++] = 0; out[o++] = 0; out[o++] = 0; out[o++] = 1;
    uint8_t hdr[32] = { 0 };
    hdr[0] = 8;
    hdr[12] = (uint8_t)(w >> 24); hdr[13] = (uint8_t)(w >> 16); hdr[14] = (uint8_t)(w >> 8); hdr[15] = (uint8_t)w;
    hdr[16] = (uint8_t)(h >> 24); hdr[17] = (uint8_t)(h >> 16); hdr[18] = (uint8_t)(h >> 8); hdr[19] = (uint8_t)h;
    hdr[22] = 360 >> 8; hdr[23] = 360 & 255;
    memcpy(out + o, hdr, 32); o += 32;
    uint8_t *row = malloc(w);
    for (uint32_t y = 0; y < h;) {
        memset(row, 255, w);
        int has = 0;
        for (int i = 0; i < n; i++)
            if (dots[i].y == y) { row[dots[i].x] = 0; has = 1; }
        uint32_t rep = 1;                               // following rows with no dots repeat the white row
        if (!has)
            while (y + rep < h && rep < 256) {
                int dot = 0;
                for (int i = 0; i < n; i++) dot |= dots[i].y == y + rep;
                if (dot) break;
                rep++;
            }
        out[o++] = (uint8_t)(rep - 1);
        for (uint32_t x = 0; x < w;) {
            uint32_t run = 1;
            while (x + run < w && run < 128 && row[x + run] == row[x]) run++;
            if (run >= 3 || x + run == w) {
                out[o++] = (uint8_t)(run - 1);
                out[o++] = row[x];
                x += run;
                continue;
            }
            uint32_t start = x;
            while (x < w && x - start < 128 && !(x + 2 < w && row[x] == row[x + 1] && row[x] == row[x + 2])) x++;
            if (x - start == 1) { out[o++] = 0; out[o++] = row[start]; }
            else { out[o++] = (uint8_t)(257 - (x - start)); memcpy(out + o, row + start, x - start); o += x - start; }
        }
        y += rep;
    }
    free(row);
    *len = o;
    return out;
}

// Rips a w x h page of black dots into a StyleWriter stage and checks that the printable
// area is exactly what the design says: `cols` kept from the source's x `left`, rows from
// `top` to h - `bottom`, and that no rect reaches past it. Returns the rects' count.
static int odd(sw_model_t m, int colour_cart, uint32_t w, uint32_t h, uint32_t left,
               uint32_t cols, uint32_t top, uint32_t bottom, uint32_t dx)
{
    // Dots on and just outside each edge of the printable area: columns left - 1 and
    // left (the first kept), left + cols - 1 (the last) and left + cols; rows top - 1,
    // top, h - bottom - 1 and h - bottom.
    const uint32_t xs[] = { left - 1, left, left + cols - 1, left + cols };
    const uint32_t ys[] = { top - 1, top, h - bottom - 1, h - bottom };
    dot_t dots[32];
    int n = 0;
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            if (xs[i] < w && ys[j] < h) dots[n++] = (dot_t){ ys[j], xs[i], IW_BLACK };
    dots[n++] = (dot_t){ top + 5, left, IW_BLACK };
    size_t len;
    uint8_t *urf = make_urf(&len, w, h, dots, n);

    sw_page_t *p;
    rip_stage_t st;
    stage_for(&p, &st, m, colour_cart);
    sink_reset();
    memsrc_t src = { urf, len, 0 };
    urf_t u;
    int rc = urf_open(&u, memsrc_read, &src);
    CHECK_EQ_INT(rc, URF_OK);
    ripjob_cfg_t cfg = { .dither = HT_THRESHOLD, .caps = sw_caps(m), .stage = &st, .colour = colour_cart };
    CHECK_EQ_INT(ripjob_run(&u, &cfg, NULL), 0);
    urf_free(&u);
    free(urf);

    uint32_t nrows = h - top - bottom, rb = (cols + 7) / 8;
    dec_t d;
    decode(&d, m, nrows, rb);
    CHECK_EQ_INT(d.bad, 0);
    CHECK_EQ_INT(d.mid_row_ends, 0);
    CHECK_EQ_INT(d.pages, 1);
    CHECK_EQ_INT(d.page_ends, 1);
    for (int i = 0; i < d.rects; i++) {
        CHECK_EQ_INT(d.rect[i].l, dx);
        CHECK_EQ_INT(d.rect[i].r, dx + cols - 1);         // never a coordinate past the printable width
        CHECK(d.rect[i].b < nrows);                       // nor a rect past the page
        CHECK(d.rect[i].t <= d.rect[i].b);
    }
    // The image: exactly the dots that fall inside, shifted to the printable area's corner.
    uint8_t *want = calloc(nrows, rb);
    int inside = 0;
    for (int i = 0; i < n; i++) {
        if (dots[i].x < left || dots[i].x >= left + cols) continue;
        if (dots[i].y < top || dots[i].y >= h - bottom) continue;
        set_bit(want + (size_t)(dots[i].y - top) * rb, dots[i].x - left);
        inside++;
    }
    CHECK(inside >= 4);
    CHECK_EQ_MEM(d.img[0][3], want, (size_t)nrows * rb);
    int rects = d.rects;
    free(want);
    dec_free(&d);
    sw_page_free(p);
    return rects;
}

static void test_odd_sizes(void)
{
    // An A4 page (2976 x 4209) on the II: 72 dropped on the left, 2832 kept, margins 72 / 114.
    CHECK(odd(SW_II, 0, 2976, 4209, 72, 2832, 72, 114, 16) > 0);
    CHECK(odd(SW_C2500, 0, 2976, 4209, 72, 2832, 45, 99, 0) > 0);
    // A 4 x 6 photo (1440 x 2160): the page's own width ends the row.
    CHECK(odd(SW_II, 0, 1440, 2160, 72, 1440 - 72, 72, 114, 16) > 0);
    CHECK(odd(SW_C2200, 0, 1440, 2160, 72, 1440 - 72, 45, 99, 0) > 0);
    // Legal (3060 x 5040): Letter's width, the sheet's height less the margins.
    CHECK(odd(SW_II, 0, 3060, 5040, 72, 2880, 72, 114, 16) > 0);
    CHECK(odd(SW_C2500, 0, 3060, 5040, 72, 2916, 45, 99, 0) > 0);
    CHECK(odd(SW_ORIGINAL, 0, 3060, 3960, 72, 2880, 72, 114, 0) > 0);
    // The same through a colour cartridge's gray page.
    CHECK(odd(SW_C2400, 1, 2976, 4209, 72, 2832, 45, 99, 0) > 0);
}

int main(void)
{
    test_letter_ii();
    test_blank_and_one_dot();
    test_splits_blocks();
    test_colour_c2500();
    test_original_raw();
    test_dense_1500_colour();
    test_short_feed();
    test_pages_and_misc();
    test_odd_sizes();
    free(g_buf);
    TEST_END;
}
