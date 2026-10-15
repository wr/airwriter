// SPDX-License-Identifier: GPL-3.0-or-later
// Host test for the led_grid frame renderer, panel language v4
// (docs/superpowers/specs/2026-10-14-status-language-design.md): caps, which
// dots each state may light, that the array varies dot to dot, the boot spin,
// the Wi-Fi-connected green moment, gamer mode, joining's outward rings,
// setup's two cues, attention, failure, the bidirectional print head, and
// the pilot light with its twinkle.
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "led_grid_core.h"
#include "printer_state_core.h"

static const lg_opts_t WHITE_MODE = { .gamer = 0 };
static const lg_opts_t GAMER_MODE = { .gamer = 1 };

static int max3(const uint8_t px[3])
{
    int m = px[0];
    if (px[1] > m) m = px[1];
    if (px[2] > m) m = px[2];
    return m;
}

static int min3(const uint8_t px[3])
{
    int m = px[0];
    if (px[1] < m) m = px[1];
    if (px[2] < m) m = px[2];
    return m;
}

static ps_view_t view(ps_state_t st, uint32_t since)
{
    ps_view_t v;
    memset(&v, 0, sizeof v);
    v.state = st;
    v.since_ms = since;
    return v;
}

static int idx(int r, int c)
{
    for (int i = 0; i < LG_NUM_LEDS; i++)
        if (LG_MAP[i].r == r && LG_MAP[i].c == c) return i;
    return -1;
}

static int lit_count(uint8_t f[LG_NUM_LEDS][3])
{
    int n = 0;
    for (int i = 0; i < LG_NUM_LEDS; i++) n += max3(f[i]) > 0;
    return n;
}

