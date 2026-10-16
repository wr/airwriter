// SPDX-License-Identifier: GPL-3.0-or-later
// Frame renderer for the 37-dot art panel, panel language v4 with W-1049's
// tasks, moments and acknowledgements: a pure function of (view, options, t),
// so the host tests can walk every phase. The spec is
// docs/superpowers/specs/2026-10-14-status-language-design.md.
//
// Material physics measured on board 1: FR4 dots are the crisp pixels; mask
// dots pass ~10% (B) / ~5% (L), red well, blue as teal. So information lives
// on the FR4 dots, and mask dots join in only for the boot spin and the
// printing / attention / failed states, at LG_MASK_CAP.
//
// It is an array, not one LED: every pattern uses position (sweeps, ripples,
// the print head), and each dot carries a fixed variation from a hash of its
// row and column -- its own phase, period and hue within the state's family.
//
// Levels are perceptual and go through gamma 2.2 before the cap; the pilot
// light and its twinkle are driven linearly, because gamma would round those
// few counts to zero.
#include "led_grid_core.h"
#include <math.h>
#include <string.h>

#define TAU 6.2831853f

typedef struct { float r, g, b; } lg_c3;

static const lg_c3 DARK = {0, 0, 0};
static const lg_c3 FULL = {1, 1, 1};

static float clampf(float x) { return x < 0 ? 0 : x > 1 ? 1 : x; }
static float ss(float e0, float e1, float x)
{
    float t = clampf((x - e0) / (e1 - e0));
    return t * t * (3 - 2 * t);
}
static float gauss(float x, float w) { return expf(-(x * x) / w); }
static lg_c3 sc(lg_c3 a, float k) { return (lg_c3){a.r * k, a.g * k, a.b * k}; }
static lg_c3 add(lg_c3 a, lg_c3 b) { return (lg_c3){a.r + b.r, a.g + b.g, a.b + b.b}; }
static lg_c3 mix(lg_c3 a, lg_c3 b, float k)
{
    return (lg_c3){a.r + (b.r - a.r) * k, a.g + (b.g - a.g) * k, a.b + (b.b - a.b) * k};
}

// lo -> hi -> lo over period_ms, smooth at both ends; lo at phase 0.
static float pulse(uint32_t t, uint32_t period_ms, float lo, float hi)
{
    float ph = (float)(t % period_ms) / (float)period_ms;
    return lo + (hi - lo) * (0.5f - 0.5f * cosf(TAU * ph));
}

static lg_c3 hsv(float h, float s, float v)
{
    h = fmodf(fmodf(h, 360.0f) + 360.0f, 360.0f) / 60.0f;
    int i = (int)h;
    float f = h - (float)i, p = v * (1 - s), q = v * (1 - s * f), t = v * (1 - s * (1 - f));
    switch (i % 6) {
    case 0:  return (lg_c3){v, t, p};
    case 1:  return (lg_c3){q, v, p};
    case 2:  return (lg_c3){p, v, t};
    case 3:  return (lg_c3){p, q, v};
    case 4:  return (lg_c3){t, p, v};
    default: return (lg_c3){v, p, q};
    }
}

// Fixed per-dot variation in [0,1): a hash of (row, col) and a salt, so every
// dot gets its own phase, period and hue, the same on every boot.
static float dot_hash(int r, int c, int salt)
{
    uint32_t x = (uint32_t)r * 73856093u ^ (uint32_t)c * 19349663u ^ (uint32_t)salt * 83492791u;
    x ^= x >> 13;
    x *= 1274126177u;
    x ^= x >> 16;
    return (float)(x % 10000u) / 10000.0f;
}

// A hue inside a family: base +- spread per dot, optionally drifting +- drift.
static float jitter(int r, int c, float base, float spread, uint32_t t, float drift)
{
    float h = base + (dot_hash(r, c, 3) - 0.5f) * 2 * spread;
    if (drift != 0)
        h += drift * sinf(TAU * (float)(t % 9000) / 9000.0f + dot_hash(r, c, 4) * TAU);
    return h;
}

// Ready's neutral white: each dot a touch warm or a touch cool. (Tweaks made
// on 10-14/15 were judged while the board was really in Printer offline, so
// they were reverted to this, the approved v4 white.)
static lg_c3 white_of(int r, int c)
{
    static const lg_c3 warm = {1.00f, 0.92f, 0.82f}, cool = {0.86f, 0.92f, 1.00f};
    return mix(warm, cool, dot_hash(r, c, 5));
}

