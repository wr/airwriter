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

    /* An empty message is SHORT rather than a parse of uninitialised memory. */
    CHECK_EQ_INT(ipp_req_parse(&r, m.b, 0), IPP_REQ_SHORT);

    TEST_END;
}
