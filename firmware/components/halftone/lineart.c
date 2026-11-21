// SPDX-License-Identifier: GPL-3.0-or-later
// W-1174: crisp text and thin lines ahead of the screen (lineart.h).
#include "lineart.h"
#include "bigmem.h"
#include <stdlib.h>
#include <string.h>

#define PAPER    236                // a closing at or over this is paper or near-paper around
#define DARK     191                // a feature this dark or darker prints solid
#define LIGHT    231                // 192 to this: a thin light line
#define WHITE    253                // this and over: nothing to decide
#define GRAY_LO   48                // a wide gray area: a 9 x 9 square all over this...
#define GRAY_HI  200                // ...and none over this
#define ZONE       6                // dots around a wide gray area left to the screen

// The rings, each k_len rows of `words` words. A plane whose outside is paper has its bits
// past w set.
enum {
    RQ,             // gray < WHITE: something to decide (outside 0)
    RD3P,           // {gray >= PAPER}, dilated 3 across (outside 1)
    RG,             // {GRAY_LO < gray <= GRAY_HI}, eroded 4 across (outside 0)
    RE3,            // RD3P dilated 3 down, then eroded 3 across: row r - 3 (outside 1)
    RB,             // RE3 eroded 3 down: the 7 x 7 closing of {gray >= PAPER}, row r - 6
    RZ,             // a wide gray area, dilated ZONE across: row r - 4 (outside 0)
    RP,             // {gray >= PAPER} itself (outside 1)
    RINGS,
    // Scratch rows, one each.
    SP = RINGS, SG, ST, SW, SN,
    PLANES
};

// Each ring holds the rows read back, and no more (board 1's 300 has about 115 KB of PSRAM for
// a page once its copy is made; W-1174's first build, at 16 rows a ring, failed there): a row is
// written as row r, or r - 3, r - 4 or r - 6 (step), and read as far back as decide's, r - 16.
static const uint8_t k_len[RINGS] = {
    [RQ] = 11,      // written r, read r - 10
    [RD3P] = 7,     // r - 6 to r
    [RG] = 9,       // r - 8 to r
    [RE3] = 7,      // written r - 3, read r - 9 to r - 3
    [RB] = 5,       // written r - 6, read r - 10
    [RZ] = 13,      // written r - 4, read r - 16 to r - 4
    [RP] = 12,      // written r, read r - 11 to r - 9
};
static const uint8_t k_off[RINGS] = { 0, 11, 18, 27, 34, 39, 52 };   // running sums of k_len
// What a row of paper leaves in each ring: its words all set, or all clear.
static const uint8_t k_paper[RINGS] = {
    [RQ] = 0, [RD3P] = 0xFF, [RG] = 0, [RE3] = 0xFF, [RB] = 0xFF, [RZ] = 0, [RP] = 0xFF,
};
#define RING_ROWS 64
#define GRAY_ROWS (LA_DELAY + 5)    // written r, read r - 14 to r - 6
#define BRIGHT   250                // white paper, on both sides of a thin light line
#define SIDE       4                // how far it may be: a 3-dot line and its soft edge
// A row whose every input, LA_DELAY rows each way, was paper comes out as it went in, and
// leaves paper in every ring (lineart.h): about half a page's rows.
#define ALL_PAPER (2 * LA_DELAY + 1)

static inline uint32_t slot(int32_t row, uint32_t len)
{
    int32_t m = row % (int32_t)len;
    return (uint32_t)(m < 0 ? m + (int32_t)len : m);
}

static inline uint32_t *ring(const la_t *la, int plane, int32_t row)
{
    return la->plane + ((size_t)k_off[plane] + slot(row, k_len[plane])) * la->words;
}

static inline uint32_t *scratch(const la_t *la, int plane)
{
    return la->plane + ((size_t)RING_ROWS + (size_t)(plane - RINGS)) * la->words;
}

static inline uint8_t *gray(const la_t *la, int32_t row)
{
    return la->gray + (size_t)slot(row, GRAY_ROWS) * la->w;
}

// The bits past w in the last word.
static inline uint32_t tail(const la_t *la)
{
    uint32_t used = la->w & 31;
    return used ? ~((1u << used) - 1) : 0;
}

