// SPDX-License-Identifier: GPL-3.0-or-later
#include "ipp_attrs.h"

// Margins in hundredths of a millimetre, per SPEC.md 3.5. The tractor feed
// means this printer physically cannot reach the edges, and declaring
// borderless would make iOS lay out edge-to-edge and silently clip.
#define MARGIN_SIDE 635    // 6.35mm = 0.25in
#define MARGIN_ENDS 1270   // 12.7mm = 0.5in

static const char *const k_ipp_versions[]  = { "1.1", "2.0" };
static const char *const k_charset[]       = { "utf-8" };
static const char *const k_language[]      = { "en-us" };
static const char *const k_doc_formats[]   = { "image/urf", "application/octet-stream" };
static const char *const k_compression[]   = { "none" };
static const char *const k_media[]         = { "na_letter_8.5x11in" };
static const char *const k_media_source[]  = { "main" };
static const char *const k_media_type[]    = { "stationery" };
static const char *const k_sides[]         = { "one-sided" };
static const char *const k_color_modes[]   = { "monochrome" };
static const char *const k_output_bin[]    = { "face-up" };
static const char *const k_uri_auth[]      = { "none" };
static const char *const k_uri_security[]  = { "none" };
static const char *const k_features[]      = { "ipp-everywhere" };
static const char *const k_which_jobs[]    = { "completed", "not-completed" };

// urf-supported carries the same capabilities as the TXT record's URF key,
// split one keyword per value. AirPrint clients read this one, not the TXT.
static const char *const k_urf[] = {
    IPP_URF_VERSION, "CP1", "RS72-144", "W8", "DM1", "IS1", "MT1", "FN3"
};

static const char *const k_job_creation[] = {
    "copies", "media", "media-col", "print-color-mode", "print-quality",
    "printer-resolution", "sides", "job-name"
};

static const int32_t k_operations[] = {
    IPP_OP_PRINT_JOB, IPP_OP_VALIDATE_JOB, IPP_OP_CANCEL_JOB,
    IPP_OP_GET_JOB_ATTRIBUTES, IPP_OP_GET_JOBS, IPP_OP_GET_PRINTER_ATTRIBUTES
};

// 3 draft, 4 normal, 5 high. One pass at 144x72 is the only mode v1 has, so
// all three map to the same output for now; iOS wants the choice to exist.
static const int32_t k_qualities[] = { 3, 4, 5 };
static const int32_t k_finishings[] = { 3 };   // 'none'

#define NELEM(a) (sizeof (a) / sizeof (a)[0])

// Letter, in hundredths of a millimetre: 8.5 x 11in.
#define LETTER_X 21590
#define LETTER_Y 27940

// media-col is how IPP Everywhere carries a sheet size together with the
// margins that apply to it. iOS lays out against these, so they have to agree
// with the flat media-*-margin-supported values above.
static void write_media_col(ipp_enc_t *e, const char *name)
{
    ipp_col_begin(e, name);

    ipp_col_member(e, "media-size");
    ipp_col_begin(e, NULL);                  // nested collection, no name
    ipp_col_member(e, "x-dimension");
    ipp_addl_int(e, IPP_TAG_INTEGER, LETTER_X);
    ipp_col_member(e, "y-dimension");
    ipp_addl_int(e, IPP_TAG_INTEGER, LETTER_Y);
    ipp_col_end(e);

    ipp_col_member(e, "media-bottom-margin");
    ipp_addl_int(e, IPP_TAG_INTEGER, MARGIN_ENDS);
    ipp_col_member(e, "media-top-margin");
    ipp_addl_int(e, IPP_TAG_INTEGER, MARGIN_ENDS);
    ipp_col_member(e, "media-left-margin");
    ipp_addl_int(e, IPP_TAG_INTEGER, MARGIN_SIDE);
    ipp_col_member(e, "media-right-margin");
    ipp_addl_int(e, IPP_TAG_INTEGER, MARGIN_SIDE);

    ipp_col_member(e, "media-source");
    ipp_addl_str(e, IPP_TAG_KEYWORD, "main");
    ipp_col_member(e, "media-type");
    ipp_addl_str(e, IPP_TAG_KEYWORD, "stationery");

    ipp_col_end(e);
}

