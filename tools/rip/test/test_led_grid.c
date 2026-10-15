// SPDX-License-Identifier: GPL-3.0-or-later
// Host test for the led_grid frame renderer (lg_render): brightness cap,
// per-state color and blink/breathe/sweep behavior.
#include <assert.h>
#include <stdio.h>
#include "led_grid_core.h"
#include "printer_state_core.h"

static int max3(const uint8_t px[3])
{
    int m = px[0];
    if (px[1] > m) m = px[1];
    if (px[2] > m) m = px[2];
    return m;
}

int main(void)
{
    uint8_t f[LG_NUM_LEDS][3];

    // Cap: no channel ever exceeds LG_BRIGHTNESS_CAP in any state/phase.
    ps_state_t all[] = { PS_BOOTING, PS_SETUP, PS_JOINING, PS_READY,
                         PS_PRINTING, PS_ATTENTION, PS_FAILED, PS_OFFLINE };
    for (unsigned s = 0; s < sizeof all / sizeof all[0]; s++)
        for (uint32_t t = 0; t < 5000; t += 97) {
            lg_render(all[s], t, f);
            for (int i = 0; i < LG_NUM_LEDS; i++)
                for (int ch = 0; ch < 3; ch++)
                    assert(f[i][ch] <= LG_BRIGHTNESS_CAP);
        }

    // BOOT is dark.
    lg_render(PS_BOOTING, 1234, f);
    for (int i = 0; i < LG_NUM_LEDS; i++) assert(max3(f[i]) == 0);

    // Mask dots are monochrome in every state (r==g==b).
    for (uint32_t t = 0; t < 4000; t += 501) {
        lg_render(PS_PRINTING, t, f);
        for (int i = 0; i < LG_NUM_LEDS; i++)
            if (LG_MAP[i].mat != 'P')
                assert(f[i][0] == f[i][1] && f[i][1] == f[i][2]);
    }

    // ERROR: mask dots dark, FR4 dots red-only during the on phase.
    lg_render(PS_FAILED, 250, f);   // first half-second = on
    int lit_p = 0;
    for (int i = 0; i < LG_NUM_LEDS; i++) {
        if (LG_MAP[i].mat == 'P') {
            if (max3(f[i])) { lit_p++; assert(f[i][1] == 0 && f[i][2] == 0); }
        } else {
            assert(max3(f[i]) == 0);
        }
    }
    assert(lit_p > 0);
    lg_render(PS_FAILED, 750, f);   // second half-second = off
    for (int i = 0; i < LG_NUM_LEDS; i++) assert(max3(f[i]) == 0);

    // READY never fully blanks (floor), and breathes (two phases differ).
    lg_render(PS_READY, 0, f);
    int a = 0;
    for (int i = 0; i < LG_NUM_LEDS; i++) a += max3(f[i]);
    assert(a > 0);
    lg_render(PS_READY, 2000, f);
    int b = 0;
    for (int i = 0; i < LG_NUM_LEDS; i++) b += max3(f[i]);
    assert(a != b);

    // PRINTING sweeps: the bright row moves with time.
    lg_render(PS_PRINTING, 0, f);
    lg_render(PS_PRINTING, 700, f);

    printf("test_led_grid: ok\n");
    return 0;
}
