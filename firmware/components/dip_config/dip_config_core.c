// SPDX-License-Identifier: GPL-3.0-or-later
// DIP bank decode + debounce, pure C. Bit i of on_bits = DIP(i+1) switched ON
// (the glue reads closed-to-GND as ON). The assignments below are the
// canonical firmware semantics -- the silkscreen only numbers the switches.
#include "dip_config_core.h"
#include <string.h>

void dip_decode(uint8_t on, dip_config_t *out)
{
    memset(out, 0, sizeof *out);
    out->mode         = (uint8_t)(on & 0x03);
    out->dev_mode     = (uint8_t)((on >> 2) & 1);
    out->test_on_boot = (uint8_t)((on >> 3) & 1);
    out->dither_ovr   = (uint8_t)((on >> 4) & 0x03);
    out->gamer        = (uint8_t)((on >> 6) & 1);
    out->spare        = (uint8_t)((on >> 7) & 1);
}

int dip_stable(dip_deb_t *d, uint8_t sample, uint8_t *out)
{
    d->s[0] = d->s[1]; d->s[1] = d->s[2]; d->s[2] = sample;
    if (d->n < 3) { d->n++; return 0; }
    if (d->s[0] == d->s[1] && d->s[1] == d->s[2] && d->s[2] != *out) {
        *out = d->s[2];
        return 1;
    }
    return 0;
}

btn_ev_t btn_step(btn_t *b, int pressed, uint32_t now_ms)
{
    uint8_t s = pressed ? 1 : 0;
    if (!b->seeded) {
        b->seeded = 1;
        b->last = b->down = s;
        b->armed = !s;
        return BTN_EV_NONE;
    }
    // A deliberate press does not need the DIPs' three settled samples to be
    // believed: two equal samples in a row change the level.
    uint8_t same = s == b->last;
    b->last = s;
    if (same && s != b->down) {
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

uint8_t dip_physical(uint8_t net_bits)
{
    uint8_t out = 0;
    for (int i = 0; i < 8; i++)
        if (net_bits & (1u << i)) out = (uint8_t)(out | (1u << (7 - i)));
    return out;
}
