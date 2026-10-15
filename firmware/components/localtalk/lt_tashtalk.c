// SPDX-License-Identifier: GPL-3.0-or-later
// TashTalk's host protocol; see lt_tashtalk.h.
#include "lt_tashtalk.h"
#include <string.h>

void tt_rx_init(tt_rx_t *r)
{
    memset(r, 0, sizeof *r);
}

static void end(tt_rx_t *r, tt_event_t ev, tt_frame_fn fn, void *ctx)
{
    if (r->over && ev == TT_FRAME) ev = TT_OVERFLOW;
    fn(ctx, ev, r->buf, r->len);
    r->len = 0;
    r->over = 0;
}

void tt_rx_feed(tt_rx_t *r, const uint8_t *p, size_t n, tt_frame_fn fn, void *ctx)
{
    for (size_t i = 0; i < n; i++) {
        uint8_t b = p[i];
        if (!r->esc) {
            if (b == 0x00) { r->esc = 1; continue; }
            if (r->len < sizeof r->buf) r->buf[r->len++] = b; else r->over = 1;
            continue;
        }
        r->esc = 0;
        switch (b) {
        case 0xFF:
            if (r->len < sizeof r->buf) r->buf[r->len++] = 0x00; else r->over = 1;
            break;
        case 0xFD: end(r, TT_FRAME, fn, ctx);   break;
        case 0xFC: end(r, TT_BAD_CRC, fn, ctx); break;
        case 0xFE: end(r, TT_FRAMING, fn, ctx); break;
        case 0xFA: end(r, TT_ABORT, fn, ctx);   break;
        default:   end(r, TT_UNKNOWN, fn, ctx); break;
        }
    }
}

size_t tt_send_cmd(uint8_t *out, size_t cap, const uint8_t *frame, size_t n)
{
    if (n < LLAP_HDR || n > LLAP_MAX || 1 + n + 2 > cap) return 0;
    uint16_t fcs = llap_fcs(frame, n);
    out[0] = TT_CMD_SEND;
    memcpy(out + 1, frame, n);
    out[1 + n] = (uint8_t)fcs;
    out[2 + n] = (uint8_t)(fcs >> 8);
    return 1 + n + 2;
}

size_t tt_nodes_cmd(uint8_t out[33], uint8_t node)
{
    memset(out, 0, 33);
    out[0] = TT_CMD_NODES;
    if (node) out[1 + node / 8] = (uint8_t)(1u << (node % 8));
    return 33;
}
