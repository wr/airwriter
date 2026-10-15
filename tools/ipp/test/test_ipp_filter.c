// SPDX-License-Identifier: GPL-3.0-or-later
//
// Get-Printer-Attributes honours requested-attributes (RFC 8011 4.2.5.1).
// macOS flags a printer that ignores it with cups-ipp-conformance-failure-report;
// the cases are the ones CUPS's get-printer-attributes-suite.test checks.
#include "test_util.h"
#include "ipp_req.h"
#include "ipp_attrs.h"
#include "ipp_walk.h"
#include <stdarg.h>

typedef struct { uint8_t b[1024]; size_t n; } bld_t;

static void u8(bld_t *m, uint8_t v) { m->b[m->n++] = v; }
static void u16(bld_t *m, uint16_t v) { u8(m, (uint8_t)(v >> 8)); u8(m, (uint8_t)v); }
static void raw(bld_t *m, const char *s) { while (*s) u8(m, (uint8_t)*s++); }

static void attr(bld_t *m, uint8_t tag, const char *name, const char *val)
{
    u8(m, tag);
    u16(m, (uint16_t)strlen(name)); raw(m, name);
    u16(m, (uint16_t)strlen(val));  raw(m, val);
}

// A Get-Printer-Attributes whose requested-attributes are the NULL-terminated
// arguments (none at all: the attribute is absent), followed by another
// operation attribute so the list's end is found by the parser, not the buffer.
static void gpa(bld_t *m, ipp_req_t *r, ...)
{
    m->n = 0;
    u16(m, 0x0200); u16(m, IPP_OP_GET_PRINTER_ATTRIBUTES);
    u16(m, 0); u16(m, 1);
    u8(m, IPP_TAG_OPERATION);
    attr(m, IPP_TAG_CHARSET, "attributes-charset", "utf-8");
    attr(m, IPP_TAG_URI, "printer-uri", "ipp://aw.local/ipp/print");
    va_list ap;
    va_start(ap, r);
    const char *first = va_arg(ap, const char *);
    if (first) {
        attr(m, IPP_TAG_KEYWORD, "requested-attributes", first);
        for (const char *v; (v = va_arg(ap, const char *)); )
            attr(m, IPP_TAG_KEYWORD, "", v);
    }
    va_end(ap);
    attr(m, IPP_TAG_NAME, "requesting-user-name", "wells");
    u8(m, IPP_TAG_END);
    int rc = ipp_req_parse(r, m->b, m->n);
    if (rc != IPP_REQ_OK) fprintf(stderr, "parse failed: %d\n", rc);
}

static const ipp_printer_t P = {
    .uri = "ipp://aw.local:631/ipp/print", .name = "ImageWriter",
    .info = "Apple ImageWriter II", .uuid = "urn:uuid:41575254-4552-0002-10bd-a36e2434",
    .adminurl = "http://aw.local/", .location = "",
    .state = IPP_PSTATE_IDLE, .state_reason = "none",
    .accepting_jobs = 1, .queued_jobs = 0, .uptime_s = 5,
};

static uint8_t out[4096];

static size_t encode(const ipp_req_t *r)
{
    ipp_enc_t e;
    ipp_enc_init(&e, out, sizeof out);
    ipp_resp_begin(&e, 0x0200, IPP_STATUS_OK, 1);
    ipp_write_op_attrs(&e);
    ipp_write_printer_attrs(&e, &P, r ? &r->requested : NULL);
    ipp_end(&e);
    CHECK(ipp_enc_ok(&e));
    return ipp_enc_len(&e);
}

static int has(size_t n, const char *name)
{
    ipp_found_t f;
    return ipp_find(out, n, name, &f);
}

// One printer-description and one job-template attribute stand for each group.
#define PD_PROBE "printer-name"
#define JT_PROBE "copies-default"

