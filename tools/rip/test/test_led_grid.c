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

// Near-white: Ready's warmest or coolest dot keeps every channel above ~65%
// of its brightest (0.82^2.2), so 60% less a count of rounding is a tint, not
// a colour. Relative, so it holds at any drive level.
static int neutral(const uint8_t px[3])
{
    return 5 * min3(px) + 5 >= 3 * max3(px);
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

static int frame_sum(uint8_t f[LG_NUM_LEDS][3])
{
    int s = 0;
    for (int i = 0; i < LG_NUM_LEDS; i++) s += f[i][0] + f[i][1] + f[i][2];
    return s;
}

static lg_opts_t act(lg_act_t a, uint32_t ms)
{
    lg_opts_t o = { .gamer = 0, .act = (uint8_t)a, .act_ms = ms };
    return o;
}

// The factory-reset overlay (W-1045): it covers every state, sleep included;
// the countdown fills clockwise and goes orange to red; the point of no return
// and the confirmation; caps, and a draw no worse than the boot spin's.
static void test_reset(uint8_t f[LG_NUM_LEDS][3], uint8_t g[LG_NUM_LEDS][3])
{
    const lg_act_t acts[] = { LG_ACT_RESET_HOLD, LG_ACT_RESET_CANCEL, LG_ACT_RESET_DONE,
                              LG_ACT_RESET_CONFIRM, LG_ACT_RESET_FAILED };

    // The boot spin's heaviest frame is the budget.
    int spin_max = 0;
    for (uint32_t s = 0; s < LG_BOOT_MS; s += 10) {
        ps_view_t v = view(PS_BOOTING, s);
        lg_render(&v, &WHITE_MODE, s, f);
        if (frame_sum(f) > spin_max) spin_max = frame_sum(f);
    }
    for (int a = 0; a < 5; a++)
        for (uint32_t s = 0; s < 9000; s += 10) {
            ps_view_t v = view(PS_READY, 0);
            lg_opts_t o = act(acts[a], s);
            o.act_from = 1000;
            lg_render(&v, &o, s, f);
            assert(frame_sum(f) <= spin_max);
            for (int i = 0; i < LG_NUM_LEDS; i++)
                for (int ch = 0; ch < 3; ch++)
                    assert(f[i][ch] <= (LG_MAP[i].mat != 'P' ? LG_MASK_CAP : LG_ALERT_CAP));
        }

    // Over any state, asleep or not: the same frame.
    {
        ps_view_t a = view(PS_PRINTING, 5000), b = view(PS_READY, 400000);
        b.asleep = 1; b.asleep_ms = 60000;
        lg_opts_t o = act(LG_ACT_RESET_HOLD, 3000);
        lg_render(&a, &o, 3000, f);
        lg_render(&b, &o, 3000, g);
        assert(memcmp(f, g, LG_NUM_LEDS * 3) == 0);
    }

    // The countdown: a few dots near 12 o'clock at the start, about half the
    // panel half way (the right side, the hand running clockwise), all of it
    // at the end. Orange at the start, red at the end.
    {
        ps_view_t v = view(PS_READY, 0);
        int bright[3];
        const uint32_t at[3] = { 300, LG_RESET_FILL_MS / 2, LG_RESET_FILL_MS };
        for (int k = 0; k < 3; k++) {
            lg_opts_t o = act(LG_ACT_RESET_HOLD, at[k]);
            lg_render(&v, &o, at[k], f);
            bright[k] = 0;
            for (int i = 0; i < LG_NUM_LEDS; i++)
                bright[k] += LG_MAP[i].mat == 'P' ? f[i][0] > 8 : f[i][0] > 16;
        }
        assert(bright[0] <= 4);
        assert(bright[1] >= 12 && bright[1] <= 26);
        assert(bright[2] == LG_NUM_LEDS);

        lg_opts_t o = act(LG_ACT_RESET_HOLD, LG_RESET_FILL_MS / 2);
        lg_render(&v, &o, 0, f);
        assert(max3(f[idx(3, 4)]) > 8 && max3(f[idx(3, 2)]) <= 2);   // 3 o'clock lit, 9 not

        int orange = idx(0, 4), red = orange;           // just right of 12 o'clock
        o = act(LG_ACT_RESET_HOLD, 900);
        lg_render(&v, &o, 0, f);
        o = act(LG_ACT_RESET_HOLD, LG_RESET_FILL_MS);
        lg_render(&v, &o, 0, g);
        assert(f[orange][0] > 0 && f[orange][1] * 5 > f[orange][0]);  // green in it: orange
        assert(g[red][0] > 0 && g[red][1] * 8 < g[red][0]);           // barely any: red
    }

    // Cancel winds back to dark; then the state shows again.
    {
        ps_view_t v = view(PS_READY, 0);
        lg_opts_t o = act(LG_ACT_RESET_CANCEL, LG_RESET_CANCEL_MS - 1);
        o.act_from = 800;
        lg_render(&v, &o, 0, f);
        for (int i = 0; i < LG_NUM_LEDS; i++) assert(max3(f[i]) <= 1);
        assert(!lg_act_over(LG_ACT_RESET_CANCEL, LG_RESET_CANCEL_MS - 1));
        assert(lg_act_over(LG_ACT_RESET_CANCEL, LG_RESET_CANCEL_MS));
        o.act_ms = LG_RESET_CANCEL_MS;
        lg_render(&v, &o, 1000, f);
        lg_render(&v, &WHITE_MODE, 1000, g);
        assert(memcmp(f, g, LG_NUM_LEDS * 3) == 0);
    }

    // Point of no return: FR4 white, mask red, at once. Then the drain, the
    // two white beats on FR4 dots only, and dark. DONE stays dark until the
    // restart; CONFIRM hands back to the state.
    {
        ps_view_t v = view(PS_READY, 0);
        lg_opts_t o = act(LG_ACT_RESET_DONE, 50);
        lg_render(&v, &o, 0, f);
        assert(lit_count(f) == LG_NUM_LEDS);
        for (int i = 0; i < LG_NUM_LEDS; i++) {
            if (LG_MAP[i].mat == 'P') assert(neutral(f[i]));
            else assert(f[i][0] > 4 * (f[i][1] + f[i][2]));
        }
        o.act_ms = 1250;
        lg_render(&v, &o, 0, f);
        assert(lit_count(f) == 0);
        o.act_ms = 1400;
        lg_render(&v, &o, 0, f);
        for (int i = 0; i < LG_NUM_LEDS; i++)
            assert((max3(f[i]) > 0) == (LG_MAP[i].mat == 'P'));
        o.act_ms = 1550;
        lg_render(&v, &o, 0, f);
        assert(lit_count(f) == 0);
        for (uint32_t s = 1900; s < 60000; s += 997) {
            o.act_ms = s;
            lg_render(&v, &o, s, f);
            assert(lit_count(f) == 0);
        }
        assert(!lg_act_over(LG_ACT_RESET_DONE, 600000));
        assert(!lg_act_over(LG_ACT_RESET_CONFIRM, LG_RESET_DONE_MS - 1));
        assert(lg_act_over(LG_ACT_RESET_CONFIRM, LG_RESET_DONE_MS));
        assert(lg_act_over(LG_ACT_RESET_FAILED, LG_RESET_FAILED_MS));
    }
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
        assert(neutral(f[p]));
        v = view(PS_READY, 800);
        lg_render(&v, &WHITE_MODE, 800, f);
        assert(neutral(f[p]));
    }

    // Ready is a neutral white that breathes: no dot strongly tinted, and the
    // pilot's brightness moves over a breath (each dot on its own phase).
    {
        ps_view_t v = view(PS_READY, 5000);
        int lo = 255, hi8 = 0;
        for (uint32_t t = 0; t < 9000; t += 50) {
            lg_render(&v, &WHITE_MODE, t, f);
            for (int i = 0; i < LG_NUM_LEDS; i++) {
                if (LG_MAP[i].mat != 'P') continue;
                assert(neutral(f[i]));
            }
            if (max3(f[p]) < lo) lo = max3(f[p]);
            if (max3(f[p]) > hi8) hi8 = max3(f[p]);
        }
        assert(hi8 - lo >= 2);
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

    // Smooth fades: the 16-bit render resolves a slow fade (the offline
    // embers) far more finely than the few whole counts an 8-bit frame gets
    // at this brightness, and
    // agrees with the 8-bit frame to within rounding.
    {
        static uint16_t hi[LG_NUM_LEDS][3];
        ps_view_t v = view(PS_OFFLINE, 5000);
        static unsigned char seen[65536];
        int distinct = 0;
        for (uint32_t t = 0; t < 13000; t += 10) {
            lg_render16(&v, &WHITE_MODE, t, hi);
            lg_render(&v, &WHITE_MODE, t, f);
            int b = hi[p][0];          // red: the embers' dominant channel
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

    // Dim dots keep their hue. At the bottom of a fade a dot is under one
    // count; the floor must lift its brightest channel and scale the rest, not
    // lift every channel to one count -- that turned dim warm embers and dim
    // blue pulses white on board 1. Averaged through the dither: embers at
    // their dim floor stay warm (red > green > blue), the joining pulse at its
    // dim floor stays blue (blue > green > red).
    {
        static uint16_t hi[LG_NUM_LEDS][3];
        lg_dither_t d;
        double sum[3];

        memset(&d, 0, sizeof d);
        memset(sum, 0, sizeof sum);
        ps_view_t v = view(PS_OFFLINE, 5000);
        int dim = -1;                       // an FR4 dot sitting at the ember floor
        for (int i = 0; i < LG_NUM_LEDS && dim < 0; i++) {
            if (LG_MAP[i].mat != 'P') continue;
            lg_render16(&v, &WHITE_MODE, 0, hi);
            if (hi[i][0] > 0 && hi[i][0] <= 256) dim = i;
        }
        assert(dim >= 0);
        for (int k = 0; k < 400; k++) {
            lg_render16(&v, &WHITE_MODE, 0, hi);   // frozen time: the floor
            lg_dither(&d, hi, f);
            for (int ch = 0; ch < 3; ch++) sum[ch] += f[dim][ch];
        }
        assert(sum[0] > sum[1] && sum[1] > sum[2]);

        memset(&d, 0, sizeof d);
        memset(sum, 0, sizeof sum);
        v = view(PS_JOINING, 0);
        int far = idx(7, 4);
        for (int k = 0; k < 400; k++) {
            lg_render16(&v, &WHITE_MODE, 0, hi);   // t=0: ring at the centre, far dot at the floor
            lg_dither(&d, hi, f);
            for (int ch = 0; ch < 3; ch++) sum[ch] += f[far][ch];
        }
        assert(sum[2] > sum[1] && sum[1] > sum[0]);
    }

    // W-1055: a cable test's answer sweeps left to right and back over every
    // dot, then pulses out of the test button: green for a crossover, red for
    // anything else. The band is on column 3 at 350 ms and again at 1050 ms.
    {
        ps_view_t v = view(PS_OFFLINE, 60000);
        int mid = idx(6, 3), edge = idx(6, 0), mask = idx(4, 3), near = idx(3, 4), far = idx(7, 4);
        for (int k = 0; k < 2; k++) {
            v.cable = k ? PS_CABLE_FAIL : PS_CABLE_PASS;
            v.cable_ms = 350;
            lg_render(&v, &WHITE_MODE, 0, f);
            for (int i = 0; i < LG_NUM_LEDS; i++)
                assert(max3(f[i]) <= (LG_MAP[i].mat == 'P' ? LG_BRIGHTNESS_CAP : LG_MASK_CAP));
            assert(max3(f[mid]) > 2 * max3(f[edge]));        // a band, not a fill
            assert(max3(f[mask]) > 0);                       // mask dots join in
            if (!k) assert(f[mid][1] > f[mid][0] && f[mid][1] > f[mid][2]);
            else    assert(f[mid][0] > f[mid][1] && f[mid][0] > f[mid][2]);
        }
        v.cable = PS_CABLE_PASS;
        v.cable_ms = 1050;
        lg_render(&v, &WHITE_MODE, 0, f);
        assert(max3(f[mid]) > 2 * max3(f[edge]));
        v.cable_ms = 1400;                                   // the pulse starts at the button
        lg_render(&v, &WHITE_MODE, 0, f);
        assert(max3(f[near]) > 2 * max3(f[far]));
        v.cable_ms = 1400 + 590;                             // and reaches the bottom row
        lg_render(&v, &WHITE_MODE, 0, f);
        assert(max3(f[far]) > max3(f[near]));
        v.cable_ms = PS_CABLE_MS - 1;                        // faded out
        lg_render(&v, &WHITE_MODE, 0, f);
        assert(lit_count(f) == 0);
        // Red instead blinks the six dots around the button twice, the rest dark.
        v.cable = PS_CABLE_FAIL;
        const uint32_t at[] = { 1500, 1700, 1900, 2100 };
        const int lit[] = { 1, 0, 1, 0 };
        for (int k = 0; k < 4; k++) {
            v.cable_ms = at[k];
            lg_render(&v, &WHITE_MODE, 0, f);
            assert(lit_count(f) == (lit[k] ? 6 : 0));
            if (lit[k]) assert(f[near][0] > f[near][1] && max3(f[far]) == 0);
        }
    }

    test_reset(f, g);
    printf("test_led_grid: ok\n");
    return 0;
}
