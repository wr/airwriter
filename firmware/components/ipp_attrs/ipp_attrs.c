// SPDX-License-Identifier: GPL-3.0-or-later
#include "ipp_attrs.h"
#include <string.h>

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
static const char *const k_color_modes_colour[] = { "monochrome", "color" };
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
static const char *const k_urf_colour[] = {
    IPP_URF_VERSION, "CP1", "RS72-144", "W8", "SRGB24", "DM1", "IS1", "MT1", "FN3"
};

static const char *const k_job_creation[] = {
    "copies", "media", "media-col", "print-color-mode", "print-quality",
    "printer-resolution", "sides", "job-name"
};

// Print-Job only, no Create-Job / Send-Document: with those two advertised
// the iPhone held every job at "Waiting" and never submitted it, re-polling
// Get-Printer-Attributes instead (W-1040 bench, 2026-10-15). Print-Job needs
// neither: iOS and macOS find the job with Get-Jobs while it uploads.
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

int ipp_names_has(const ipp_names_t *names, const char *name)
{
    if (!names || !names->n) return 0;
    size_t want = strlen(name), pos = 0;
    while (pos + 3 <= names->len) {
        size_t nlen = (size_t)((names->p[pos + 1] << 8) | names->p[pos + 2]);
        pos += 3 + nlen;                       // tag, name-length, name
        if (pos + 2 > names->len) break;
        size_t vlen = (size_t)((names->p[pos] << 8) | names->p[pos + 1]);
        pos += 2;
        if (pos + vlen > names->len) break;
        if (vlen == want && memcmp(names->p + pos, name, want) == 0) return 1;
        pos += vlen;
    }
    return 0;
}

// Which requested-attributes group an attribute answers to (RFC 8011 5.2 and
// 5.4). EXPLICIT is sent only when named.
enum { PD, JT, EXPLICIT };

typedef struct {
    ipp_enc_t         *e;
    const ipp_names_t *req;      // NULL: no list, everything but EXPLICIT
    int                pd, jt;   // the list names 'all' or the group
    int                opened;   // printer-attributes delimiter written
} sel_t;

static int want(sel_t *s, const char *name, int group)
{
    int yes = !s->req ? group != EXPLICIT
            : ipp_names_has(s->req, name) || (group == PD && s->pd)
                                          || (group == JT && s->jt);
    if (yes && !s->opened) {
        ipp_group(s->e, IPP_TAG_PRINTER);
        s->opened = 1;
    }
    return yes;
}

