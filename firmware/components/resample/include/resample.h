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
