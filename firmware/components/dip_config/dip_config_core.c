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
