// SPDX-License-Identifier: GPL-3.0-or-later
//
// The firmware's IPP parser and attribute encoder behind a bare HTTP socket on
// the host, so ipptool's conformance suites can run against them without a
// board:
//
//   make -C tools/ipp gpa_server && tools/ipp/build/gpa_server 8631 &
//   ipptool -tI ipp://127.0.0.1:8631/ipp/print \
//       /usr/share/cups/ipptool/get-printer-attributes-suite.test
//
// With --laser it is also a shared LaserWriter II NT at /ipp/laser (W-1082),
// its attributes from the board's own lw_caps on a LaserWriter's answers.
//
// It answers Get-Printer-Attributes only; anything else gets
// server-error-operation-not-supported. One request per connection.
#include "ipp_req.h"
#include "ipp_attrs.h"
#include "lw_caps.h"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

static uint8_t req[16384], resp[8192];

static const ipp_printer_t P = {
    .uri = "ipp://127.0.0.1/ipp/print", .name = "ImageWriter",
    .info = "Apple ImageWriter II", .uuid = "urn:uuid:41575254-4552-0002-000000000000",
    .adminurl = "http://127.0.0.1/", .location = "",
    .state = IPP_PSTATE_IDLE, .state_reason = "none",
    .accepting_jobs = 1, .queued_jobs = 0, .uptime_s = 1,
};

// Reads the HTTP request into `req`; returns the body's offset and sets *len
// to the body length, or -1. Handles Content-Length and chunked bodies.
static long read_request(int fd, size_t *len)
{
    size_t have = 0;
    char *hdr_end = NULL;
    while (!hdr_end) {
        ssize_t n = recv(fd, req + have, sizeof req - 1 - have, 0);
        if (n <= 0) return -1;
        have += (size_t)n;
        req[have] = 0;
        hdr_end = strstr((char *)req, "\r\n\r\n");
    }
    size_t body = (size_t)(hdr_end - (char *)req) + 4;
    const char *cl = strcasestr((char *)req, "\r\ncontent-length:");
    int chunked = strcasestr((char *)req, "\r\ntransfer-encoding: chunked") != NULL;
    if (strcasestr((char *)req, "\r\nexpect: 100-continue"))
        send(fd, "HTTP/1.1 100 Continue\r\n\r\n", 25, 0);

    if (cl && !chunked) {
        size_t want = body + (size_t)strtoul(cl + 17, NULL, 10);
        while (have < want) {
            ssize_t n = recv(fd, req + have, sizeof req - have, 0);
            if (n <= 0) return -1;
            have += (size_t)n;
        }
        *len = want - body;
        return (long)body;
    }
    // Chunked: gather the raw stream to the terminating chunk, then decode.
    while (!memmem(req + body, have - body, "\r\n0\r\n\r\n", 7) &&
           !(have - body >= 5 && memcmp(req + body, "0\r\n\r\n", 5) == 0)) {
        ssize_t n = recv(fd, req + have, sizeof req - have, 0);
        if (n <= 0) return -1;
        have += (size_t)n;
    }
    size_t in = body, outp = body;
    for (;;) {
        size_t sz = strtoul((char *)req + in, NULL, 16);
        char *eol = strstr((char *)req + in, "\r\n");
        if (!eol) return -1;
        in = (size_t)(eol - (char *)req) + 2;
        if (!sz) break;
        memmove(req + outp, req + in, sz);
        outp += sz;
        in += sz + 2;
    }
    *len = outp - body;
    return (long)body;
}

static const char k_nt[] =
    "aw:product=LaserWriter II NT\raw:version=47.0\raw:level=1\raw:res=300 300\r"
    "aw:area=14.16 8.4 597.84 783.6\raw:probe=256 1\raw:end\r";

int main(int argc, char **argv)
{
    int port = argc > 1 ? atoi(argv[1]) : 8631;
    int laser = argc > 2 && !strcmp(argv[2], "--laser");
    lw_parse_t lp;
    lw_parse_init(&lp);
    lw_parse_feed(&lp, (const uint8_t *)k_nt, sizeof k_nt - 1);
    static lw_caps_t lc;
    lw_caps_fill(&lc, &lp.info);
    static const uint8_t mac[6] = { 0xa1, 0xb2, 0xc3, 0xd4, 0xe5, 0xf6 };
    static char uuid[46];
    lw_uuid(uuid, mac, 1, (const uint8_t *)"\x11LaserWriter II NT");
    ipp_printer_t L = P;
    L.uri = "ipp://127.0.0.1/ipp/laser";
    L.name = "LaserWriter II NT";
    L.info = "LaserWriter II NT";
    L.uuid = uuid;
    L.caps = &lc.caps;
    const char *res[IPP_PRINTERS] = { "ipp/print", laser ? IPP_RESOURCE_LASER : NULL };
    int ls = socket(AF_INET, SOCK_STREAM, 0), one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((uint16_t)port),
                             .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    if (bind(ls, (struct sockaddr *)&a, sizeof a) < 0 || listen(ls, 4) < 0) {
        perror("listen");
        return 1;
    }
    fprintf(stderr, "gpa_server on 127.0.0.1:%d\n", port);

    for (;;) {
        int fd = accept(ls, NULL, NULL);
        if (fd < 0) continue;
        size_t blen = 0;
        long at = read_request(fd, &blen);
        ipp_req_t r;
        if (at < 0 || ipp_req_parse(&r, req + at, blen) != IPP_REQ_OK) { close(fd); continue; }
        char path[96] = "/";
        sscanf((char *)req, "%*s %95s", path);
        int idx = ipp_route(path, res);

        ipp_enc_t e;
        ipp_enc_init(&e, resp, sizeof resp);
        int gpa = r.op == IPP_OP_GET_PRINTER_ATTRIBUTES && idx >= 0;
        ipp_resp_begin(&e, r.version, idx < 0 ? IPP_STATUS_ERR_NOT_FOUND
                                      : gpa ? IPP_STATUS_OK : IPP_STATUS_ERR_OP_NOT_SUPPORTED,
                       r.request_id);
        ipp_write_op_attrs(&e);
        if (gpa) ipp_write_printer_attrs(&e, idx ? &L : &P, &r.requested);
        ipp_end(&e);

        char h[160];
        int hl = snprintf(h, sizeof h, "HTTP/1.1 200 OK\r\nContent-Type: application/ipp\r\n"
                          "Content-Length: %zu\r\nConnection: close\r\n\r\n", ipp_enc_len(&e));
        send(fd, h, (size_t)hl, 0);
        send(fd, resp, ipp_enc_len(&e), 0);
        close(fd);
    }
}
