// SPDX-License-Identifier: GPL-3.0-or-later
//
// The attribute set is what decides whether the printer appears in the iOS
// print sheet at all, and a missing or wrong attribute shows up as silence
// rather than an error. These checks pin the ones that carry that weight.
#include "test_util.h"
#include "ipp_attrs.h"
#include "ipp_walk.h"

static uint8_t buf[4096];

int main(void)
{
    ipp_printer_t p = {
        .uri            = "ipp://imagewriter-ab12.local:631/ipp/print",
        .name           = "AirWriter",
        .info           = "Apple ImageWriter II",
        .uuid           = "urn:uuid:564e4552-4f4e-4943-4100-0123456789ab",
        .adminurl       = "http://imagewriter-ab12.local/",
        .location       = "",
        .state          = IPP_PSTATE_IDLE,
        .state_reason   = "none",
        .accepting_jobs = 1,
        .queued_jobs    = 0,
        .uptime_s       = 61,
    };

    ipp_enc_t e;
    ipp_enc_init(&e, buf, sizeof buf);
    ipp_resp_begin(&e, IPP_VERSION_1_1, IPP_STATUS_OK, 1);
    ipp_write_op_attrs(&e);
    ipp_write_printer_attrs(&e, &p, NULL);
    ipp_end(&e);
    CHECK(ipp_enc_ok(&e));

    size_t n = ipp_enc_len(&e);
    ipp_found_t f;

    /* The response must open with charset then natural language, in that
     * order, before any other attribute. RFC 8011 requires it and clients
     * enforce it. */
    CHECK_EQ_INT(buf[8], IPP_TAG_OPERATION);
    CHECK(ipp_find(buf, n, "attributes-charset", &f));
    CHECK_EQ_INT(f.group, IPP_TAG_OPERATION);
    CHECK(ipp_val_is(&f, "utf-8"));
    CHECK(ipp_find(buf, n, "attributes-natural-language", &f));
    CHECK(ipp_val_is(&f, "en-us"));

    /* Identity. printer-uuid is what iOS caches capabilities against. */
    CHECK(ipp_find(buf, n, "printer-name", &f));
    CHECK(ipp_val_is(&f, "AirWriter"));
    CHECK_EQ_INT(f.group, IPP_TAG_PRINTER);
    CHECK(ipp_find(buf, n, "printer-uuid", &f));
    CHECK(ipp_val_is(&f, p.uuid));
    CHECK(ipp_find(buf, n, "printer-make-and-model", &f));
    CHECK(ipp_val_is(&f, "Apple ImageWriter II"));

    /* State. */
    CHECK(ipp_find(buf, n, "printer-state", &f));
    CHECK_EQ_INT(ipp_val_int(&f), IPP_PSTATE_IDLE);
    CHECK(ipp_find(buf, n, "printer-is-accepting-jobs", &f));
    CHECK_EQ_INT(f.vlen, 1);
    CHECK_EQ_INT(f.val[0], 1);
    CHECK(ipp_find(buf, n, "printer-up-time", &f));
    CHECK_EQ_INT(ipp_val_int(&f), 61);

    /* The format we can actually decode, and only that one. Advertising
     * image/pwg-raster would fail every job sent in it -- urf_decode
     * recognises the PWG magic but rejects it. */
    CHECK(ipp_find(buf, n, "document-format-default", &f));
    CHECK(ipp_val_is(&f, "image/urf"));
    CHECK(ipp_find(buf, n, "document-format-supported", &f));
    CHECK(ipp_val_is(&f, "image/urf"));

    /* urf-supported is the attribute AirPrint clients read for capabilities.
     * RS72-144 is the claim the real iPhone capture confirmed. */
    CHECK(ipp_find(buf, n, "urf-supported", &f));
    CHECK_EQ_INT(f.nvalues, 8);
    CHECK(ipp_val_is(&f, "V1.4"));

    /* Six operations, matching SPEC.md 3.4. Not Create-Job (0x0005) or
     * Send-Document (0x0006): advertising them made the iPhone hold every
     * job at "Waiting" (W-1040). */
    CHECK(ipp_find(buf, n, "operations-supported", &f));
    CHECK_EQ_INT(f.nvalues, 6);
    CHECK_EQ_INT(ipp_val_int(&f), IPP_OP_PRINT_JOB);
    {
        int seen_create_or_send = 0;
        const uint8_t *v = f.val;
        for (int i = 0; i < f.nvalues; i++) {
            int32_t op = (int32_t)(((uint32_t)v[0] << 24) | ((uint32_t)v[1] << 16) |
                                   ((uint32_t)v[2] << 8) | (uint32_t)v[3]);
            seen_create_or_send |= op == 0x0005 || op == 0x0006;
            v += 4 + 1 + 2 + 2;   /* this value, then the next's tag, name-len, value-len */
        }
        CHECK(!seen_create_or_send);
    }
    CHECK(!ipp_find(buf, n, "multiple-operation-time-out", &f));

    /* A printer with nothing to say sends no printer-state-message. */
    CHECK(!ipp_find(buf, n, "printer-state-message", &f));

    /* Honest margins. The tractor feed cannot reach the edge, and declaring
     * zero would make iOS lay out edge-to-edge and clip silently. */
    CHECK(ipp_find(buf, n, "media-left-margin-supported", &f));
    CHECK_EQ_INT(ipp_val_int(&f), 635);
    CHECK(ipp_find(buf, n, "media-right-margin-supported", &f));
    CHECK_EQ_INT(ipp_val_int(&f), 635);
    CHECK(ipp_find(buf, n, "media-top-margin-supported", &f));
    CHECK_EQ_INT(ipp_val_int(&f), 1270);
    CHECK(ipp_find(buf, n, "media-bottom-margin-supported", &f));
    CHECK_EQ_INT(ipp_val_int(&f), 1270);

    /* 144 across, 72 down, dpi units. This is the one pass the printer has. */
    CHECK(ipp_find(buf, n, "printer-resolution-default", &f));
    CHECK_EQ_INT(f.vlen, 9);
    CHECK_EQ_INT(f.val[3], 144);
    CHECK_EQ_INT(f.val[7], 72);
    CHECK_EQ_INT(f.val[8], IPP_RES_DPI);

    /* Monochrome only, one sided, letter. */
    CHECK(ipp_find(buf, n, "color-supported", &f));
    CHECK_EQ_INT(f.val[0], 0);
    CHECK(ipp_find(buf, n, "print-color-mode-supported", &f));
    CHECK(ipp_val_is(&f, "monochrome"));
    CHECK(ipp_find(buf, n, "sides-supported", &f));
    CHECK(ipp_val_is(&f, "one-sided"));
    CHECK(ipp_find(buf, n, "media-default", &f));
    CHECK(ipp_val_is(&f, "na_letter_8.5x11in"));

    /* The whole set has to fit the buffer the server hands it with room to
     * spare, or Get-Printer-Attributes fails on target and nowhere else.
     * Printed because it is the number RESP_MAX in ipp_server.c is chosen
     * against, and media-col is not cheap. */
    printf("    printer attributes encode to %zu bytes\n", n);
    CHECK(n < 4096);

    /* Stopped mid-job: the state, an explicit -error reason (RFC 8011
     * 5.4.12: a stopped printer MUST carry an error reason), and the words. */
    p.state         = IPP_PSTATE_STOPPED;
    p.state_reason  = "media-empty-error";
    p.state_message = "Out of paper";
    ipp_enc_init(&e, buf, sizeof buf);
    ipp_resp_begin(&e, IPP_VERSION_1_1, IPP_STATUS_OK, 1);
    ipp_write_op_attrs(&e);
    ipp_write_printer_attrs(&e, &p, NULL);
    ipp_end(&e);
    CHECK(ipp_enc_ok(&e));
    n = ipp_enc_len(&e);
    CHECK(ipp_find(buf, n, "printer-state", &f));
    CHECK_EQ_INT(ipp_val_int(&f), IPP_PSTATE_STOPPED);
    CHECK(ipp_find(buf, n, "printer-state-reasons", &f));
    CHECK(ipp_val_is(&f, "media-empty-error"));
    CHECK(ipp_find(buf, n, "printer-state-message", &f));
    CHECK_EQ_INT(f.tag, IPP_TAG_TEXT);
    CHECK(ipp_val_is(&f, "Out of paper"));

    /* Job attributes, the subset iOS polls while printing. */
    ipp_job_t j = {
        .id = 7, .uri = "ipp://aw.local:631/ipp/print/7",
        .printer_uri = p.uri, .name = "Safari", .user = "wells",
        .state = IPP_JSTATE_PROCESSING, .state_reason = "job-printing",
        .impressions_done = 0, .time_at_creation = 61,
    };
    ipp_enc_init(&e, buf, sizeof buf);
    ipp_resp_begin(&e, IPP_VERSION_1_1, IPP_STATUS_OK, 2);
    ipp_write_op_attrs(&e);
    ipp_write_job_attrs(&e, &j);
    ipp_end(&e);
    CHECK(ipp_enc_ok(&e));
    n = ipp_enc_len(&e);

    CHECK(ipp_find(buf, n, "job-id", &f));
    CHECK_EQ_INT(ipp_val_int(&f), 7);
    CHECK_EQ_INT(f.group, IPP_TAG_JOB);
    CHECK(ipp_find(buf, n, "job-state", &f));
    CHECK_EQ_INT(ipp_val_int(&f), IPP_JSTATE_PROCESSING);
    CHECK(ipp_find(buf, n, "job-state-reasons", &f));
    CHECK(ipp_val_is(&f, "job-printing"));
    CHECK(ipp_find(buf, n, "job-uri", &f));
    CHECK(ipp_val_is(&f, "ipp://aw.local:631/ipp/print/7"));

    TEST_END;
}