static size_t gray_bytes(uint32_t w)  { return (size_t)(GRAY_ROWS + 1) * w; }   // and the row out
static size_t plane_bytes(uint32_t w)
{
    return (size_t)(RING_ROWS + PLANES - RINGS) * ((w + 31) / 32) * sizeof(uint32_t);
}

size_t la_bytes(uint32_t w)
{
    return gray_bytes(w) + plane_bytes(w);
}

int la_init(la_t *la, uint32_t w)
{
    memset(la, 0, sizeof *la);
    if (!w) return -1;
    la->w = w;
    la->words = (w + 31) / 32;
    la->gray = big_malloc_only(gray_bytes(w));
    la->plane = big_malloc_only(plane_bytes(w));
    if (!la->gray || !la->plane) {
        la_free(la);
        return -1;
    }
    la->out = la->gray + (size_t)GRAY_ROWS * w;
    return 0;
}

void la_free(la_t *la)
{
    free(la->gray);
    free(la->plane);
    la->gray = NULL;
    la->plane = NULL;
    la->out = NULL;
}

void la_page(la_t *la)
{
    memset(la->gray, 255, (size_t)GRAY_ROWS * la->w);
    for (int p = 0; p < RINGS; p++)
        memset(la->plane + (size_t)k_off[p] * la->words, k_paper[p],
               (size_t)k_len[p] * la->words * sizeof(uint32_t));
    la->r = 0;
    la->h = -1;
    la->paper = ALL_PAPER;                  // above the page is paper
    memset(la->gset, 0, sizeof la->gset);
    memset(la->zset, 0, sizeof la->zset);
}

static int any(const la_t *la, const uint32_t *row)
{
    uint32_t a = 0;
    for (uint32_t i = 0; i < la->words; i++) a |= row[i];
    return a != 0;
}

// dst = src dilated (or eroded) k dots across: each dot the OR (AND) of the 2k + 1 around it,
// `fill` beyond the row.
static void across(const la_t *la, uint32_t *dst, const uint32_t *src, int k, int erode,
                   uint32_t fill)
{
    const uint32_t n = la->words;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t c = src[i], lo = i ? src[i - 1] : fill, hi = i + 1 < n ? src[i + 1] : fill;
        uint32_t acc = c;
        for (int d = 1; d <= k; d++) {
            uint32_t right = (c >> d) | (hi << (32 - d));   // dot x + d
            uint32_t left  = (c << d) | (lo >> (32 - d));   // dot x - d
            if (erode) acc &= right & left;
            else       acc |= right | left;
        }
        dst[i] = acc;
    }
}

// dst = the OR (AND) of a ring's rows from..to.
static void down(const la_t *la, uint32_t *dst, int plane, int32_t from, int32_t to, int and)
{
    memcpy(dst, ring(la, plane, from), la->words * sizeof(uint32_t));
    for (int32_t r = from + 1; r <= to; r++) {
        const uint32_t *s = ring(la, plane, r);
        if (and) for (uint32_t i = 0; i < la->words; i++) dst[i] &= s[i];
        else     for (uint32_t i = 0; i < la->words; i++) dst[i] |= s[i];
    }
}

// Row r's own planes, from its gray. 1 if it is all paper (255).
static int planes_of(la_t *la, const uint8_t *g)
{
    uint32_t *p = scratch(la, SP), *gr = scratch(la, SG), *q = ring(la, RQ, la->r);
    int paper = 1;
    for (uint32_t i = 0; i < la->words; i++) {
        uint32_t x0 = i * 32, n = la->w - x0 < 32 ? la->w - x0 : 32;
        uint32_t wp = ~0u, wg = 0, wq = 0;
        const uint8_t *s = g + x0;
        uint32_t b = 0;
        while (b < n && s[b] == 255) b++;
        if (b < n) paper = 0;
        for (; b < n; b++) {                    // past the first dot that isn't paper
            uint32_t v = s[b], bit = 1u << b;
            if (v < PAPER) wp &= ~bit;
            if (v - (GRAY_LO + 1) <= (uint32_t)(GRAY_HI - GRAY_LO - 1)) wg |= bit;
            if (v < WHITE) wq |= bit;
        }
        p[i] = wp;
        gr[i] = wg;
        q[i] = wq;
    }
    return paper;
}

