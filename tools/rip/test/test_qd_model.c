// SPDX-License-Identifier: GPL-3.0-or-later
// W-1102: the QuickDraw LaserWriters' model table: the Personal LaserWriter 300's sheets, the
// setup values its page is sent with, and what AirPrint is told about it.
#include "test_util.h"
#include "qd_model.h"
#include "apple_tables_lw.h"

static void test_letter(void)
{
    const qd_spec_t *s = qd_spec(QD_PLW300);
    CHECK(s && s->engine == 0x29 && s->budget == 500000 && s->density == 32);
    if (!s) return;
    CHECK_EQ_INT(s->ctrl, 2);
    CHECK_EQ_INT(s->src_cassette, 0x4C);
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
    CHECK(qd_spec(QD_NONE) == NULL);
    CHECK(qd_spec(QD_MODELS) == NULL);
    CHECK(qd_spec((qd_model_t)-1) == NULL);
}

static void test_engine_names(void)
{
    CHECK_EQ_INT(qd_model_from_engine(0x29), QD_PLW300);
    CHECK_EQ_INT(qd_model_from_engine(0x0D), QD_NONE);
    CHECK_EQ_INT(qd_model_from_engine(0x23), QD_NONE);
    CHECK_EQ_INT(qd_model_from_engine(0x55), QD_NONE);
    CHECK(!strcmp(qd_engine_name(0x29), "Personal LaserWriter 300"));
    CHECK(!strcmp(qd_engine_name(0x0D), "Personal LaserWriter LS"));
    CHECK(qd_engine_name(0x23) == NULL);
    CHECK(!strcmp(qd_engine_name(0x55), "LaserWriter Select 300"));
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
    const qd_spec_t *s = qd_spec(QD_PLW300);
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
    }
}

int main(void)
{
    test_letter();
    test_engine_names();
    test_media_for();
    test_caps();
    test_every_size_fits();
    TEST_END;
}
