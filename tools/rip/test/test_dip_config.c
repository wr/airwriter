// SPDX-License-Identifier: GPL-3.0-or-later
// Host test for the DIP bank decode + debounce (dip_decode, dip_stable).
#include <assert.h>
#include <stdio.h>
#include "dip_config_core.h"

int main(void)
{
    dip_config_t c;
    dip_decode(0x00, &c);
    assert(c.mode == 0 && !c.dev_mode && !c.test_on_boot && c.dither_ovr == 0);
    dip_decode(0x01, &c); assert(c.mode == 1);            // DIP1
    dip_decode(0x02, &c); assert(c.mode == 2);            // DIP2
    dip_decode(0x04, &c); assert(c.dev_mode == 1);        // DIP3
    dip_decode(0x08, &c); assert(c.test_on_boot == 1);    // DIP4
    dip_decode(0x30, &c); assert(c.dither_ovr == 3);      // DIP5+6
    dip_decode(0xC0, &c); assert(c.spare == 3);           // DIP7+8

    // Debounce: a value must be seen 3 polls straight; report change once.
    dip_deb_t d = {0};
    uint8_t v = 0xFF;
    assert(dip_stable(&d, 0x05, &v) == 0);
    assert(dip_stable(&d, 0x04, &v) == 0);     // bounce restarts agreement
    assert(dip_stable(&d, 0x04, &v) == 0);
    assert(dip_stable(&d, 0x04, &v) == 1 && v == 0x04);
    assert(dip_stable(&d, 0x04, &v) == 0);     // stable, no re-report

    printf("test_dip_config: ok\n");
    return 0;
}