// Ink if v is darker than halfway between paper and the darkest dot within 2 dots of it, else
// paper: (255 + darkest) / 2 is at most v just where a dot within 2 is 2v - 254 or darker.
static uint8_t crisp(const la_t *la, int32_t x, int32_t y, int v)
{
    const int lim = 2 * v - 254;
    if (lim < 0) return 0;
    const int32_t w = (int32_t)la->w;
    for (int32_t r = y - 2; r <= y + 2; r++) {
        const uint8_t *s = gray(la, r);
        for (int32_t xx = x - 2; xx <= x + 2; xx++)
            if (xx >= 0 && xx < w && s[xx] <= lim) return 255;
    }
    return 0;
}

// White paper within SIDE dots above x and within SIDE below; to the left and to the right.
static int paper_ud(const la_t *la, int32_t x, int32_t y)
{
    int a = 0, b = 0;
    for (int d = 1; d <= SIDE && !a; d++) a = gray(la, y - d)[x] >= BRIGHT;
    for (int d = 1; d <= SIDE && !b; d++) b = gray(la, y + d)[x] >= BRIGHT;
    return a && b;
}

static int paper_lr(const uint8_t *g, int32_t x, int32_t w)
{
    int a = 0, b = 0;
    for (int d = 1; d <= SIDE && !a; d++) a = x - d < 0 || g[x - d] >= BRIGHT;
    for (int d = 1; d <= SIDE && !b; d++) b = x + d >= w || g[x + d] >= BRIGHT;
    return a && b;
}

// A dot within one of x, y at or under `at`.
static int near_at(const uint8_t *up, const uint8_t *g, const uint8_t *dn, int32_t x, int32_t w,
                   int at)
{
    for (int32_t xx = x - 1; xx <= x + 1; xx++)
        if (xx >= 0 && xx < w && (up[xx] <= at || g[xx] <= at || dn[xx] <= at)) return 1;
    return 0;
}

// The dots of row y that aren't left to the screen: thin features and their fringe, and the
// edge of any dark shape on paper (lineart.h). The row itself when none change.
static const uint8_t *decide(la_t *la, int32_t y)
{
    const uint8_t *g = gray(la, y), *up = gray(la, y - 1), *dn = gray(la, y + 1);
    uint8_t *o = la->out;
    int copied = 0;
    uint32_t *zone = scratch(la, ST), *near = scratch(la, SN);
    // Gray areas are few: most rows have none near, and their zone is empty.
    int zoned = 0;
    for (int32_t r = y - ZONE; r <= y + ZONE; r++) zoned |= la->zset[slot(r, k_len[RZ])];
    if (zoned) down(la, zone, RZ, y - ZONE, y + ZONE, 0);
    else memset(zone, 0, la->words * sizeof(uint32_t));
    // Paper within a dot: where a shape's edge can be.
    down(la, scratch(la, SW), RP, y - 1, y + 1, 0);
    across(la, near, scratch(la, SW), 1, 0, ~0u);
    const uint32_t *b = ring(la, RB, y), *q = ring(la, RQ, y);
    const int32_t w = (int32_t)la->w;
    for (uint32_t i = 0; i < la->words; i++) {
        uint32_t m = (b[i] | near[i]) & q[i] & ~zone[i];
        while (m) {
            const uint32_t bit = (uint32_t)__builtin_ctz(m);
            int32_t x = (int32_t)(i * 32 + bit);
            m &= m - 1;
            int v = g[x], to = v;
            if (!(b[i] >> bit & 1)) {
                // Not thin: the edge of a wider shape, crisp when the shape is dark there.
                if (near_at(up, g, dn, x, w, DARK)) to = crisp(la, x, y, v);
            } else if (v <= DARK) {
                to = crisp(la, x, y, v);
            } else if (v <= LIGHT) {
                // A thin light line has white paper on both sides. A photo's pale grain, or a
                // line on a tint, has none, and is the screen's.
                // Next to dark text it is that text's soft edge, on paper or a tint.
                int ud = paper_ud(la, x, y), lr = paper_lr(g, x, w);
                if (ud || lr) {
                    int lf = x > 0 ? g[x - 1] : 255, rt = x + 1 < w ? g[x + 1] : 255;
                    int line = (ud && v < up[x] && v <= dn[x]) || (lr && v < lf && v <= rt);
                    to = line ? 0 : 255;
                } else if (near_at(up, g, dn, x, w, DARK)) {
                    to = 255;
                }
            } else if (near_at(up, g, dn, x, w, DARK) ||
                       (near_at(up, g, dn, x, w, LIGHT) && (paper_ud(la, x, y) || paper_lr(g, x, w)))) {
                to = 255;
            }
            if (to == v) continue;
            if (!copied) {
                memcpy(o, g, la->w);
                copied = 1;
            }
            o[x] = (uint8_t)to;
        }
    }
    return copied ? o : g;
}

