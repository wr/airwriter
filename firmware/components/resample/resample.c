// SPDX-License-Identifier: GPL-3.0-or-later
#include "resample.h"
#include "bigmem.h"
#include <stdlib.h>
#include <string.h>

int rs_init_n(rs_t *r, uint32_t src_w, uint32_t src_h, uint32_t dst_w, uint32_t dst_h,
              uint32_t channels, rs_row_fn emit, void *ctx)
{
    memset(r, 0, sizeof *r);
    if (!src_w || !src_h || !dst_w || !dst_h || !channels) return -1;
    r->src_w = src_w; r->src_h = src_h;
    r->dst_w = dst_w; r->dst_h = dst_h;
    r->ch    = channels;
    r->emit  = emit;  r->ctx   = ctx;
    r->acc = big_calloc((size_t)dst_w * channels, sizeof *r->acc);
    r->out = big_calloc((size_t)dst_w * channels, 1);
    if (!r->acc || !r->out) { rs_free(r); return -1; }
    return 0;
}

int rs_init(rs_t *r, uint32_t src_w, uint32_t src_h,
            uint32_t dst_w, uint32_t dst_h, rs_row_fn emit, void *ctx)
{
    return rs_init_n(r, src_w, src_h, dst_w, dst_h, 1, emit, ctx);
}

// Fold one source row into the accumulator, box averaging horizontally.
static void fold(rs_t *r, const uint8_t *src)
{
    if (r->src_w == r->dst_w) {
        // One pixel to a box: the row as it is. A 360 dpi page crops to the
        // printer's width and goes through here, 12 million pixels a page.
        for (size_t i = 0; i < (size_t)r->dst_w * r->ch; i++) r->acc[i] += src[i];
        r->acc_rows++;
        return;
    }
    for (uint32_t x = 0; x < r->dst_w; x++) {
        uint32_t x0 = (uint32_t)((uint64_t)x       * r->src_w / r->dst_w);
        uint32_t x1 = (uint32_t)((uint64_t)(x + 1) * r->src_w / r->dst_w);
        if (x1 <= x0) x1 = x0 + 1;            // upscaling: take one pixel
        if (x1 > r->src_w) x1 = r->src_w;
        for (uint32_t c = 0; c < r->ch; c++) {
            uint32_t sum = 0;
            for (uint32_t sx = x0; sx < x1; sx++) sum += src[sx * r->ch + c];
            r->acc[x * r->ch + c] += sum / (x1 - x0);
        }
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
            for (uint32_t i = 0; i < r->dst_w * r->ch; i++)
                r->out[i] = (uint8_t)(r->acc[i] / r->acc_rows);
            r->emit(r->ctx, r->out, r->dst_w);
            r->dst_y++;
        }
        // Only reset once something was emitted. Resetting unconditionally
        // throws away every source row that did not complete a target row,
        // which silently breaks all downscaling.
        memset(r->acc, 0, (size_t)r->dst_w * r->ch * sizeof *r->acc);
        r->acc_rows = 0;
    }
    return 0;
}

void rs_free(rs_t *r)
{
    free(r->acc); free(r->out);
    r->acc = NULL; r->out = NULL;
}
