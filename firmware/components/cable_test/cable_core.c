// SPDX-License-Identifier: GPL-3.0-or-later
// The cable tester's pure core; see cable_core.h.
#include "cable_core.h"
#include <string.h>
#include "lt_proto.h"

// ------------------------------------------------------------- encoding

typedef struct {
    uint8_t *runs;
    size_t   n, cap;
    int      over;
} enc_t;

// FM0: a 1 is one whole-cell run, a 0 two half-cell runs.
static void put_bit(enc_t *e, int bit)
{
    int need = bit ? 1 : 2;
    if (e->n + (size_t)need > e->cap) { e->over = 1; return; }
    if (bit) e->runs[e->n++] = 2;
    else { e->runs[e->n++] = 1; e->runs[e->n++] = 1; }
}

static void put_raw(enc_t *e, uint8_t byte)
{
    for (int i = 0; i < 8; i++) put_bit(e, (byte >> i) & 1);
}

static void put_stuffed(enc_t *e, uint8_t byte, int *ones)
{
    for (int i = 0; i < 8; i++) {
        int bit = (byte >> i) & 1;
        put_bit(e, bit);
        *ones = bit ? *ones + 1 : 0;
        if (*ones == 5) { put_bit(e, 0); *ones = 0; }
    }
}

size_t ct_fm0_encode(const uint8_t *frame, size_t n, uint8_t *runs, size_t cap)
{
    if (n < LLAP_HDR || n > CT_FRAME_MAX) return 0;
    enc_t e = { .runs = runs, .cap = cap };
    for (int i = 0; i < 4; i++) put_bit(&e, 1);
    for (int i = 0; i < 3; i++) put_raw(&e, 0x7E);
    uint16_t fcs = llap_fcs(frame, n);
    int ones = 0;
    for (size_t i = 0; i < n; i++) put_stuffed(&e, frame[i], &ones);
    put_stuffed(&e, (uint8_t)fcs, &ones);
    put_stuffed(&e, (uint8_t)(fcs >> 8), &ones);
    put_raw(&e, 0x7E);
    for (int i = 0; i < 13; i++) put_bit(&e, 1);
    return e.over ? 0 : e.n;
}

void ct_runs_to_ticks(const uint8_t *runs, size_t n, uint32_t res_hz, uint16_t *ticks)
{
    uint64_t halves = 0, prev = 0;
    for (size_t i = 0; i < n; i++) {
        halves += runs[i];
        uint64_t at = (halves * res_hz + CT_HALF_RATE / 2) / CT_HALF_RATE;
        ticks[i] = (uint16_t)(at - prev);
        prev = at;
    }
}

// ------------------------------------------------------------- decoding

#define BITS_MAX ((CT_FRAME_MAX + 2) * 8 + 8)

typedef struct {
    // FM0: whether runs are known to start on bit boundaries, and a half
    // cell held while waiting for the other half of a 0.
    int     aligned, half;
    // HDLC: consecutive 1s, whether a flag has opened a frame, and its bits.
    int     ones, in_frame;
    size_t  nbits;
    uint8_t bits[BITS_MAX];
    ct_frame_fn fn;
    void   *ctx;
} dec_t;

static void hunt(dec_t *d)
{
    d->in_frame = 0;
    d->nbits = 0;
}

static void frame_end(dec_t *d)
{
    if (d->nbits % 8 || d->nbits / 8 < LLAP_HDR + 2) return;
    uint8_t f[CT_FRAME_MAX + 2];
    size_t n = d->nbits / 8;
    for (size_t i = 0; i < n; i++) {
        uint8_t b = 0;
        for (int k = 0; k < 8; k++) b |= (uint8_t)(d->bits[i * 8 + k] << k);
        f[i] = b;
    }
    if (llap_fcs_ok(f, n)) d->fn(d->ctx, f, n - 2);
}

static void hdlc_bit(dec_t *d, int bit)
{
    if (bit) {
        if (++d->ones >= 7) { hunt(d); return; }      // abort, or the line idling in 1s
        if (!d->in_frame) return;
        if (d->nbits == BITS_MAX) { hunt(d); return; } // longer than anything we look for
        d->bits[d->nbits++] = 1;
        return;
    }
    int ones = d->ones;
    d->ones = 0;
    if (ones == 5) return;                             // stuffed
    if (ones == 6) {                                   // a flag: 0 111111 0
        if (d->in_frame) {
            // Its leading 0 and six 1s went in as data; take them back out.
            d->nbits = d->nbits >= 7 ? d->nbits - 7 : 0;
            frame_end(d);
        }
        d->in_frame = 1;
        d->nbits = 0;
        return;
    }
    if (!d->in_frame) return;
    if (d->nbits == BITS_MAX) { hunt(d); return; }
    d->bits[d->nbits++] = 0;
}

