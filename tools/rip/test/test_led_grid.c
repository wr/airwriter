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

// Options with one action running, in its family's slot.
static lg_opts_t act(lg_act_t a, uint32_t ms)
{
    lg_opts_t o;
    memset(&o, 0, sizeof o);
    o.task[lg_family(a)] = (lg_slot_t){ .act = (uint8_t)a, .ms = ms };
    return o;
}

static lg_slot_t *slot(lg_opts_t *o, lg_act_t a) { return &o->task[lg_family(a)]; }

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
            slot(&o, acts[a])->arg = 1000;
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
        slot(&o, LG_ACT_RESET_CANCEL)->arg = 800;
        lg_render(&v, &o, 0, f);
        for (int i = 0; i < LG_NUM_LEDS; i++) assert(max3(f[i]) <= 1);
        assert(!lg_act_over(LG_ACT_RESET_CANCEL, LG_RESET_CANCEL_MS - 1));
        assert(lg_act_over(LG_ACT_RESET_CANCEL, LG_RESET_CANCEL_MS));
        slot(&o, LG_ACT_RESET_CANCEL)->ms = LG_RESET_CANCEL_MS;
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
        slot(&o, LG_ACT_RESET_DONE)->ms = 1250;
        lg_render(&v, &o, 0, f);
        assert(lit_count(f) == 0);
        slot(&o, LG_ACT_RESET_DONE)->ms = 1400;
        lg_render(&v, &o, 0, f);
        for (int i = 0; i < LG_NUM_LEDS; i++)
            assert((max3(f[i]) > 0) == (LG_MAP[i].mat == 'P'));
        slot(&o, LG_ACT_RESET_DONE)->ms = 1550;
        lg_render(&v, &o, 0, f);
        assert(lit_count(f) == 0);
        for (uint32_t s = 1900; s < 60000; s += 997) {
            slot(&o, LG_ACT_RESET_DONE)->ms = s;
            lg_render(&v, &o, s, f);
            assert(lit_count(f) == 0);
        }
        assert(!lg_act_over(LG_ACT_RESET_DONE, 600000));
        assert(!lg_act_over(LG_ACT_RESET_CONFIRM, LG_RESET_DONE_MS - 1));
        assert(lg_act_over(LG_ACT_RESET_CONFIRM, LG_RESET_DONE_MS));
        assert(lg_act_over(LG_ACT_RESET_FAILED, LG_RESET_FAILED_MS));
    }
}

// ------------------------------------------------------------ W-1049

static ps_view_t moment(ps_state_t st, ps_state_t prev, uint32_t since)
{
    ps_view_t v = view(st, since);
    v.prev = prev;
    return v;
}

static lg_opts_t blip(lg_blip_t b, uint32_t ms)
{
    lg_opts_t o;
    memset(&o, 0, sizeof o);
    o.blip = (uint8_t)b;
    o.blip_ms = ms;
    return o;
}

static int spin_heaviest(void)
{
    uint8_t f[LG_NUM_LEDS][3];
    int m = 0;
    for (uint32_t s = 0; s < LG_BOOT_MS; s += 10) {
        ps_view_t v = view(PS_BOOTING, s);
        lg_render(&v, &WHITE_MODE, s, f);
        if (frame_sum(f) > m) m = frame_sum(f);
    }
    return m;
}

// Caps (the alert cap only where red is allowed), the boot spin's draw as the
// budget, and, for FR4-only patterns, dark mask dots.
static void check_frame(uint8_t f[LG_NUM_LEDS][3], int spin_max, int alert_ok, int fr4_only)
{
    assert(frame_sum(f) <= spin_max);
    for (int i = 0; i < LG_NUM_LEDS; i++) {
        int fr4 = LG_MAP[i].mat == 'P';
        if (fr4_only && !fr4) assert(max3(f[i]) == 0);
        for (int ch = 0; ch < 3; ch++)
            assert(f[i][ch] <= (!fr4 ? LG_MASK_CAP : alert_ok ? LG_ALERT_CAP : LG_BRIGHTNESS_CAP));
    }
}

static int same(uint8_t a[LG_NUM_LEDS][3], uint8_t b[LG_NUM_LEDS][3])
{
    return memcmp(a, b, LG_NUM_LEDS * 3) == 0;
}

static int fr4_lit(uint8_t f[LG_NUM_LEDS][3], int over)
{
    int n = 0;
    for (int i = 0; i < LG_NUM_LEDS; i++) n += LG_MAP[i].mat == 'P' && max3(f[i]) > over;
    return n;
}

