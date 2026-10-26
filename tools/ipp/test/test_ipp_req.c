// SPDX-License-Identifier: GPL-3.0-or-later
#include "test_util.h"
#include "ipp_req.h"
#include "ipp_proto.h"
#include <stdlib.h>

/* A small builder, so the test cases read as IPP rather than as hex. */
typedef struct { uint8_t b[512]; size_t n; } bld_t;

static void u8(bld_t *m, uint8_t v) { m->b[m->n++] = v; }
static void u16(bld_t *m, uint16_t v) { u8(m, (uint8_t)(v >> 8)); u8(m, (uint8_t)v); }
static void u32(bld_t *m, uint32_t v) { u16(m, (uint16_t)(v >> 16)); u16(m, (uint16_t)v); }

static void bytes(bld_t *m, const char *s)
{
    for (const char *p = s; *p; p++) u8(m, (uint8_t)*p);
}

static void hdr(bld_t *m, uint16_t op, uint32_t id)
{
    m->n = 0;
    u16(m, IPP_VERSION_1_1);
    u16(m, op);
    u32(m, id);
}

static void attr(bld_t *m, uint8_t tag, const char *name, const char *val)
{
    u8(m, tag);
    u16(m, (uint16_t)strlen(name)); bytes(m, name);
    u16(m, (uint16_t)strlen(val));  bytes(m, val);
}

static void attr_i(bld_t *m, uint8_t tag, const char *name, int32_t val)
{
    u8(m, tag);
    u16(m, (uint16_t)strlen(name)); bytes(m, name);
    u16(m, 4); u32(m, (uint32_t)val);
}

/* A value with raw bytes, for the empty-named parts of a collection. */
static void attr_raw(bld_t *m, uint8_t tag, const char *name, const void *val, size_t n)
{
    u8(m, tag);
    u16(m, (uint16_t)strlen(name)); bytes(m, name);
    u16(m, (uint16_t)n);
    for (size_t i = 0; i < n; i++) u8(m, ((const uint8_t *)val)[i]);
}

/* A collection member: its name, then its integer value under an empty name
 * (RFC 8010 3.1.6). */
static void member_i(bld_t *m, const char *member, int32_t val)
{
    attr(m, IPP_TAG_MEMBERNAME, "", member);
    attr_i(m, IPP_TAG_INTEGER, "", val);
}

