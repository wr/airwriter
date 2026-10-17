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
    w->nrows      = 0;
    w->pending_lf = 0;
}

static void flush(iw_t *w)
{
    const uint8_t *rows[8];
    for (uint32_t i = 0; i < 8; i++)
        rows[i] = i < w->nrows ? w->rows[i] : NULL;   // pad short strips blank

    iw_transpose(rows, w->cols, w->width_dots);

    // The LF that separates this strip from the next is held back until we
    // know there is a next strip: emit the *previous* strip's deferred LF
    // now, immediately before this strip's header, rather than after this
    // strip's own CR. That way the page's last strip never gets one. At
    // 8/72in per line feed, an LF after the final strip pushes 1/9in into
    // the next form before the form feed fires, ejecting a blank sheet.
    static const uint8_t lf[] = { '\n' };
    if (w->pending_lf) w->out(w->ctx, lf, sizeof lf);

    uint8_t hdr[6] = { 0x1B, 'G', 0, 0, 0, 0 };
    uint32_t n = w->width_dots;                        // one byte per column
    hdr[2] = (uint8_t)('0' + (n / 1000) % 10);
    hdr[3] = (uint8_t)('0' + (n / 100)  % 10);
    hdr[4] = (uint8_t)('0' + (n / 10)   % 10);
    hdr[5] = (uint8_t)('0' +  n         % 10);
    w->out(w->ctx, hdr, sizeof hdr);
    w->out(w->ctx, w->cols, n);

    static const uint8_t cr[] = { '\r' };
    w->out(w->ctx, cr, sizeof cr);
    w->pending_lf = 1;

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

static const char k_ink_digit[IW_INKS] = { '1', '2', '3', '0' };

void iwc_init(iwc_t *w, uint32_t width_dots, iw_out_fn out, void *ctx)
{
    memset(w, 0, sizeof *w);
    w->width_dots = width_dots > IW_MAX_DOTS ? IW_MAX_DOTS : width_dots;
    w->out = out;
    w->ctx = ctx;
    w->ink = -1;
}

void iwc_begin_page(iwc_t *w)
{
    static const uint8_t init[] = { 0x1B, 'c', 0x1B, 'p', 0x1B, 'T', '1', '6' };
    w->out(w->ctx, init, sizeof init);
    w->nrows   = 0;
    w->held_lf = 0;
    w->ink     = -1;     // ESC c selects black, but only once it has run
}

static void select_ink(iwc_t *w, int ink)
{
    if (w->ink == ink) return;
    const uint8_t sel[3] = { 0x1B, 'K', (uint8_t)k_ink_digit[ink] };
    w->out(w->ctx, sel, sizeof sel);
    w->ink = ink;
}

static void flush_colour(iwc_t *w)
{
    static const uint8_t lf[] = { '\n' }, cr[] = { '\r' };
    for (int ink = 0; ink < IW_INKS; ink++) {
        const uint8_t *rows[8];
        for (uint32_t i = 0; i < 8; i++)
            rows[i] = i < w->nrows ? w->rows[ink][i] : NULL;
        iw_transpose(rows, w->cols, w->width_dots);

        uint32_t n = w->width_dots;
        while (n && !w->cols[n - 1]) n--;
        if (!n) continue;

        for (; w->held_lf; w->held_lf--) w->out(w->ctx, lf, sizeof lf);
        select_ink(w, ink);
        uint8_t hdr[6] = { 0x1B, 'G',
                           (uint8_t)('0' + (n / 1000) % 10), (uint8_t)('0' + (n / 100) % 10),
                           (uint8_t)('0' + (n / 10) % 10),   (uint8_t)('0' + n % 10) };
        w->out(w->ctx, hdr, sizeof hdr);
        w->out(w->ctx, w->cols, n);
        w->out(w->ctx, cr, sizeof cr);
    }
    // This strip's own LF, owed to whatever prints next; never sent after a
    // page's last strip (see flush above).
    w->held_lf++;
    w->nrows = 0;
}

void iwc_row(iwc_t *w, const uint8_t *const bits[IW_INKS])
{
    for (int ink = 0; ink < IW_INKS; ink++)
        memcpy(w->rows[ink][w->nrows], bits[ink], (w->width_dots + 7) / 8);
    if (++w->nrows == 8) flush_colour(w);
}

void iwc_end_page(iwc_t *w)
{
    if (w->nrows) flush_colour(w);
    if (w->ink >= 0 && w->ink != IW_BLACK) select_ink(w, IW_BLACK);
    static const uint8_t ff[] = { 0x0C };
    w->out(w->ctx, ff, sizeof ff);
}

void iw_resync(iw_out_fn out, void *ctx)
{
    static const uint8_t nuls[128];
    for (uint32_t left = IW_RESYNC_NULS; left; ) {
        uint32_t n = left < sizeof nuls ? left : sizeof nuls;
        out(ctx, nuls, n);
        left -= n;
    }
    static const uint8_t can[] = { 0x18 };   // CTRL-X: cancel current line
    out(ctx, can, sizeof can);
}

iw_ribbon_t iw_self_id_ribbon(const uint8_t *reply, size_t n)
{
    for (size_t i = 0; i + 4 <= n; i++) {
        uint8_t c[4];
        for (size_t k = 0; k < 4; k++) c[k] = reply[i + k] & 0x7F;
        if (c[0] != 'I' || c[1] != 'W') continue;
        if (c[2] < '0' || c[2] > '9' || c[3] < '0' || c[3] > '9') continue;
        for (size_t k = i + 4; k < n; k++)
            if ((reply[k] & 0x7F) == 'C') return IW_RIBBON_COLOUR;
        return IW_RIBBON_BLACK;
    }
    return IW_RIBBON_UNKNOWN;
}

void iw_link_job_start(iw_link_t *l)
{
    l->started = 0;
}

int iw_link_need_resync(iw_link_t *l)
{
    if (l->started) return 0;
    l->started = 1;
    int need = l->dirty;
    l->dirty = 1;          // in flight: a cut from here on leaves it mid-command
    return need;
}

void iw_link_job_end(iw_link_t *l, int clean)
{
    if (clean && l->started) l->dirty = 0;
    l->started = 0;
}

enum { SC_TEXT = 0, SC_ESC, SC_DIGITS, SC_SKIP, SC_LIST, SC_KEY, SC_WIDTH };

void iw_scan_init(iw_scan_t *s)
{
    memset(s, 0, sizeof *s);
}

static void scan_byte(iw_scan_t *s, uint8_t b)
{
    switch (s->state) {
    case SC_TEXT:
        if (b == 0x1B) s->state = SC_ESC;
        else if (b == 0x1F) { s->state = SC_SKIP; s->left = 1; s->after = SC_TEXT; }   // CTRL-_ n
        return;
    case SC_ESC:
        s->cmd = b;
        s->num = 0;
        s->after = SC_TEXT;
        switch (b) {
        case 'G': case 'S': case 'V': case 'H': case 'F':
            s->state = SC_DIGITS; s->digits = 4; return;
        case 'g': case 'L': case 'u': case 'R':
            s->state = SC_DIGITS; s->digits = 3; return;
        case 'T':
            s->state = SC_DIGITS; s->digits = 2; return;
        case 'Z': case 'D':
            s->state = SC_SKIP; s->left = 2; return;
        case 'a': case 's': case 'l': case 'K':
            s->state = SC_SKIP; s->left = 1; return;
        case '(': case ')':
            s->state = SC_LIST; return;
        case 'I':
            s->state = SC_KEY; return;
        default:
            s->state = SC_TEXT; return;
        }
    case SC_DIGITS:
        s->num = s->num * 10 + (b >= '0' && b <= '9' ? (uint32_t)(b - '0') : 0);
        if (--s->digits) return;
        switch (s->cmd) {
        case 'G': case 'S': s->left = s->num;     break;
        case 'g':           s->left = s->num * 8; break;
        case 'V': case 'R': s->left = 1;          break;
        default:            s->left = 0;          break;
        }
        s->state = s->left ? SC_SKIP : SC_TEXT;
        return;
    case SC_SKIP:
        if (!--s->left) s->state = s->after;
        return;
    case SC_LIST:
        if (b == '.') s->state = SC_TEXT;
        return;
    case SC_KEY:                               // a character to define, or CTRL-D
        s->state = b == 0x04 ? SC_TEXT : SC_WIDTH;
        return;
    case SC_WIDTH: {                           // its data bytes: A-P or a-p is 1-16
        uint32_t w = (b >= 'A' && b <= 'P') ? (uint32_t)(b - 'A' + 1)
                   : (b >= 'a' && b <= 'p') ? (uint32_t)(b - 'a' + 1) : 0;
        if (w) { s->state = SC_SKIP; s->left = w; s->after = SC_KEY; }
        else   s->state = SC_KEY;
        return;
    }
    }
}

void iw_scan(iw_scan_t *s, const uint8_t *buf, size_t n)
{
    // A graphics payload is most of a stream: skip it whole.
    while (n) {
        if (s->state == SC_SKIP && s->left > 1) {
            size_t k = s->left - 1 < n ? s->left - 1 : n;
            s->left -= (uint32_t)k;
            buf += k;
            n -= k;
            continue;
        }
        scan_byte(s, *buf++);
        n--;
    }
}

int iw_scan_between(const iw_scan_t *s)
{
    return s->state == SC_TEXT;
}
