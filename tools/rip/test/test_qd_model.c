// SPDX-License-Identifier: GPL-3.0-or-later
// W-1102: the QuickDraw LaserWriters' model table: the Personal LaserWriter 300's sheets, the
// setup values its page is sent with, and what AirPrint is told about it. W-1145: the LS's.
#include "test_util.h"
#include "qd_model.h"
#include "apple_tables_lw.h"
#include "qd_wire.h"
#include "sw_code.h"

static void test_letter(void)
{
    const qd_spec_t *s = qd_spec(QD_PLW300);
    CHECK(s && s->engine == 0x29 && s->budget == 500000 && s->density == 32);
    if (!s) return;
    CHECK_EQ_INT(s->driver, QD_DRV_LW300);
    CHECK_EQ_INT(s->row_bits, 12);
    CHECK_EQ_INT(s->src_main, 0x4C);
    CHECK_EQ_INT(s->src_manual, 0x4F);
    CHECK_EQ_INT(s->bit_cassette, 0x20);
    CHECK_EQ_INT(s->bit_manual, 0x40);
    CHECK_EQ_INT(s->density_cmd, 0xA7);
    CHECK(s->screen8 == apple_lw_screen8 && s->tone == apple_lw_tone);
    CHECK(!strcmp(s->name, "Personal LaserWriter 300"));
    CHECK(!strcmp(s->icon, "plw300"));
    const qd_media_t *l = &s->media[0];
    CHECK(!strcmp(l->media, "na_letter_8.5x11in"));
    CHECK_EQ_INT(l->paper_w, 2550); CHECK_EQ_INT(l->paper_h, 3300);
    CHECK_EQ_INT(l->page_w, 2550);  CHECK_EQ_INT(l->page_h, 3300);
    CHECK_EQ_INT(l->img_w, 2400);   CHECK_EQ_INT(l->img_h, 3175);
    qd_setup_t u = qd_setup(l);
    CHECK_EQ_INT(u.top, -62); CHECK_EQ_INT(u.left, -35); CHECK_EQ_INT(u.width, -300);
    CHECK_EQ_INT(qd_row_bytes(l), 300);
    for (int i = 0; i < s->n_media; i++) CHECK_EQ_INT(s->media[i].size, 0x21);
    CHECK(qd_spec(QD_NONE) == NULL);
    CHECK(qd_spec(QD_MODELS) == NULL);
    CHECK(qd_spec((qd_model_t)-1) == NULL);
}

static void test_engine_names(void)
{
    CHECK_EQ_INT(qd_model_from_engine(0x29), QD_PLW300);
    CHECK_EQ_INT(qd_model_from_engine(0x0D), QD_PLWLS);
    CHECK_EQ_INT(qd_model_from_engine(0x23), QD_NONE);
    CHECK_EQ_INT(qd_model_from_engine(0x55), QD_SEL300);     // W-1146: any other is a Select 300
    CHECK_EQ_INT(qd_model_from_engine(0x00), QD_SEL300);
    CHECK(!strcmp(qd_engine_name(0x29), "Personal LaserWriter 300"));
    CHECK(!strcmp(qd_engine_name(0x0D), "Personal LaserWriter LS"));
    CHECK(qd_engine_name(0x23) == NULL);
    CHECK(!strcmp(qd_engine_name(0x55), "LaserWriter Select 300"));
}