int main(void)
{
    bld_t m;
    ipp_req_t r;

    /* Get-Printer-Attributes, the shape iOS sends before it will list us. */
    hdr(&m, IPP_OP_GET_PRINTER_ATTRIBUTES, 0xDEADBEEF);
    u8(&m, IPP_TAG_OPERATION);
    attr(&m, IPP_TAG_CHARSET,  "attributes-charset", "utf-8");
    attr(&m, IPP_TAG_LANGUAGE, "attributes-natural-language", "en-us");
    attr(&m, IPP_TAG_URI,      "printer-uri", "ipp://aw.local:631/ipp/print");
    attr(&m, IPP_TAG_KEYWORD,  "requested-attributes", "all");
    u8(&m, IPP_TAG_END);

    CHECK_EQ_INT(ipp_req_parse(&r, m.b, m.n), IPP_REQ_OK);
    CHECK_EQ_INT(r.version, IPP_VERSION_1_1);
    CHECK_EQ_INT(r.op, IPP_OP_GET_PRINTER_ATTRIBUTES);
    CHECK_EQ_INT(r.request_id, 0xDEADBEEF);
    CHECK(strcmp(r.printer_uri, "ipp://aw.local:631/ipp/print") == 0);
    CHECK_EQ_INT(r.job_id, -1);
    CHECK_EQ_INT(r.truncated, 0);
    CHECK_EQ_INT(r.header_len, m.n);      /* no document follows */

    /* Print-Job: attributes in two groups, then document data. header_len must
     * point at the first document byte -- everything downstream of this feeds
     * urf_decode from that offset. */
    hdr(&m, IPP_OP_PRINT_JOB, 9);
    u8(&m, IPP_TAG_OPERATION);
    attr(&m, IPP_TAG_CHARSET,  "attributes-charset", "utf-8");
    attr(&m, IPP_TAG_URI,      "printer-uri", "ipp://aw.local/ipp/print");
    attr(&m, IPP_TAG_NAME,     "job-name", "Safari");
    attr(&m, IPP_TAG_NAME,     "requesting-user-name", "wells");
    attr(&m, IPP_TAG_MIMETYPE, "document-format", "image/urf");
    u8(&m, IPP_TAG_JOB);
    attr(&m, IPP_TAG_KEYWORD,  "media", "na_letter_8.5x11in");
    attr_i(&m, IPP_TAG_INTEGER, "copies", 1);
    u8(&m, IPP_TAG_END);
    size_t body_at = m.n;
    bytes(&m, "UNIRAST");

    CHECK_EQ_INT(ipp_req_parse(&r, m.b, m.n), IPP_REQ_OK);
    CHECK_EQ_INT(r.op, IPP_OP_PRINT_JOB);
    CHECK(strcmp(r.job_name, "Safari") == 0);
    CHECK(strcmp(r.requesting_user, "wells") == 0);
    CHECK(strcmp(r.document_format, "image/urf") == 0);
    CHECK_EQ_INT(r.header_len, body_at);
    CHECK_EQ_INT(m.b[r.header_len], 'U');

    /* Every truncation of that request must report SHORT, never BAD and never
     * a bogus OK. The body arrives chunked, so a partial header is the normal
     * case, not a corrupt one. */
    for (size_t cut = 0; cut < body_at; cut++)
        CHECK_EQ_INT(ipp_req_parse(&r, m.b, cut), IPP_REQ_SHORT);

    /* job-id lives in the operation group of Cancel-Job and Get-Job-Attributes. */
    hdr(&m, IPP_OP_CANCEL_JOB, 3);
    u8(&m, IPP_TAG_OPERATION);
    attr(&m, IPP_TAG_URI, "printer-uri", "ipp://aw.local/ipp/print");
    attr_i(&m, IPP_TAG_INTEGER, "job-id", 7);
    u8(&m, IPP_TAG_END);
    CHECK_EQ_INT(ipp_req_parse(&r, m.b, m.n), IPP_REQ_OK);
    CHECK_EQ_INT(r.job_id, 7);

    /* A job may be named by job-uri instead of printer-uri + job-id
     * (RFC 8011 4.1.5); the id is the URI's last path segment. */
    hdr(&m, IPP_OP_GET_JOB_ATTRIBUTES, 5);
    u8(&m, IPP_TAG_OPERATION);
    attr(&m, IPP_TAG_URI, "job-uri", "ipp://aw.local:631/ipp/print/42");
    u8(&m, IPP_TAG_END);
    CHECK_EQ_INT(ipp_req_parse(&r, m.b, m.n), IPP_REQ_OK);
    CHECK_EQ_INT(r.job_id, 42);

    /* An explicit job-id wins over a job-uri, whichever comes first. */
    hdr(&m, IPP_OP_GET_JOB_ATTRIBUTES, 6);
    u8(&m, IPP_TAG_OPERATION);
    attr(&m, IPP_TAG_URI, "job-uri", "ipp://aw.local:631/ipp/print/42");
    attr_i(&m, IPP_TAG_INTEGER, "job-id", 43);
    u8(&m, IPP_TAG_END);
    CHECK_EQ_INT(ipp_req_parse(&r, m.b, m.n), IPP_REQ_OK);
    CHECK_EQ_INT(r.job_id, 43);

    /* A job-uri with no number on the end names no job we have. */
    hdr(&m, IPP_OP_GET_JOB_ATTRIBUTES, 7);
    u8(&m, IPP_TAG_OPERATION);
    attr(&m, IPP_TAG_URI, "job-uri", "ipp://aw.local:631/ipp/print/");
    u8(&m, IPP_TAG_END);
    CHECK_EQ_INT(ipp_req_parse(&r, m.b, m.n), IPP_REQ_OK);
    CHECK_EQ_INT(r.job_id, 0);

    /* A job-name longer than the field is truncated and flagged, not rejected:
     * an over-long name is not a reason to refuse to print. */
    char longname[IPP_NAME_MAX + 40];
    memset(longname, 'x', sizeof longname - 1);
    longname[sizeof longname - 1] = '\0';
    hdr(&m, IPP_OP_PRINT_JOB, 1);
    u8(&m, IPP_TAG_OPERATION);
    attr(&m, IPP_TAG_NAME, "job-name", longname);
    u8(&m, IPP_TAG_END);
    CHECK_EQ_INT(ipp_req_parse(&r, m.b, m.n), IPP_REQ_OK);
    CHECK_EQ_INT(r.truncated, 1);
    CHECK_EQ_INT(strlen(r.job_name), IPP_NAME_MAX - 1);

    /* An attribute name that also exists in the job group must not overwrite
     * the operation-group value. iOS sends "media" in both on some jobs. */
    hdr(&m, IPP_OP_PRINT_JOB, 1);
    u8(&m, IPP_TAG_OPERATION);
    attr(&m, IPP_TAG_NAME, "job-name", "real");
    u8(&m, IPP_TAG_JOB);
    attr(&m, IPP_TAG_NAME, "job-name", "decoy");
    u8(&m, IPP_TAG_END);
    CHECK_EQ_INT(ipp_req_parse(&r, m.b, m.n), IPP_REQ_OK);
    CHECK(strcmp(r.job_name, "real") == 0);

    /* A multi-value attribute must be walked, not stumbled over: the second
     * value carries an empty name and would otherwise look like a new one. */
    hdr(&m, IPP_OP_GET_PRINTER_ATTRIBUTES, 1);
    u8(&m, IPP_TAG_OPERATION);
    attr(&m, IPP_TAG_KEYWORD, "requested-attributes", "printer-name");
    attr(&m, IPP_TAG_KEYWORD, "", "media-supported");
    attr(&m, IPP_TAG_KEYWORD, "", "urf-supported");
    attr(&m, IPP_TAG_NAME, "job-name", "after");
    u8(&m, IPP_TAG_END);
    CHECK_EQ_INT(ipp_req_parse(&r, m.b, m.n), IPP_REQ_OK);
    CHECK(strcmp(r.job_name, "after") == 0);

    /* A length that runs past the buffer is SHORT, not a read past the end.
     * Claim 300 bytes of value in a 40-byte message. */
    hdr(&m, IPP_OP_PRINT_JOB, 1);
    u8(&m, IPP_TAG_OPERATION);
    u8(&m, IPP_TAG_NAME);
    u16(&m, 8); bytes(&m, "job-name");
    u16(&m, 300); bytes(&m, "short");
    CHECK_EQ_INT(ipp_req_parse(&r, m.b, m.n), IPP_REQ_SHORT);


    /* W-1103: print-quality comes from the Job group, enum 3 draft, 4 normal,
     * 5 high; 0 when the client sent none. */
    for (int q = 3; q <= 5; q++) {
        hdr(&m, IPP_OP_PRINT_JOB, 20);
        u8(&m, IPP_TAG_OPERATION);
        attr(&m, IPP_TAG_CHARSET,  "attributes-charset", "utf-8");
        attr(&m, IPP_TAG_URI,      "printer-uri", "ipp://aw.local/ipp/print");
        attr(&m, IPP_TAG_NAME,     "job-name", "Photo");
        attr(&m, IPP_TAG_MIMETYPE, "document-format", "image/urf");
        u8(&m, IPP_TAG_JOB);
        attr_i(&m, IPP_TAG_ENUM, "print-quality", q);
        u8(&m, IPP_TAG_END);
        CHECK_EQ_INT(ipp_req_parse(&r, m.b, m.n), IPP_REQ_OK);
        CHECK_EQ_INT(r.opts.quality, q);
        CHECK(strcmp(r.job_name, "Photo") == 0);
        CHECK(strcmp(r.document_format, "image/urf") == 0);
    }

    /* None sent: 0, as for every request before it. */
    hdr(&m, IPP_OP_PRINT_JOB, 21);
    u8(&m, IPP_TAG_OPERATION);
    attr(&m, IPP_TAG_NAME, "job-name", "Plain");
    u8(&m, IPP_TAG_JOB);
    attr_i(&m, IPP_TAG_INTEGER, "copies", 1);
    u8(&m, IPP_TAG_END);
    CHECK_EQ_INT(ipp_req_parse(&r, m.b, m.n), IPP_REQ_OK);
    CHECK_EQ_INT(r.opts.quality, 0);
    hdr(&m, IPP_OP_GET_PRINTER_ATTRIBUTES, 22);
    u8(&m, IPP_TAG_OPERATION);
    attr(&m, IPP_TAG_KEYWORD, "requested-attributes", "all");
    u8(&m, IPP_TAG_END);
    CHECK_EQ_INT(ipp_req_parse(&r, m.b, m.n), IPP_REQ_OK);
    CHECK_EQ_INT(r.opts.quality, 0);

    /* What iOS sends: a Job group of unknown attributes and media-col, a
     * collection with a collection inside it, and print-quality after them.
     * The members carry empty names and must be walked, not read as
     * attributes of their own; one of them is called print-quality and is not
     * the job's. The Operation group's values still land where they did. */
    {
        static const uint8_t four[4] = { 0, 0, 0, 4 };
        hdr(&m, IPP_OP_PRINT_JOB, 23);
        u8(&m, IPP_TAG_OPERATION);
        attr(&m, IPP_TAG_CHARSET,  "attributes-charset", "utf-8");
        attr(&m, IPP_TAG_URI,      "printer-uri", "ipp://aw.local/ipp/print");
        attr(&m, IPP_TAG_NAME,     "job-name", "Safari");
        attr(&m, IPP_TAG_NAME,     "requesting-user-name", "wells");
        attr(&m, IPP_TAG_MIMETYPE, "document-format", "image/urf");
        u8(&m, IPP_TAG_JOB);
        attr(&m, IPP_TAG_KEYWORD,  "sides", "one-sided");
        attr_raw(&m, IPP_TAG_BEGIN_COLLECTION, "media-col", NULL, 0);
        attr(&m, IPP_TAG_MEMBERNAME, "", "media-size");
        attr_raw(&m, IPP_TAG_BEGIN_COLLECTION, "", NULL, 0);
        member_i(&m, "x-dimension", 21590);
        member_i(&m, "y-dimension", 27940);
        attr_raw(&m, IPP_TAG_END_COLLECTION, "", NULL, 0);
        member_i(&m, "media-top-margin", 100);
        attr(&m, IPP_TAG_MEMBERNAME, "", "print-quality");
        attr_raw(&m, IPP_TAG_ENUM, "", four, 4);
        attr(&m, IPP_TAG_MEMBERNAME, "", "media-type");
        attr(&m, IPP_TAG_KEYWORD, "", "stationery");
        attr_raw(&m, IPP_TAG_END_COLLECTION, "", NULL, 0);
        attr(&m, IPP_TAG_KEYWORD,  "media", "na_letter_8.5x11in");
        attr(&m, IPP_TAG_KEYWORD,  "x-made-up", "one");
        attr(&m, IPP_TAG_KEYWORD,  "", "two");
        attr_i(&m, IPP_TAG_ENUM, "print-quality", 5);
        attr_i(&m, IPP_TAG_INTEGER, "copies", 1);
        u8(&m, IPP_TAG_END);
        size_t doc_at = m.n;
        bytes(&m, "UNIRAST");
        CHECK_EQ_INT(ipp_req_parse(&r, m.b, m.n), IPP_REQ_OK);
        CHECK_EQ_INT(r.opts.quality, 5);
        CHECK(strcmp(r.job_name, "Safari") == 0);
        CHECK(strcmp(r.requesting_user, "wells") == 0);
        CHECK(strcmp(r.document_format, "image/urf") == 0);
        CHECK(strcmp(r.printer_uri, "ipp://aw.local/ipp/print") == 0);
        CHECK_EQ_INT(r.header_len, doc_at);
        for (size_t cut = 0; cut < doc_at; cut++)
            CHECK_EQ_INT(ipp_req_parse(&r, m.b, cut), IPP_REQ_SHORT);

        /* The same request without the job's own print-quality: the member
         * that shares its name does not stand in for it. */
        hdr(&m, IPP_OP_PRINT_JOB, 24);
        u8(&m, IPP_TAG_OPERATION);
        attr(&m, IPP_TAG_NAME, "job-name", "Safari");
        u8(&m, IPP_TAG_JOB);
        attr_raw(&m, IPP_TAG_BEGIN_COLLECTION, "media-col", NULL, 0);
        attr(&m, IPP_TAG_MEMBERNAME, "", "print-quality");
        attr_raw(&m, IPP_TAG_ENUM, "", four, 4);
        attr_raw(&m, IPP_TAG_END_COLLECTION, "", NULL, 0);
        u8(&m, IPP_TAG_END);
        CHECK_EQ_INT(ipp_req_parse(&r, m.b, m.n), IPP_REQ_OK);
        CHECK_EQ_INT(r.opts.quality, 0);
        CHECK(strcmp(r.job_name, "Safari") == 0);
    }

    /* Only the Job group's: print-quality among the operation attributes, or
     * after a Printer group, is not the job's. */
    hdr(&m, IPP_OP_PRINT_JOB, 25);
    u8(&m, IPP_TAG_OPERATION);
    attr_i(&m, IPP_TAG_ENUM, "print-quality", 3);
    attr(&m, IPP_TAG_NAME, "job-name", "Op");
    u8(&m, IPP_TAG_PRINTER);
    attr_i(&m, IPP_TAG_ENUM, "print-quality", 5);
    u8(&m, IPP_TAG_END);
    CHECK_EQ_INT(ipp_req_parse(&r, m.b, m.n), IPP_REQ_OK);
    CHECK_EQ_INT(r.opts.quality, 0);
    CHECK(strcmp(r.job_name, "Op") == 0);

    /* A value the printer has no meaning for, or the wrong size or type, is
     * as good as none; the first value of a list is the one read. */
    hdr(&m, IPP_OP_PRINT_JOB, 26);
    u8(&m, IPP_TAG_JOB);
    attr_i(&m, IPP_TAG_ENUM, "print-quality", 7);
    u8(&m, IPP_TAG_END);
    CHECK_EQ_INT(ipp_req_parse(&r, m.b, m.n), IPP_REQ_OK);
    CHECK_EQ_INT(r.opts.quality, 0);
    hdr(&m, IPP_OP_PRINT_JOB, 27);
    u8(&m, IPP_TAG_JOB);
    attr(&m, IPP_TAG_KEYWORD, "print-quality", "high");
    u8(&m, IPP_TAG_END);
    CHECK_EQ_INT(ipp_req_parse(&r, m.b, m.n), IPP_REQ_OK);
    CHECK_EQ_INT(r.opts.quality, 0);
    hdr(&m, IPP_OP_PRINT_JOB, 28);
    u8(&m, IPP_TAG_JOB);
    attr_i(&m, IPP_TAG_ENUM, "print-quality", 3);
    attr_i(&m, IPP_TAG_ENUM, "", 5);
    u8(&m, IPP_TAG_END);
    CHECK_EQ_INT(ipp_req_parse(&r, m.b, m.n), IPP_REQ_OK);
    CHECK_EQ_INT(r.opts.quality, 3);

    /* An empty message is SHORT rather than a parse of uninitialised memory. */
    CHECK_EQ_INT(ipp_req_parse(&r, m.b, 0), IPP_REQ_SHORT);

    TEST_END;
}
