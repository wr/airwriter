// SPDX-License-Identifier: GPL-3.0-or-later
//
// The cable tester's pure core (W-1055): LocalTalk frames as line runs and
// back, so the ESP32 can put a frame on the printer port with RMT and read one
// off it, and the verdict from the two legs. No ESP-IDF; tools/rip tests it.
//
// The line code is LocalTalk's as TashTalk 2.1.3 sends it (SendPreamble,
// SendByte, SendPostamble in tashtalk-2.1.3-one-chip.asm): FM0 at 230.4
// kbit/s -- a transition at the start of every bit cell and a second one
// mid-cell for a 0 -- bytes LSB first, and a 0 stuffed after five 1s in the
// frame and its FCS. Before the frame four 1s and three flags (0x7E); after
// it a flag and thirteen 1s. Polarity carries nothing, so the decoder ignores
// it and so does a cable with a pair swapped.
#pragma once
#include <stddef.h>
#include <stdint.h>

#define CT_BIT_RATE   230400
#define CT_HALF_RATE  (2 * CT_BIT_RATE)  // FM0 half cells a second
#define CT_FRAME_MAX  32                 // LLAP bytes we build or look for, FCS excluded
// Runs an encoded frame of CT_FRAME_MAX bytes can take, stuffing included.
#define CT_RUNS_MAX   (2 * (4 + 24 + (CT_FRAME_MAX + 2) * 8 * 6 / 5 + 8 + 13 + 8))

// The frame as runs between transitions, in half cells (1 or 2 each),
// starting at the first bit boundary: the first run's level is the opposite
// of the idle line's. Returns the count, 0 if the frame is too long or the
// runs do not fit in cap.
size_t ct_fm0_encode(const uint8_t *frame, size_t n, uint8_t *runs, size_t cap);

// Runs in half cells to ticks of a clock at res_hz. Every transition is
// rounded to its nearest tick rather than every run, so the error never adds
// up (40 MHz / 460800 is 86.8 ticks a half cell).
void ct_runs_to_ticks(const uint8_t *runs, size_t n, uint32_t res_hz, uint16_t *ticks);

// Finds LLAP frames in a capture: the durations between transitions, in
// ticks at res_hz. fn gets each frame whose FCS checks, FCS stripped. A run
// that is no FM0 half or whole cell (noise, the line idling) resets the
// search, so noise before and after a frame does no harm.
typedef void (*ct_frame_fn)(void *ctx, const uint8_t *f, size_t n);
void ct_fm0_decode(const uint32_t *ticks, size_t n, uint32_t res_hz, ct_frame_fn fn, void *ctx);

// 1 if the capture holds `want` (FCS excluded) with a good FCS.
int ct_capture_has(const uint32_t *ticks, size_t n, uint32_t res_hz,
                   const uint8_t *want, size_t want_n);

// Only Apple's crossover works with the ImageWriter II, so the test answers
// that and no more: a straight cable and an unplugged one both fail.
//   back: J4 -> J2, TashTalk's frame heard on the printer port's RxD.
//   fwd:  J2 -> J4, ours relayed by TashTalk. Tried only after back passes,
//         so a printer on J2 never sees a test frame.
typedef enum {
    CT_PASS = 0,     // both ways: a crossover
    CT_NO_LOOP,      // nothing came back: straight-through, broken, or not in both ports
    CT_ONE_WAY,      // crossed, but J2's transmit pair (the print data) is broken
} ct_verdict_t;

ct_verdict_t ct_verdict(int back_ok, int fwd_ok);
const char  *ct_verdict_name(ct_verdict_t v);   // "pass", "no_loop", "one_way"
const char  *ct_verdict_text(ct_verdict_t v);   // a sentence for the web page