static void test_w1049(uint8_t f[LG_NUM_LEDS][3], uint8_t g[LG_NUM_LEDS][3])
{
    const int spin_max = spin_heaviest();

    // Moments: within the caps and the spin's draw, FR4-only where the state
    // they open is, drawn (not the plain state) mid-way, and the plain state
    // exactly once they end.
    struct { ps_state_t st, prev; uint8_t cancelled, src; uint32_t len; int alert, fr4_only; uint8_t jf; } M[] = {
        { PS_OFFLINE,  PS_READY,     0, 0,                LG_GONE_MS,     0, 1, 0 },   // printer gone
        { PS_READY,    PS_OFFLINE,   0, 0,                LG_BACK_MS,     0, 1, 0 },   // printer back
        { PS_PRINTING, PS_ATTENTION, 0, 0,                LG_BACK_MS,     0, 0, 0 },   // paper back
        { PS_READY,    PS_PRINTING,  0, LG_SRC_AIRPRINT,  LG_PAGE_MS,     0, 0, 0 },   // page out
        { PS_READY,    PS_PRINTING,  0, LG_SRC_LOCALTALK, LG_PAGE_MS,     0, 0, 0 },
        { PS_READY,    PS_PRINTING,  1, LG_SRC_AIRPRINT,  LG_CANCEL_MS,   0, 0, 0 },   // cancelled
        { PS_SETUP,    PS_JOINING,   0, 0,                LG_JOINFAIL_MS, 1, 0, 1 },   // could not join
        { PS_SETUP,    PS_BOOTING,   0, 0,                LG_JOINFAIL_MS, 1, 0, 1 },   // ... straight from the Spin
    };
    for (unsigned m = 0; m < sizeof M / sizeof M[0]; m++) {
        lg_opts_t o;
        memset(&o, 0, sizeof o);
        o.src = M[m].src;
        for (uint32_t s = 0; s < M[m].len + 300; s += 37) {
            ps_view_t v = moment(M[m].st, M[m].prev, s);
            v.cancelled = M[m].cancelled;
            v.join_failed = M[m].jf;
            lg_render(&v, &o, 50000 + s, f);
            check_frame(f, spin_max, M[m].alert, M[m].fr4_only);
        }
        ps_view_t v = moment(M[m].st, M[m].prev, 500), plain = view(M[m].st, 500);
        v.cancelled = M[m].cancelled;
        v.join_failed = M[m].jf;
        lg_render(&v, &o, 50500, f);
        lg_render(&plain, &o, 50500, g);
        assert(!same(f, g));
        v.since_ms = plain.since_ms = M[m].len;
        lg_render(&v, &o, 50000 + M[m].len, f);
        lg_render(&plain, &o, 50000 + M[m].len, g);
        assert(same(f, g));
    }

    // Any other change of state has no moment: Setup with no failed join
    // (no credentials) opens plainly, whatever came before.
    {
        ps_view_t s = moment(PS_SETUP, PS_JOINING, 300), q = view(PS_SETUP, 300);
        lg_render(&s, &WHITE_MODE, 9000, f);
        lg_render(&q, &WHITE_MODE, 9000, g);
        assert(same(f, g));
    }
    {
        ps_view_t a = moment(PS_OFFLINE, PS_ATTENTION, 300), b = view(PS_OFFLINE, 300);
        a.cancelled = 1;
        lg_render(&a, &WHITE_MODE, 9000, f);
        lg_render(&b, &WHITE_MODE, 9000, g);
        assert(same(f, g));
        a = moment(PS_READY, PS_FAILED, 300); b = view(PS_READY, 300);
        lg_render(&a, &WHITE_MODE, 9000, f);
        lg_render(&b, &WHITE_MODE, 9000, g);
        assert(same(f, g));
    }

    // Cancelled is not page out, and both still show ink on the mask dots.
    {
        ps_view_t a = moment(PS_READY, PS_PRINTING, 300), b = a;
        b.cancelled = 1;
        lg_render(&a, &WHITE_MODE, 7000, f);
        lg_render(&b, &WHITE_MODE, 7000, g);
        assert(!same(f, g));
        int fm = 0, gm = 0;
        for (int i = 0; i < LG_NUM_LEDS; i++)
            if (LG_MAP[i].mat != 'P') { fm |= max3(f[i]) > 0; gm |= max3(g[i]) > 0; }
        assert(fm && gm);
    }

    // LocalTalk inks in the stripes: green at the top, blue at the bottom.
    // AirPrint inks warm on the same dots. The head is on row 3 here.
    {
        static uint16_t hi[LG_NUM_LEDS][3];
        ps_view_t v = view(PS_PRINTING, 5000);
        lg_opts_t o;
        memset(&o, 0, sizeof o);
        const int top = idx(0, 2), bottom = idx(7, 4);
        o.src = LG_SRC_LOCALTALK;
        lg_render16(&v, &o, 5250, hi);
        assert(hi[top][1] > hi[top][0]);
        assert(hi[bottom][2] > hi[bottom][0]);
        o.src = LG_SRC_AIRPRINT;
        lg_render16(&v, &o, 5250, hi);
        for (int k = 0; k < 2; k++) {
            int i = k ? bottom : top;
            assert(hi[i][0] >= hi[i][1] && hi[i][1] >= hi[i][2] && hi[i][0] > 0);
        }
    }

    // Tasks: caps and draw, FR4-only for firmware.
    struct { lg_act_t a; uint16_t arg; uint32_t len; int alert, fr4_only; } T[] = {
        { LG_ACT_FW_DOWNLOADING, 0,    6000, 0, 1 },
        { LG_ACT_FW_DOWNLOADING, 1000, 6000, 0, 1 },
        { LG_ACT_FW_DOWNLOADING, LG_PROGRESS_NONE, 6000, 0, 1 },
        { LG_ACT_FW_INSTALLING,  500,  6000, 0, 1 },
        { LG_ACT_FW_VERIFYING,   0,    6000, 0, 1 },
        { LG_ACT_FW_VERIFIED,    0,    6000, 0, 1 },
        { LG_ACT_PIC_FLASHING,   0,    6000, 0, 0 },
        { LG_ACT_PIC_OK,         0,    LG_PIC_OK_MS, 0, 0 },
        { LG_ACT_PIC_FAILED,     0,    LG_PIC_FAILED_MS, 1, 0 },
    };
    for (unsigned k = 0; k < sizeof T / sizeof T[0]; k++)
        for (uint32_t s = 0; s < T[k].len; s += 37) {
            ps_view_t v = view(PS_READY, 100000);
            lg_opts_t o = act(T[k].a, s);
            slot(&o, T[k].a)->arg = T[k].arg;
            lg_render(&v, &o, 20000 + s, f);
            check_frame(f, spin_max, T[k].alert, T[k].fr4_only);
        }
    assert(lg_act_over(LG_ACT_PIC_OK, LG_PIC_OK_MS) && !lg_act_over(LG_ACT_PIC_OK, LG_PIC_OK_MS - 1));
    assert(lg_act_over(LG_ACT_PIC_FAILED, LG_PIC_FAILED_MS));
    assert(!lg_act_over(LG_ACT_PIC_FLASHING, 600000) && !lg_act_over(LG_ACT_FW_VERIFIED, 600000));
    assert(!lg_act_over(LG_ACT_FW_DOWNLOADING, 600000) && !lg_act_over(LG_ACT_FW_VERIFYING, 600000));

    // Verified goes dark and stays dark until the restart.
    {
        ps_view_t v = view(PS_READY, 100000);
        lg_opts_t o = act(LG_ACT_FW_VERIFIED, LG_FW_VERIFIED_MS + 100);
        lg_render(&v, &o, 0, f);
        assert(lit_count(f) == 0);
    }

    // Firmware progress: more of the panel lit as it rises; a length-unknown
    // download still shows.
    {
        ps_view_t v = view(PS_READY, 100000);
        lg_opts_t o = act(LG_ACT_FW_DOWNLOADING, 0);
        slot(&o, LG_ACT_FW_DOWNLOADING)->arg = 100;
        lg_render(&v, &o, 300, f);
        slot(&o, LG_ACT_FW_DOWNLOADING)->arg = 900;
        lg_render(&v, &o, 300, g);
        assert(fr4_lit(g, 4) > fr4_lit(f, 4));
        slot(&o, LG_ACT_FW_DOWNLOADING)->arg = LG_PROGRESS_NONE;
        lg_render(&v, &o, 300, f);
        assert(fr4_lit(f, 4) > 0);
    }

    // Priority: the reset covers firmware, which shows again once the cancel
    // has wound back; firmware covers a TashTalk flash.
    {
        ps_view_t v = view(PS_READY, 100000);
        lg_opts_t both = act(LG_ACT_RESET_HOLD, 3000), one = both;
        *slot(&both, LG_ACT_FW_DOWNLOADING) = (lg_slot_t){ LG_ACT_FW_DOWNLOADING, 500, 4000 };
        lg_render(&v, &both, 1000, f);
        lg_render(&v, &one, 1000, g);
        assert(same(f, g));
        *slot(&both, LG_ACT_RESET_CANCEL) = (lg_slot_t){ LG_ACT_RESET_CANCEL, 400, LG_RESET_CANCEL_MS };
        one = act(LG_ACT_FW_DOWNLOADING, 4000);
        slot(&one, LG_ACT_FW_DOWNLOADING)->arg = 500;
        lg_render(&v, &both, 1000, f);
        lg_render(&v, &one, 1000, g);
        assert(same(f, g));
        *slot(&one, LG_ACT_PIC_FLASHING) = (lg_slot_t){ LG_ACT_PIC_FLASHING, 0, 700 };
        lg_render(&v, &one, 1000, f);
        assert(same(f, g));
    }

    // Held back: refused and rolled back draw the state.
    {
        ps_view_t v = view(PS_READY, 100000);
        lg_render(&v, &WHITE_MODE, 4000, g);
        const lg_act_t held[] = { LG_ACT_FW_REFUSED, LG_ACT_FW_ROLLED_BACK, LG_ACT_FW_NONE, LG_ACT_PIC_NONE };
        for (int k = 0; k < 4; k++) {
            assert(lg_act_over(held[k], 0));
            lg_opts_t o = act(held[k], 100);
            lg_render(&v, &o, 4000, f);
            assert(same(f, g));
        }
    }

    // Acknowledgements: caps and draw over Ready and Printing; over on time;
    // dropped in Booting, Needs attention and Job failed, and under a task.
    {
        const lg_blip_t B[] = { LG_BLIP_MAC, LG_BLIP_LT_ON, LG_BLIP_BUSY, LG_BLIP_SAVED };
        const uint32_t L[] = { LG_BLIP_MAC_MS, LG_BLIP_LT_ON_MS, LG_BLIP_BUSY_MS, LG_BLIP_SAVED_MS };
        for (int k = 0; k < 4; k++) {
            assert(!lg_blip_over(B[k], L[k] - 1) && lg_blip_over(B[k], L[k]));
            for (uint32_t s = 0; s < L[k]; s += 23) {
                lg_opts_t o = blip(B[k], s);
                ps_view_t v = view(PS_READY, 100000);
                lg_render(&v, &o, 30000 + s, f);
                check_frame(f, spin_max, 0, 0);
                v = view(PS_PRINTING, 100000);
                lg_render(&v, &o, 30000 + s, f);
                check_frame(f, spin_max, 0, 0);
            }
            const ps_state_t quiet[] = { PS_BOOTING, PS_ATTENTION, PS_FAILED };
            for (int q = 0; q < 3; q++) {
                ps_view_t v = view(quiet[q], 1500);
                lg_opts_t o = blip(B[k], L[k] / 3);
                lg_render(&v, &o, 31000, f);
                lg_render(&v, &WHITE_MODE, 31000, g);
                assert(same(f, g));
            }
            ps_view_t v = view(PS_READY, 100000);
            lg_opts_t o = blip(B[k], L[k] / 3), t = act(LG_ACT_FW_VERIFYING, 900);
            o.task[LG_FAM_FW] = t.task[LG_FAM_FW];
            lg_render(&v, &o, 31000, f);
            lg_render(&v, &t, 31000, g);
            assert(same(f, g));
        }
    }

    // Busy: orange, and only round the test button.
    {
        ps_view_t v = view(PS_READY, 100000);
        lg_opts_t o = blip(LG_BLIP_BUSY, 150);
        lg_render(&v, &o, 40000, f);
        lg_render(&v, &WHITE_MODE, 40000, g);
        for (int i = 0; i < LG_NUM_LEDS; i++) {
            float d = (float)((LG_MAP[i].r - 3) * (LG_MAP[i].r - 3) + (LG_MAP[i].c - 3) * (LG_MAP[i].c - 3));
            if (d > 1.6f * 1.6f) assert(memcmp(f[i], g[i], 3) == 0);
        }
        int b = idx(3, 4);
        assert(f[b][0] > f[b][1] && f[b][1] > f[b][2]);
    }

    // Review fix: the glue's clocks are 64-bit and saturate, so a one-shot set
    // early in a long uptime stays over (a 32-bit difference went negative
    // after 24.8 days and froze its first frame until day 49.7).
    {
        const uint64_t day = 86400000ull;
        assert(lg_elapsed(25 * day + 5000, 5000) == (uint32_t)(25 * day));
        assert(lg_elapsed(60 * day, 5000) == UINT32_MAX);
        assert(lg_elapsed(5000, 6000) == 0);                 // set after `now` was read
        assert(lg_act_over(LG_ACT_PIC_OK, lg_elapsed(30 * day, 5000)));
        assert(lg_blip_over(LG_BLIP_SAVED, lg_elapsed(30 * day, 5000)));
    }

    // Review fix: an acknowledgement is not drawn over an all-dot moment or a
    // cable test's answer, whose frames would then draw more than the Spin.
    {
        const lg_blip_t B[] = { LG_BLIP_MAC, LG_BLIP_LT_ON, LG_BLIP_BUSY, LG_BLIP_SAVED };
        const uint32_t L[] = { LG_BLIP_MAC_MS, LG_BLIP_LT_ON_MS, LG_BLIP_BUSY_MS, LG_BLIP_SAVED_MS };
        for (int k = 0; k < 4; k++)
            for (int m = 0; m < 4; m++)
                for (uint32_t s = 0; s < 2000; s += 41) {
                    ps_view_t v = m == 0 ? moment(PS_PRINTING, PS_ATTENTION, s)
                                : m == 1 ? moment(PS_READY, PS_PRINTING, s)
                                : m == 2 ? moment(PS_SETUP, PS_JOINING, s)
                                         : view(PS_READY, 100000);
                    if (m == 1) v.cancelled = s & 1;
                    if (m == 3) { v.cable = 1 + (s & 1); v.cable_ms = s; }
                    lg_opts_t o = blip(B[k], s % L[k]);
                    lg_render(&v, &o, 60000 + s, f);
                    check_frame(f, spin_max, m == 2, 0);
                }
        ps_view_t v = moment(PS_PRINTING, PS_ATTENTION, 500);
        lg_opts_t o = blip(LG_BLIP_LT_ON, 500);
        lg_render(&v, &o, 61000, f);
        lg_render(&v, &WHITE_MODE, 61000, g);
        assert(same(f, g));
    }

    // Asleep: a task shows at once at full level, and so does an
    // acknowledgement.
    {
        ps_view_t v = view(PS_READY, 400000);
        v.asleep = 1; v.asleep_ms = 20000;
        lg_opts_t o = act(LG_ACT_FW_VERIFYING, 400);
        lg_render(&v, &o, 0, f);
        assert(fr4_lit(f, 2) >= 6);
        o = blip(LG_BLIP_SAVED, 300);
        lg_render(&v, &o, 0, f);
        assert(lit_count(f) > 2);
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
        // A board settled early starts going out at 1.7 s and is dark at 3.0 s.
        v = view(PS_BOOTING, LG_BOOT_MS);
        v.spin_out = 1;
        v.spin_out_ms = LG_SPIN_OUT_MS;
        lg_render(&v, &WHITE_MODE, LG_BOOT_MS, f);
        assert(lit_count(f) == 0);
    }

    // W-1049's quiet power-on: a Spin still waiting keeps turning, lit but
    // eased down after 3 s, never heavier than its opening; once the board
    // settles it goes out over LG_SPIN_OUT_MS.
    {
        int heaviest = 0;
        for (uint32_t s = 0; s < 3000; s += 10) {
            ps_view_t v = view(PS_BOOTING, s);
            lg_render(&v, &WHITE_MODE, s, f);
            if (frame_sum(f) > heaviest) heaviest = frame_sum(f);
        }
        int waiting = 0;
        for (uint32_t s = 3000; s < 30000; s += 97) {
            ps_view_t v = view(PS_BOOTING, s);
            lg_render(&v, &WHITE_MODE, s, f);
            assert(lit_count(f) > LG_NUM_LEDS / 2);
            assert(frame_sum(f) <= heaviest);
            if (s >= 4000 && frame_sum(f) > waiting) waiting = frame_sum(f);
        }
        assert(waiting < heaviest * 2 / 3);         // eased to 60% while it waits
        ps_view_t v = view(PS_BOOTING, 20000);
        v.spin_out = 1;
        v.spin_out_ms = 900;
        lg_render(&v, &WHITE_MODE, 20000, f);
        assert(max3(f[idx(3, 4)]) > 0);              // the middle still lit ...
        assert(max3(f[idx(7, 4)]) == 0);             // ... the far dots gone first
        v.spin_out_ms = LG_SPIN_OUT_MS;
        lg_render(&v, &WHITE_MODE, 20000, f);
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
    test_w1049(f, g);
    printf("test_led_grid: ok\n");
    return 0;
}
