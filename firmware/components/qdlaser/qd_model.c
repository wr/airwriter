// SPDX-License-Identifier: GPL-3.0-or-later
#include "qd_model.h"
#include "apple_tables_lw.h"
#include <stddef.h>

// A margin of `d` dots at 300 dpi in 1/100 mm, rounded outward so a client never lays out
// into it.
#define QD_OUT(d) (((d) * 2540 + 299) / 300)

// The Personal LaserWriter 300's sheets, Letter first: the IPP name and size (1/100 mm),
// Apple's paper (PREC 3 of Apple's LaserWriter 300 driver 1.1, in 1/120 in, times 2.5 and
// rounded down), the page a client sends at 300 dpi, and the image area the driver gives the
// printer for that paper, read out of the same driver's page setup (CODE -8191, -8195).
#define PLW300_SHEETS(X) \
    X("na_letter_8.5x11in", 21590, 27940, 2550, 3300, 2550, 3300, 2400, 3175)

#define QD_MEDIA_ROW(NAME, X_, Y_, PW, PH, GW, GH, IW, IH) { NAME, PW, PH, GW, GH, IW, IH },
static const qd_media_t k_plw300_media[] = { PLW300_SHEETS(QD_MEDIA_ROW) };
#define N_PLW300 ((uint8_t)(sizeof k_plw300_media / sizeof k_plw300_media[0]))

// What AirPrint is told for each sheet: the rip keeps img_w dots from the middle of the
// page's width, and the rows from (page_h - img_h) / 2 down; the rest is the margin.
#define QD_CAPS_ROW(NAME, X_, Y_, PW, PH, GW, GH, IW, IH) { \
    NAME, X_, Y_, \
    QD_OUT(((GW) - (IW)) / 2), QD_OUT((GW) - (IW) - ((GW) - (IW)) / 2), \
    QD_OUT(((GH) - (IH)) / 2), QD_OUT((GH) - (IH) - ((GH) - (IH)) / 2), \
    GW, ((GW) - (IW)) / 2, IW, GH },
static const printer_media_t k_plw300_caps_media[] = { PLW300_SHEETS(QD_CAPS_ROW) };

static const qd_spec_t s_spec[QD_MODELS] = {
    [QD_PLW300] = {
        .name = "Personal LaserWriter 300", .engine = QD_ENGINE_PLW300,
        .ctrl = 2, .page_ram = 0,
        .src_cassette = 0x4C, .src_manual = 0x4F, .bit_cassette = 0x20, .bit_manual = 0x40,
        .density_cmd = 0xA7, .density = 32,
        .budget = 500000,
        .screen8 = apple_lw_screen8, .tone = apple_lw_tone,
        .media = k_plw300_media, .n_media = N_PLW300,
        .icon = "plw300",
    },
};

static const char *const k_sources[] = { "main", "manual" };

// The scalar media fields repeat the first sheet (Letter), for what reads them. `ppm` is a
// nominal figure: it is only advertised.
static const printer_caps_t s_caps[QD_MODELS] = {
    [QD_PLW300] = {
        .make_model = "Apple Personal LaserWriter 300",
        .device_id  = "MFG:Apple;MDL:Personal LaserWriter 300;CMD:URF;",
        .urf_rs     = "RS300",
        .res_x = 300, .res_y = 300,
        .media = "na_letter_8.5x11in", .media_x = 21590, .media_y = 27940,
        .margin_l = QD_OUT(75), .margin_r = QD_OUT(75), .margin_t = QD_OUT(62), .margin_b = QD_OUT(63),
        .ppm = 2,
        .rip_dpi_x = 300, .rip_dpi_y = 300,
        .max_dots = 2400, .crop_margin = 0,
        .media_list = k_plw300_caps_media, .n_media = N_PLW300,
        .sources = k_sources, .n_sources = 2,
    },
};

const qd_spec_t *qd_spec(qd_model_t m)
{
    return m > QD_NONE && m < QD_MODELS ? &s_spec[m] : NULL;
}

qd_model_t qd_model_from_engine(uint8_t engine)
{
    return engine == QD_ENGINE_PLW300 ? QD_PLW300 : QD_NONE;
}

const char *qd_engine_name(uint8_t engine)
{
    switch (engine) {
    case QD_ENGINE_PLW300: return s_spec[QD_PLW300].name;
    case QD_ENGINE_LS:     return "Personal LaserWriter LS";
    case QD_ENGINE_JP:     return NULL;
    default:               return "LaserWriter Select 300";
    }
}

const qd_media_t *qd_media_for(qd_model_t m, uint32_t w, uint32_t h)
{
    const qd_spec_t *s = qd_spec(m);
    if (!s || !s->n_media) return NULL;
    for (int i = 0; i < s->n_media; i++) {
        const qd_media_t *md = &s->media[i];
        uint32_t d = md->page_h > h ? md->page_h - h : h - md->page_h;
        if (md->img_w == w && d <= 2) return md;
    }
    for (int i = 0; i < s->n_media; i++)
        if (s->media[i].img_w == w) return &s->media[i];
    return &s->media[0];
}

// Apple's driver's page setup for the 300: the top is half the paper the image area leaves,
// the left margin is (paper - image + 0x1A0) / 2 in dots sent in bytes, and what it cannot
// carry past 0x1FF goes into the row width.
qd_setup_t qd_setup(const qd_media_t *md)
{
    int m = (md->paper_w - md->img_w + 0x1A0) / 2;
    qd_setup_t u;
    u.top = (int16_t)-((md->paper_h - md->img_h) / 2);
    u.left = (int16_t)-((m > 0x1FF ? 0x1FF : m) >> 3);
    u.width = (int16_t)-(md->img_w / 8 + (m > 0x1FF ? (m - 0x1FF + 7) / 8 : 0));
    return u;
}

const printer_caps_t *qd_caps(qd_model_t m)
{
    return m > QD_NONE && m < QD_MODELS ? &s_caps[m] : NULL;
}
