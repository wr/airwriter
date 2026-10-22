// SPDX-License-Identifier: GPL-3.0-or-later
#include "lw_caps.h"
#include <stdio.h>
#include <string.h>

static const struct {
    const char *media;
    int32_t     x, y;           // 1/100 mm
    int32_t     w_pt, h_pt;     // 1/100 pt
    uint32_t    w_px;           // at 300 dpi
} k_paper[] = {
    [LW_LETTER] = { "na_letter_8.5x11in", 21590, 27940, 61200,  79200, 2550 },
    [LW_A4]     = { "iso_a4_210x297mm",   21000, 29700, 59528,  84189, 2480 },
    [LW_LEGAL]  = { "na_legal_8.5x14in",  21590, 35560, 61200, 100800, 2550 },
};

uint32_t lw_paper_h_pt(lw_page_t p)
{
    return (uint32_t)((k_paper[p].h_pt + 99) / 100);
}

// 1/100 pt to 1/100 mm, rounded up: a margin is never claimed smaller than
// the printer's.
static int32_t margin(int32_t pt100)
{
    if (pt100 <= 0) return 0;
    return (int32_t)(((int64_t)pt100 * 2540 + 7199) / 7200);
}

static int is_apple(const char *product)
{
    return strstr(product, "LaserWriter") != NULL && strncmp(product, "Apple", 5) != 0;
}

void lw_caps_fill(lw_caps_t *c, const lw_info_t *i)
{
    lw_page_t pg = i->page <= LW_LEGAL ? i->page : LW_LETTER;
    int apple = is_apple(i->product);
    snprintf(c->make_model, sizeof c->make_model, "%s%s", apple ? "Apple " : "", i->product);
    char mfg[16];
    if (apple) {
        strcpy(mfg, "Apple");
    } else {
        size_t n = strcspn(i->product, " -");
        if (n >= sizeof mfg) n = sizeof mfg - 1;
        memcpy(mfg, i->product, n);
        mfg[n] = '\0';
    }
    snprintf(c->device_id, sizeof c->device_id, "MFG:%s;MDL:%s;CMD:URF;", mfg, i->product);

    printer_caps_t *p = &c->caps;
    memset(p, 0, sizeof *p);
    p->make_model  = c->make_model;
    p->device_id   = c->device_id;
    p->urf_rs      = "RS300";
    p->res_x = p->res_y = 300;
    p->media       = k_paper[pg].media;
    p->media_x     = k_paper[pg].x;
    p->media_y     = k_paper[pg].y;
    p->margin_l    = margin(i->area[0]);
    p->margin_b    = margin(i->area[1]);
    p->margin_r    = margin(k_paper[pg].w_pt - i->area[2]);
    p->margin_t    = margin(k_paper[pg].h_pt - i->area[3]);
    p->ppm         = 8;
    p->rip_dpi_x = p->rip_dpi_y = 300;
    p->max_dots    = k_paper[pg].w_px;
    p->crop_margin = 0;
}

void lw_uuid(char out[46], const uint8_t mac[6], uint16_t epoch, const uint8_t *pascal_name)
{
    uint32_t h = 2166136261u;                   // FNV-1a 32
    for (unsigned k = 0; k <= pascal_name[0]; k++) {
        h ^= pascal_name[k];
        h *= 16777619u;
    }
    uint16_t folded = (uint16_t)((h >> 16) ^ (h & 0xFFFF));
    snprintf(out, 46, "urn:uuid:41575254-4c57-%04x-%04x-%02x%02x%02x%02x%02x%02x",
             epoch, folded, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}
