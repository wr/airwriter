// SPDX-License-Identifier: GPL-3.0-or-later
// The test button's tap / hold / reset timing, pure C (test_button_core.h).
#include "test_button_core.h"

btn_ev_t btn_step(btn_t *b, int pressed, uint32_t now_ms)
{
    uint8_t s = pressed ? 1 : 0;
    if (!b->seeded) {
        b->seeded = 1;
        b->last = b->down = s;
        b->t_last = now_ms;
        b->armed = !s;
        return BTN_EV_NONE;
    }
    // A raw level held for BTN_DEBOUNCE_MS changes the level.
    if (s != b->last) {
        b->last = s;
        b->t_last = now_ms;
    }
    if (s != b->down && now_ms - b->t_last >= BTN_DEBOUNCE_MS) {
        b->down = s;
        if (s) {
            b->t_down = now_ms;
            b->holding = b->committed = 0;
            return BTN_EV_NONE;
        }
        if (!b->armed) { b->armed = 1; return BTN_EV_NONE; }
        return b->committed ? BTN_EV_RELEASE : b->holding ? BTN_EV_CANCEL : BTN_EV_TAP;
    }
    // Only a sample that is itself down moves the countdown on: the first
    // sample of a release must not start a countdown or commit a reset.
    if (!b->down || !b->armed || !s) return BTN_EV_NONE;
    uint32_t held = now_ms - b->t_down;
    if (!b->holding && held >= BTN_TAP_MAX_MS) { b->holding = 1; return BTN_EV_HOLD; }
    if (b->holding && !b->committed && held >= BTN_COMMIT_MS) {
        b->committed = 1;
        return BTN_EV_COMMIT;
    }
    return BTN_EV_NONE;
}