void ipp_write_op_attrs(ipp_enc_t *e)
{
    ipp_group(e, IPP_TAG_OPERATION);
    ipp_attr_str(e, IPP_TAG_CHARSET,  "attributes-charset",          "utf-8");
    ipp_attr_str(e, IPP_TAG_LANGUAGE, "attributes-natural-language", "en-us");
}

void ipp_write_printer_attrs(ipp_enc_t *e, const ipp_printer_t *p)
{
    ipp_group(e, IPP_TAG_PRINTER);

    ipp_attr_str(e, IPP_TAG_URI,  "printer-uri-supported", p->uri);
    ipp_attr_strs(e, IPP_TAG_KEYWORD, "uri-authentication-supported",
                  k_uri_auth, NELEM(k_uri_auth));
    ipp_attr_strs(e, IPP_TAG_KEYWORD, "uri-security-supported",
                  k_uri_security, NELEM(k_uri_security));

    ipp_attr_str(e, IPP_TAG_NAME, "printer-name", p->name);
    ipp_attr_str(e, IPP_TAG_TEXT, "printer-info", p->info);
    ipp_attr_str(e, IPP_TAG_TEXT, "printer-location", p->location);
    ipp_attr_str(e, IPP_TAG_TEXT, "printer-make-and-model", "Apple ImageWriter II");
    ipp_attr_str(e, IPP_TAG_URI,  "printer-uuid", p->uuid);
    ipp_attr_str(e, IPP_TAG_URI,  "printer-more-info", p->adminurl);

    // 1284 device id. Some clients match on this instead of the model string.
    ipp_attr_str(e, IPP_TAG_TEXT, "printer-device-id",
                 "MFG:Apple;MDL:ImageWriter II;CMD:URF;");

    ipp_attr_int(e, IPP_TAG_ENUM, "printer-state", p->state);
    ipp_attr_str(e, IPP_TAG_KEYWORD, "printer-state-reasons", p->state_reason);
    ipp_attr_bool(e, "printer-is-accepting-jobs", p->accepting_jobs);
    ipp_attr_int(e, IPP_TAG_INTEGER, "queued-job-count", p->queued_jobs);
    ipp_attr_int(e, IPP_TAG_INTEGER, "printer-up-time", (int32_t)p->uptime_s);

    ipp_attr_strs(e, IPP_TAG_KEYWORD, "ipp-versions-supported",
                  k_ipp_versions, NELEM(k_ipp_versions));
    ipp_attr_strs(e, IPP_TAG_KEYWORD, "ipp-features-supported",
                  k_features, NELEM(k_features));
    ipp_attr_ints(e, IPP_TAG_ENUM, "operations-supported",
                  k_operations, NELEM(k_operations));

    ipp_attr_str(e, IPP_TAG_CHARSET, "charset-configured", "utf-8");
    ipp_attr_strs(e, IPP_TAG_CHARSET, "charset-supported", k_charset, NELEM(k_charset));
    ipp_attr_str(e, IPP_TAG_LANGUAGE, "natural-language-configured", "en-us");
    ipp_attr_strs(e, IPP_TAG_LANGUAGE, "generated-natural-language-supported",
                  k_language, NELEM(k_language));

    ipp_attr_str(e, IPP_TAG_MIMETYPE, "document-format-default", "image/urf");
    ipp_attr_strs(e, IPP_TAG_MIMETYPE, "document-format-supported",
                  k_doc_formats, NELEM(k_doc_formats));
    ipp_attr_strs(e, IPP_TAG_KEYWORD, "compression-supported",
                  k_compression, NELEM(k_compression));
    ipp_attr_strs(e, IPP_TAG_KEYWORD, "urf-supported", k_urf, NELEM(k_urf));

    ipp_attr_str(e, IPP_TAG_KEYWORD, "media-default", k_media[0]);
    ipp_attr_strs(e, IPP_TAG_KEYWORD, "media-supported", k_media, NELEM(k_media));
    ipp_attr_strs(e, IPP_TAG_KEYWORD, "media-ready", k_media, NELEM(k_media));
    ipp_attr_strs(e, IPP_TAG_KEYWORD, "media-source-supported",
                  k_media_source, NELEM(k_media_source));
    ipp_attr_strs(e, IPP_TAG_KEYWORD, "media-type-supported",
                  k_media_type, NELEM(k_media_type));

    write_media_col(e, "media-col-default");
    write_media_col(e, "media-col-ready");
    write_media_col(e, "media-col-database");

    ipp_col_begin(e, "media-size-supported");
    ipp_col_member(e, "x-dimension");
    ipp_addl_int(e, IPP_TAG_INTEGER, LETTER_X);
    ipp_col_member(e, "y-dimension");
    ipp_addl_int(e, IPP_TAG_INTEGER, LETTER_Y);
    ipp_col_end(e);

    ipp_attr_int(e, IPP_TAG_INTEGER, "media-left-margin-supported",   MARGIN_SIDE);
    ipp_attr_int(e, IPP_TAG_INTEGER, "media-right-margin-supported",  MARGIN_SIDE);
    ipp_attr_int(e, IPP_TAG_INTEGER, "media-top-margin-supported",    MARGIN_ENDS);
    ipp_attr_int(e, IPP_TAG_INTEGER, "media-bottom-margin-supported", MARGIN_ENDS);

    // 144 dpi across, 72 down: one pass, bit 0 of a graphics byte is the top
    // pin of eight. SPEC.md 3.8.
    ipp_attr_res(e, "printer-resolution-default", 144, 72);
    ipp_attr_res(e, "printer-resolution-supported", 144, 72);

    ipp_attr_str(e, IPP_TAG_KEYWORD, "print-color-mode-default", "monochrome");
    ipp_attr_strs(e, IPP_TAG_KEYWORD, "print-color-mode-supported",
                  k_color_modes, NELEM(k_color_modes));
    ipp_attr_int(e, IPP_TAG_ENUM, "print-quality-default", 4);
    ipp_attr_ints(e, IPP_TAG_ENUM, "print-quality-supported",
                  k_qualities, NELEM(k_qualities));
    ipp_attr_str(e, IPP_TAG_KEYWORD, "sides-default", "one-sided");
    ipp_attr_strs(e, IPP_TAG_KEYWORD, "sides-supported", k_sides, NELEM(k_sides));
    ipp_attr_strs(e, IPP_TAG_KEYWORD, "output-bin-supported",
                  k_output_bin, NELEM(k_output_bin));
    ipp_attr_int(e, IPP_TAG_ENUM, "finishings-default", 3);
    ipp_attr_ints(e, IPP_TAG_ENUM, "finishings-supported",
                  k_finishings, NELEM(k_finishings));

    ipp_attr_int(e, IPP_TAG_INTEGER, "copies-default", 1);
    ipp_attr_range(e, "copies-supported", 1, 99);
    ipp_attr_bool(e, "color-supported", 0);
    ipp_attr_int(e, IPP_TAG_INTEGER, "pages-per-minute", 1);
    ipp_attr_str(e, IPP_TAG_KEYWORD, "pdl-override-supported", "attempted");
    ipp_attr_bool(e, "multiple-document-jobs-supported", 0);
    ipp_attr_strs(e, IPP_TAG_KEYWORD, "job-creation-attributes-supported",
                  k_job_creation, NELEM(k_job_creation));
    ipp_attr_strs(e, IPP_TAG_KEYWORD, "which-jobs-supported",
                  k_which_jobs, NELEM(k_which_jobs));
}

void ipp_write_job_attrs(ipp_enc_t *e, const ipp_job_t *j)
{
    ipp_group(e, IPP_TAG_JOB);
    ipp_attr_int(e, IPP_TAG_INTEGER, "job-id", j->id);
    ipp_attr_str(e, IPP_TAG_URI, "job-uri", j->uri);
    ipp_attr_str(e, IPP_TAG_URI, "job-printer-uri", j->printer_uri);
    ipp_attr_int(e, IPP_TAG_ENUM, "job-state", j->state);
    ipp_attr_str(e, IPP_TAG_KEYWORD, "job-state-reasons", j->state_reason);
    ipp_attr_str(e, IPP_TAG_NAME, "job-name", j->name);
    ipp_attr_str(e, IPP_TAG_NAME, "job-originating-user-name", j->user);
    ipp_attr_int(e, IPP_TAG_INTEGER, "job-impressions-completed", j->impressions_done);
    ipp_attr_int(e, IPP_TAG_INTEGER, "time-at-creation", (int32_t)j->time_at_creation);
}
