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

    TEST_END;
}
