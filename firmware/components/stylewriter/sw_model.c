// SPDX-License-Identifier: GPL-3.0-or-later
#include "sw_model.h"
#include "apple_tables.h"
#include <string.h>

// Per model, from the design's table. The original takes the 1200 family's
// geometry with no rect offset, no row delta and no cleaning. Indexed by
// sw_model_t; SW_NONE's slot is empty.
static const sw_spec_t s_spec[SW_MODELS] = {
    [SW_ORIGINAL] = { "StyleWriter", "Apple StyleWriter",
                      2880, 72, 72, 72, 114, 800,  0, 0, 0, 0 },
    [SW_II]       = { "StyleWriter II", "Apple StyleWriter II",
                      2880, 72, 72, 72, 114, 800, 16, 0, 1, 1 },
    [SW_1200]     = { "StyleWriter 1200", "Apple StyleWriter 1200",
                      2880, 72, 72, 72, 114, 800, 16, 0, 1, 1 },
    [SW_C1500]    = { "Color StyleWriter 1500", "Apple Color StyleWriter 1500",
                      2880, 72, 72, 72, 114, 800, 16, 1, 1, 1 },
    [SW_C2200]    = { "Color StyleWriter 2200", "Apple Color StyleWriter 2200",
                      2919, 72, 72, 45,  99, 400,  0, 1, 1, 1 },
    [SW_C2400]    = { "Color StyleWriter 2400", "Apple Color StyleWriter 2400",
                      2919, 72, 72, 45,  99, 400,  0, 1, 1, 1 },
    [SW_C2500]    = { "Color StyleWriter 2500", "Apple Color StyleWriter 2500",
                      2919, 72, 72, 45,  99, 400,  0, 1, 1, 1 },
};

#define BLOCK_FRAME 4            // G, the u16 length, the 00 that ends it

const sw_spec_t *sw_spec(sw_model_t m)
{
    if ((int)m <= SW_NONE || (int)m >= SW_MODELS) return NULL;
    return &s_spec[m];
}

sw_model_t sw_model_from_id(const uint8_t *r, size_t n, int *is_cs)
{
    if (is_cs) *is_cs = 0;
    size_t len = 0;
    while (len < n && r[len] != '\r') len++;
    if (len == 2 && memcmp(r, "SW", 2) == 0) return SW_II;
    if (len == 3 && memcmp(r, "SW3", 3) == 0) return SW_1200;
    if (len == 4 && memcmp(r, "IJ10", 4) == 0) return SW_ORIGINAL;
    if (len == 2 && memcmp(r, "CS", 2) == 0 && is_cs) *is_cs = 1;
    return SW_NONE;
}

sw_model_t sw_model_from_p(uint8_t p)
{
    switch (p) {
    case 0x01: return SW_C2400;
    case 0x04: return SW_C1500;
    case 0x05: return SW_C2500;
    default:   return SW_C2200;          // 02, and what the drivers do with the rest
    }
}

int sw_cart_colour(sw_model_t m, uint8_t h)
{
    switch (m) {
    case SW_C2200: case SW_C2400: case SW_C2500:
        return h == 0x81 ? 1 : h == 0x01 ? 0 : -1;
    case SW_C1500:
        return (h & 0x80) ? 1 : 0;
    case SW_ORIGINAL: case SW_II: case SW_1200:
        return 0;
    default:
        return -1;
    }
}

int32_t sw_free_bytes(sw_model_t m, uint8_t b)
{
    if (!sw_spec(m)) return 0;
    int32_t free_b = (int32_t)b * (m == SW_ORIGINAL ? 64 : 256) - 2048;
    return free_b > 0 ? free_b : 0;
}

int sw_b_empty(sw_model_t m, uint8_t b, int colour_cart)
{
    switch (m) {
    case SW_ORIGINAL: return b == 0xA0;
    case SW_II: case SW_1200: case SW_C2400: return b == 0xF8;
    case SW_C1500:    return b == (colour_cart ? 0x87 : 0x9F);
    case SW_C2200:    return (b & 0xA0) == 0xA0;
    case SW_C2500:    return b == 0x80;
    default:          return 0;
    }
}

// What the model's `B` reads for the emptiest buffer it can read as empty (the
// 1500's black cartridge reads emptier than its colour one), in free bytes.
static int32_t empty_free_min(sw_model_t m)
{
    for (int b = 0; b < 256; b++)
        if (sw_b_empty(m, (uint8_t)b, 0) || sw_b_empty(m, (uint8_t)b, 1))
            return sw_free_bytes(m, (uint8_t)b);
    return 0;
}