void ct_fm0_decode(const uint32_t *ticks, size_t n, uint32_t res_hz, ct_frame_fn fn, void *ctx)
{
    static dec_t d;                 // 300-odd bytes: kept off the caller's stack
    memset(&d, 0, sizeof d);
    d.fn = fn;
    d.ctx = ctx;
    // A run is ticks * CT_HALF_RATE / res_hz half cells; round it, and
    // anything outside 0.5 .. 2.5 is no FM0 run at all.
    uint64_t res = res_hz;
    for (size_t i = 0; i < n; i++) {
        uint64_t q2 = (uint64_t)ticks[i] * 2 * CT_HALF_RATE;   // half cells x 2 x res_hz
        int len = q2 < res ? 0 : q2 < 3 * res ? 1 : q2 < 5 * res ? 2 : 0;
        if (!len) {                                     // noise or idle: start over
            d.aligned = d.half = 0;
            d.ones = 0;
            hunt(&d);
            continue;
        }
        if (len == 2) {
            // A whole-cell run can only start on a boundary, so it fixes the
            // alignment; a half held from before it was noise.
            if (d.half) { d.half = 0; d.ones = 0; hunt(&d); }
            d.aligned = 1;
            hdlc_bit(&d, 1);
        } else if (d.aligned) {
            if (d.half) { d.half = 0; hdlc_bit(&d, 0); }
            else d.half = 1;
        }
    }
}

typedef struct { const uint8_t *want; size_t n; int found; } has_t;

static void on_has(void *ctx, const uint8_t *f, size_t n)
{
    has_t *h = ctx;
    if (n == h->n && !memcmp(f, h->want, n)) h->found = 1;
}

int ct_capture_has(const uint32_t *ticks, size_t n, uint32_t res_hz,
                   const uint8_t *want, size_t want_n)
{
    has_t h = { want, want_n, 0 };
    ct_fm0_decode(ticks, n, res_hz, on_has, &h);
    return h.found;
}

// ------------------------------------------------------------- verdict

ct_verdict_t ct_verdict(ct_tt_t tt, int back_ok, int fwd_ok)
{
    switch (tt) {
    case CT_TT_OK:          break;
    case CT_TT_NO_CHIP:     return CT_NO_CHIP;
    case CT_TT_NO_TASHTALK: return CT_NO_TASHTALK;
    case CT_TT_UNCHECKED:   return CT_CHIP_SILENT;
    }
    if (!back_ok) return CT_NO_LOOP;
    return fwd_ok ? CT_PASS : CT_ONE_WAY;
}

int ct_verdict_tested(ct_verdict_t v)
{
    return v == CT_PASS || v == CT_NO_LOOP || v == CT_ONE_WAY;
}

const char *ct_verdict_name(ct_verdict_t v)
{
    switch (v) {
    case CT_PASS:        return "pass";
    case CT_NO_LOOP:     return "no_loop";
    case CT_ONE_WAY:     return "one_way";
    case CT_NO_CHIP:     return "no_chip";
    case CT_NO_TASHTALK: return "no_tashtalk";
    case CT_CHIP_SILENT: return "chip_silent";
    }
    return "?";
}

const char *ct_verdict_text(ct_verdict_t v)
{
    switch (v) {
    case CT_PASS:
        return "Crossover: this cable works with the ImageWriter II. "
               "(Its handshake wires, pins 1 and 2, can't be tested.)";
    case CT_NO_LOOP:
        return "No crossover between the printer and LocalTalk ports. The cable is "
               "straight-through or broken, or isn't plugged into both ports. "
               "Only a crossover works with the ImageWriter II.";
    case CT_ONE_WAY:
        return "The cable is crossed but one wire pair is broken: the one that carries "
               "print data to the printer.";
    case CT_NO_CHIP:
        return "No cable was tested: AirWriter can't find a LocalTalk chip, and the test "
               "runs through it. See LocalTalk Chip (TashTalk) in the AirWriter wiki.";
    case CT_NO_TASHTALK:
        return "No cable was tested: the LocalTalk chip isn't running TashTalk, and the "
               "test runs through it. It needs flashing first.";
    case CT_CHIP_SILENT:
        return "No cable was tested: the LocalTalk chip isn't answering, and the test runs "
               "through it. It may be missing or need TashTalk: turn LocalTalk off (DIP "
               "switch 2), restart, and test again to find out.";
    }
    return "";
}