// Each dot breathes on its own 5-9 s period, 43% to 100%. (Was 16-38%, then
// 30-70%: both read too dim on board 1. Every effect now peaks at 100% of its
// cap, its lower levels scaled with it, so this is the 30-70% breath at 2.2x
// the drive.)
static float ready_level(int r, int c, uint32_t t)
{
    uint32_t period = 5000 + (uint32_t)(dot_hash(r, c, 1) * 4000);
    return 0.715f + 0.285f * sinf(TAU * (float)(t % period) / (float)period
                                  + dot_hash(r, c, 2) * TAU);
}

// Gamma 2.2, then the cap, at 8.8 fixed point.
static float level16(float level, float cap)
{
    level = clampf(level);
    return level <= 0 ? 0 : powf(level, 2.2f) * cap * 256.0f;
}

// At a cap of 48 the bottom of a slow fade drops below one count and the dot
// would blink off. Lift the dot's brightest channel to one count and scale the
// others with it, so the dither keeps its hue -- lifting every channel to one
// count turned dim embers and dim blue pulses white on board 1.
static void put_dot(uint16_t px[3], float r, float g, float b)
{
    float m = r > g ? (r > b ? r : b) : (g > b ? g : b);
    if (m < 1) {                    // under 1/256 of a count (a ripple's far tail,
        r = g = b = 0;              // even a subnormal): dark, not lifted
    } else if (m < 256) {
        float k = 256.0f / m;
        r *= k; g *= k; b *= k;
    }
    float ch[3] = { r, g, b };
    for (int i = 0; i < 3; i++)
        px[i] = (uint16_t)(ch[i] > 65280 ? 65280 : ch[i] + 0.5f);
}

static int all_dots(ps_state_t st)
{
    return st == PS_BOOTING || st == PS_PRINTING || st == PS_ATTENTION || st == PS_FAILED;
}

int lg_pilot_index(void)
{
    for (int i = 0; i < LG_NUM_LEDS; i++)
        if (LG_MAP[i].r == 3 && LG_MAP[i].c == 4) return i;
    return 0;
}

// The boot spin: a hue wheel around the test button (row 3, col 3) that fades
// in by distance and spins. W-1049's quiet power-on keeps it turning until the
// board has settled, at the pace it had reached by 2.5 s and eased to 60%
// after 3 s (less draw while Wi-Fi joins); then it collapses inward over
// LG_SPIN_OUT_MS (`out`, `e` seconds in), the far dots first and the test
// button last. A board settled early starts going out at 1.7 s, which is the
// original 3.0 s Spin exactly.
static lg_c3 spin(float dist, float ang, float t, int out, float e)
{
    float v = ss(0, 0.6f, t - dist * 0.08f) * (1 - 0.4f * ss(3.0f, 4.0f, t));
    if (out) v *= 1 - ss(0.6f, 1.3f, e + dist * 0.12f);
    float turn = t <= 2.5f ? t * 300 - t * t * 40 : 500 + (t - 2.5f) * 100;
    return hsv(ang + turn, 0.9f, v);
}

// LocalTalk's colours: the six stripes of the classic Apple logo, top to
// bottom over the panel's eight rows.
static lg_c3 stripe(int r, int c)
{
    static const float hue[6] = { 104, 41, 28, 358, 299, 197 };
    int k = (int)(((float)r + 0.5f) * 6.0f / LG_ROWS);
    return hsv(hue[k] + (dot_hash(r, c, 3) - 0.5f) * 8, 0.85f, 1);
}

