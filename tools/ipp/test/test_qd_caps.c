// SPDX-License-Identifier: GPL-3.0-or-later
//
// A Personal LaserWriter 300's AirPrint answers (W-1102): 300 dpi, gray only, Apple's six
// sheets with the image area's margins, and two paper sources, the cassette and the manual
// slot. Every other printer still has the one source, main. The size of the full answer is
// printed because RESP_MAX in ipp_server.c is chosen against it.
#include "test_util.h"
#include "ipp_attrs.h"
#include "ipp_walk.h"
#include "qd_model.h"
#include "sw_model.h"

#define RESP_MAX 7680               // ipp_server.c's buffer for an answer

static uint8_t buf[RESP_MAX];

static ipp_printer_t printer(const printer_caps_t *caps, int colour)
{
    ipp_printer_t p = {
        .uri            = "ipp://airwriter.local:631/ipp/p3",
        .name           = "Personal LaserWriter 300",
        .info           = "Apple Personal LaserWriter 300",
        .uuid           = "urn:uuid:41575254-4552-0003-a1b2c3d4e5f6",
        .adminurl       = "http://airwriter.local/",
        .icons          = { "http://airwriter.local:631/icon-48.png",
                            "http://airwriter.local:631/icon-128.png",
                            "http://airwriter.local:631/icon-512.png" },
        .location       = "",
        .state          = IPP_PSTATE_IDLE,
        .state_reason   = "none",
        .accepting_jobs = 1,
        .uptime_s       = 1,
        .colour         = colour,
        .caps           = caps,
        .supply         = IPP_SUPPLY_NONE,
    };
    return p;
}

// The answer to a Get-Printer-Attributes naming `names` (NULL-terminated), or all of it
// when names is NULL. Returns its length in buf.
static size_t answer(const ipp_printer_t *p, const char *const *names)
{
    static uint8_t store[512];
    ipp_names_t req = { 0 };
    if (names) {
        size_t k = 0;
        for (int i = 0; names[i]; i++) {
            const char *attr = i ? "" : "requested-attributes";
            size_t an = strlen(attr), vn = strlen(names[i]);
            store[k++] = IPP_TAG_KEYWORD;
            store[k++] = (uint8_t)(an >> 8); store[k++] = (uint8_t)an;
            memcpy(store + k, attr, an); k += an;
            store[k++] = (uint8_t)(vn >> 8); store[k++] = (uint8_t)vn;
            memcpy(store + k, names[i], vn); k += vn;
            req.n++;
        }
        req.p = store;
        req.len = k;
    }
    ipp_enc_t e;
    ipp_enc_init(&e, buf, sizeof buf);
    ipp_resp_begin(&e, IPP_VERSION_1_1, IPP_STATUS_OK, 1);
    ipp_write_op_attrs(&e);
    ipp_write_printer_attrs(&e, p, names ? &req : NULL);
    ipp_end(&e);
    CHECK(ipp_enc_ok(&e));
    return ipp_enc_len(&e);
}

// The values of a multi-valued keyword attribute joined with spaces.
static void words(const uint8_t *m, size_t len, const char *name, char *out, size_t cap)
{
    ipp_found_t f;
    out[0] = '\0';
    if (!ipp_find(m, len, name, &f)) return;
    const uint8_t *v = f.val;
    size_t vlen = f.vlen, k = 0;
    for (int i = 0; i < f.nvalues && k + vlen + 2 < cap; i++) {
        if (i) out[k++] = ' ';
        memcpy(out + k, v, vlen);
        k += vlen;
        v += vlen;
        vlen = (size_t)((v[3] << 8) | v[4]);
        v += 5;
    }
    out[k] = '\0';
}

static void test_plw300_attrs(void)
{
    const printer_caps_t *c = qd_caps(QD_PLW300);
    CHECK(c != NULL);
    ipp_printer_t p = printer(c, 0);
    size_t n = answer(&p, NULL);
    char w[320];

    words(buf, n, "urf-supported", w, sizeof w);
    CHECK(strstr(w, "RS300") != NULL);
    CHECK(strstr(w, "W8") != NULL);
    CHECK(strstr(w, "SRGB24") == NULL);
    words(buf, n, "print-color-mode-supported", w, sizeof w);
    CHECK(strcmp(w, "monochrome") == 0);

    words(buf, n, "media-supported", w, sizeof w);
    CHECK(strcmp(w, "na_letter_8.5x11in na_legal_8.5x14in na_executive_7.25x10.5in "
                    "na_number-10_4.125x9.5in na_monarch_3.875x7.5in iso_a4_210x297mm") == 0);
    ipp_found_t f;
    CHECK(ipp_find(buf, n, "media-default", &f));
    CHECK(ipp_val_is(&f, "na_letter_8.5x11in"));

    words(buf, n, "media-source-supported", w, sizeof w);
    CHECK(strcmp(w, "main manual") == 0);

    // Letter's image area, 2400 x 3175 dots centred on the sheet, in 1/100 mm rounded out.
    CHECK_EQ_INT(c->media_list[0].margin_l, 635);
    CHECK_EQ_INT(c->media_list[0].margin_r, 635);
    CHECK_EQ_INT(c->media_list[0].margin_t, 525);
    CHECK_EQ_INT(c->media_list[0].margin_b, 534);
    CHECK(ipp_find(buf, n, "printer-resolution-default", &f));
    CHECK_EQ_INT((f.val[2] << 8) | f.val[3], 300);
}

static void test_plw300_size(void)
{
    ipp_printer_t p = printer(qd_caps(QD_PLW300), 0);
    size_t n = answer(&p, NULL);
    printf("    Personal LaserWriter 300: printer attributes encode to %zu bytes (RESP_MAX %d)\n",
           n, RESP_MAX);
    CHECK(n < RESP_MAX);
    const char *both[] = { "all", "media-col-database", NULL };
    size_t nb = answer(&p, both);
    printf("    all and media-col-database: %zu bytes\n", nb);
    CHECK(nb < RESP_MAX);
}

static void test_other_printers_one_source(void)
{
    char w[64];
    ipp_printer_t iw = printer(NULL, 0);
    size_t n = answer(&iw, NULL);
    words(buf, n, "media-source-supported", w, sizeof w);
    CHECK(strcmp(w, "main") == 0);
    ipp_printer_t sw = printer(sw_caps(SW_II), 1);
    n = answer(&sw, NULL);
    words(buf, n, "media-source-supported", w, sizeof w);
    CHECK(strcmp(w, "main") == 0);
}

int main(void)
{
    test_plw300_attrs();
    test_plw300_size();
    test_other_printers_one_source();
    TEST_END;
}
