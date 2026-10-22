// SPDX-License-Identifier: GPL-3.0-or-later
#include "ps_page.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct ps {
    ps_cfg_t cfg;
    uint8_t *band;           // PS_BAND_ROWS rows of PS_ROW_MAX bytes
    uint32_t stride;         // bytes kept per row on this page
    uint32_t y;              // the next row's index on the page
    uint32_t y0, rows;       // the band so far: its first row and how many
    int      pages;
};

static void put(ps_t *ps, const void *b, size_t n)
{
    ps->cfg.out(ps->cfg.ctx, (const uint8_t *)b, n);
}

static void say(ps_t *ps, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

// Every line this writes fits: the longest is the band procedure's, ~90.
static void say(ps_t *ps, const char *fmt, ...)
{
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (n > 0) put(ps, line, (size_t)n < sizeof line ? (size_t)n : sizeof line - 1);
}

ps_t *ps_new(const ps_cfg_t *cfg)
{
    ps_t *ps = calloc(1, sizeof *ps);
    if (!ps) return NULL;
    ps->cfg = *cfg;
    if (!ps->cfg.paper_h_pt) ps->cfg.paper_h_pt = 792;
    ps->band = malloc((size_t)PS_BAND_ROWS * PS_ROW_MAX);
    if (!ps->band) { free(ps); return NULL; }
    return ps;
}

void ps_free(ps_t *ps)
{
    if (!ps) return;
    free(ps->band);
    free(ps);
}

// %%Title's value as a PostScript string: ( ) and \ escaped, anything that is
// not printable ASCII as \ooo, so a job name never breaks the comment.
static void put_title(ps_t *ps, const char *t)
{
    char buf[400];
    size_t k = 0;
    buf[k++] = '(';
    for (const unsigned char *p = (const unsigned char *)(t ? t : ""); *p && k < sizeof buf - 6; p++) {
        if (*p == '(' || *p == ')' || *p == '\\') { buf[k++] = '\\'; buf[k++] = (char)*p; }
        else if (*p < 0x20 || *p > 0x7E) k += (size_t)snprintf(buf + k, sizeof buf - k, "\\%03o", *p);
        else buf[k++] = (char)*p;
    }
    buf[k++] = ')';
    buf[k++] = '\n';
    put(ps, "%%Title: ", 9);
    put(ps, buf, k);
}

void ps_job_begin(ps_t *ps)
{
    say(ps, "%%!PS-Adobe-3.0\n%%%%Creator: %s\n", ps->cfg.creator ? ps->cfg.creator : "AirWriter");
    put_title(ps, ps->cfg.title);
    say(ps, "%%%%LanguageLevel: 1\n%%%%Pages: (atend)\n%%%%EndComments\n%%%%BeginProlog\n");
    // One string for every row, and one band procedure: x y w h wb awb paints
    // the h rows of wb bytes that follow it in the file at x, y (pixels from
    // the page's top-left), set bits black.
    say(ps, "/aws %d string def\n", PS_ROW_MAX);
    say(ps, "/awb { aws 0 3 -1 roll getinterval /awr exch def gsave 4 2 roll translate\n");
    say(ps, "  true [1 0 0 1 0 0] { currentfile awr %s pop } imagemask grestore } bind def\n",
        ps->cfg.binary ? "readstring" : "readhexstring");
    say(ps, "%%%%EndProlog\n");
}

void ps_job_end(ps_t *ps)
{
    say(ps, "%%%%Trailer\n%%%%Pages: %d\n%%%%EOF\n", ps->pages);
}

static void put_hex(ps_t *ps, const uint8_t *d, size_t n, size_t *col)
{
    static const char hex[] = "0123456789abcdef";
    char line[66];
    size_t k = 0;
    for (size_t i = 0; i < n; i++) {
        line[k++] = hex[d[i] >> 4];
        line[k++] = hex[d[i] & 15];
        *col += 2;
        if (*col == 64) {
            line[k++] = '\n';
            put(ps, line, k);
            k = 0;
            *col = 0;
        }
    }
    if (k) put(ps, line, k);
}

// The band gathered so far, as one imagemask trimmed to its ink.
static void flush(ps_t *ps)
{
    if (!ps->rows) return;
    uint32_t first = ps->stride, last = 0;
    for (uint32_t r = 0; r < ps->rows; r++) {
        const uint8_t *row = ps->band + (size_t)r * PS_ROW_MAX;
        for (uint32_t i = 0; i < ps->stride; i++)
            if (row[i]) {
                if (i < first) first = i;
                if (i > last) last = i;
            }
    }
    uint32_t wb = last - first + 1;
    say(ps, "%u %u %u %u %u awb\n", (unsigned)(first * 8), (unsigned)ps->y0,
        (unsigned)(wb * 8), (unsigned)ps->rows, (unsigned)wb);
    size_t col = 0;
    for (uint32_t r = 0; r < ps->rows; r++) {
        const uint8_t *row = ps->band + (size_t)r * PS_ROW_MAX + first;
        if (ps->cfg.binary) put(ps, row, wb);
        else put_hex(ps, row, wb, &col);
    }
    // The next command starts on a line of its own.
    put(ps, "\n", 1);
    ps->rows = 0;
}

static int st_begin(void *ctx, int index, uint32_t w, uint32_t h)
{
    (void)h;
    ps_t *ps = ctx;
    ps->stride = (w + 7) / 8;
    if (ps->stride > PS_ROW_MAX) ps->stride = PS_ROW_MAX;
    ps->y = ps->rows = 0;
    // User space in pixels from the paper's top-left, y down: 72/300 pt each.
    say(ps, "%%%%Page: %d %d\nsave\n0 %u translate 72 300 div dup neg scale\n",
        index + 1, index + 1, (unsigned)ps->cfg.paper_h_pt);
    return 0;
}

static void st_row(void *ctx, const uint8_t *bits)
{
    ps_t *ps = ctx;
    uint32_t i = 0;
    while (i < ps->stride && !bits[i]) i++;
    if (i == ps->stride) {
        flush(ps);                       // a blank row ends the band
    } else {
        if (!ps->rows) ps->y0 = ps->y;
        memcpy(ps->band + (size_t)ps->rows * PS_ROW_MAX, bits, ps->stride);
        if (++ps->rows == PS_BAND_ROWS) flush(ps);
    }
    ps->y++;
}

static int st_end(void *ctx)
{
    ps_t *ps = ctx;
    flush(ps);
    say(ps, "restore showpage\n");
    ps->pages++;
    return 0;
}

void ps_stage(ps_t *ps, rip_stage_t *st)
{
    st->page_begin = st_begin;
    st->row        = st_row;
    st->page_end   = st_end;
    st->ctx        = ps;
}