static lg_c3 state_dot(const ps_view_t *v, const lg_opts_t *o, int i, uint32_t t)
{
    int r = LG_MAP[i].r, c = LG_MAP[i].c;
    float dr = (float)r - 3, dc = (float)c - 3;
    float dist = hypotf(dr, dc), ang = atan2f(dr, dc) * 57.29578f;
    float h1 = dot_hash(r, c, 1), h2 = dot_hash(r, c, 2);
    uint32_t s = v->since_ms;

    if (v->cable) {
        // A cable test's answer (W-1055): a band sweeps the panel left to
        // right and back, 700 ms each way. Green, the cable works: a ring
        // pulses out of the test button and the panel fades. Red, it doesn't
        // or there isn't one: the six dots around the button blink twice.
        uint32_t cs = v->cable_ms;
        const uint32_t sweep = 700;
        float lv;
        if (cs < 2 * sweep) {
            float x = cs < sweep ? -1.0f + 8.0f * (float)cs / (float)sweep
                                 : 7.0f - 8.0f * (float)(cs - sweep) / (float)sweep;
            lv = gauss((float)c - x, 1.0f);
        } else if (v->cable == PS_CABLE_PASS) {
            lv = gauss(dist - (float)(cs - 2 * sweep) / 1000.0f * 7.0f, 1.4f);
        } else {
            uint32_t b = cs - 2 * sweep;                 // 200 on, 200 off, 200 on
            int on = dist < 1.5f && (b < 200 || (b >= 400 && b < 600));
            return on ? hsv(jitter(r, c, 2, 10, t, 0), 0.95f, 1) : DARK;
        }
        float hue = v->cable == PS_CABLE_PASS ? jitter(r, c, 130, 14, t, 0)
                                              : jitter(r, c, 2, 10, t, 0);
        float fade = 1 - ss((float)PS_CABLE_MS - 300, (float)PS_CABLE_MS, (float)cs);
        return hsv(hue, 0.9f, (0.08f + 0.92f * lv) * fade);
    }

    switch (v->state) {
    case PS_BOOTING:
        return spin(dist, ang, (float)s / 1000.0f, v->spin_out, (float)v->spin_out_ms / 1000.0f);

    case PS_SETUP: {
        float hue = jitter(r, c, 220, 16, t, 6);
        if (v->phone_joined) {
            uint32_t period = 3000 + (uint32_t)(h1 * 2500);
            float lvl = 0.88f + 0.12f * sinf(TAU * (float)(t % period) / (float)period + h2 * TAU);
            return hsv(hue, 0.85f, lvl);
        }
        // Radar sweep: the arm turns once per 2.6 s; a dot flares as it passes
        // and decays behind it.
        float arm = (float)(t % 2600) / 2600.0f * 360.0f;
        float behind = fmodf(fmodf(arm - ang, 360.0f) + 360.0f, 360.0f);
        return hsv(hue, 0.85f, 0.09f + 0.91f * expf(-behind / 55.0f));
    }

    case PS_JOINING: {
        float ring = (float)(t % 900) / 900.0f * 4.8f;          // outward from the test button
        return hsv(jitter(r, c, 215, 18, t, 0), 0.85f, 0.10f + 0.90f * gauss(dist - ring, 1.1f));
    }

    case PS_READY: {
        lg_c3 base = o && o->gamer
            ? hsv((float)c * 28 + (float)r * 14 + (float)t / 40.0f, 0.75f,
                  0.07f + 0.93f * ready_level(r, c, t))
            : sc(white_of(r, c), ready_level(r, c, t));
        if (!v->greeted || s >= 3200) return base;
        // Wi-Fi just connected: green, with a ripple outward, then into base.
        float ring = (float)s / 1400.0f * 5.2f;
        lg_c3 green = hsv(jitter(r, c, 130, 14, t, 0), 0.8f, 0.5f + 0.5f * gauss(dist - ring, 1.2f));
        return mix(green, base, ss(2200, 3200, (float)s));
    }

    case PS_OFFLINE: {
        uint32_t period = 7000 + (uint32_t)(h1 * 6000);
        float ph = sinf(TAU * (float)(t % period) / (float)period + h2 * TAU);
        float ember = ph > 0 ? ph * ph * ph : 0;
        // 15% swelling to 100%: the first tune (5-32%) and the second (9-60%)
        // were lost in daylight like Ready's.
        return hsv(jitter(r, c, 34, 9, t, 0), 0.5f + 0.12f * h1, 0.15f + 0.85f * ember);
    }

    case PS_PRINTING: {
        // A print head crosses the current row in 1.5 s, inking it behind;
        // rows advance top to bottom and alternate direction, like the
        // ImageWriter's bidirectional head.
        uint32_t line = t / 1500;
        int row = (int)(line % LG_ROWS), ltr = (line % 2) == 0;
        float f = (float)(t % 1500) / 1500.0f;
        float x = ltr ? f * 7.6f - 0.3f : 6.3f - f * 7.6f;
        // AirPrint and the test page ink warm; a LocalTalk job inks each row
        // in its stripe, and the stripes show across the whole panel.
        int lt = o && o->src == LG_SRC_LOCALTALK;
        lg_c3 ink = lt ? stripe(r, c) : hsv(jitter(r, c, 38, 10, t, 0), 0.4f, 1);
        if (r == row) {
            int inked = ltr ? ((float)c < x) : ((float)c > x);
            return add(sc(FULL, gauss((float)c - x, 0.6f)), sc(ink, inked ? 0.35f : 0));
        }
        int back = (row - r + LG_ROWS) % LG_ROWS;
        if (back == 1) return sc(ink, lt ? 0.35f : 0.25f);
        if (back == 2) return sc(ink, lt ? 0.22f : 0.12f);
        return sc(ink, lt ? 0.12f : 0.03f);
    }

    case PS_ATTENTION: {
        // Two wide amber rings out of the test button, 350 ms apart, every
        // 2 s, over a dim amber floor so the panel is never dark while it
        // needs a person. Deep amber: at hue 36 the green die (brighter per
        // count) washed it toward white through the FR4.
        uint32_t ph = t % 2000;
        float lv = 0.12f;
        if (ph < 1200) lv += gauss(dist - (float)ph / 1000.0f * 6, 1.6f);
        if (ph >= 350 && ph < 1550) lv += gauss(dist - (float)(ph - 350) / 1000.0f * 6, 1.6f);
        return hsv(jitter(r, c, 24, 6, t, 0), 1, clampf(lv));
    }

    case PS_FAILED: {
        lg_c3 red = hsv(jitter(r, c, 2, 10, t, 0), 0.95f, 1);
        if (s < 1200) {
            int flash = (int)(s / 400);
            return (s % 400) < 200 && dot_hash(r, c, 10 + flash) > 0.35f ? red : DARK;
        }
        float period = 2000 * (0.85f + 0.3f * h1);
        float ph = fmodf((float)(s - 1200), period) / period;
        return sc(red, 0.33f + 0.67f * (0.5f - 0.5f * cosf(TAU * ph + h2 * 1.5f)));
    }
    }
    return DARK;
}

