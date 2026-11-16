// SPDX-License-Identifier: GPL-3.0-or-later
// ESP glue for the test button: owns its GPIO and the poll task. The tap,
// hold and reset timing is the pure code in test_button_core.h.
#pragma once
#include "esp_err.h"
#include "test_button_core.h"

typedef struct {
    // A tap: released within BTN_TAP_MAX_MS. (The pin is pulled up, so idle
    // reads 1 and a press reads 0.)
    void (*on_tap)(void);
    // The button held past a tap (W-1045): BTN_EV_HOLD when the countdown
    // starts, then CANCEL, or COMMIT and later RELEASE. Runs on the poll
    // task, so it must not block.
    void (*on_hold)(btn_ev_t ev);
} test_button_cb_t;

// Configures the button's pin as an input with the internal pull-up, then
// starts the poll task (BTN_POLL_MS). ESP_OK no-op, no task created, when the board
// has no button (pin < 0). The callbacks are copied out of *cb before this
// returns; cb may be NULL, and either member of it may be NULL. Call once.
esp_err_t test_button_start(const test_button_cb_t *cb);