int main(void)
{
    uint8_t f[LG_NUM_LEDS][3], g[LG_NUM_LEDS][3];
    const ps_state_t all[] = { PS_BOOTING, PS_SETUP, PS_JOINING, PS_ATTENTION,
                               PS_FAILED, PS_PRINTING, PS_OFFLINE, PS_READY };
    const int n_all = (int)(sizeof all / sizeof all[0]);
    const int p = lg_pilot_index();
    assert(p == idx(3, 4) && LG_MAP[p].mat == 'P');

    // Caps: FR4 dots never exceed LG_BRIGHTNESS_CAP, mask dots LG_MASK_CAP,
    // in any state, phase or mode.
    for (int mode = 0; mode < 2; mode++)
        for (int s = 0; s < n_all; s++)
            for (uint32_t t = 0; t < 12000; t += 37) {
                ps_view_t v = view(all[s], t);
                v.greeted = t < 6000;
                lg_render(&v, mode ? &GAMER_MODE : &WHITE_MODE, t, f);
                for (int i = 0; i < LG_NUM_LEDS; i++)
                    for (int ch = 0; ch < 3; ch++)
                        assert(f[i][ch] <= (LG_MAP[i].mat != 'P' ? LG_MASK_CAP
                                          : all[s] == PS_ATTENTION ? LG_ALERT_CAP
                                                                   : LG_BRIGHTNESS_CAP));
            }

    // FR4-only states leave every mask dot dark and always light some FR4 dot.
    const ps_state_t fr4_only[] = { PS_SETUP, PS_JOINING, PS_OFFLINE, PS_READY };
    for (int s = 0; s < 4; s++)
        for (uint32_t t = 0; t < 14000; t += 113) {
            ps_view_t v = view(fr4_only[s], t);
            lg_render(&v, &WHITE_MODE, t, f);
            int any = 0;
            for (int i = 0; i < LG_NUM_LEDS; i++) {
                if (LG_MAP[i].mat != 'P') assert(max3(f[i]) == 0);
                else if (max3(f[i])) any = 1;
            }
            assert(any);
        }

    // An array, not one LED: in every state the FR4 dots show several
    // different colours at once. (A failed job is probed in its pulse phase:
    // between glitch flashes it is dark by design.)
    for (int s = 0; s < n_all; s++) {
        ps_view_t v = view(all[s], all[s] == PS_FAILED ? 5000 : 1000);
        lg_render(&v, &WHITE_MODE, 4321, f);
        int distinct = 0;
        for (int i = 0; i < LG_NUM_LEDS; i++) {
            if (LG_MAP[i].mat != 'P') continue;
            int seen = 0;
            for (int j = 0; j < i; j++)
                if (LG_MAP[j].mat == 'P' && memcmp(f[i], f[j], 3) == 0) seen = 1;
            distinct += !seen;
        }
        if (all[s] != PS_ATTENTION) assert(distinct >= 3);   // attention may be between rings
    }

    // Boot spin: lit mid-way, mask dots included; dark by LG_BOOT_MS.
    {
        ps_view_t v = view(PS_BOOTING, 1000);
        lg_render(&v, &WHITE_MODE, 1000, f);
        int fr4 = 0, mask = 0;
        for (int i = 0; i < LG_NUM_LEDS; i++) {
            if (!max3(f[i])) continue;
            if (LG_MAP[i].mat == 'P') fr4 = 1; else mask = 1;
        }
        assert(fr4 && mask);
        v = view(PS_BOOTING, LG_BOOT_MS);
        lg_render(&v, &WHITE_MODE, LG_BOOT_MS, f);
        assert(lit_count(f) == 0);
    }

    // Ready is neutral white. A greeted Ready opens green (Wi-Fi just
    // connected) and is white again by 3.2 s; an ungreeted one never greens.
    {
        ps_view_t v = view(PS_READY, 800);
        v.greeted = 1;
        lg_render(&v, &WHITE_MODE, 800, f);
        assert(f[p][1] > 2 * f[p][0] && f[p][1] > 2 * f[p][2]);
        v.since_ms = 4000;
        lg_render(&v, &WHITE_MODE, 4000, f);
        assert(abs(f[p][1] - f[p][0]) <= 3 && abs(f[p][1] - f[p][2]) <= 3);
        v = view(PS_READY, 800);
        lg_render(&v, &WHITE_MODE, 800, f);
        assert(abs(f[p][1] - f[p][0]) <= 3 && abs(f[p][1] - f[p][2]) <= 3);
    }

    // Ready's white must not read amber through the tan FR4: no dot ever
    // drives more red than blue (tuned on board 1).
    for (uint32_t t = 0; t < 12000; t += 97) {
        ps_view_t v = view(PS_READY, 5000);
        lg_render(&v, &WHITE_MODE, t, f);
        for (int i = 0; i < LG_NUM_LEDS; i++)
            if (LG_MAP[i].mat == 'P') assert(f[i][2] >= f[i][0]);
    }

    // Ready's white point, as actually sent: averaged over a breath through
    // the dither, red at most half of blue and green at least 0.8 of it.
    // Tuned on board 1, where (0.6, 0.74, 1) still read reddish.
    {
        static uint16_t hi[LG_NUM_LEDS][3];
        lg_dither_t d;
        memset(&d, 0, sizeof d);
        double sum[LG_NUM_LEDS][3];
        memset(sum, 0, sizeof sum);
        ps_view_t v = view(PS_READY, 5000);
        for (uint32_t t = 0; t < 9000; t += 10) {
            lg_render16(&v, &WHITE_MODE, t, hi);
            lg_dither(&d, hi, f);
            for (int i = 0; i < LG_NUM_LEDS; i++)
                for (int ch = 0; ch < 3; ch++) sum[i][ch] += f[i][ch];
        }
        for (int i = 0; i < LG_NUM_LEDS; i++) {
            if (LG_MAP[i].mat != 'P') continue;
            assert(sum[i][0] <= 0.5 * sum[i][2]);
            assert(sum[i][1] >= 0.8 * sum[i][2]);
        }
    }

    // Gamer mode: Ready drifts through colour -- saturated dots, and the
    // pilot's hue moves over time.
    {
        ps_view_t v = view(PS_READY, 5000);
        lg_render(&v, &GAMER_MODE, 0, f);
        int chroma = 0;
        for (int i = 0; i < LG_NUM_LEDS; i++)
            if (LG_MAP[i].mat == 'P' && max3(f[i]) - min3(f[i]) >= 3) chroma = 1;
        assert(chroma);
        lg_render(&v, &GAMER_MODE, 7000, g);
        assert(memcmp(f[p], g[p], 3) != 0);
    }

    // Joining: rings move outward -- a near dot peaks before a far one.
    {
        int near = p, far = idx(7, 4);
        assert(far >= 0 && LG_MAP[far].mat == 'P');
        int best_near = -1, best_far = -1, t_near = 0, t_far = 0;
        for (uint32_t t = 0; t < 900; t += 5) {
            ps_view_t v = view(PS_JOINING, t);
            lg_render(&v, &WHITE_MODE, t, f);
            if (max3(f[near]) > best_near) { best_near = max3(f[near]); t_near = (int)t; }
            if (max3(f[far])  > best_far)  { best_far  = max3(f[far]);  t_far  = (int)t; }
        }
        assert(t_near < t_far);
    }

    // Setup: the radar sweep swings a dot from dim to bright; with a phone
    // joined it holds steady with only a shimmer.
    {
        int lo = 255, hi = 0, jlo = 255, jhi = 0;
        for (uint32_t t = 0; t < 6000; t += 50) {
            ps_view_t v = view(PS_SETUP, t);
            lg_render(&v, &WHITE_MODE, t, f);
            if (max3(f[p]) < lo) lo = max3(f[p]);
            if (max3(f[p]) > hi) hi = max3(f[p]);
            v.phone_joined = 1;
            lg_render(&v, &WHITE_MODE, t, f);
            if (max3(f[p]) < jlo) jlo = max3(f[p]);
            if (max3(f[p]) > jhi) jhi = max3(f[p]);
        }
        assert(hi - lo >= 15);
        assert(jhi - jlo <= 10 && jlo > 0);
    }

    // Attention must read from across the room: deep amber, not whitish
    // (somewhere red is at least 4x green, never any blue), brighter than the
    // calm states' cap at the burst, mask dots joining in, and never dark --
    // between bursts every dot holds a dim amber floor.
    {
        int mask = 0, deep = 0, peak = 0;
        for (uint32_t t = 0; t < 1600; t += 20) {
            ps_view_t v = view(PS_ATTENTION, t);
            lg_render(&v, &WHITE_MODE, t, f);
            for (int i = 0; i < LG_NUM_LEDS; i++) {
                assert(max3(f[i]) > 0);
                assert(f[i][0] >= f[i][1] && f[i][2] == 0);
                if (f[i][0] >= 4 * f[i][1] && f[i][0] > 20) deep = 1;
                if (LG_MAP[i].mat == 'P' && f[i][0] > peak) peak = f[i][0];
                if (LG_MAP[i].mat != 'P' && f[i][0] > 20) mask = 1;
            }
        }
        assert(mask && deep);
        assert(peak > LG_BRIGHTNESS_CAP);
        ps_view_t v = view(PS_ATTENTION, 0);
        lg_render(&v, &WHITE_MODE, 1900, f);
        for (int i = 0; i < LG_NUM_LEDS; i++)
            if (LG_MAP[i].mat == 'P') assert(f[i][0] > 0 && f[i][0] < peak / 3);
    }

    // Failed: a glitch flash lights a scattered subset, not all and not
    // none; between flashes it is dark; afterwards the red never goes dark.
    {
        ps_view_t v = view(PS_FAILED, 100);
        lg_render(&v, &WHITE_MODE, 0, f);
        int n = lit_count(f);
        assert(n > 3 && n < LG_NUM_LEDS - 3);
        for (int i = 0; i < LG_NUM_LEDS; i++)
            if (max3(f[i])) assert(f[i][0] >= f[i][1] && f[i][0] >= f[i][2]);
        v.since_ms = 300;
        lg_render(&v, &WHITE_MODE, 0, f);
        assert(lit_count(f) == 0);
        for (uint32_t s = 1200; s < 8000; s += 100) {
            v.since_ms = s;
            lg_render(&v, &WHITE_MODE, 0, f);
            assert(f[p][0] > 0);
        }
    }

    // Printing: rows advance top to bottom, and odd rows print right to left.
    {
        int r0 = idx(0, 2), r4 = idx(4, 2), a = idx(1, 1), b = idx(1, 6);
        assert(r0 >= 0 && r4 >= 0 && a >= 0 && b >= 0);
        ps_view_t v = view(PS_PRINTING, 0);
        lg_render(&v, &WHITE_MODE, 450, f);                 // row 0, head near col 2
        assert(max3(f[r0]) > max3(f[r4]));
        lg_render(&v, &WHITE_MODE, 4 * 1500 + 750, f);      // row 4
        assert(max3(f[r4]) > max3(f[r0]));
        lg_render(&v, &WHITE_MODE, 1500 + 100, f);          // row 1 starts at the right
        assert(max3(f[b]) > max3(f[a]));
    }

    // Asleep: after the fade, only the pilot -- dim, near-white -- except
    // for a once-every-30 s twinkle on one other FR4 dot.
    {
        ps_view_t v = view(PS_READY, 400000);
        v.asleep = 1;
        v.asleep_ms = 20000;
        for (uint32_t t = 2000; t < 29500; t += 500) {
            lg_render(&v, &WHITE_MODE, t, f);
            for (int i = 0; i < LG_NUM_LEDS; i++)
                assert(i == p ? max3(f[i]) > 0 : max3(f[i]) == 0);
            assert(max3(f[p]) <= 7);
            assert(max3(f[p]) - min3(f[p]) <= 2);
        }
        lg_render(&v, &WHITE_MODE, 750, f);
        assert(lit_count(f) == 2);
        v.state = PS_OFFLINE;
        lg_render(&v, &WHITE_MODE, 5000, f);
        assert(lit_count(f) == 1 && max3(f[p]) > 0);
        v.state = PS_READY;
        lg_render(&v, &GAMER_MODE, 2000, f);
        lg_render(&v, &GAMER_MODE, 12000, g);
        assert(memcmp(f[p], g[p], 3) != 0);
    }

    // Smooth fades: the 16-bit render resolves a Ready breath far more finely
    // than the 6 whole counts an 8-bit frame gets at this brightness, and
    // agrees with the 8-bit frame to within rounding.
    {
        static uint16_t hi[LG_NUM_LEDS][3];
        ps_view_t v = view(PS_READY, 5000);
        static unsigned char seen[65536];
        int distinct = 0;
        for (uint32_t t = 0; t < 9000; t += 10) {
            lg_render16(&v, &WHITE_MODE, t, hi);
            lg_render(&v, &WHITE_MODE, t, f);
            int b = hi[p][2];
            if (!seen[b]) { seen[b] = 1; distinct++; }
            for (int i = 0; i < LG_NUM_LEDS; i++)
                for (int ch = 0; ch < 3; ch++)
                    assert(abs((int)((hi[i][ch] + 128) >> 8) - (int)f[i][ch]) <= 1);
        }
        assert(distinct >= 100);
    }

    // Temporal dither: over time the 8-bit output averages to the 16-bit
    // level; whole counts pass through untouched (no shimmer); zero stays dark.
    {
        static uint16_t hi[LG_NUM_LEDS][3];
        lg_dither_t d;
        memset(&d, 0, sizeof d);
        for (int i = 0; i < LG_NUM_LEDS; i++) {
            hi[i][0] = 384;                 // 1.5 counts
            hi[i][1] = 3 * 256;             // exactly 3
            hi[i][2] = 0;
        }
        long sum0 = 0;
        for (int k = 0; k < 200; k++) {
            lg_dither(&d, hi, f);
            sum0 += f[0][0];
            assert(f[0][1] == 3 && f[0][2] == 0);
            assert(f[0][0] == 1 || f[0][0] == 2);
        }
        assert(sum0 == 300);                // 200 frames x 1.5
    }

    printf("test_led_grid: ok\n");
    return 0;
}