// ---------------------------------------------------------------- moments
// W-1049: some states open with a moment drawn from the state before
// (ps_view_t.prev), keyed off since_ms. Wherever its wave has not reached yet
// still shows the old state. Any other change of state has none.
//
// The sockets are on the bottom edge: the printer (J2) under column 0.9,
// LocalTalk (J4) under 3.2. Waves start from row 8.6, just below the grid (the
// sockets are 4.6 rows lower), so they enter at once.
#define PORT_ROW    8.6f
#define PRINTER_COL 0.9f
#define LT_COL      3.2f
#define FAR         10.5f       // a socket to the far corner, in rows
#define CROSS_MS    1100.0f     // a cable wave's time to cross the panel

static float dist_to(int r, int c, float pr, float pc)
{
    return hypotf((float)r - pr, (float)c - pc);
}

// Light added over light: an acknowledgement on top of the state.
static lg_c3 screen(lg_c3 a, lg_c3 b)
{
    return (lg_c3){1 - (1 - clampf(a.r)) * (1 - clampf(b.r)),
                   1 - (1 - clampf(a.g)) * (1 - clampf(b.g)),
                   1 - (1 - clampf(a.b)) * (1 - clampf(b.b))};
}

// Green says connected, or it worked (FeatherFrame's); purple, updating.
static lg_c3 green_of(int r, int c, uint32_t t) { return hsv(jitter(r, c, 130, 14, t, 0), 0.8f, 1); }
static lg_c3 purple_of(int r, int c, uint32_t t) { return hsv(jitter(r, c, 280, 10, t, 0), 0.9f, 1); }

static int join_failed(const ps_view_t *v)      // its two red pulses, before Setup fades in
{
    return v->state == PS_SETUP && v->join_failed && v->since_ms < 1000;
}

static int page_out(const ps_view_t *v)
{
    return v->state == PS_READY && v->prev == PS_PRINTING && !v->cancelled
        && v->since_ms < LG_PAGE_MS;
}

static int cancel_out(const ps_view_t *v)
{
    return v->state == PS_READY && v->prev == PS_PRINTING && v->cancelled
        && v->since_ms < LG_CANCEL_MS;
}

static int moment_all(const ps_view_t *v)       // the moment lights mask dots too
{
    return page_out(v) || cancel_out(v) || join_failed(v);
}

// An acknowledgement over an all-dot moment or a cable test's answer would
// draw more than the boot spin (review of W-1049): it is not drawn then.
static int busy_panel(const ps_view_t *v)
{
    return moment_all(v) || v->cable
        || (v->state == PS_PRINTING && v->prev == PS_ATTENTION && v->since_ms < LG_BACK_MS);
}

