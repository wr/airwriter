// SPDX-License-Identifier: GPL-3.0-or-later
#include "ipp_req.h"
#include "ipp_proto.h"
#include <string.h>

typedef struct { const uint8_t *p; size_t len, pos; } cur_t;

static int need(cur_t *c, size_t n) { return c->pos + n <= c->len; }

static uint16_t rd_u16(cur_t *c)
{
    uint16_t v = (uint16_t)((c->p[c->pos] << 8) | c->p[c->pos + 1]);
    c->pos += 2;
    return v;
}

static uint32_t rd_u32(cur_t *c)
{
    uint32_t v = ((uint32_t)c->p[c->pos] << 24) | ((uint32_t)c->p[c->pos + 1] << 16) |
                 ((uint32_t)c->p[c->pos + 2] << 8) | (uint32_t)c->p[c->pos + 3];
    c->pos += 4;
    return v;
}

// Copies a wire value into a fixed field, always NUL-terminated. Over-long
// values are truncated and flagged rather than failing the request.
static void store(ipp_req_t *r, char *dst, size_t cap,
                  const uint8_t *val, size_t n)
{
    if (n >= cap) { n = cap - 1; r->truncated = 1; }
    memcpy(dst, val, n);
    dst[n] = '\0';
}

static int name_is(const uint8_t *name, size_t nlen, const char *want)
{
    return nlen == strlen(want) && memcmp(name, want, nlen) == 0;
}

// The job-id a job-uri names: its last path segment, which is how
// ipp_job_start builds it. 0 -- never a job we issued -- if that is not a
// number.
static int32_t job_uri_id(const uint8_t *val, size_t n)
{
    size_t i = n;
    while (i > 0 && val[i - 1] != '/') i--;
    if (i == n || n - i > 9) return 0;
    int32_t id = 0;
    for (; i < n; i++) {
        if (val[i] < '0' || val[i] > '9') return 0;
        id = id * 10 + (val[i] - '0');
    }
    return id;
}

int ipp_req_parse(ipp_req_t *r, const uint8_t *buf, size_t len)
{
    memset(r, 0, sizeof *r);
    r->job_id = -1;

    cur_t c = { buf, len, 0 };
    if (!need(&c, 8)) return IPP_REQ_SHORT;

    r->version    = rd_u16(&c);
    r->op         = rd_u16(&c);
    r->request_id = rd_u32(&c);

    // The version is checked by the caller, not here: an unsupported version
    // still needs a well-formed response carrying the request id, so parsing
    // must continue far enough to have read it.

    uint8_t group = 0;
    int have_job_id = 0;   // an explicit job-id outranks a job-uri
    for (;;) {
        if (!need(&c, 1)) return IPP_REQ_SHORT;
        uint8_t tag = c.p[c.pos++];

        if (tag == IPP_TAG_END) { r->header_len = c.pos; return IPP_REQ_OK; }

        if (tag <= 0x0F) {           // delimiter: opens a new group
            group = tag;
            continue;
        }

        if (!need(&c, 2)) return IPP_REQ_SHORT;
        size_t nlen = rd_u16(&c);
        if (!need(&c, nlen)) return IPP_REQ_SHORT;
        const uint8_t *name = c.p + c.pos;
        c.pos += nlen;

        if (!need(&c, 2)) return IPP_REQ_SHORT;
        size_t vlen = rd_u16(&c);
        if (!need(&c, vlen)) return IPP_REQ_SHORT;
        const uint8_t *val = c.p + c.pos;
        c.pos += vlen;

        // nlen 0 is an additional value for the previous attribute. Every
        // field below is single-valued, so the first value wins and the rest
        // are skipped -- which the length walk above has already done.
        if (nlen == 0) continue;

        // Job attributes may legally repeat operation-attribute names with
        // different meaning, so only read from the operation group.
        if (group != IPP_TAG_OPERATION) continue;

        if (name_is(name, nlen, "printer-uri"))
            store(r, r->printer_uri, sizeof r->printer_uri, val, vlen);
        else if (name_is(name, nlen, "job-name"))
            store(r, r->job_name, sizeof r->job_name, val, vlen);
        else if (name_is(name, nlen, "requesting-user-name"))
            store(r, r->requesting_user, sizeof r->requesting_user, val, vlen);
        else if (name_is(name, nlen, "document-format"))
            store(r, r->document_format, sizeof r->document_format, val, vlen);
        else if (name_is(name, nlen, "job-id") && vlen == 4) {
            r->job_id = (int32_t)(((uint32_t)val[0] << 24) | ((uint32_t)val[1] << 16) |
                                  ((uint32_t)val[2] << 8) | (uint32_t)val[3]);
            have_job_id = 1;
        } else if (name_is(name, nlen, "job-uri") && !have_job_id)
            r->job_id = job_uri_id(val, vlen);
    }
}