void ipp_write_printer_attrs(ipp_enc_t *e, const ipp_printer_t *p,
                             const ipp_names_t *requested)
{
    sel_t s = { .e = e, .req = (requested && requested->n) ? requested : NULL };
    if (s.req) {
        int all = ipp_names_has(s.req, "all");
        s.pd = all || ipp_names_has(s.req, "printer-description");
        s.jt = all || ipp_names_has(s.req, "job-template");
    }
#define W(name, group) want(&s, name, group)

    if (W("printer-uri-supported", PD))
        ipp_attr_str(e, IPP_TAG_URI, "printer-uri-supported", p->uri);
    if (W("uri-authentication-supported", PD))
        ipp_attr_strs(e, IPP_TAG_KEYWORD, "uri-authentication-supported",
                      k_uri_auth, NELEM(k_uri_auth));
    if (W("uri-security-supported", PD))
        ipp_attr_strs(e, IPP_TAG_KEYWORD, "uri-security-supported",
                      k_uri_security, NELEM(k_uri_security));

    if (W("printer-name", PD))
        ipp_attr_str(e, IPP_TAG_NAME, "printer-name", p->name);
    if (W("printer-info", PD))
        ipp_attr_str(e, IPP_TAG_TEXT, "printer-info", p->info);
    if (W("printer-location", PD))
        ipp_attr_str(e, IPP_TAG_TEXT, "printer-location", p->location);
    if (W("printer-make-and-model", PD))
        ipp_attr_str(e, IPP_TAG_TEXT, "printer-make-and-model", "Apple ImageWriter II");
    if (W("printer-uuid", PD))
        ipp_attr_str(e, IPP_TAG_URI, "printer-uuid", p->uuid);
    if (W("printer-more-info", PD))
        ipp_attr_str(e, IPP_TAG_URI, "printer-more-info", p->adminurl);

    // 1284 device id. Some clients match on this instead of the model string.
    if (W("printer-device-id", PD))
        ipp_attr_str(e, IPP_TAG_TEXT, "printer-device-id",
                     "MFG:Apple;MDL:ImageWriter II;CMD:URF;");

    if (W("printer-state", PD))
        ipp_attr_int(e, IPP_TAG_ENUM, "printer-state", p->state);
    if (W("printer-state-reasons", PD))
        ipp_attr_str(e, IPP_TAG_KEYWORD, "printer-state-reasons", p->state_reason);
    if (p->state_message && p->state_message[0] && W("printer-state-message", PD))
        ipp_attr_str(e, IPP_TAG_TEXT, "printer-state-message", p->state_message);
    if (W("printer-is-accepting-jobs", PD))
        ipp_attr_bool(e, "printer-is-accepting-jobs", p->accepting_jobs);
    if (W("queued-job-count", PD))
        ipp_attr_int(e, IPP_TAG_INTEGER, "queued-job-count", p->queued_jobs);
    if (W("printer-up-time", PD))
        ipp_attr_int(e, IPP_TAG_INTEGER, "printer-up-time", (int32_t)p->uptime_s);

    if (W("ipp-versions-supported", PD))
        ipp_attr_strs(e, IPP_TAG_KEYWORD, "ipp-versions-supported",
                      k_ipp_versions, NELEM(k_ipp_versions));
    if (W("ipp-features-supported", PD))
        ipp_attr_strs(e, IPP_TAG_KEYWORD, "ipp-features-supported",
                      k_features, NELEM(k_features));
    if (W("operations-supported", PD))
        ipp_attr_ints(e, IPP_TAG_ENUM, "operations-supported",
                      k_operations, NELEM(k_operations));

    if (W("charset-configured", PD))
        ipp_attr_str(e, IPP_TAG_CHARSET, "charset-configured", "utf-8");
    if (W("charset-supported", PD))
        ipp_attr_strs(e, IPP_TAG_CHARSET, "charset-supported", k_charset, NELEM(k_charset));
    if (W("natural-language-configured", PD))
        ipp_attr_str(e, IPP_TAG_LANGUAGE, "natural-language-configured", "en-us");
    if (W("generated-natural-language-supported", PD))
        ipp_attr_strs(e, IPP_TAG_LANGUAGE, "generated-natural-language-supported",
                      k_language, NELEM(k_language));

    if (W("document-format-default", PD))
        ipp_attr_str(e, IPP_TAG_MIMETYPE, "document-format-default", "image/urf");
    if (W("document-format-supported", PD))
        ipp_attr_strs(e, IPP_TAG_MIMETYPE, "document-format-supported",
                      k_doc_formats, NELEM(k_doc_formats));
    if (W("compression-supported", PD))
        ipp_attr_strs(e, IPP_TAG_KEYWORD, "compression-supported",
                      k_compression, NELEM(k_compression));
    if (W("urf-supported", PD))
        ipp_attr_strs(e, IPP_TAG_KEYWORD, "urf-supported",
                      p->colour ? k_urf_colour : k_urf,
                      p->colour ? NELEM(k_urf_colour) : NELEM(k_urf));

    if (W("media-default", JT))
        ipp_attr_str(e, IPP_TAG_KEYWORD, "media-default", k_media[0]);
    if (W("media-supported", JT))
        ipp_attr_strs(e, IPP_TAG_KEYWORD, "media-supported", k_media, NELEM(k_media));
    if (W("media-ready", JT))
        ipp_attr_strs(e, IPP_TAG_KEYWORD, "media-ready", k_media, NELEM(k_media));
    if (W("media-source-supported", JT))
        ipp_attr_strs(e, IPP_TAG_KEYWORD, "media-source-supported",
                      k_media_source, NELEM(k_media_source));
    if (W("media-type-supported", JT))
        ipp_attr_strs(e, IPP_TAG_KEYWORD, "media-type-supported",
                      k_media_type, NELEM(k_media_type));

    if (W("media-col-default", JT))  write_media_col(e, "media-col-default");
    if (W("media-col-ready", JT))    write_media_col(e, "media-col-ready");
    if (W("media-col-database", EXPLICIT)) write_media_col(e, "media-col-database");

    if (W("media-size-supported", JT)) {
        ipp_col_begin(e, "media-size-supported");
        ipp_col_member(e, "x-dimension");
        ipp_addl_int(e, IPP_TAG_INTEGER, LETTER_X);
        ipp_col_member(e, "y-dimension");
        ipp_addl_int(e, IPP_TAG_INTEGER, LETTER_Y);
        ipp_col_end(e);
    }

    if (W("media-left-margin-supported", JT))
        ipp_attr_int(e, IPP_TAG_INTEGER, "media-left-margin-supported",   MARGIN_SIDE);
    if (W("media-right-margin-supported", JT))
        ipp_attr_int(e, IPP_TAG_INTEGER, "media-right-margin-supported",  MARGIN_SIDE);
    if (W("media-top-margin-supported", JT))
        ipp_attr_int(e, IPP_TAG_INTEGER, "media-top-margin-supported",    MARGIN_ENDS);
    if (W("media-bottom-margin-supported", JT))
        ipp_attr_int(e, IPP_TAG_INTEGER, "media-bottom-margin-supported", MARGIN_ENDS);

    // 144 dpi across, 72 down: one pass, bit 0 of a graphics byte is the top
    // pin of eight. SPEC.md 3.8.
    if (W("printer-resolution-default", JT))
        ipp_attr_res(e, "printer-resolution-default", 144, 72);
    if (W("printer-resolution-supported", JT))
        ipp_attr_res(e, "printer-resolution-supported", 144, 72);

    if (W("print-color-mode-default", JT))
        ipp_attr_str(e, IPP_TAG_KEYWORD, "print-color-mode-default",
                     p->colour ? "color" : "monochrome");
    if (W("print-color-mode-supported", JT))
        ipp_attr_strs(e, IPP_TAG_KEYWORD, "print-color-mode-supported",
                      p->colour ? k_color_modes_colour : k_color_modes,
                      p->colour ? NELEM(k_color_modes_colour) : NELEM(k_color_modes));
    if (W("print-quality-default", JT))
        ipp_attr_int(e, IPP_TAG_ENUM, "print-quality-default", 4);
    if (W("print-quality-supported", JT))
        ipp_attr_ints(e, IPP_TAG_ENUM, "print-quality-supported",
                      k_qualities, NELEM(k_qualities));
    if (W("sides-default", JT))
        ipp_attr_str(e, IPP_TAG_KEYWORD, "sides-default", "one-sided");
    if (W("sides-supported", JT))
        ipp_attr_strs(e, IPP_TAG_KEYWORD, "sides-supported", k_sides, NELEM(k_sides));
    if (W("output-bin-supported", JT))
        ipp_attr_strs(e, IPP_TAG_KEYWORD, "output-bin-supported",
                      k_output_bin, NELEM(k_output_bin));
    if (W("finishings-default", JT))
        ipp_attr_int(e, IPP_TAG_ENUM, "finishings-default", 3);
    if (W("finishings-supported", JT))
        ipp_attr_ints(e, IPP_TAG_ENUM, "finishings-supported",
                      k_finishings, NELEM(k_finishings));

    if (W("copies-default", JT))
        ipp_attr_int(e, IPP_TAG_INTEGER, "copies-default", 1);
    if (W("copies-supported", JT))
        ipp_attr_range(e, "copies-supported", 1, 99);
    if (W("color-supported", PD))
        ipp_attr_bool(e, "color-supported", p->colour ? 1 : 0);
    if (W("pages-per-minute", PD))
        ipp_attr_int(e, IPP_TAG_INTEGER, "pages-per-minute", 1);
    if (W("pdl-override-supported", PD))
        ipp_attr_str(e, IPP_TAG_KEYWORD, "pdl-override-supported", "attempted");
    if (W("multiple-document-jobs-supported", PD))
        ipp_attr_bool(e, "multiple-document-jobs-supported", 0);
    if (W("job-creation-attributes-supported", PD))
        ipp_attr_strs(e, IPP_TAG_KEYWORD, "job-creation-attributes-supported",
                      k_job_creation, NELEM(k_job_creation));
    if (W("which-jobs-supported", PD))
        ipp_attr_strs(e, IPP_TAG_KEYWORD, "which-jobs-supported",
                      k_which_jobs, NELEM(k_which_jobs));
#undef W
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
