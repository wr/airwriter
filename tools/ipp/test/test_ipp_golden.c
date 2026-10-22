// SPDX-License-Identifier: GPL-3.0-or-later
//
// The ImageWriter's IPP answers, byte for byte (W-1082). iOS keys a printer on
// its printer-uuid with its capabilities cached against it (docs/traps.md), so
// a second printer on the board must not move a single byte of the first's.
// These are the answers an idle board gives, recorded before the printer
// became one of several; GOLDEN_REGEN=1 rewrites them, which only a change
// meant to reach every ImageWriter's clients should ever do.
#include "test_util.h"
#include "ipp_attrs.h"

#include <stdlib.h>

static uint8_t buf[8192];

static const char *const k_icons[3] = {
    "http://airwriter.local:631/icon-48.png",
    "http://airwriter.local:631/icon-128.png",
    "http://airwriter.local:631/icon-512.png",
};

// As ipp_server.c's fill_printer builds it for an idle board.
static ipp_printer_t imagewriter(int colour)
{
    ipp_printer_t p = {
        .uri            = "ipp://airwriter.local:631/ipp/print",
        .name           = "ImageWriter II",
        .info           = "Apple ImageWriter II",
        .uuid           = "urn:uuid:41575254-4552-0002-a1b2c3d4e5f6",
        .adminurl       = "http://airwriter.local/",
        .location       = "",
        .state          = IPP_PSTATE_IDLE,
        .state_reason   = "none",
        .state_message  = NULL,
        .accepting_jobs = 1,
        .queued_jobs    = 0,
        .uptime_s       = 1,
        .colour         = colour,
    };
    for (int i = 0; i < 3; i++) p.icons[i] = k_icons[i];
    return p;
}

// requested-attributes holding the one name, as ipp_req_parse leaves it: the
// value tag, the attribute's name, then the value.
static ipp_names_t one_name(uint8_t *store, const char *name)
{
    static const char attr[] = "requested-attributes";
    size_t an = sizeof attr - 1, vn = strlen(name), k = 0;
    store[k++] = IPP_TAG_KEYWORD;
    store[k++] = (uint8_t)(an >> 8); store[k++] = (uint8_t)an;
    memcpy(store + k, attr, an); k += an;
    store[k++] = (uint8_t)(vn >> 8); store[k++] = (uint8_t)vn;
    memcpy(store + k, name, vn); k += vn;
    ipp_names_t n = { store, k, 1 };
    return n;
}

static size_t printer_answer(const ipp_printer_t *p, const ipp_names_t *req)
{
    ipp_enc_t e;
    ipp_enc_init(&e, buf, sizeof buf);
    ipp_resp_begin(&e, IPP_VERSION_1_1, IPP_STATUS_OK, 7);
    ipp_write_op_attrs(&e);
    ipp_write_printer_attrs(&e, p, req);
    ipp_end(&e);
    CHECK(ipp_enc_ok(&e));
    return ipp_enc_len(&e);
}

static size_t job_answer(void)
{
    ipp_job_t j = {
        .id = 3, .uri = "ipp://airwriter.local:631/ipp/print/3",
        .printer_uri = "ipp://airwriter.local:631/ipp/print",
        .name = "Letter", .user = "wells", .state = IPP_JSTATE_PROCESSING,
        .state_reason = "job-printing", .impressions_done = 0, .time_at_creation = 40,
    };
    ipp_enc_t e;
    ipp_enc_init(&e, buf, sizeof buf);
    ipp_resp_begin(&e, IPP_VERSION_1_1, IPP_STATUS_OK, 9);
    ipp_write_op_attrs(&e);
    ipp_write_job_attrs(&e, &j);
    ipp_end(&e);
    CHECK(ipp_enc_ok(&e));
    return ipp_enc_len(&e);
}

static int regen;

// Compares `len` bytes of buf with the file, or writes them when regenerating.
static void golden(const char *file, const uint8_t *got, size_t len)
{
    char path[128];
    snprintf(path, sizeof path, "test/golden/%s", file);
    if (regen) {
        FILE *f = fopen(path, "wb");
        CHECK(f != NULL);
        if (!f) return;
        CHECK(fwrite(got, 1, len, f) == len);
        fclose(f);
        printf("    wrote %s (%zu bytes)\n", path, len);
        return;
    }
    static uint8_t want[8192];
    FILE *f = fopen(path, "rb");
    CHECK(f != NULL);
    if (!f) return;
    size_t n = fread(want, 1, sizeof want, f);
    fclose(f);
    CHECK_EQ_INT(len, n);
    if (len == n) CHECK_EQ_MEM(got, want, n);
}

// The _ipp._tcp TXT record as ipp_server.c's add_print_services writes it,
// one key=value per line.
static size_t txt(char *out, size_t cap, int colour)
{
    const char *items[][2] = {
        { "txtvers",  "1" },
        { "rp",       "ipp/print" },
        { "ty",       "Apple ImageWriter II" },
        { "product",  "(AirWriter)" },
        { "pdl",      "image/urf" },
        { "adminurl", "http://airwriter.local/" },
        { "priority", "10" },
        { "UUID",     "41575254-4552-0002-a1b2c3d4e5f6" },
        { "Color",    colour ? "T" : "F" },
        { "Duplex",   "F" },
        { "URF",      colour ? IPP_URF_STRING_COLOUR : IPP_URF_STRING },
        { "TLS",      "" },
        { "note",     "" },
    };
    size_t k = 0;
    for (size_t i = 0; i < sizeof items / sizeof items[0]; i++)
        k += (size_t)snprintf(out + k, cap - k, "%s=%s\n", items[i][0], items[i][1]);
    return k;
}

int main(void)
{
    regen = getenv("GOLDEN_REGEN") != NULL;
    uint8_t names[64];

    ipp_printer_t p = imagewriter(0);
    golden("imagewriter-gpa-all.bin", buf, printer_answer(&p, NULL));

    ipp_names_t mcd = one_name(names, "media-col-database");
    golden("imagewriter-gpa-mcd.bin", buf, printer_answer(&p, &mcd));

    ipp_printer_t c = imagewriter(1);
    golden("imagewriter-gpa-colour.bin", buf, printer_answer(&c, NULL));

    golden("imagewriter-job.bin", buf, job_answer());

    char t[1024];
    size_t tn = txt(t, sizeof t, 0);
    tn += txt(t + tn, sizeof t - tn, 1);
    golden("imagewriter-txt.txt", (const uint8_t *)t, tn);

    TEST_END;
}