uint32_t sw_block_cap(sw_model_t m, int planes)
{
    const sw_spec_t *s = sw_spec(m);
    if (!s) return 0;
    if (planes < 1) planes = 1;
    uint32_t cap;
    switch (m) {
    case SW_ORIGINAL:
        cap = 0x2000;
        break;
    case SW_C2200: case SW_C2400: case SW_C2500:
        cap = planes > 1 ? 0x79FA : 0x7E77;
        break;
    default:                                 // II, 1200, 1500
        cap = 0x76ED - (uint32_t)planes * (s->dots_across / 8);
        break;
    }
    // An empty buffer, read one step of `B` short (the reading is quantised, and the 2200's
    // already varies at empty), still takes the block with its framing.
    int32_t room = empty_free_min(m) - (m == SW_ORIGINAL ? 64 : 256) - BLOCK_FRAME;
    uint32_t fits = room > 0 ? (uint32_t)room : 0;
    return cap < fits ? cap : fits;
}

uint32_t sw_printable_w(sw_model_t m, uint32_t paper_w)
{
    const sw_spec_t *s = sw_spec(m);
    if (!s) return 0;
    uint32_t sides = (uint32_t)s->margin_l + s->margin_r;
    uint32_t room = paper_w > sides ? paper_w - sides : 0;
    return room < s->dots_across ? room : s->dots_across;
}

// The sheets, at 360 dpi: the page's width the phone sends (Letter and Legal are the
// same), where the engine's printable area begins and how wide it is on that paper
// (sw_printable_w), and the margins in 1/100 mm rounded outward. The right margin is
// whatever the paper leaves past the printable width. Per family: the original, II,
// 1200 and 1500 print 2880 dots across with 72/108, 72, 114 dots of margin on Letter;
// the 2200, 2400 and 2500 print 2916 on Letter (their 2919 is wider than the paper
// less both side margins) with 72, 45 and 99. test_sw_model.c holds these to sw_spec.
#define LETTER "na_letter_8.5x11in", 21590, 27940
#define A4     "iso_a4_210x297mm",   21000, 29700
#define LEGAL  "na_legal_8.5x14in",  21590, 35560
static const printer_media_t k_media_1200[] = {
    { LETTER, 508, 762, 508, 805, 3060, 72, 2880 },
    { A4,     508, 508, 508, 805, 2976, 72, 2832 },
    { LEGAL,  508, 762, 508, 805, 3060, 72, 2880 },
};
static const printer_media_t k_media_cs[] = {
    { LETTER, 508, 508, 318, 699, 3060, 72, 2916 },
    { A4,     508, 508, 318, 699, 2976, 72, 2832 },
    { LEGAL,  508, 508, 318, 699, 3060, 72, 2916 },
};
#undef LETTER
#undef A4
#undef LEGAL

// The scalar media fields repeat the first sheet (Letter), for what reads them.
// `ppm` is a nominal figure: it is only advertised.
#define SW_CAPS(MODEL, DOTS, MEDIA, LM, LR, LT, LB) { \
    .make_model  = "Apple " MODEL, \
    .device_id   = "MFG:Apple;MDL:" MODEL ";CMD:URF;", \
    .urf_rs      = "RS360", \
    .res_x = 360, .res_y = 360, \
    .media       = "na_letter_8.5x11in", .media_x = 21590, .media_y = 27940, \
    .margin_l = LM, .margin_r = LR, .margin_t = LT, .margin_b = LB, \
    .ppm         = 2, \
    .rip_dpi_x = 360, .rip_dpi_y = 360, \
    .max_dots    = DOTS, .crop_margin = 0, \
    .media_list  = MEDIA, .n_media = 3, }

static const printer_caps_t s_caps[SW_MODELS] = {
    [SW_ORIGINAL] = SW_CAPS("StyleWriter",            2880, k_media_1200, 508, 762, 508, 805),
    [SW_II]       = SW_CAPS("StyleWriter II",         2880, k_media_1200, 508, 762, 508, 805),
    [SW_1200]     = SW_CAPS("StyleWriter 1200",       2880, k_media_1200, 508, 762, 508, 805),
    [SW_C1500]    = SW_CAPS("Color StyleWriter 1500", 2880, k_media_1200, 508, 762, 508, 805),
    [SW_C2200]    = SW_CAPS("Color StyleWriter 2200", 2919, k_media_cs,   508, 508, 318, 699),
    [SW_C2400]    = SW_CAPS("Color StyleWriter 2400", 2919, k_media_cs,   508, 508, 318, 699),
    [SW_C2500]    = SW_CAPS("Color StyleWriter 2500", 2919, k_media_cs,   508, 508, 318, 699),
};

const printer_caps_t *sw_caps(sw_model_t m)
{
    if ((int)m <= SW_NONE || (int)m >= SW_MODELS) return NULL;
    return &s_caps[m];
}

const uint8_t *sw_tone(sw_model_t m, ht_algo_t algo)
{
    if ((int)m <= SW_NONE || (int)m >= SW_MODELS) return NULL;
    int cs = m == SW_C2200 || m == SW_C2400 || m == SW_C2500;
    int diffusing = algo == HT_ATKINSON || algo == HT_FLOYD;
    return cs ? apple_tone_cs[diffusing] : apple_tone_sw[diffusing];
}