static lg_c3 view_dot(const ps_view_t *v, const lg_opts_t *o, int i, uint32_t t)
{
    int r = LG_MAP[i].r, c = LG_MAP[i].c, fr4 = LG_MAP[i].mat == 'P';
    float s = (float)v->since_ms;
    lg_c3 now = state_dot(v, o, i, t);
    ps_view_t was = *v;
    was.state = v->prev;
    was.since_ms = 100000;      // its own opening moments long over
    was.greeted = was.phone_joined = 0;

    // Printer back: a green wave out of the printer socket.
    if (((v->state == PS_READY && v->prev == PS_OFFLINE) ||
         (v->state == PS_PRINTING && v->prev == PS_ATTENTION)) && s < LG_BACK_MS) {
        float ds = s - dist_to(r, c, PORT_ROW, PRINTER_COL) / FAR * CROSS_MS;
        lg_c3 green = green_of(r, c, t);
        if (ds < 0) return mix(state_dot(&was, o, i, t), green, gauss(ds / 160.0f, 1));
        return mix(green, now, ss(150, 900, ds));
    }
    // Printer gone: the white drains into the printer socket, a glint at the
    // front, the embers coming up behind it.
    if (v->state == PS_OFFLINE && v->prev == PS_READY && s < LG_GONE_MS) {
        float ds = s - (FAR - dist_to(r, c, PORT_ROW, PRINTER_COL)) / FAR * CROSS_MS;
        lg_c3 glint = sc(white_of(r, c), gauss(ds / 130.0f, 1));
        if (ds < 0) return screen(state_dot(&was, o, i, t), glint);
        return screen(sc(now, ss(250, 900, ds)), glint);
    }
    // Cancelled: the print stops where it stood and fades; no page out.
    if (cancel_out(v)) {
        lg_c3 stopped = sc(state_dot(&was, o, i, t - (uint32_t)s), 1 - ss(0, 600, s));
        return fr4 ? screen(stopped, sc(now, ss(300, LG_CANCEL_MS, s))) : stopped;
    }
    // Page out: the inked sheet feeds up off the top, Ready below its edge.
    if (page_out(v)) {
        float edge = 8.5f - s / 900.0f * 10.0f;
        float off = s / 900.0f * 10.0f;
        lg_c3 ink = o && o->src == LG_SRC_LOCALTALK ? stripe(r, c)
                                                    : hsv(jitter(r, c, 38, 10, t, 0), 0.4f, 1);
        float band = 0.33f + 0.67f * (0.5f + 0.5f * cosf(TAU * ((float)r + off) / 3.0f));
        float k = ss(-0.5f, 0.5f, (float)r - edge);     // 1 below the sheet
        return mix(sc(ink, band), fr4 ? sc(now, ss(0, 1, k)) : DARK, k);
    }
    // Could not join: two red pulses (FeatherFrame's), then Setup. After the
    // saved network was tried, from Joining or straight from the Spin.
    if (v->state == PS_SETUP && v->join_failed && s < LG_JOINFAIL_MS) {
        if (s < 1000) {
            float x = sinf(3.1415927f * fmodf(s, 500) / 500.0f);
            return hsv(jitter(r, c, 2, 8, t, 0), 0.95f, x * x);
        }
        return sc(now, ss(1000, LG_JOINFAIL_MS, s));
    }
    return now;
}

// ---------------------------------------------------------------- tasks
// The factory reset (W-1045), a firmware update and a TashTalk flash (W-1049),
// drawn instead of the state. The reset is orange turning red because it is a
// warning a person is choosing to walk into; mask dots join in, since red and
// amber survive the mask. FR4 dots take the alert cap in red phases and the
// normal cap otherwise, which keeps every frame's draw under the boot spin's
// (no current limit on rev A).

lg_fam_t lg_family(uint8_t act)
{
    if (act >= LG_ACT_PIC_NONE) return LG_FAM_PIC;
    if (act >= LG_ACT_FW_NONE)  return LG_FAM_FW;
    return LG_FAM_RESET;
}

int lg_act_over(uint8_t act, uint32_t act_ms)
{
    switch (act) {
    case LG_ACT_RESET_CANCEL:  return act_ms >= LG_RESET_CANCEL_MS;
    case LG_ACT_RESET_CONFIRM: return act_ms >= LG_RESET_DONE_MS;
    case LG_ACT_RESET_FAILED:  return act_ms >= LG_RESET_FAILED_MS;
    case LG_ACT_PIC_OK:        return act_ms >= LG_PIC_OK_MS;
    case LG_ACT_PIC_FAILED:    return act_ms >= LG_PIC_FAILED_MS;
    case LG_ACT_RESET_HOLD:
    case LG_ACT_RESET_DONE:
    case LG_ACT_FW_DOWNLOADING:
    case LG_ACT_FW_INSTALLING:
    case LG_ACT_FW_VERIFYING:
    case LG_ACT_FW_VERIFIED:
    case LG_ACT_PIC_FLASHING:  return 0;    // until replaced
    default:                   return 1;    // the NONEs, and REFUSED / ROLLED_BACK (held back)
    }
}

