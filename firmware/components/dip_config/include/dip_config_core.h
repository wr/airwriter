// SPDX-License-Identifier: GPL-3.0-or-later
// DIP bank decode + debounce, pure C. Bit i of on_bits = DIP(i+1) switched ON
// (the glue reads closed-to-GND as ON). The assignments below are the
// canonical firmware semantics -- the silkscreen only numbers the switches.
#pragma once
#include <stdint.h>

typedef struct {
    uint8_t mode;         // DIP1-2: 0 AirPrint, 1 +EtherTalk, 2 +LocalTalk, 3 rsvd
    uint8_t dev_mode;     // DIP3
    uint8_t test_on_boot; // DIP4
    uint8_t dither_ovr;   // DIP5-6: 0 none, 1 floyd, 2 bayer, 3 threshold
    uint8_t gamer;        // DIP7: Ready and Asleep drift through RGB instead of white
    uint8_t spare;        // DIP8 raw
} dip_config_t;

void dip_decode(uint8_t on_bits, dip_config_t *out);  // bit i = switch labelled i+1 ON

// Rev A: S1 pin n sits under the switch labelled 9-n (the part's printed
// numbers run opposite to its pins), so net DIPn is label 9-n. Turns bits in
// net order (bit i = net DIP(i+1)) into label order. Check S1's pin 1 on rev B.
uint8_t dip_physical(uint8_t net_bits);

typedef struct { uint8_t s[3]; uint8_t n; } dip_deb_t; // 3-sample debouncer

// Caller seeds *out with 0xFF (an impossible sample) so the first stable read always reports.
int dip_stable(dip_deb_t *d, uint8_t sample, uint8_t *out); // 1 when settled+changed

// The test button (W-1045): a tap prints a test page; held, it counts down to
// a factory reset on the panel and commits at BTN_COMMIT_MS. Releasing before
// that cancels. Times run from the debounced press.
#define BTN_TAP_MAX_MS  1000    // released before this: a tap
#define BTN_COMMIT_MS   8000    // held this long: the reset happens, release or not
#define BTN_FILL_MS     (BTN_COMMIT_MS - BTN_TAP_MAX_MS)    // the panel's countdown

typedef enum {
    BTN_EV_NONE,
    BTN_EV_TAP,         // released before BTN_TAP_MAX_MS: print a test page
    BTN_EV_HOLD,        // still down at BTN_TAP_MAX_MS: start the countdown
    BTN_EV_CANCEL,      // released during the countdown: nothing happens
    BTN_EV_COMMIT,      // down for BTN_COMMIT_MS: reset now
    BTN_EV_RELEASE,     // released after a commit
} btn_ev_t;

typedef struct {
    uint8_t  seeded;    // a first sample has been taken
    uint8_t  last;      // last raw sample, 1 = pressed
    uint8_t  down;      // debounced: two equal samples in a row
    uint8_t  armed;     // seen up since power-on (see btn_step)
    uint8_t  holding;   // HOLD reported for this press
    uint8_t  committed; // COMMIT reported for this press
    uint32_t t_down;    // when the debounced press began
} btn_t;

// One poll sample, pressed = 1. Zero-initialise the btn_t. A press already
// down at the first sample is ignored until it is let go: holding the button
// through power-up is the bootloader's factory reset (W-1046), and the app
// must not read the tail of that hold as a tap or a second reset.
btn_ev_t btn_step(btn_t *b, int pressed, uint32_t now_ms);