// W-1145: the LS, from its own driver (Personal LW LS 7.1): its sources, no density, paper from
// 83 02, 9-bit rows, and each sheet's setup by the driver's rule, from its paper and Apple's
// image area: top (paper h - image h) / 2, left (paper w - image w + 0xB7) / 16, the row
// ((image w + 24) / 8) & ~3 bytes. #10's area is 1,133 dots, kept as 1,136.
static void test_ls(void)
{
    const qd_spec_t *s = qd_spec(QD_PLWLS);
    CHECK(s != NULL);
    if (!s) return;
    CHECK(!strcmp(s->name, "Personal LaserWriter LS"));
    CHECK(!strcmp(s->icon, "plwls"));
    CHECK_EQ_INT(s->engine, 0x0D);
    CHECK_EQ_INT(s->driver, QD_DRV_LS);
    CHECK_EQ_INT(s->src_main, 0x52);       // the multipurpose tray, never the optional cassette
    CHECK_EQ_INT(s->src_manual, 0);
    CHECK_EQ_INT(s->density_cmd, 0);
    CHECK_EQ_INT(s->row_bits, 9);
    CHECK(s->budget > 0 && s->budget <= 500000);
    CHECK(s->screen8 == apple_lw_screen8 && s->tone == apple_lw_tone);
    static const struct { const char *media; int area_w, size, top, left, width; } want[] = {
        { "na_letter_8.5x11in",       2400, 0x04,  63, 20, 300 },
        { "iso_a4_210x297mm",         2400, 0x01,  63, 16, 300 },
        { "na_legal_8.5x14in",        2000, 0x0C, 225, 45, 252 },
        { "iso_b5_176x250mm",         2000, 0x09,  63, 16, 252 },
        { "na_number-10_4.125x9.5in", 1133, 0x21,  63, 18, 144 },
    };
    CHECK_EQ_INT(s->n_media, 5);
    for (int i = 0; i < s->n_media && i < 5; i++) {
        const qd_media_t *m = &s->media[i];
        CHECK(!strcmp(m->media, want[i].media));
        CHECK_EQ_INT(m->size, want[i].size);
        CHECK_EQ_INT(m->img_w, (want[i].area_w + 7) / 8 * 8);
        qd_setup_t u = qd_setup(m);
        CHECK_EQ_INT(-u.top, (m->paper_h - m->img_h) / 2);
        CHECK_EQ_INT(-u.left, (m->paper_w - want[i].area_w + 0xB7) / 16);
        CHECK_EQ_INT(-u.width, ((want[i].area_w + 24) / 8) & ~3);
        CHECK_EQ_INT(-u.top, want[i].top);
        CHECK_EQ_INT(-u.left, want[i].left);
        CHECK_EQ_INT(-u.width, want[i].width);
        CHECK_EQ_INT(qd_row_bytes(m), (size_t)want[i].width);
        CHECK(qd_row_bytes(m) * 8 >= m->img_w);
        CHECK(qd_media_for(QD_PLWLS, m->img_w, m->page_h) == m);
    }
    const printer_caps_t *c = qd_caps(QD_PLWLS);
    CHECK(c != NULL);
    if (!c) return;
    CHECK(!strcmp(c->make_model, "Apple Personal LaserWriter LS"));
    CHECK(!strcmp(c->device_id, "MFG:Apple;MDL:Personal LaserWriter LS;CMD:URF;"));
    CHECK(!strcmp(c->urf_rs, "RS300"));
    CHECK_EQ_INT(c->n_media, s->n_media);
    CHECK_EQ_INT(c->n_sources, 1);
    CHECK(c->sources && !strcmp(c->sources[0], "main"));
    for (int i = 0; i < c->n_media; i++) {
        CHECK(!strcmp(c->media_list[i].media, s->media[i].media));
        CHECK_EQ_INT(c->media_list[i].rip_w, s->media[i].img_w);
    }
}

static void test_media_for(void)
{
    const qd_spec_t *s = qd_spec(QD_PLW300);
    CHECK(qd_media_for(QD_PLW300, 2400, 3300) == &s->media[0]);
    CHECK(qd_media_for(QD_PLW300, 2400, 3302) == &s->media[0]);       // within 2
    const qd_media_t *legal = NULL;
    for (int i = 0; i < s->n_media; i++)
        if (!strcmp(s->media[i].media, "na_legal_8.5x14in")) legal = &s->media[i];
    if (legal && legal->img_w == 2400) CHECK(qd_media_for(QD_PLW300, 2400, 4200) == legal);
    for (int i = 0; i < s->n_media; i++)                               // each sheet names itself
        CHECK(qd_media_for(QD_PLW300, s->media[i].img_w, s->media[i].page_h) == &s->media[i]);
    int any1200 = 0;
    for (int i = 0; i < s->n_media; i++) any1200 |= s->media[i].img_w == 1200;
    if (!any1200) CHECK(qd_media_for(QD_PLW300, 1200, 1800) == &s->media[0]);
    CHECK(qd_media_for(QD_NONE, 2400, 3300) == NULL);
}