int main(void)
{
    bld_t m;
    ipp_req_t r;
    size_t n;

    /* The parser keeps the list in place and counts it. */
    gpa(&m, &r, "printer-state", "printer-state-reasons", "media-col-database", NULL);
    CHECK_EQ_INT(r.requested.n, 3);
    CHECK(ipp_names_has(&r.requested, "printer-state"));
    CHECK(ipp_names_has(&r.requested, "media-col-database"));
    CHECK(!ipp_names_has(&r.requested, "printer"));            /* no prefix match */
    CHECK(!ipp_names_has(&r.requested, "requesting-user-name")); /* list ended */
    CHECK(strcmp(r.requesting_user, "wells") == 0);

    /* No list: everything except media-col-database. */
    gpa(&m, &r, NULL);
    CHECK_EQ_INT(r.requested.n, 0);
    n = encode(&r);
    CHECK(has(n, PD_PROBE));
    CHECK(has(n, JT_PROBE));
    CHECK(has(n, "media-col-default"));
    CHECK(!has(n, "media-col-database"));
    CHECK(has(n, "urf-supported"));
    size_t n_default = n;

    /* NULL request: the same. */
    CHECK_EQ_INT(encode(NULL), n_default);

    /* 'all': the same set, still no media-col-database. */
    gpa(&m, &r, "all", NULL);
    n = encode(&r);
    CHECK_EQ_INT(n, n_default);
    CHECK(!has(n, "media-col-database"));

    /* 'all' plus the database by name. */
    gpa(&m, &r, "all", "media-col-database", NULL);
    n = encode(&r);
    CHECK(has(n, "media-col-database"));
    CHECK(has(n, PD_PROBE));
    CHECK(n > n_default);

    /* The database alone. */
    gpa(&m, &r, "media-col-database", NULL);
    n = encode(&r);
    CHECK(has(n, "media-col-database"));
    CHECK(!has(n, PD_PROBE));
    CHECK(!has(n, JT_PROBE));

    /* 'none': no printer attributes, and no empty printer group either. */
    gpa(&m, &r, "none", NULL);
    n = encode(&r);
    CHECK(!has(n, PD_PROBE));
    CHECK(!has(n, JT_PROBE));
    CHECK(!has(n, "printer-state"));
    {
        int groups = 0;
        for (size_t i = 8; i < n; ) {        /* walk tags, counting printer groups */
            uint8_t t = out[i++];
            if (t == IPP_TAG_END) break;
            if (t <= 0x0F) { groups += t == IPP_TAG_PRINTER; continue; }
            size_t nl = (size_t)((out[i] << 8) | out[i + 1]); i += 2 + nl;
            size_t vl = (size_t)((out[i] << 8) | out[i + 1]); i += 2 + vl;
        }
        CHECK_EQ_INT(groups, 0);
    }

    /* 'printer-description': state and identity, no job-template defaults. */
    gpa(&m, &r, "printer-description", NULL);
    n = encode(&r);
    CHECK(has(n, PD_PROBE));
    CHECK(has(n, "printer-state"));
    CHECK(has(n, "operations-supported"));
    CHECK(has(n, "color-supported"));
    CHECK(!has(n, JT_PROBE));
    CHECK(!has(n, "media-default"));
    CHECK(!has(n, "sides-supported"));
    CHECK(!has(n, "media-col-database"));

    /* 'job-template': the job defaults and their supported values only. */
    gpa(&m, &r, "job-template", NULL);
    n = encode(&r);
    CHECK(has(n, JT_PROBE));
    CHECK(has(n, "media-default"));
    CHECK(has(n, "print-quality-supported"));
    CHECK(has(n, "printer-resolution-default"));
    CHECK(!has(n, PD_PROBE));
    CHECK(!has(n, "printer-state"));
    CHECK(!has(n, "color-supported"));
    CHECK(!has(n, "media-col-database"));

    /* Explicit names: exactly those. This is how iOS asks. */
    gpa(&m, &r, "printer-state", "printer-state-reasons", "urf-supported", NULL);
    n = encode(&r);
    CHECK(has(n, "printer-state"));
    CHECK(has(n, "printer-state-reasons"));
    CHECK(has(n, "urf-supported"));
    CHECK(!has(n, PD_PROBE));
    CHECK(!has(n, JT_PROBE));

    /* printer-state-message is only there when there is one to send, even
     * when named. */
    gpa(&m, &r, "printer-state-message", "printer-state", NULL);
    n = encode(&r);
    CHECK(!has(n, "printer-state-message"));
    CHECK(has(n, "printer-state"));

    /* An unknown name is ignored, not an error. */
    gpa(&m, &r, "no-such-attribute", "printer-name", NULL);
    n = encode(&r);
    CHECK(has(n, PD_PROBE));
    CHECK(!has(n, JT_PROBE));

    TEST_END;
}
