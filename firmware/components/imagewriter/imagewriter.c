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