static void test_caps(void)
{
    const qd_spec_t *s = qd_spec(QD_PLW300);
    const printer_caps_t *c = qd_caps(QD_PLW300);
    CHECK(c != NULL);
    if (!c) return;
    CHECK(!strcmp(c->urf_rs, "RS300"));
    CHECK(!strcmp(c->make_model, "Apple Personal LaserWriter 300"));
    CHECK(!strcmp(c->device_id, "MFG:Apple;MDL:Personal LaserWriter 300;CMD:URF;"));
    CHECK_EQ_INT(c->res_x, 300); CHECK_EQ_INT(c->res_y, 300);
    CHECK_EQ_INT(c->rip_dpi_x, 300); CHECK_EQ_INT(c->rip_dpi_y, 300);
    CHECK_EQ_INT(c->max_dots, 2400);
    CHECK_EQ_INT(c->crop_margin, 0);
    CHECK_EQ_INT(c->n_media, s->n_media);
    CHECK_EQ_INT(c->n_sources, 2);
    CHECK(c->sources && !strcmp(c->sources[0], "main") && !strcmp(c->sources[1], "manual"));
    const printer_media_t *l = &c->media_list[0];
    CHECK(!strcmp(l->media, "na_letter_8.5x11in"));
    CHECK_EQ_INT(l->x, 21590); CHECK_EQ_INT(l->y, 27940);
    CHECK_EQ_INT(l->margin_l, 635); CHECK_EQ_INT(l->margin_r, 635);
    CHECK_EQ_INT(l->margin_t, 525); CHECK_EQ_INT(l->margin_b, 534);
    CHECK_EQ_INT(l->rip_left, 75); CHECK_EQ_INT(l->rip_w, 2400);
    CHECK_EQ_INT(l->page_w, 2550); CHECK_EQ_INT(l->page_h, 3300);
    CHECK(!strcmp(c->media, "na_letter_8.5x11in"));                    // the scalars repeat Letter
    CHECK_EQ_INT(c->margin_t, 525); CHECK_EQ_INT(c->margin_b, 534);
    for (int i = 0; i < c->n_media; i++) {                             // every sheet from its row
        const qd_media_t *m = &s->media[i];
        const printer_media_t *p = &c->media_list[i];
        CHECK(!strcmp(p->media, m->media));
        CHECK_EQ_INT(p->page_w, m->page_w); CHECK_EQ_INT(p->page_h, m->page_h);
        CHECK_EQ_INT(p->rip_w, m->img_w);
        CHECK_EQ_INT(p->rip_left, (m->page_w - m->img_w) / 2);
        // outward: the margin in 1/100 mm covers the dots it stands for
        int top = (m->page_h - m->img_h) / 2, bottom = m->page_h - m->img_h - top;
        CHECK(p->margin_t * 300L >= top * 2540L && (p->margin_t - 1) * 300L < top * 2540L);
        CHECK(p->margin_b * 300L >= bottom * 2540L && (p->margin_b - 1) * 300L < bottom * 2540L);
    }
    CHECK(qd_caps(QD_NONE) == NULL);
}

static void test_every_size_fits(void)
{
    for (qd_model_t md = QD_PLW300; md < QD_MODELS; md++) {
    const qd_spec_t *s = qd_spec(md);
    CHECK(s->n_media >= 1 && s->n_media <= 6);
    for (int i = 0; i < s->n_media; i++) {
        const qd_media_t *m = &s->media[i];
        CHECK_EQ_INT(m->img_w % 8, 0);
        CHECK(m->img_w <= m->paper_w && m->img_h <= m->paper_h);
        CHECK(m->img_w <= m->page_w && m->img_h <= m->page_h);
        qd_setup_t u = qd_setup(m);
        CHECK(u.left >= -63 && u.left <= 0);
        CHECK(u.top <= 0);
        CHECK(u.width <= -(m->img_w / 8));
        CHECK(qd_row_bytes(m) * 8 >= m->img_w);
        CHECK(sw_code_max(qd_row_bytes(m)) <= qd_row_max(s->row_bits));
    }
    }
}

