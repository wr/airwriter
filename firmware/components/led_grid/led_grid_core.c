// SPDX-License-Identifier: GPL-3.0-or-later
// Frame renderer for the 37-dot art panel. Pure function of (state, t) so the
// host tests can walk every phase. Material physics from the panel spec:
// P (bare FR4) is the only full-RGB pixel; B/L (teal-through-mask) are
// brightness-only -- the mask is the colour filter, not the LED. The global
// cap is the LAST multiply: rev A has no hardware current limit.
#include "led_grid_core.h"
#include <math.h>
#include <string.h>

static uint8_t lg_tri(uint32_t t, uint32_t period)
{
    uint32_t ph = (t % period) * 512 / period;      // 0..511
    return (uint8_t)(ph < 256 ? ph : 511 - ph);
}

static void set_all(uint8_t out[LG_NUM_LEDS][3], uint8_t r, uint8_t g, uint8_t b)
{
    for (int i = 0; i < LG_NUM_LEDS; i++) {
        out[i][0] = r; out[i][1] = g; out[i][2] = b;
    }
}

void lg_render(ps_state_t st, uint32_t t_ms, uint8_t out[LG_NUM_LEDS][3])
{
    memset(out, 0, LG_NUM_LEDS * 3);

    switch (st) {
    case PS_BOOT:
        break;
    case PS_SETUP:
        set_all(out, lg_tri(t_ms, 1200), lg_tri(t_ms, 1200), lg_tri(t_ms, 1200));
        break;
    case PS_READY: {
        uint8_t v = (uint8_t)(8 + (uint32_t)lg_tri(t_ms, 4000) * 247 / 255);
        set_all(out, v, v, v);
        break;
    }
    case PS_WIFI_CONNECTING:
        for (int i = 0; i < LG_NUM_LEDS; i++) {
            float d = hypotf((float)LG_MAP[i].c - 3.0f, (float)LG_MAP[i].r - 3.5f);
            uint32_t ph = (uint32_t)(t_ms / 6 + 4096) - (uint32_t)(d * 32.0f);
            uint8_t v = (uint8_t)(ph % 256);
            out[i][0] = (uint8_t)(v / 2); out[i][1] = (uint8_t)(v / 2);
            out[i][2] = v;               // cool white-blue on the RGB dots
        }
        break;
    case PS_PRINTING: {
        int row = (int)((t_ms % 1500) * LG_ROWS / 1500);
        for (int i = 0; i < LG_NUM_LEDS; i++) {
            int dr = LG_MAP[i].r - row;
            if (dr < 0) dr = -dr;
            uint8_t v = dr == 0 ? 255 : dr == 1 ? 64 : 16;
            out[i][0] = v; out[i][1] = v; out[i][2] = v;
        }
        break;
    }
    case PS_BLOCKED: {
        uint32_t ph = t_ms % 2000;
        int on = ph < 150 || (ph >= 300 && ph < 450);
        if (on) for (int i = 0; i < LG_NUM_LEDS; i++) out[i][0] = 255;
        break;
    }
    case PS_ERROR: {
        int on = (t_ms % 1000) < 500;
        if (on)
            for (int i = 0; i < LG_NUM_LEDS; i++)
                if (LG_MAP[i].mat == 'P') out[i][0] = 255;
        break;
    }
    }

    for (int i = 0; i < LG_NUM_LEDS; i++) {
        if (LG_MAP[i].mat != 'P') {
            uint8_t v = out[i][0];
            if (out[i][1] > v) v = out[i][1];
            if (out[i][2] > v) v = out[i][2];
            // ERROR wants mask dots dark, and its red never reaches them:
            // monochrome conversion happens only on what the state painted.
            if (st == PS_ERROR) v = 0;
            out[i][0] = out[i][1] = out[i][2] = v;
        }
        for (int ch = 0; ch < 3; ch++)
            out[i][ch] = (uint8_t)((uint32_t)out[i][ch] * LG_BRIGHTNESS_CAP / 255);
    }
}