// The countdown at fill p (0..1): a hand sweeps clockwise from 12 o'clock
// round the test button, lighting each dot it passes, so the panel reads as a
// clock running out. The newest dots glow; the rest beat faster as it fills.
static lg_c3 reset_fill(int r, int c, float p, uint32_t s, uint32_t t)
{
    float dr = (float)r - 3, dc = (float)c - 3;
    float turn = atan2f(dc, -dr) / TAU;                 // 0 at 12 o'clock, clockwise
    if (turn < 0) turn += 1;
    // Orange to red, late: through gamma 2.2 anything under ~30 degrees
    // already reads red on the FR4, so the hue holds near orange for the first
    // half and runs to red in the second.
    float hue = jitter(r, c, 38 - 38 * p * sqrtf(p), 3, t, 0);
    float ahead = p - turn;                            // > 0: the hand has passed it
    float lit = clampf(ahead / 0.03f + 0.5f);
    float glow = ahead > 0 ? expf(-ahead / 0.06f) : 1;
    // 1 Hz rising to 3 Hz across the fill, as a phase so the rate can climb
    // without a jump.
    float sec = (float)s / 1000.0f, fill = LG_RESET_FILL_MS / 1000.0f;
    float beat = 0.88f + 0.12f * cosf(TAU * (sec + sec * sec / fill));
    float lvl = lit * (0.55f + 0.45f * glow) * beat;
    return hsv(hue, 1, lvl > 0.05f ? lvl : 0.05f);
}

// The firmware fill at p (0..1): purple rising from the bottom like a level,
// soft over a row, with a quick shimmer running up through it every 0.8 s
// (FeatherFrame's "working" pace) that peaks at 100%.
static float fw_fill(int r, float p, uint32_t t)
{
    float line = 8.0f - p * 8.6f;
    float in = ss(-0.5f, 0.5f, (float)r - line);
    float span = 8.0f - line;
    float band = 8.0f - (float)(t % 800) / 800.0f * (span + 1.0f);
    float shimmer = span > 0.3f ? 0.37f * gauss((float)r - band, 0.6f) : 0;
    return 0.07f + in * (0.56f + shimmer);
}

static void task_frame(const lg_slot_t *k, uint32_t t, uint16_t out[LG_NUM_LEDS][3])
{
    uint32_t s = k->ms;
    for (int i = 0; i < LG_NUM_LEDS; i++) {
        int r = LG_MAP[i].r, c = LG_MAP[i].c, fr4 = LG_MAP[i].mat == 'P';
        float dist = hypotf((float)r - 3, (float)c - 3);
        lg_c3 col = DARK;
        int red = 0, white = 0;
        switch (k->act) {
        case LG_ACT_RESET_HOLD:
            col = reset_fill(r, c, clampf((float)s / LG_RESET_FILL_MS), s, t);
            red = 1;
            break;
        case LG_ACT_RESET_CANCEL: {
            float back = 1 - ss(0, LG_RESET_CANCEL_MS, (float)s);
            col = sc(reset_fill(r, c, (float)k->arg / 1000.0f * back, 0, t), back);
            red = 1;
            break;
        }
        case LG_ACT_RESET_DONE:
        case LG_ACT_RESET_CONFIRM: {
            lg_c3 rd = hsv(jitter(r, c, 2, 4, t, 0), 1, 1);
            red = 1;
            if (s < 180) {                              // the point of no return
                col = fr4 ? white_of(r, c) : rd;
                white = fr4;
            } else if (s < 1180) {                      // drains into the test button
                float radius = 5.3f * (1 - (float)(s - 180) / 1000.0f);
                col = sc(rd, clampf((radius - dist) / 0.8f + 0.5f));
            } else if (fr4 && ((s >= 1300 && s < 1480) || (s >= 1660 && s < 1840))) {
                col = white_of(r, c);                   // done: two beats of white
                white = 1;
            }
            break;
        }
        case LG_ACT_RESET_FAILED:
        case LG_ACT_PIC_FAILED: {
            ps_view_t f = { .state = PS_FAILED, .since_ms = s };
            col = state_dot(&f, NULL, i, t);
            red = 1;
            break;
        }
        case LG_ACT_FW_DOWNLOADING:
        case LG_ACT_FW_INSTALLING: {
            if (!fr4) break;
            float lvl;
            if (k->arg == LG_PROGRESS_NONE) {           // length unknown: a band climbs
                float band = 9.0f - (float)(t % 1200) / 1200.0f * 11.0f;
                lvl = 0.12f + 0.88f * gauss((float)r - band, 1.0f);
            } else {
                lvl = fw_fill(r, (float)(k->arg > 1000 ? 1000 : k->arg) / 1000.0f, t);
            }
            col = sc(purple_of(r, c, t), lvl);
            break;
        }
        case LG_ACT_FW_VERIFYING:
            if (fr4) col = sc(purple_of(r, c, t),
                              pulse(t + (uint32_t)(dot_hash(r, c, 1) * 250), 800, 0.35f, 1));
            break;
        case LG_ACT_FW_VERIFIED: {                      // green rises through the purple
            if (!fr4) break;
            float edge = 8.5f - (float)s / 500.0f * 10.0f;
            col = mix(sc(purple_of(r, c, t), 0.71f), green_of(r, c, t),
                      ss(-0.5f, 0.5f, (float)r - edge));
            col = sc(col, 1 - ss(1200, LG_FW_VERIFIED_MS, (float)s));
            break;
        }
        case LG_ACT_PIC_FLASHING: {                     // a bar sweeps across the stripes
            float x = (float)(t % 500) / 500.0f * 8.0f - 0.5f;
            col = add(sc(stripe(r, c), 0.35f), sc(FULL, 0.9f * gauss((float)c - x, 0.35f)));
            break;
        }
        case LG_ACT_PIC_OK: {                           // a green ripple from the LocalTalk socket
            float d = dist_to(r, c, PORT_ROW, LT_COL), front = (float)s / 900.0f * FAR;
            float lvl = 0.85f * gauss(d - front, 1.2f) + (front > d ? 0.35f : 0);
            col = sc(green_of(r, c, t), clampf(lvl) * (1 - ss(800, LG_PIC_OK_MS, (float)s)));
            break;
        }
        }
        float cap = !fr4 ? LG_MASK_CAP : red && !white ? LG_ALERT_CAP : LG_BRIGHTNESS_CAP;
        put_dot(out[i], level16(col.r, cap), level16(col.g, cap), level16(col.b, cap));
    }
}

