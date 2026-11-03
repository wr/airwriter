// SPDX-License-Identifier: GPL-3.0-or-later
//
// A StyleWriter's AirPrint answers (W-1103): Letter, A4 and Legal with the
// engine's margins, where the ImageWriter has always offered one sheet. The
// ImageWriter's own answers stay byte for byte (test_ipp_golden.c); here its
// caps only have to say they list no sheets. The size of a full answer is
// printed because RESP_MAX in ipp_server.c (5120) is chosen against it.
#include "test_util.h"
#include "ipp_attrs.h"
#include "ipp_walk.h"
#include "sw_model.h"

#define RESP_MAX 5120               // ipp_server.c's buffer for an answer

static uint8_t buf[RESP_MAX];

static ipp_printer_t printer(sw_model_t m, int colour)
{
    ipp_printer_t p = {
        .uri            = "ipp://airwriter.local:631/ipp/p1",
        .name           = "StyleWriter",
        .info           = "Apple StyleWriter",
        .uuid           = "urn:uuid:41575254-4552-0002-a1b2c3d4e5f6",
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
        .caps           = sw_caps(m),
    };
    return p;
}

// The answer to a Get-Printer-Attributes naming `names` (an ipp_names_t built
// here), or all of it when names is NULL. Returns its length in buf.
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

// A media-col, or a media-size-supported collection: whatever integers its
// members gave (media-size's x and y are one level down in a media-col).
typedef struct { int32_t x, y, l, r, t, b; int has_size; } mcol_t;

// Reads the collection values of attribute `name` into out[], at most max, and
// returns how many top-level values it has (0 when it is not there).
static int collections(const uint8_t *m, size_t len, const char *name, mcol_t *out, int max)
{
    size_t pos = 8;
    int found = 0, depth = 0, n = 0;
    char member[40] = "";
    while (pos < len) {
        uint8_t tag = m[pos++];
        if (tag == 0x03) break;
        if (tag <= 0x0F) { if (found) break; continue; }
        size_t nlen = (size_t)((m[pos] << 8) | m[pos + 1]);
        const uint8_t *nm = m + pos + 2;
        pos += 2 + nlen;
        size_t vlen = (size_t)((m[pos] << 8) | m[pos + 1]);
        const uint8_t *v = m + pos + 2;
        pos += 2 + vlen;
        if (!found) {
            found = tag == 0x34 && nlen == strlen(name) && !memcmp(nm, name, nlen);
            if (!found) continue;
        } else if (nlen && depth == 0) {
            break;                                  // the next attribute
        }
        if (tag == 0x34) {
            if (depth == 0) {
                if (n < max) memset(&out[n], 0, sizeof out[n]);
                n++;
            }
            if (depth == 1) member[0] = '\0';
            depth++;
        } else if (tag == 0x37) {
            depth--;
        } else if (tag == 0x4A) {
            snprintf(member, sizeof member, "%.*s", (int)vlen, (const char *)v);
        } else if (tag == 0x21 && n >= 1 && n <= max) {
            mcol_t *c = &out[n - 1];
            int32_t x = (int32_t)(((uint32_t)v[0] << 24) | ((uint32_t)v[1] << 16) | ((uint32_t)v[2] << 8) | v[3]);
            if (!strcmp(member, "x-dimension"))              { c->x = x; c->has_size = 1; }
            else if (!strcmp(member, "y-dimension"))         c->y = x;
            else if (!strcmp(member, "media-left-margin"))   c->l = x;
            else if (!strcmp(member, "media-right-margin"))  c->r = x;
            else if (!strcmp(member, "media-top-margin"))    c->t = x;
            else if (!strcmp(member, "media-bottom-margin")) c->b = x;
        }
    }
    return n;
}

