// SPDX-License-Identifier: GPL-3.0-or-later
//
// Minimal HTTP/1.1 framing for the IPP transport: request line, headers, a
// body reader that hides the identity/chunked distinction, and a response
// writer. Split out from ipp_server.c so the framing can be reasoned about --
// and eventually tested -- without any IPP in the way.
#pragma once
#include <stdint.h>
#include <stddef.h>

typedef struct {
    int      fd;
    uint8_t  buf[1024];
    size_t   have, pos;
    int      err;
} hrb_t;

void hrb_init(hrb_t *r, int fd);
int  hrb_line(hrb_t *r, char *out, size_t cap);
int  hrb_read(hrb_t *r, uint8_t *out, size_t n);

typedef struct {
    char method[8];
    char uri[96];
    long content_len;      // -1 when absent
    int  chunked;
    int  expect_continue;
    int  keep_alive;
} hreq_t;

int hreq_read(hrb_t *r, hreq_t *out);

// Yields body bytes in arrival order whichever framing the client chose.
// Returns the byte count, 0 at the end of the body, or -1 on a truncated or
// malformed body.
typedef struct {
    hrb_t *r;
    int    chunked;
    long   left;           // identity: bytes left; chunked: left in this chunk
    int    done;
} hbody_t;

void hbody_init(hbody_t *b, hrb_t *r, long content_len, int chunked);
int  hbody_read(hbody_t *b, uint8_t *out, size_t cap);

// Reads the body to its end and discards it. A keep-alive connection cannot
// carry another request until the previous body is consumed.
int  hbody_drain(hbody_t *b);

int hresp_send(int fd, int status, const char *reason, const char *ctype,
               const void *body, size_t len, int keep_alive);
int hresp_continue(int fd);
