// SPDX-License-Identifier: GPL-3.0-or-later
//
// What the StyleWriter page stage holds (W-1103, R15). It codes rows as they arrive and
// keeps no band: the block being filled and the row before, a plane each. For the most it
// can be asked, the 1500 in colour (a band of 800 rows x 4 planes x 360 bytes would be 1.15 MB,
// beside the 1 MB job buffer in the board's 2 MB of PSRAM), that is about the largest block cap
// and two rows of four planes. bigmem.h counts what is asked for (BIGMEM_COUNT is defined for
// this binary), so this holds the stage to its allocation, once, before a row is fed.
#include "test_util.h"
#include "sw_page.h"
#include "sw_code.h"
#include <stdlib.h>

size_t big_asked_bytes, big_asked_n;

static size_t frames_n, blocks_n;
static void sink(void *ctx, const uint8_t *f, size_t n)
{
    (void)ctx; (void)n;
    frames_n++;
    blocks_n += f[0] == SWF_BLOCK;
}

// A page of w x h rows of noise in every plane (the most coded data a row can come to).
static void feed_noise(rip_stage_t *st, uint32_t w, uint32_t h, int ink)
{
    uint32_t rb = (w + 7) / 8, v = 12345;
    uint8_t *pl[IW_INKS];
    for (int k = 0; k < IW_INKS; k++) pl[k] = malloc(rb);
    CHECK_EQ_INT(st->page_begin(st->ctx, 0, w, h), 0);
    for (uint32_t y = 0; y < h; y++) {
        for (int k = 0; k < IW_INKS; k++)
            for (uint32_t i = 0; i < rb; i++) { v = v * 1103515245u + 12345u; pl[k][i] = (uint8_t)(v >> 16); }
        if (ink) st->ink_row(st->ctx, (const uint8_t *const *)pl);
        else     st->row(st->ctx, pl[IW_BLACK]);
    }
    CHECK_EQ_INT(st->page_end(st->ctx), 0);
    for (int k = 0; k < IW_INKS; k++) free(pl[k]);
}

int main(void)
{
    for (int m = SW_ORIGINAL; m < SW_MODELS; m++)
        for (int cart = 0; cart < 2; cart++) {
            sw_page_cfg_t cfg = { .model = (sw_model_t)m, .colour_cart = cart, .out = sink };
            big_asked_bytes = big_asked_n = 0;
            sw_page_t *p = sw_page_new(&cfg);
            CHECK(p != NULL);
            if (!p) continue;

            // The largest block the stage can be asked for, and two rows of the planes it prints.
            uint32_t cap = sw_block_cap((sw_model_t)m, 1);
            if (cart && sw_block_cap((sw_model_t)m, 4) > cap) cap = sw_block_cap((sw_model_t)m, 4);
            size_t asked = big_asked_bytes;
            CHECK_EQ_INT(big_asked_n, 1);                       // one allocation, made at the start
            CHECK(asked < cap + 4u * 365 * 2);
            if (m == SW_C1500 && cart)
                printf("    the 1500 in colour asks for %zu bytes (a block cap of %u)\n", asked, cap);

            // Pages of the densest data, gray and colour, ask for nothing more.
            rip_stage_t st;
            sw_page_stage(p, &st);
            frames_n = blocks_n = 0;
            uint32_t w = (sw_caps((sw_model_t)m)->media_list[0].rip_w);
            feed_noise(&st, w, 600, 0);
            if (cart) feed_noise(&st, w, 600, 1);
            CHECK(blocks_n >= 4);                              // several blocks of whole rows
            CHECK_EQ_INT(big_asked_bytes, asked);
            CHECK_EQ_INT(big_asked_n, 1);
            sw_page_free(p);
        }
    TEST_END;
}