// ---------------------------------------------------------------- acknowledgements

uint32_t lg_elapsed(uint64_t now_ms, uint64_t t0_ms)
{
    if (t0_ms > now_ms) return 0;
    uint64_t d = now_ms - t0_ms;
    return d > UINT32_MAX ? UINT32_MAX : (uint32_t)d;
}

int lg_blip_over(uint8_t blip, uint32_t ms)
{
    switch (blip) {
    case LG_BLIP_MAC:   return ms >= LG_BLIP_MAC_MS;
    case LG_BLIP_LT_ON: return ms >= LG_BLIP_LT_ON_MS;
    case LG_BLIP_BUSY:  return ms >= LG_BLIP_BUSY_MS;
    case LG_BLIP_SAVED: return ms >= LG_BLIP_SAVED_MS;
    default:            return 1;
    }
}

static lg_c3 blip_dot(uint8_t blip, uint32_t ms, int i, uint32_t t)
{
    int r = LG_MAP[i].r, c = LG_MAP[i].c;
    float s = (float)ms, dist = hypotf((float)r - 3, (float)c - 3);
    switch (blip) {
    case LG_BLIP_MAC: {             // an arc of stripes out of the LocalTalk socket
        float front = s / 900.0f * FAR;
        float lvl = gauss(dist_to(r, c, PORT_ROW, LT_COL) - front, 0.7f);
        return sc(stripe(r, c), lvl * (1 - ss(800, LG_BLIP_MAC_MS, s)));
    }
    case LG_BLIP_LT_ON: {           // the stripes rise out of it and fade
        float edge = 8.5f - s / 700.0f * 10.0f;
        float lit = ss(-0.5f, 0.5f, (float)r - edge);
        float lvl = 0.715f * lit + 0.415f * gauss((float)r - edge, 0.5f);   // peaks at 100%
        return sc(stripe(r, c), lvl * (1 - ss(1000, LG_BLIP_LT_ON_MS, s)));
    }
    case LG_BLIP_BUSY: {            // two orange pulses round the test button: not now
        if (dist < 0.5f || dist > 1.6f) return DARK;
        float x = sinf(3.1415927f * fmodf(s, 300) / 300.0f);
        return hsv(jitter(r, c, 30, 4, t, 0), 1, x * x);
    }
    case LG_BLIP_SAVED: {           // a white ring closes into the test button
        if (s < 600)
            return sc(white_of(r, c), gauss(dist - 4.5f * (1 - s / 600.0f), 0.5f));
        return sc(white_of(r, c), dist < 1.6f ? 0.75f * (1 - ss(600, LG_BLIP_SAVED_MS, s)) : 0);
    }
    }
    return DARK;
}