// Every value of a multi-valued integer attribute, in order; returns the count.
static int ints(const uint8_t *m, size_t len, const char *name, int32_t *out, int max)
{
    ipp_found_t f;
    if (!ipp_find(m, len, name, &f)) return 0;
    const uint8_t *v = f.val;
    size_t vlen = f.vlen;
    int n = 0;
    for (int i = 0; i < f.nvalues; i++) {
        if (n < max && vlen == 4)
            out[n] = (int32_t)(((uint32_t)v[0] << 24) | ((uint32_t)v[1] << 16) | ((uint32_t)v[2] << 8) | v[3]);
        n++;
        v += vlen;
        vlen = (size_t)((v[3] << 8) | v[4]);
        v += 5;
    }
    return n;
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

static void sheets(sw_model_t m, int colour, int32_t right_letter, int32_t top, int32_t bottom)
{
    ipp_printer_t p = printer(m, colour);

    // The answer to everything (as GPA with no list) and with the database too.
    size_t n = answer(&p, NULL);
    printf("    %s%s: printer attributes encode to %zu bytes (RESP_MAX %d)\n",
           sw_spec(m)->name, colour ? ", colour" : "", n, RESP_MAX);
    CHECK(n < RESP_MAX);
    ipp_found_t f;
    CHECK(!ipp_find(buf, n, "media-col-database", &f));        // only when named

    char w[160];
    words(buf, n, "media-supported", w, sizeof w);
    CHECK(strcmp(w, "na_letter_8.5x11in iso_a4_210x297mm na_legal_8.5x14in") == 0);
    words(buf, n, "media-ready", w, sizeof w);
    CHECK(strcmp(w, "na_letter_8.5x11in iso_a4_210x297mm na_legal_8.5x14in") == 0);
    CHECK(ipp_find(buf, n, "media-default", &f));
    CHECK(ipp_val_is(&f, "na_letter_8.5x11in"));               // the default is the first sheet

    mcol_t c[4];
    CHECK_EQ_INT(collections(buf, n, "media-col-default", c, 4), 1);
    CHECK_EQ_INT(c[0].x, 21590);
    CHECK_EQ_INT(c[0].y, 27940);
    CHECK_EQ_INT(c[0].r, right_letter);
    CHECK_EQ_INT(collections(buf, n, "media-col-ready", c, 4), 3);
    CHECK_EQ_INT(collections(buf, n, "media-size-supported", c, 4), 3);
    CHECK_EQ_INT(c[0].x, 21590);  CHECK_EQ_INT(c[0].y, 27940);
    CHECK_EQ_INT(c[1].x, 21000);  CHECK_EQ_INT(c[1].y, 29700);
    CHECK_EQ_INT(c[2].x, 21590);  CHECK_EQ_INT(c[2].y, 35560);

    // The margins, sheet by sheet, in the database the Mac reads and as lists.
    const char *only[] = { "media-col-database", NULL };
    size_t nd = answer(&p, only);
    printf("    media-col-database alone: %zu bytes\n", nd);
    CHECK_EQ_INT(collections(buf, nd, "media-col-database", c, 4), 3);
    CHECK_EQ_INT(c[0].x, 21590);  CHECK_EQ_INT(c[0].y, 27940);
    CHECK_EQ_INT(c[0].l, 508);    CHECK_EQ_INT(c[0].r, right_letter);
    CHECK_EQ_INT(c[0].t, top);    CHECK_EQ_INT(c[0].b, bottom);
    CHECK_EQ_INT(c[1].x, 21000);  CHECK_EQ_INT(c[1].y, 29700);
    CHECK_EQ_INT(c[1].l, 508);    CHECK_EQ_INT(c[1].r, 508);
    CHECK_EQ_INT(c[1].t, top);    CHECK_EQ_INT(c[1].b, bottom);
    CHECK_EQ_INT(c[2].x, 21590);  CHECK_EQ_INT(c[2].y, 35560);
    CHECK_EQ_INT(c[2].l, 508);    CHECK_EQ_INT(c[2].r, right_letter);
    CHECK_EQ_INT(c[2].t, top);    CHECK_EQ_INT(c[2].b, bottom);

    // The most a client can ask for: everything and the database as well.
    const char *both[] = { "all", "media-col-database", NULL };
    size_t nb = answer(&p, both);
    printf("    all and media-col-database: %zu bytes\n", nb);
    CHECK(nb < RESP_MAX);

    // The four -supported lists hold each distinct margin once, in order.
    n = answer(&p, NULL);
    int32_t v[4];
    CHECK_EQ_INT(ints(buf, n, "media-left-margin-supported", v, 4), 1);
    CHECK_EQ_INT(v[0], 508);
    CHECK_EQ_INT(ints(buf, n, "media-top-margin-supported", v, 4), 1);
    CHECK_EQ_INT(v[0], top);
    CHECK_EQ_INT(ints(buf, n, "media-bottom-margin-supported", v, 4), 1);
    CHECK_EQ_INT(v[0], bottom);
    if (right_letter == 508) {
        CHECK_EQ_INT(ints(buf, n, "media-right-margin-supported", v, 4), 1);
        CHECK_EQ_INT(v[0], 508);
    } else {
        CHECK_EQ_INT(ints(buf, n, "media-right-margin-supported", v, 4), 2);
        CHECK_EQ_INT(v[0], right_letter);
        CHECK_EQ_INT(v[1], 508);
    }

    // 360 dpi, one pass.
    CHECK(ipp_find(buf, n, "printer-resolution-default", &f));
    CHECK_EQ_INT(f.vlen, 9);
    CHECK_EQ_INT((f.val[2] << 8) | f.val[3], 360);
    CHECK_EQ_INT((f.val[6] << 8) | f.val[7], 360);
    CHECK(ipp_find(buf, n, "urf-supported", &f));
    CHECK(ipp_val_is(&f, "V1.4"));
    CHECK(ipp_find(buf, n, "printer-make-and-model", &f));
    CHECK(ipp_val_is(&f, sw_spec(m)->make_model));

    // Asking for one thing gets that thing only, still shaped as a list.
    const char *just[] = { "media-supported", NULL };
    n = answer(&p, just);
    words(buf, n, "media-supported", w, sizeof w);
    CHECK(strcmp(w, "na_letter_8.5x11in iso_a4_210x297mm na_legal_8.5x14in") == 0);
    CHECK(!ipp_find(buf, n, "media-col-ready", &f));
}

int main(void)
{
    // The ImageWriter lists no sheets: it is the one scalar sheet it always was.
    CHECK_EQ_INT(printer_caps_imagewriter.n_media, 0);
    CHECK(printer_caps_imagewriter.media_list == NULL);
    for (int m = 1; m <= 3; m++) {
        CHECK_EQ_INT(printer_caps_imagewriter_model(m)->n_media, 0);
        CHECK(printer_caps_imagewriter_model(m)->media_list == NULL);
    }

    // The caps themselves: three sheets, the margins the design gives.
    const printer_caps_t *ii = sw_caps(SW_II);
    CHECK_EQ_INT(ii->n_media, 3);
    CHECK_EQ_INT(ii->media_list[0].margin_l, 508);
    CHECK_EQ_INT(ii->media_list[0].margin_r, 762);
    CHECK_EQ_INT(ii->media_list[0].margin_t, 508);
    CHECK_EQ_INT(ii->media_list[0].margin_b, 805);
    CHECK_EQ_INT(ii->media_list[0].rip_left, 72);
    CHECK_EQ_INT(ii->media_list[0].rip_w, 2880);
    const printer_caps_t *c25 = sw_caps(SW_C2500);
    CHECK_EQ_INT(c25->n_media, 3);
    CHECK_EQ_INT(c25->media_list[0].margin_r, 508);
    CHECK_EQ_INT(c25->media_list[0].margin_t, 318);
    CHECK_EQ_INT(c25->media_list[0].margin_b, 699);
    CHECK_EQ_INT(c25->media_list[0].rip_w, 2916);
    CHECK_EQ_INT(ii->media_list[1].rip_w, 2832);
    CHECK_EQ_INT(c25->media_list[1].rip_w, 2832);

    sheets(SW_II, 0, 762, 508, 805);
    sheets(SW_II, 1, 762, 508, 805);              // colour adds the URF's SRGB24 and a colour mode
    sheets(SW_C2500, 0, 508, 318, 699);
    sheets(SW_C2500, 1, 508, 318, 699);
    sheets(SW_ORIGINAL, 0, 762, 508, 805);

    TEST_END;
}
