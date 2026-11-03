// SPDX-License-Identifier: GPL-3.0-or-later
// Host test for the led_grid frame renderer, panel language v4
// (docs/superpowers/specs/2026-10-14-status-language-design.md): caps, which
// dots each state may light, that the array varies dot to dot, the boot wave,
// the Wi-Fi-connected green moment, gamer mode, joining's outward rings,
// setup's two cues, attention, failure, the bidirectional print head, and
// the asleep fireflies.
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
// and the confirmation; caps, and a draw no worse than the boot wave's.
static void test_reset(uint8_t f[LG_NUM_LEDS][3], uint8_t g[LG_NUM_LEDS][3])
{
    const lg_act_t acts[] = { LG_ACT_RESET_HOLD, LG_ACT_RESET_CANCEL, LG_ACT_RESET_DONE,
                              LG_ACT_RESET_CONFIRM, LG_ACT_RESET_FAILED };

    // The boot wave's heaviest frame is the budget.
    int boot_max = 0;
    for (uint32_t s = 0; s < LG_BOOT_MS; s += 10) {
        ps_view_t v = view(PS_BOOTING, s);
        lg_render(&v, &WHITE_MODE, s, f);
        if (frame_sum(f) > boot_max) boot_max = frame_sum(f);
    }
    for (int a = 0; a < 5; a++)
        for (uint32_t s = 0; s < 9000; s += 10) {
            ps_view_t v = view(PS_READY, 0);
            lg_opts_t o = act(acts[a], s);
            slot(&o, acts[a])->arg = 1000;
            lg_render(&v, &o, s, f);
            assert(frame_sum(f) <= boot_max);
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
        assert(max3(f[idx(3, 4)]) > 8 && max3(f[idx(3, 2)]) <= LG_GLOW / 256);   // 3 o'clock lit, 9 at most the glow

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

static int boot_heaviest(void)
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

// Caps (the alert cap only where red is allowed), the boot wave's draw as the
// budget, and, for FR4-only patterns, dark mask dots.
static void check_frame(uint8_t f[LG_NUM_LEDS][3], int boot_max, int alert_ok, int fr4_only)
{
    assert(frame_sum(f) <= boot_max);
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
    const int boot_max = boot_heaviest();

    // Moments: within the caps and the boot wave's draw, FR4-only where the state
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
        { PS_SETUP,    PS_BOOTING,   0, 0,                LG_JOINFAIL_MS, 1, 0, 1 },   // ... straight from the wave
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
            check_frame(f, boot_max, M[m].alert, M[m].fr4_only);
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
            check_frame(f, boot_max, T[k].alert, T[k].fr4_only);
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
                check_frame(f, boot_max, 0, 0);
                v = view(PS_PRINTING, 100000);
                lg_render(&v, &o, 30000 + s, f);
                check_frame(f, boot_max, 0, 0);
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
    // cable test's answer, whose frames would then draw more than the boot wave.
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
                    if (m == 3) { v.cable = 1 + s % 3; v.cable_ms = s; }
                    lg_opts_t o = blip(B[k], s % L[k]);
                    lg_render(&v, &o, 60000 + s, f);
                    check_frame(f, boot_max, m == 2, 0);
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
    // between glitch flashes it is dark by design. Setup is probed in a flash
    // of its pairing blink: its glow between flashes is too dim for the dots'
    // own hues to show.)
    for (int s = 0; s < n_all; s++) {
        ps_view_t v = view(all[s], all[s] == PS_FAILED ? 5000 : 1000);
        lg_render(&v, &WHITE_MODE, all[s] == PS_SETUP ? 4890 : 4321, f);
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

    // Boot wave: in from the top-left corner, lit mid-way, mask dots included;
    // diagonal bands that move; dark by LG_BOOT_MS.
    {
        ps_view_t v = view(PS_BOOTING, 200);
        lg_render(&v, &WHITE_MODE, 200, f);
        assert(max3(f[idx(0, 2)]) > 0 && max3(f[idx(7, 4)]) == 0);
        v = view(PS_BOOTING, 1000);
        lg_render(&v, &WHITE_MODE, 1000, f);
        int fr4 = 0, mask = 0;
        for (int i = 0; i < LG_NUM_LEDS; i++) {
            if (!max3(f[i])) continue;
            if (LG_MAP[i].mat == 'P') fr4 = 1; else mask = 1;
        }
        assert(fr4 && mask);
        // The FR4 dots on one diagonal show one colour, the next diagonal's
        // another, and a second later it has moved on.
        v = view(PS_BOOTING, 1500);
        lg_render(&v, &WHITE_MODE, 1500, f);
        const uint8_t *d6 = f[idx(2, 4)];
        assert(!memcmp(d6, f[idx(6, 0)], 3) && !memcmp(d6, f[idx(5, 1)], 3)
               && !memcmp(d6, f[idx(4, 2)], 3));
        assert(memcmp(d6, f[idx(3, 4)], 3));
        v = view(PS_BOOTING, 2500);
        lg_render(&v, &WHITE_MODE, 2500, g);
        assert(memcmp(d6, g[idx(2, 4)], 3));
        // A board settled early starts going out at 1.7 s and is dark at 3.0 s.
        v = view(PS_BOOTING, LG_BOOT_MS);
        v.spin_out = 1;
        v.spin_out_ms = LG_SPIN_OUT_MS;
        lg_render(&v, &WHITE_MODE, LG_BOOT_MS, f);
        assert(lit_count(f) == 0);
    }

    // W-1049's quiet power-on: a boot wave still waiting keeps flowing, lit
    // but eased down after 3 s, never heavier than its opening; once the board
    // settles it rolls off over LG_SPIN_OUT_MS, the bottom-right corner last.
    {
        int heaviest = 0;
        for (uint32_t s = 0; s < 3000; s += 10) {
            ps_view_t v = view(PS_BOOTING, s);
            lg_render(&v, &WHITE_MODE, s, f);
            if (frame_sum(f) > heaviest) heaviest = frame_sum(f);
        }
        // The whole panel's budget: no heavier than the Spin it replaced,
        // whose heaviest frame (6990) board 1 drew 0.46 A at, all told
        // (2026-10-16). Rev A has no current limit.
        assert(heaviest <= 6990);
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
        assert(max3(f[idx(7, 6)]) > 0);              // the bottom-right corner still lit ...
        assert(max3(f[idx(0, 2)]) == 0 && max3(f[idx(1, 1)]) == 0);   // ... the top-left gone first
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

    // Setup: the pairing blink swings a dot from dim to bright, every clear
    // dot flashing together; with a phone joined it holds steady with only a
    // shimmer, under half its level (relative, so it holds at any drive).
    {
        ps_view_t v = view(PS_SETUP, 0);
        lg_render(&v, &WHITE_MODE, 90, f);                  // the first flash
        lg_render(&v, &WHITE_MODE, 1200, g);                // the glow between
        for (int i = 0; i < LG_NUM_LEDS; i++)
            if (LG_MAP[i].mat == 'P') assert(max3(f[i]) > 3 * max3(g[i]) && max3(g[i]) > 0);
    }
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
        assert(2 * (jhi - jlo) <= jhi && jlo > 0);
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

    // Asleep: fireflies. After the fade, a breath starts at a new FR4 dot
    // every LG_FIREFLY_MS and lasts LG_BREATH_MS, near-white, from off up to
    // the cap and out again, so at most three dots are lit, never the same one
    // twice at once; every FR4 dot takes a turn before any repeats.
    {
        ps_view_t v = view(PS_READY, 400000);
        v.asleep = 1;
        v.asleep_ms = 20000;
        const uint32_t b = 10, t0 = b * LG_FIREFLY_MS;
        int peak = 0, d = lg_sleep_dot(b);
        for (uint32_t t = t0; t < t0 + LG_BREATH_MS; t += 50) {
            lg_render(&v, &WHITE_MODE, t, f);
            for (int i = 0; i < LG_NUM_LEDS; i++)
                if (max3(f[i])) assert(LG_MAP[i].mat == 'P' && neutral(f[i]));
            assert(lit_count(f) <= 3);
            if (max3(f[d]) > peak) peak = max3(f[d]);
        }
        assert(peak * 10 >= LG_BRIGHTNESS_CAP * 9 && peak <= LG_BRIGHTNESS_CAP);
        lg_render(&v, &WHITE_MODE, t0, f);
        assert(max3(f[d]) == 0);                            // each breath from off
        lg_render(&v, &WHITE_MODE, t0 + LG_BREATH_MS / 2, f);
        assert(lit_count(f) == 3);                          // three at once

        int n = 0;
        for (int i = 0; i < LG_NUM_LEDS; i++) n += LG_MAP[i].mat == 'P';
        for (uint32_t k = 0; k < 2000; k++) {
            assert(LG_MAP[lg_sleep_dot(k)].mat == 'P');
            for (uint32_t j = 1; j < LG_BREATH_MS / LG_FIREFLY_MS && j <= k; j++)
                assert(lg_sleep_dot(k) != lg_sleep_dot(k - j));
            if (k % (uint32_t)n == 0) {
                int seen[LG_NUM_LEDS] = {0};
                for (uint32_t j = k; j < k + (uint32_t)n; j++) seen[lg_sleep_dot(j)]++;
                for (int i = 0; i < LG_NUM_LEDS; i++)
                    assert(seen[i] == (LG_MAP[i].mat == 'P'));
            }
        }

        v.state = PS_OFFLINE;
        lg_render(&v, &WHITE_MODE, t0 + LG_BREATH_MS / 2, f);
        assert(lit_count(f) == 3 && max3(f[d]) > 0);
        v.state = PS_READY;                                 // gamer: a colour, not white
        lg_render(&v, &GAMER_MODE, t0 + LG_BREATH_MS / 2, f);
        assert(max3(f[d]) - min3(f[d]) >= 3);
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

    // W-1115: nothing rests below the glow. Every level a state holds, long
    // after it began (no moment), over 20 s: each dot it lights is at the
    // glow or over (a count of float rounding allowed). FR4 dots in every
    // state; mask dots too in the all-dot ones.
    {
        static uint16_t hi[LG_NUM_LEDS][3];
        struct { ps_state_t st; int gamer, lt, joined; } S[] = {
            { PS_SETUP, 0, 0, 0 }, { PS_SETUP, 0, 0, 1 }, { PS_JOINING, 0, 0, 0 },
            { PS_READY, 0, 0, 0 }, { PS_READY, 1, 0, 0 }, { PS_OFFLINE, 0, 0, 0 },
            { PS_PRINTING, 0, 0, 0 }, { PS_PRINTING, 0, 1, 0 }, { PS_ATTENTION, 0, 0, 0 },
            { PS_FAILED, 0, 0, 0 } };
        int at_glow = 0;
        for (unsigned k = 0; k < sizeof S / sizeof *S; k++) {
            ps_view_t v = view(S[k].st, 60000);
            v.phone_joined = (uint8_t)S[k].joined;
            lg_opts_t o = { .gamer = (uint8_t)S[k].gamer,
                            .src = (uint8_t)(S[k].lt ? LG_SRC_LOCALTALK : LG_SRC_AIRPRINT) };
            int all = S[k].st == PS_PRINTING || S[k].st == PS_ATTENTION || S[k].st == PS_FAILED;
            for (uint32_t t = 0; t < 20000; t += 37) {
                v.since_ms = 60000 + t;     // Failed's pulse runs on since_ms
                lg_render16(&v, &o, t, hi);
                for (int i = 0; i < LG_NUM_LEDS; i++) {
                    if (LG_MAP[i].mat != 'P' && !all) continue;
                    if (S[k].st == PS_PRINTING && LG_MAP[i].r == (int)((t / 1500) % LG_ROWS))
                        continue;           // the head's row: dark ahead of the head
                    int m = hi[i][0] > hi[i][1] ? (hi[i][0] > hi[i][2] ? hi[i][0] : hi[i][2])
                                                : (hi[i][1] > hi[i][2] ? hi[i][1] : hi[i][2]);
                    assert(m >= LG_GLOW - 1);
                    at_glow += m <= LG_GLOW + 1;
                }
            }
        }
        assert(at_glow > 0);                // the embers and the rest do sit on it
        // The cable test's strip while the band runs, the reset countdown's
        // dots still to come, the firmware fill's empty rows.
        ps_view_t v = view(PS_READY, 60000);
        v.cable = PS_CABLE_PASS;
        for (uint32_t ms = 0; ms < 1200; ms += 17) {
            v.cable_ms = ms;
            lg_render16(&v, &WHITE_MODE, 0, hi);
            for (int i = 0; i < LG_NUM_LEDS; i++)
                if (LG_MAP[i].r >= 5 && LG_MAP[i].c <= 4) assert(hi[i][0] + hi[i][1] + hi[i][2] > 0 &&
                    (hi[i][0] >= LG_GLOW - 1 || hi[i][1] >= LG_GLOW - 1 || hi[i][2] >= LG_GLOW - 1));
        }
        v = view(PS_READY, 60000);
        for (uint32_t ms = 0; ms < LG_RESET_FILL_MS; ms += 53) {
            lg_opts_t o = act(LG_ACT_RESET_HOLD, ms);
            lg_render16(&v, &o, ms, hi);
            for (int i = 0; i < LG_NUM_LEDS; i++)
                assert(hi[i][0] >= LG_GLOW - 1 || hi[i][1] >= LG_GLOW - 1 || hi[i][2] >= LG_GLOW - 1);
        }
        for (uint32_t ms = 0; ms < 3000; ms += 23) {
            lg_opts_t o = act(LG_ACT_FW_VERIFYING, ms);
            lg_render16(&v, &o, ms, hi);
            for (int i = 0; i < LG_NUM_LEDS; i++)
                if (LG_MAP[i].mat == 'P')
                    assert(hi[i][0] >= LG_GLOW - 1 || hi[i][1] >= LG_GLOW - 1 || hi[i][2] >= LG_GLOW - 1);
            o = act(LG_ACT_PIC_FLASHING, ms);
            lg_render16(&v, &o, ms, hi);
            for (int i = 0; i < LG_NUM_LEDS; i++)
                assert(hi[i][0] >= LG_GLOW - 1 || hi[i][1] >= LG_GLOW - 1 || hi[i][2] >= LG_GLOW - 1);
        }
        // Back from Needs attention to Printing: before the green wave reaches
        // a dot it shows the amber it had, on the glow of the cap it is drawn
        // under now (not Needs attention's alert cap).
        {
            ps_view_t m = view(PS_PRINTING, 30);
            m.prev = PS_ATTENTION;
            lg_render16(&m, &WHITE_MODE, 0, hi);
            int far = idx(0, 4);            // the far corner from the printer socket
            assert(hi[far][0] >= LG_GLOW - 1);
        }
        for (int arg = 0; arg <= 1001; arg += 77) {
            lg_opts_t o = act(LG_ACT_FW_DOWNLOADING, 5000);
            o.task[LG_FAM_FW].arg = (uint16_t)(arg > 1000 ? LG_PROGRESS_NONE : arg);
            lg_render16(&v, &o, 5000, hi);
            for (int i = 0; i < LG_NUM_LEDS; i++)
                if (LG_MAP[i].mat == 'P')
                    assert(hi[i][0] >= LG_GLOW - 1 || hi[i][1] >= LG_GLOW - 1 || hi[i][2] >= LG_GLOW - 1);
        }
    }

    // W-1115: a fade to dark passes straight through. Falling asleep, a Ready
    // dot dims without a pause on the glow and reaches dark; the asleep
    // breath rises from dark through the levels under the glow.
    {
        static uint16_t hi[LG_NUM_LEDS][3];
        ps_view_t v = view(PS_READY, 60000);
        v.asleep = 1;
        const int dot = idx(0, 2);
        int prev = 1 << 30, under = 0, on_glow = 0, under_one = 0;
        for (uint32_t ms = 5000; ms <= 10000; ms += 25) {
            v.asleep_ms = ms;
            lg_render16(&v, &WHITE_MODE, 777, hi);  // frozen time: only the fade moves
            if (dot == lg_sleep_dot(777 / LG_FIREFLY_MS)) continue;
            int m = hi[dot][0] > hi[dot][1] ? (hi[dot][0] > hi[dot][2] ? hi[dot][0] : hi[dot][2])
                                            : (hi[dot][1] > hi[dot][2] ? hi[dot][1] : hi[dot][2]);
            assert(m <= prev);
            prev = m;
            under += m > 0 && m < LG_GLOW;
            on_glow += abs(m - LG_GLOW) <= 1;
            under_one += m > 0 && m < 256;
        }
        assert(prev == 0 && under >= 10 && on_glow <= 2);     // through it, not resting on it
        assert(under_one >= 3);             // and under one count: no floor at all
        v.asleep_ms = 60000;
        under = 0;
        int d = lg_sleep_dot(3);
        for (uint32_t t = 3 * LG_FIREFLY_MS; t < 3 * LG_FIREFLY_MS + 1000; t += 10) {
            lg_render16(&v, &WHITE_MODE, t, hi);
            int m = hi[d][0] > hi[d][1] ? (hi[d][0] > hi[d][2] ? hi[d][0] : hi[d][2])
                                        : (hi[d][1] > hi[d][2] ? hi[d][1] : hi[d][2]);
            under += m > 0 && m < LG_GLOW;
        }
        assert(under >= 5);
    }

    // W-1115: a dot under LG_BLEND is rounded and holds still, every level
    // from dark to the blend line, from another level's leftover error.
    {
        static uint16_t hi[LG_NUM_LEDS][3];
        for (int lvl = 0; lvl < LG_BLEND; lvl++) {
            lg_dither_t d;
            memset(&d, 0, sizeof d);
            for (int i = 0; i < LG_NUM_LEDS; i++)
                for (int ch = 0; ch < 3; ch++) hi[i][ch] = (uint16_t)(LG_BLEND + 101 + 37 * i + ch);
            for (int k = 0; k < 13; k++) lg_dither(&d, hi, f);
            const int want[3] = { lvl, lvl * 5 / 8, lvl / 6 };
            for (int ch = 0; ch < 3; ch++) hi[0][ch] = (uint16_t)want[ch];
            for (int k = 0; k < 300; k++) {
                lg_dither(&d, hi, f);
                for (int ch = 0; ch < 3; ch++) assert(f[0][ch] == (want[ch] + 128) >> 8);
            }
        }
    }

    // Over the blend line the dither averages to the 16-bit level, and no
    // channel steps back and forth slower than 50 times a second: frames go
    // out at 400 fps, so a dithered channel shows both of its values in every
    // 8 frames running, and one within LG_DITHER_HOLD of a whole count holds
    // it. Every steady level of channel 0 from 8 to 16 counts, the others at
    // different ones (dim channels of a bright dot included), with the error
    // left over from another level. 300 frames: long enough for a sigma-delta
    // to step at even 1/256 of a count, so a hold that is too narrow shows.
    {
        static uint16_t hi[LG_NUM_LEDS][3];
        const int span = 256 / LG_DITHER_HOLD;      // 8 frames
        for (int lvl = LG_BLEND; lvl <= 2 * LG_BLEND; lvl++) {
            lg_dither_t d;
            memset(&d, 0, sizeof d);
            const int want[3] = { lvl, 3 * LG_BLEND - lvl, (lvl * 7) % LG_BLEND };
            for (int i = 0; i < LG_NUM_LEDS; i++)
                for (int ch = 0; ch < 3; ch++) hi[i][ch] = (uint16_t)((want[ch] + 101 + 37 * i) % (2 * LG_BLEND));
            for (int k = 0; k < 13; k++) lg_dither(&d, hi, f);
            for (int ch = 0; ch < 3; ch++) hi[0][ch] = (uint16_t)want[ch];
            enum { N = 300 };
            static int seen[3][N];
            for (int k = 0; k < N; k++) {
                lg_dither(&d, hi, f);
                for (int ch = 0; ch < 3; ch++) seen[ch][k] = f[0][ch];
            }
            for (int ch = 0; ch < 3; ch++) {
                int whole = want[ch] >> 8, frac = want[ch] & 0xFF;
                int hold = frac < LG_DITHER_HOLD || frac > 256 - LG_DITHER_HOLD;
                long sum = 0;
                for (int k = 0; k < N; k++) {
                    if (hold) assert(seen[ch][k] == whole + (frac >= 128));
                    else assert(seen[ch][k] == whole || seen[ch][k] == whole + 1);
                    sum += seen[ch][k];
                }
                if (hold) continue;
                assert(labs(sum * 256 - (long)want[ch] * N) <= 2 * 256);
                for (int k = span; k + span <= N; k++) {
                    int lo = 0, up = 0;
                    for (int j = k; j < k + span; j++) {
                        lo |= seen[ch][j] == whole;
                        up |= seen[ch][j] == whole + 1;
                    }
                    assert(lo && up);
                }
            }
        }
    }

    // Dim dots keep their hue at the glow: its brightest channel is lifted
    // and the others scaled with it, not every channel lifted -- that turned
    // dim warm embers and dim blue pulses white on board 1. Embers at their
    // floor stay warm (red > green > blue), the joining pulse at its floor
    // stays blue (blue over twice red and twice green), both holding still.
    {
        static uint16_t hi[LG_NUM_LEDS][3];
        lg_dither_t d;
        memset(&d, 0, sizeof d);
        ps_view_t v = view(PS_OFFLINE, 5000);
        lg_render16(&v, &WHITE_MODE, 0, hi);        // frozen time: the floor
        int dim = -1;                       // an FR4 dot sitting at the ember floor
        for (int i = 0; i < LG_NUM_LEDS && dim < 0; i++)
            if (LG_MAP[i].mat == 'P' && abs((int)hi[i][0] - LG_GLOW) <= 1) dim = i;
        assert(dim >= 0);
        lg_dither(&d, hi, f);
        uint8_t first[3] = { f[dim][0], f[dim][1], f[dim][2] };
        assert(first[0] == LG_GLOW / 256 && first[0] > first[1] && first[1] > first[2]);
        for (int k = 0; k < 100; k++) {
            lg_dither(&d, hi, f);
            assert(!memcmp(f[dim], first, 3));
        }

        v = view(PS_JOINING, 0);
        int far = idx(7, 4);
        lg_render16(&v, &WHITE_MODE, 0, hi);        // t=0: ring at the centre, far dot at the floor
        assert(abs((int)hi[far][2] - LG_GLOW) <= 1);
        lg_dither(&d, hi, f);
        assert(f[far][2] > 2 * f[far][1] && f[far][2] > 2 * f[far][0]);
    }

    // W-1055: a cable test's answer stays in the bottom three rows between
    // the sockets (columns 0-4), the rest dark. A band runs from the LocalTalk
    // end to the printer end and back; then green glows there, or red blinks
    // twice.
    {
        ps_view_t v = view(PS_OFFLINE, 60000);
        int lt_end = idx(5, 4), pr_end = idx(5, 1), mask = idx(7, 2);
        const int outside[] = { idx(4, 2), idx(3, 4), idx(6, 6), idx(5, 6), idx(0, 4) };
        for (int k = 0; k < 2; k++) {
            v.cable = k ? PS_CABLE_FAIL : PS_CABLE_PASS;
            for (uint32_t ms = 0; ms < PS_CABLE_MS; ms += 37) {
                v.cable_ms = ms;
                lg_render(&v, &WHITE_MODE, 0, f);
                for (int i = 0; i < LG_NUM_LEDS; i++)
                    assert(max3(f[i]) <= (LG_MAP[i].mat == 'P' ? LG_BRIGHTNESS_CAP : LG_MASK_CAP));
                for (size_t j = 0; j < sizeof outside / sizeof outside[0]; j++)
                    assert(max3(f[outside[j]]) == 0);
                assert(lit_count(f) <= 10);
            }
            v.cable_ms = 30;                                 // the band starts at the LocalTalk end
            lg_render(&v, &WHITE_MODE, 0, f);
            assert(max3(f[lt_end]) > 2 * max3(f[pr_end]));
            v.cable_ms = 430;                                // and reaches the printer end
            lg_render(&v, &WHITE_MODE, 0, f);
            assert(max3(f[pr_end]) > 2 * max3(f[lt_end]));
            assert(max3(f[mask]) > 0);                       // mask dots join in
            if (!k) assert(f[pr_end][1] > f[pr_end][0] && f[pr_end][1] > f[pr_end][2]);
            else    assert(f[pr_end][0] > f[pr_end][1] && f[pr_end][0] > f[pr_end][2]);
            v.cable_ms = 1100;                               // and back, over the glow
            lg_render(&v, &WHITE_MODE, 0, f);
            assert(max3(f[lt_end]) > 2 * max3(f[pr_end]));
        }
        v.cable = PS_CABLE_PASS;                             // green glows over the strip
        v.cable_ms = 1500;
        lg_render(&v, &WHITE_MODE, 0, f);
        assert(lit_count(f) == 10);
        assert(f[lt_end][1] > f[lt_end][0] && f[pr_end][1] > f[pr_end][2]);
        v.cable_ms = PS_CABLE_MS - 1;                        // faded out
        lg_render(&v, &WHITE_MODE, 0, f);
        assert(lit_count(f) == 0);
        // Red instead blinks the strip twice.
        v.cable = PS_CABLE_FAIL;
        const uint32_t at[] = { 1300, 1500, 1700, 1900 };
        const int lit[] = { 1, 0, 1, 0 };
        for (int k = 0; k < 4; k++) {
            v.cable_ms = at[k];
            lg_render(&v, &WHITE_MODE, 0, f);
            assert(lit_count(f) == (lit[k] ? 10 : 0));
            if (lit[k]) assert(f[lt_end][0] > f[lt_end][1]);
        }
    }

    // W-1063: with no TashTalk to run the test, no sweep: the five dots by the
    // LocalTalk socket blink red twice, the rest dark, and that's all.
    {
        ps_view_t v = view(PS_OFFLINE, 60000);
        v.cable = PS_CABLE_NO_CHIP;
        int lt = idx(7, 4), button = idx(3, 4), top = idx(0, 4);
        const uint32_t at[] = { 100, 300, 500, 700, 900 };
        const int lit[] = { 0, 1, 0, 1, 0 };
        for (int k = 0; k < 5; k++) {
            v.cable_ms = at[k];
            lg_render(&v, &WHITE_MODE, 0, f);
            assert(lit_count(f) == (lit[k] ? 5 : 0));
            if (lit[k]) assert(f[lt][0] > f[lt][1] && f[lt][0] > f[lt][2]);
            assert(max3(f[button]) == 0 && max3(f[top]) == 0);
        }
    }

    test_reset(f, g);
    test_w1049(f, g);
    printf("test_led_grid: ok\n");
    return 0;
}
