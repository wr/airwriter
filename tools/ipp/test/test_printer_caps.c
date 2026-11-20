// SPDX-License-Identifier: GPL-3.0-or-later
//
// printer_caps (W-1082): what a printer advertises and how the rip lays a
// page out for it, in one struct. The ImageWriter's must say exactly what
// the constants it replaced said.
#include "test_util.h"
#include "printer_caps.h"
#include "ipp_attrs.h"

int main(void)
{
    const printer_caps_t *iw = &printer_caps_imagewriter;
    char s[96];

    CHECK(printer_caps_urf(iw, 0, s, sizeof s) == strlen(IPP_URF_STRING));
    CHECK(strcmp(s, IPP_URF_STRING) == 0);
    CHECK(printer_caps_urf(iw, 1, s, sizeof s) == strlen(IPP_URF_STRING_COLOUR));
    CHECK(strcmp(s, IPP_URF_STRING_COLOUR) == 0);
    CHECK_EQ_INT(printer_caps_urf(iw, 0, s, 10), 0);          // too short: nothing

    printer_caps_t laser = *iw;
    laser.urf_rs = "RS300";
    printer_caps_urf(&laser, 0, s, sizeof s);
    CHECK(strcmp(s, "V1.4,CP1,RS300,W8,DM1,IS1,MT1,FN3") == 0);

    const char *kw[9];
    CHECK_EQ_INT(printer_caps_urf_keywords(iw, 0, kw), 8);
    CHECK(strcmp(kw[0], "V1.4") == 0 && strcmp(kw[2], "RS72-144") == 0 && strcmp(kw[7], "FN3") == 0);
    CHECK_EQ_INT(printer_caps_urf_keywords(iw, 1, kw), 9);
    CHECK(strcmp(kw[4], "SRGB24") == 0 && strcmp(kw[8], "FN3") == 0);

    CHECK(strcmp(iw->make_model, "Apple ImageWriter II") == 0);
    CHECK(strcmp(iw->device_id, "MFG:Apple;MDL:ImageWriter II;CMD:URF;") == 0);
    CHECK(strcmp(iw->media, "na_letter_8.5x11in") == 0);
    CHECK_EQ_INT(iw->res_x, 144);
    CHECK_EQ_INT(iw->res_y, 72);
    CHECK_EQ_INT(iw->rip_dpi_x, 144);
    CHECK_EQ_INT(iw->rip_dpi_y, 72);
    CHECK_EQ_INT(iw->max_dots, 1152);
    CHECK_EQ_INT(iw->crop_margin, 1);
    CHECK_EQ_INT(iw->media_x, 21590);
    CHECK_EQ_INT(iw->media_y, 27940);
    CHECK_EQ_INT(iw->margin_l, 635);
    CHECK_EQ_INT(iw->margin_r, 635);
    CHECK_EQ_INT(iw->margin_t, 2540);
    CHECK_EQ_INT(iw->margin_b, 1270);
    CHECK_EQ_INT(iw->ppm, 1);

        // W-1088: each ImageWriter names itself; the stream, and so the URF, is one.
    {
        const printer_caps_t *ii = printer_caps_imagewriter_model(1);
        const printer_caps_t *lq = printer_caps_imagewriter_model(2);
        const printer_caps_t *iw = printer_caps_imagewriter_model(3);
        CHECK(ii == &printer_caps_imagewriter);
        CHECK(strcmp(lq->make_model, "Apple ImageWriter LQ") == 0);
        CHECK(strcmp(lq->device_id, "MFG:Apple;MDL:ImageWriter LQ;CMD:URF;") == 0);
        CHECK(strcmp(iw->make_model, "Apple ImageWriter") == 0);
        char a[64], b[64];
        printer_caps_urf(ii, 0, a, sizeof a);
        printer_caps_urf(lq, 0, b, sizeof b);
        CHECK(strcmp(a, b) == 0);
        printer_caps_urf(ii, 1, a, sizeof a);
        printer_caps_urf(lq, 1, b, sizeof b);
        CHECK(strcmp(a, b) == 0);
        CHECK(printer_caps_imagewriter_model(0) == &printer_caps_imagewriter);
    }

    // W-1173: every ImageWriter has continuous fanfold beside Letter, 8.5 in by 1 in to 10 ft,
    // no top or bottom margin.
    {
        const printer_roll_t *r = iw->roll;
        CHECK(r != NULL);
        CHECK(printer_caps_imagewriter_model(2)->roll == r);
        CHECK(printer_caps_imagewriter_model(3)->roll == r);
        CHECK_EQ_INT(r->x, 21590);
        CHECK_EQ_INT(r->len_min, 2540);
        CHECK_EQ_INT(r->len_max, 304800);
        CHECK_EQ_INT(r->margin_l, 635);
        CHECK_EQ_INT(r->margin_r, 635);
        CHECK(strcmp(r->min_kw, "roll_min_8.5x1in") == 0);
        CHECK(strcmp(r->max_kw, "roll_max_8.5x120in") == 0);
        CHECK(strcmp(r->ready_kw, "roll_current_8.5x0in") == 0);

        // A job asks for the roll by naming one, or by a length more than 1/8 in from the sheet's.
        CHECK_EQ_INT(printer_caps_wants_roll(iw, 27940, 0), 0);
        CHECK_EQ_INT(printer_caps_wants_roll(iw, 27940 + 318, 0), 0);
        CHECK_EQ_INT(printer_caps_wants_roll(iw, 27940 - 318, 0), 0);
        CHECK_EQ_INT(printer_caps_wants_roll(iw, 27940 + 319, 0), 1);
        CHECK_EQ_INT(printer_caps_wants_roll(iw, 27940 - 319, 0), 1);
        CHECK_EQ_INT(printer_caps_wants_roll(iw, 101600, 0), 1);
        CHECK_EQ_INT(printer_caps_wants_roll(iw, 0, 1), 1);
        CHECK_EQ_INT(printer_caps_wants_roll(iw, 0, 0), 0);
        // A printer with no roll never prints one.
        printer_caps_t sheets = *iw;
        sheets.roll = NULL;
        CHECK_EQ_INT(printer_caps_wants_roll(&sheets, 101600, 1), 0);
        CHECK_EQ_INT(printer_caps_wants_roll(NULL, 101600, 1), 0);
    }
    TEST_END;
}
