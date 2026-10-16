// SPDX-License-Identifier: GPL-3.0-or-later
// Frame renderer for the 37-dot art panel, panel language v4: a pure function
// of (view, options, t), so the host tests can walk every phase. The spec is
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

// Each dot breathes on its own 5-9 s period, 30% to 70%. (Was 16-38%: on
// board 1 that peaked under 6 counts and vanished in daylight; this is the
// same breath at 4x the drive.)
static float ready_level(int r, int c, uint32_t t)
{
    uint32_t period = 5000 + (uint32_t)(dot_hash(r, c, 1) * 4000);
    return 0.50f + 0.20f * sinf(TAU * (float)(t % period) / (float)period
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
// in by distance, spins, then collapses inward and is dark by LG_BOOT_MS.
static lg_c3 spin(float dist, float ang, float t)
{
    float v = ss(0, 0.6f, t - dist * 0.08f) * (1 - ss(2.3f, 3.0f, t + dist * 0.12f));
    return hsv(ang + t * 300 - t * t * 40, 0.9f, v);
}

static lg_c3 state_dot(const ps_view_t *v, const lg_opts_t *o, int i, uint32_t t)
{
    int r = LG_MAP[i].r, c = LG_MAP[i].c;
    float dr = (float)r - 3, dc = (float)c - 3;
    float dist = hypotf(dr, dc), ang = atan2f(dr, dc) * 57.29578f;
    float h1 = dot_hash(r, c, 1), h2 = dot_hash(r, c, 2);
    uint32_t s = v->since_ms;

    switch (v->state) {
    case PS_BOOTING:
        return spin(dist, ang, (float)s / 1000.0f);

    case PS_SETUP: {
        float hue = jitter(r, c, 220, 16, t, 6);
        if (v->phone_joined) {
            uint32_t period = 3000 + (uint32_t)(h1 * 2500);
            float lvl = 0.58f + 0.08f * sinf(TAU * (float)(t % period) / (float)period + h2 * TAU);
            return hsv(hue, 0.85f, lvl);
        }
        // Radar sweep: the arm turns once per 2.6 s; a dot flares as it passes
        // and decays behind it.
        float arm = (float)(t % 2600) / 2600.0f * 360.0f;
        float behind = fmodf(fmodf(arm - ang, 360.0f) + 360.0f, 360.0f);
        return hsv(hue, 0.85f, 0.08f + 0.80f * expf(-behind / 55.0f));
    }

    case PS_JOINING: {
        float ring = (float)(t % 900) / 900.0f * 4.8f;          // outward from the test button
        return hsv(jitter(r, c, 215, 18, t, 0), 0.85f, 0.08f + 0.75f * gauss(dist - ring, 1.1f));
    }

    case PS_READY: {
        lg_c3 base = o && o->gamer
            ? hsv((float)c * 28 + (float)r * 14 + (float)t / 40.0f, 0.75f, ready_level(r, c, t) + 0.05f)
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
        // 9% swelling to 60%: 4x the drive of the first tune (5-32%), which
        // was lost in daylight like Ready's.
        return hsv(jitter(r, c, 34, 9, t, 0), 0.5f + 0.12f * h1, 0.09f + 0.51f * ember);
    }

    case PS_PRINTING: {
        // A print head crosses the current row in 1.5 s, inking it behind;
        // rows advance top to bottom and alternate direction, like the
        // ImageWriter's bidirectional head.
        uint32_t line = t / 1500;
        int row = (int)(line % LG_ROWS), ltr = (line % 2) == 0;
        float f = (float)(t % 1500) / 1500.0f;
        float x = ltr ? f * 7.6f - 0.3f : 6.3f - f * 7.6f;
        lg_c3 ink = hsv(jitter(r, c, 38, 10, t, 0), 0.4f, 1);
        if (r == row) {
            int inked = ltr ? ((float)c < x) : ((float)c > x);
            return add(sc(FULL, gauss((float)c - x, 0.6f)), sc(ink, inked ? 0.35f : 0));
        }
        int back = (row - r + LG_ROWS) % LG_ROWS;
        if (back == 1) return sc(ink, 0.25f);
        if (back == 2) return sc(ink, 0.12f);
        return sc(ink, 0.03f);
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
        return sc(red, 0.2f + 0.4f * (0.5f - 0.5f * cosf(TAU * ph + h2 * 1.5f)));
    }
    }
    return DARK;
}

// ---------------------------------------------------------------- actions
// The factory reset (W-1045), drawn over every state. Orange turning red
// because it is a warning a person is choosing to walk into; mask dots join
// in, since red and amber survive the mask. FR4 dots take the alert cap in the
// red phases; the white beats stay at the normal cap, which keeps the frame's
// draw under the boot spin's (no current limit on rev A).

int lg_act_over(uint8_t act, uint32_t act_ms)
{
    switch (act) {
    case LG_ACT_RESET_CANCEL:  return act_ms >= LG_RESET_CANCEL_MS;
    case LG_ACT_RESET_CONFIRM: return act_ms >= LG_RESET_DONE_MS;
    case LG_ACT_RESET_FAILED:  return act_ms >= LG_RESET_FAILED_MS;
    case LG_ACT_NONE:          return 1;
    default:                   return 0;    // HOLD and DONE last until replaced
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

static void reset_frame(const lg_opts_t *o, uint32_t t, uint16_t out[LG_NUM_LEDS][3])
{
    uint32_t s = o->act_ms;
    for (int i = 0; i < LG_NUM_LEDS; i++) {
        int r = LG_MAP[i].r, c = LG_MAP[i].c, fr4 = LG_MAP[i].mat == 'P';
        float dist = hypotf((float)r - 3, (float)c - 3);
        lg_c3 col = DARK;
        int white = 0;
        switch (o->act) {
        case LG_ACT_RESET_HOLD: {
            float p = clampf((float)s / LG_RESET_FILL_MS);
            col = reset_fill(r, c, p, s, t);
            break;
        }
        case LG_ACT_RESET_CANCEL: {
            float p = (float)o->act_from / 1000.0f * (1 - ss(0, LG_RESET_CANCEL_MS, (float)s));
            col = sc(reset_fill(r, c, p, 0, t), 1 - ss(0, LG_RESET_CANCEL_MS, (float)s));
            break;
        }
        case LG_ACT_RESET_DONE:
        case LG_ACT_RESET_CONFIRM: {
            lg_c3 red = hsv(jitter(r, c, 2, 4, t, 0), 1, 1);
            if (s < 180) {                              // the point of no return
                col = fr4 ? white_of(r, c) : red;
                white = fr4;
            } else if (s < 1180) {                      // drains into the test button
                float radius = 5.3f * (1 - (float)(s - 180) / 1000.0f);
                col = sc(red, clampf((radius - dist) / 0.8f + 0.5f));
            } else if (fr4 && ((s >= 1300 && s < 1480) || (s >= 1660 && s < 1840))) {
                col = sc(white_of(r, c), 0.7f);         // done: two beats of white
                white = 1;
            }
            break;
        }
        case LG_ACT_RESET_FAILED: {
            ps_view_t f = { .state = PS_FAILED, .since_ms = s };
            col = state_dot(&f, NULL, i, t);
            break;
        }
        }
        float cap = !fr4 ? LG_MASK_CAP : white ? LG_BRIGHTNESS_CAP : LG_ALERT_CAP;
        put_dot(out[i], level16(col.r, cap), level16(col.g, cap), level16(col.b, cap));
    }
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
    if (o && !lg_act_over(o->act, o->act_ms)) {
        reset_frame(o, t_ms, out);
        return;
    }
    int all = all_dots(v->state);
    int gamer = o && o->gamer;
    float sleep_k = v->asleep ? clampf((float)v->asleep_ms / 10000.0f) : 0;   // 10 s fade

    for (int i = 0; i < LG_NUM_LEDS; i++) {
        int fr4 = LG_MAP[i].mat == 'P';
        if (!fr4 && !all) continue;            // FR4-only state: mask dot stays dark
        lg_c3 c = sc(state_dot(v, o, i, t_ms), 1 - sleep_k);
        float cap = !fr4 ? LG_MASK_CAP
                  : v->state == PS_ATTENTION ? LG_ALERT_CAP : LG_BRIGHTNESS_CAP;
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
