// SPDX-License-Identifier: GPL-3.0-or-later
// The test button (W-1045), pure C: a tap prints a test page; held, it counts
// down to a factory reset on the panel and commits at BTN_COMMIT_MS. Releasing
// before that cancels. Times run from the debounced press.
#pragma once
#include <stdint.h>

// The pin is read every BTN_POLL_MS, and a level held for BTN_DEBOUNCE_MS is
// the button's; shorter is contact bounce. So any tap of BTN_DEBOUNCE_MS +
// BTN_POLL_MS or more counts, however it falls between reads (W-1164).
#define BTN_POLL_MS       10
#define BTN_DEBOUNCE_MS   30

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
    uint8_t  down;      // debounced: the raw level held for BTN_DEBOUNCE_MS
    uint8_t  armed;     // seen up since power-on (see btn_step)
    uint8_t  holding;   // HOLD reported for this press
    uint8_t  committed; // COMMIT reported for this press
    uint32_t t_last;    // when the raw level last changed
    uint32_t t_down;    // when the debounced press began
} btn_t;

// One poll sample, pressed = 1, taken every BTN_POLL_MS or so (the times are
// what count, not the number of samples). Zero-initialise the btn_t. A press already
// down at the first sample is ignored until it is let go: holding the button
// through power-up is the bootloader's factory reset (W-1046), and the app
// must not read the tail of that hold as a tap or a second reset.
btn_ev_t btn_step(btn_t *b, int pressed, uint32_t now_ms);