// W-1146: the Select 300, from its own driver (LW Select 300 1.0): its sources and density,
// Letter, A4 and Legal, the 300's path's setup (top (paper h - image h) / 2 + 0x27, left
// (2550 - image w + 0x16A) / 16 bytes, the row in bytes) and page memory's (left and width in
// dots).
static void test_sel(void)
{
    const qd_spec_t *s = qd_spec(QD_SEL300);
    CHECK(s != NULL);
    if (!s) return;
    CHECK(!strcmp(s->name, "LaserWriter Select 300"));
    CHECK(!strcmp(s->icon, "lwselect"));
    CHECK_EQ_INT(s->driver, QD_DRV_SEL);
    CHECK_EQ_INT(s->src_main, 0x51);
    CHECK_EQ_INT(s->src_manual, 0x4F);
    CHECK_EQ_INT(s->density_cmd, 0x58);
    CHECK_EQ_INT(qd_value(s->density), 0xE9);  // Apple's default as its driver sends it
    CHECK_EQ_INT(s->row_bits, 12);
    CHECK(s->budget > 0 && s->budget <= 500000);
    static const struct { const char *media; int top, left, width, pm_left, pm_width; } want[] = {
        { "na_letter_8.5x11in", 101, 32, 300, 256, 2400 },
        { "iso_a4_210x297mm",   101, 32, 300, 256, 2400 },
        { "na_legal_8.5x14in",  264, 57, 250, 456, 2000 },
    };
    CHECK_EQ_INT(s->n_media, 3);
    for (int i = 0; i < s->n_media && i < 3; i++) {
        const qd_media_t *m = &s->media[i];
        CHECK(!strcmp(m->media, want[i].media));
        qd_setup_t u = qd_setup(m), v = qd_setup_pm(m);
        CHECK_EQ_INT(-u.top, (m->paper_h - m->img_h) / 2 + 0x27);
        CHECK_EQ_INT(-u.left, (2550 - m->img_w + 0x16A) / 16);
        CHECK_EQ_INT(-u.width, m->img_w / 8);
        CHECK_EQ_INT(-u.top, want[i].top);
        CHECK_EQ_INT(-u.left, want[i].left);
        CHECK_EQ_INT(-u.width, want[i].width);
        CHECK_EQ_INT(v.top, u.top);
        CHECK_EQ_INT(-v.left, want[i].pm_left);
        CHECK(-v.left <= 0x1FF);               // its driver's cap, never reached
        CHECK_EQ_INT(-v.width, want[i].pm_width);
        CHECK_EQ_INT(qd_row_bytes(m), (size_t)want[i].width);
        CHECK(qd_media_for(QD_SEL300, m->img_w, m->page_h) == m);
    }
    const printer_caps_t *c = qd_caps(QD_SEL300);
    CHECK(c != NULL);
    if (!c) return;
    CHECK(!strcmp(c->make_model, "Apple LaserWriter Select 300"));
    CHECK(!strcmp(c->device_id, "MFG:Apple;MDL:LaserWriter Select 300;CMD:URF;"));
    CHECK_EQ_INT(c->n_media, s->n_media);
    CHECK_EQ_INT(c->n_sources, 2);
    for (int i = 0; i < c->n_media; i++) {
        CHECK(!strcmp(c->media_list[i].media, s->media[i].media));
        CHECK_EQ_INT(c->media_list[i].rip_w, s->media[i].img_w);
    }
}

int main(void)
{
    test_letter();
    test_engine_names();
    test_ls();
    test_sel();
    test_media_for();
    test_caps();
    test_every_size_fits();
    TEST_END;
}