// Row r in (NULL: paper below the page); row r - LA_DELAY out, or NULL above the page.
static const uint8_t *step(la_t *la, const uint8_t *g)
{
    const int32_t r = la->r;
    uint8_t *gr = gray(la, r);
    int paper = 1;
    if (g) {
        memcpy(gr, g, la->w);
        paper = planes_of(la, g);
    } else {
        memset(gr, 255, la->w);
    }
    la->paper = paper ? la->paper + 1 : 0;
    la->r = r + 1;
    const int32_t y = r - LA_DELAY;
    if (la->paper >= ALL_PAPER) {
        // Nothing to do but leave paper in the rows this step writes.
        static const struct { uint8_t plane; int8_t at; } k_rows[] = {
            { RQ, 0 }, { RD3P, 0 }, { RG, 0 }, { RP, 0 }, { RE3, -3 }, { RB, -6 }, { RZ, -4 },
        };
        for (size_t k = 0; k < sizeof k_rows / sizeof k_rows[0]; k++)
            memset(ring(la, k_rows[k].plane, r + k_rows[k].at), k_paper[k_rows[k].plane],
                   la->words * sizeof(uint32_t));
        la->gset[slot(r, k_len[RG])] = 0;
        la->zset[slot(r - 4, k_len[RZ])] = 0;
        return y < 0 ? NULL : gray(la, y);
    }
    if (!g) {
        memset(ring(la, RQ, r), 0, la->words * sizeof(uint32_t));
        memset(scratch(la, SP), 0xFF, la->words * sizeof(uint32_t));
        memset(scratch(la, SG), 0, la->words * sizeof(uint32_t));
    }
    const uint32_t t = tail(la);
    uint32_t *tmp = scratch(la, ST), *wide = scratch(la, SW);
    scratch(la, SP)[la->words - 1] |= t;
    ring(la, RQ, r)[la->words - 1] &= ~t;
    memcpy(ring(la, RP, r), scratch(la, SP), la->words * sizeof(uint32_t));
    across(la, ring(la, RD3P, r), scratch(la, SP), 3, 0, ~0u);
    across(la, ring(la, RG, r), scratch(la, SG), 4, 1, 0);
    la->gset[slot(r, k_len[RG])] = (uint8_t)any(la, ring(la, RG, r));
    // The closing of paper: dilated 7 x 7, then eroded 7 x 7.
    down(la, tmp, RD3P, r - 6, r, 0);
    across(la, ring(la, RE3, r - 3), tmp, 3, 1, ~0u);
    down(la, ring(la, RB, r - 6), RE3, r - 9, r - 3, 1);
    // Wide gray: every dot of the 9 x 9 around over GRAY_LO and none over GRAY_HI. A square
    // across a black shape's soft edge has near-black in it, so the edge is not gray.
    // A row of the 9 with no gray leaves none: the usual case, and the zone row is empty.
    int gray9 = 1;
    for (int32_t k = r - 8; k <= r; k++) gray9 &= la->gset[slot(k, k_len[RG])];
    uint32_t *zr = ring(la, RZ, r - 4);
    if (gray9) {
        down(la, wide, RG, r - 8, r, 1);
        wide[la->words - 1] &= ~t;
        across(la, zr, wide, ZONE, 0, 0);
        la->zset[slot(r - 4, k_len[RZ])] = (uint8_t)any(la, zr);
    } else {
        memset(zr, 0, la->words * sizeof(uint32_t));
        la->zset[slot(r - 4, k_len[RZ])] = 0;
    }
    if (y < 0) return NULL;
    return decide(la, y);
}

const uint8_t *la_push(la_t *la, const uint8_t *row)
{
    return step(la, row);
}

const uint8_t *la_flush(la_t *la)
{
    if (la->h < 0) la->h = la->r;
    while (la->r - LA_DELAY < la->h) {
        const uint8_t *o = step(la, NULL);
        if (o) return o;
    }
    return NULL;
}
