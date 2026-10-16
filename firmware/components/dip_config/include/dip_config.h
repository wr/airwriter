// SPDX-License-Identifier: GPL-3.0-or-later
// ESP glue for the DIP bank and test button: owns the GPIOs and the poll
// task. Debounce and decode are the pure code in dip_config_core.h.
#pragma once
#include "esp_err.h"
#include "dip_config_core.h"

typedef struct {
    // Debounced DIP reading. Fires once for the boot-time read (as soon as
    // it settles, ~300ms in) and again on every settled change after that.
    void (*on_change)(const dip_config_t *cfg);
    // A tap of the test button: released within BTN_TAP_MAX_MS. (The pin is
    // pulled up, so idle reads 1 and a press reads 0.)
    void (*on_test_button)(void);
    // The test button held past a tap (W-1045): BTN_EV_HOLD when the
    // countdown starts, then CANCEL, or COMMIT and later RELEASE. Runs on the
    // poll task, so it must not block.
    void (*on_hold)(btn_ev_t ev);
} dip_config_cb_t;

// Configures every DIP pin and the test-button pin that is >= 0 as an input
// with the internal pull-up, then starts the 100ms poll task. ESP_OK no-op,
// no task created, when every one of those pins is < 0 (no switches on this
// board). The callbacks are copied out of *cb before this returns, so the
// struct itself need not outlive the call; cb may be NULL, and either member
// of it may be NULL. Call once.
esp_err_t dip_config_start(const dip_config_cb_t *cb);