// The asleep pilot (and the rare twinkle): neutral white, or a slowly cycling
// hue in gamer mode. Linear drive, as a fraction of LG_BRIGHTNESS_CAP. A
// channel under one count snaps to 0 or 1 rather than being dithered, which
// at these levels would read as a flicker.
static void put_linear(uint16_t px[3], lg_c3 col, float frac)
{
    float lvl = frac * LG_BRIGHTNESS_CAP * 256.0f;
    float ch3[3] = { col.r * lvl, col.g * lvl, col.b * lvl };
    for (int ch = 0; ch < 3; ch++) {
        float x = ch3[ch];
        uint16_t v = x < 256 ? (x >= 128 ? 256 : 0) : (uint16_t)(x > 65280 ? 65280 : x);
        if (v > px[ch]) px[ch] = v;
    }
}

void lg_render16(const ps_view_t *v, const lg_opts_t *o, uint32_t t_ms,
                 uint16_t out[LG_NUM_LEDS][3])
{
    memset(out, 0, sizeof(uint16_t) * LG_NUM_LEDS * 3);
    // A task owns the panel: the first running one, in family order.
    if (o)
        for (int f = 0; f < LG_FAMS; f++)
            if (!lg_act_over(o->task[f].act, o->task[f].ms)) {
                task_frame(&o->task[f], t_ms, out);
                return;
            }
    int all = all_dots(v->state) || moment_all(v) || v->cable;   // the cable sweep uses every dot
    int gamer = o && o->gamer;
    int blip = o && !lg_blip_over(o->blip, o->blip_ms) && v->state != PS_BOOTING
               && v->state != PS_ATTENTION && v->state != PS_FAILED && !busy_panel(v);
    float sleep_k = v->asleep ? clampf((float)v->asleep_ms / 10000.0f) : 0;   // 10 s fade

    for (int i = 0; i < LG_NUM_LEDS; i++) {
        int fr4 = LG_MAP[i].mat == 'P';
        if (!fr4 && !all && !blip) continue;   // FR4-only state: mask dot stays dark
        lg_c3 c = fr4 || all ? sc(view_dot(v, o, i, t_ms), 1 - sleep_k) : DARK;
        if (blip) c = screen(c, blip_dot(o->blip, o->blip_ms, i, t_ms));
        float cap = !fr4 ? LG_MASK_CAP
                  : v->state == PS_ATTENTION || join_failed(v) ? LG_ALERT_CAP : LG_BRIGHTNESS_CAP;
        put_dot(out[i], level16(c.r, cap), level16(c.g, cap), level16(c.b, cap));
    }

    if (!v->asleep) return;

    int p = lg_pilot_index();
    lg_c3 pilot = gamer ? hsv((float)t_ms / 60.0f, 0.75f, 1) : white_of(LG_MAP[p].r, LG_MAP[p].c);
    put_linear(out[p], pilot, pulse(t_ms, 6000, 0.03f, 0.12f) * sleep_k);

    // Every 30 s one other FR4 dot twinkles once, so the panel reads as alive.
    uint32_t ph = t_ms % 30000;
    if (ph < 1500) {
        int fr4[LG_NUM_LEDS], n = 0;
        for (int i = 0; i < LG_NUM_LEDS; i++)
            if (LG_MAP[i].mat == 'P' && i != p) fr4[n++] = i;
        int tw = fr4[(t_ms / 30000 * 5 + 3) % (uint32_t)n];
        lg_c3 col = gamer ? hsv((float)t_ms / 60.0f + 120, 0.75f, 1)
                          : white_of(LG_MAP[tw].r, LG_MAP[tw].c);
        put_linear(out[tw], col, 0.08f * sinf(3.1415927f * (float)ph / 1500.0f) * sleep_k);
    }
}

void lg_render(const ps_view_t *v, const lg_opts_t *o, uint32_t t_ms,
               uint8_t out[LG_NUM_LEDS][3])
{
    uint16_t hi[LG_NUM_LEDS][3];
    lg_render16(v, o, t_ms, hi);
    for (int i = 0; i < LG_NUM_LEDS; i++)
        for (int ch = 0; ch < 3; ch++) {
            uint32_t x = ((uint32_t)hi[i][ch] + 128u) >> 8;
            out[i][ch] = (uint8_t)(x > 255 ? 255 : x);
        }
}

void lg_dither(lg_dither_t *d, const uint16_t in[LG_NUM_LEDS][3],
               uint8_t out[LG_NUM_LEDS][3])
{
    for (int i = 0; i < LG_NUM_LEDS; i++)
        for (int ch = 0; ch < 3; ch++) {
            uint32_t acc = (uint32_t)in[i][ch] + d->err[i][ch];
            uint32_t x = acc >> 8;
            out[i][ch] = (uint8_t)(x > 255 ? 255 : x);
            d->err[i][ch] = (uint8_t)(acc & 0xFF);
        }
}
