// SPDX-License-Identifier: GPL-3.0-or-later
//
// The HTTP framing IPP needs, over a plain socket.
//
// esp_http_server cannot do this job: it implements chunked *responses* but
// not chunked *requests*, and a Print-Job body is chunked in practice --
// SPEC.md 3.4 says so, and CUPS confirmed it by sending Content-Length: absent
// with Transfer-Encoding: chunked. The framework reports content_len 0 and
// httpd_req_recv then returns 0 forever, so the document is unreachable. There
// is no public API to drain the bytes the header parser already buffered
// either, so the whole transport is done here instead.
//
// This file knows about HTTP and nothing about IPP.
#include "ipp_http.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include "esp_log.h"

static const char *TAG = "ipp_http";

// ------------------------------------------------------------ read buffer

void hrb_init(hrb_t *r, int fd)
{
    r->fd = fd;
    r->have = r->pos = 0;
    r->err = 0;
}

// Refills only when empty. Returns bytes available, 0 at end of stream, or -1.
static int rb_fill(hrb_t *r)
{
    if (r->pos < r->have) return (int)(r->have - r->pos);
    if (r->err) return -1;

    int n = recv(r->fd, r->buf, sizeof r->buf, 0);
    if (n == 0) return 0;
    if (n < 0) {
        ESP_LOGD(TAG, "recv: %d", errno);
        r->err = 1;
        return -1;
    }
    r->pos = 0;
    r->have = (size_t)n;
    return n;
}

// Reads one CRLF-terminated line into `out`, without the terminator. Returns
// the length, 0 for a blank line, or -1 on error or end of stream. A line
// longer than the buffer is an error rather than a silent split, because a
// split header line would parse as two wrong ones.
int hrb_line(hrb_t *r, char *out, size_t cap)
{
    size_t n = 0;
    for (;;) {
        int avail = rb_fill(r);
        if (avail <= 0) return -1;

        char c = (char)r->buf[r->pos++];
        if (c == '\n') { out[n] = '\0'; return (int)n; }
        if (c == '\r') continue;
        if (n + 1 >= cap) return -1;
        out[n++] = c;
    }
}

int hrb_read(hrb_t *r, uint8_t *out, size_t n)
{
    int avail = rb_fill(r);
    if (avail <= 0) return avail;
    size_t take = (size_t)avail < n ? (size_t)avail : n;
    memcpy(out, r->buf + r->pos, take);
    r->pos += take;
    return (int)take;
}

// ------------------------------------------------------------------ body

void hbody_init(hbody_t *b, hrb_t *r, long content_len, int chunked)
{
    b->r        = r;
    b->chunked  = chunked;
    b->left     = chunked ? 0 : (content_len > 0 ? content_len : 0);
    b->done     = (!chunked && content_len <= 0);
}

// Consumes the CRLF that terminates a chunk's data.
static int eat_crlf(hrb_t *r)
{
    char junk[4];
    return hrb_line(r, junk, sizeof junk) < 0 ? -1 : 0;
}

int hbody_read(hbody_t *b, uint8_t *out, size_t cap)
{
    if (b->done) return 0;

    if (b->chunked && b->left == 0) {
        char line[32];
        if (hrb_line(b->r, line, sizeof line) < 0) return -1;
        // The size may carry chunk extensions after a semicolon; strtol stops
        // at the first character that is not hex, which handles both forms.
        long sz = strtol(line, NULL, 16);
        if (sz < 0) return -1;
        if (sz == 0) {
            // Trailer section, then the terminating blank line.
            for (;;) {
                int n = hrb_line(b->r, line, sizeof line);
                if (n < 0) return -1;
                if (n == 0) break;
            }
            b->done = 1;
            return 0;
        }
        b->left = sz;
    }

    if (!b->chunked && b->left == 0) { b->done = 1; return 0; }

    size_t want = cap < (size_t)b->left ? cap : (size_t)b->left;
    int n = hrb_read(b->r, out, want);
    if (n <= 0) return n == 0 ? -1 : -1;    // a short body is a failed job

    b->left -= n;
    if (b->chunked && b->left == 0 && eat_crlf(b->r) < 0) return -1;
    return n;
}

int hbody_drain(hbody_t *b)
{
    uint8_t junk[256];
    for (;;) {
        int n = hbody_read(b, junk, sizeof junk);
        if (n == 0) return 0;
        if (n < 0) return -1;
    }
}

// --------------------------------------------------------------- request

int hreq_read(hrb_t *r, hreq_t *out)
{
    char line[256];

    // Skip any leading blank lines a client may leave between requests.
    int n;
    do { n = hrb_line(r, line, sizeof line); } while (n == 0);
    if (n < 0) return -1;

    memset(out, 0, sizeof *out);
    out->content_len = -1;
    out->keep_alive  = 1;          // HTTP/1.1 default

    char *sp = strchr(line, ' ');
    if (!sp) return -1;
    *sp = '\0';
    strncpy(out->method, line, sizeof out->method - 1);

    char *uri = sp + 1;
    sp = strchr(uri, ' ');
    if (sp) *sp = '\0';
    strncpy(out->uri, uri, sizeof out->uri - 1);
    if (sp && strstr(sp + 1, "1.0")) out->keep_alive = 0;

    for (;;) {
        n = hrb_line(r, line, sizeof line);
        if (n < 0) return -1;
        if (n == 0) break;                    // end of headers

        char *colon = strchr(line, ':');
        if (!colon) continue;
        *colon = '\0';
        char *val = colon + 1;
        while (*val == ' ' || *val == '\t') val++;

        if (strcasecmp(line, "Content-Length") == 0) {
            out->content_len = strtol(val, NULL, 10);
        } else if (strcasecmp(line, "Transfer-Encoding") == 0) {
            if (strcasestr(val, "chunked")) out->chunked = 1;
        } else if (strcasecmp(line, "Expect") == 0) {
            if (strcasestr(val, "100-continue")) out->expect_continue = 1;
        } else if (strcasecmp(line, "Connection") == 0) {
            if (strcasestr(val, "close")) out->keep_alive = 0;
        }
    }
    return 0;
}

// -------------------------------------------------------------- response

static int send_all(int fd, const void *buf, size_t n)
{
    const uint8_t *p = buf;
    while (n) {
        int w = send(fd, p, n, 0);
        if (w <= 0) return -1;
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

int hresp_send(int fd, int status, const char *reason, const char *ctype,
               const void *body, size_t len, int keep_alive)
{
    char hdr[192];
    int n = snprintf(hdr, sizeof hdr,
                     "HTTP/1.1 %d %s\r\n"
                     "Content-Type: %s\r\n"
                     "Content-Length: %u\r\n"
                     "Connection: %s\r\n"
                     "\r\n",
                     status, reason, ctype, (unsigned)len,
                     keep_alive ? "keep-alive" : "close");
    if (n < 0 || (size_t)n >= sizeof hdr) return -1;
    if (send_all(fd, hdr, (size_t)n) < 0) return -1;
    return len ? send_all(fd, body, len) : 0;
}

int hresp_continue(int fd)
{
    static const char msg[] = "HTTP/1.1 100 Continue\r\n\r\n";
    return send_all(fd, msg, sizeof msg - 1);
}
