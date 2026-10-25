// SPDX-License-Identifier: GPL-3.0-or-later
//
// The board's LaserWriter side on the host (W-1082): one EtherTalk link
// (et_link, its node) with the PAP client, exchanging Ethernet frames over
// stdin and stdout, so the firmware's own C can print to the fake LaserWriter
// in tools/ethertalk/etpeer.py before anything goes to a board. Each frame on
// the pipe is a big-endian u16 length and the frame. What happens goes to
// stderr, a line each:
//
//   found <name> <net>.<node>:<sock>
//   status <text>
//   output <line>                     (the printer's output, read back)
//   query product=<p> level=<n> binary=<0|1> page=<p> complete=<0|1>
//         start_page=on|off|unknown refused=<0|1>
//   ended ok|canceled|busy|dead|refused|dropped
//
//   lwhost lookup
//   lwhost query NAME
//   lwhost start-page NAME on|off       (W-1099: the start page job, read back as a query)
//   lwhost print NAME FILE [--cancel-after BYTES]
//   lwhost status NAME
//   ... [--dead-ms MS]                (the client's silence limit, for tests)
//
// tools/ethertalk/test_interop.py runs it against the fake.
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <time.h>
#include <unistd.h>
#include "et_link.h"
#include "pap_client.h"
#include "lw_query.h"

static et_link_t    s_link;
static pap_client_t *s_pc;
static int          s_ended = -1;
static uint8_t     *s_job;
static size_t       s_job_len, s_job_pos, s_cancel_after;
static int          s_canceled;
static lw_parse_t   s_parse;
static int          s_query;
static char         s_line[512];
static size_t       s_line_n;

static struct {
    uint8_t    name[NBP_NAME_MAX + 1];
    lt_route_t r;
    int        valid;
} s_found;
static const char *s_want;              // the NBP name asked for, or NULL for any

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static void out_frame(void *ctx, const uint8_t *f, size_t n)
{
    (void)ctx;
    uint8_t hdr[2] = { (uint8_t)(n >> 8), (uint8_t)n };
    if (fwrite(hdr, 1, 2, stdout) != 2 || fwrite(f, 1, n, stdout) != n) exit(3);
    fflush(stdout);
}

static void log_line(void *ctx, const char *line)
{
    (void)ctx;
    fprintf(stderr, "log %s\n", line);
}

static void found(void *ctx, const nbp_tuple_t *t, const lt_route_t *r)
{
    (void)ctx;
    char name[3 * NBP_NAME_MAX + 1];
    nbp_utf8(name, sizeof name, t->obj);
    fprintf(stderr, "found %s %u.%u:%u\n", name, r->addr.net, r->addr.node, r->addr.sock);
    if (s_want && strcmp(name, s_want)) return;
    if (s_found.valid) return;
    memcpy(s_found.name, t->obj, sizeof s_found.name);
    s_found.r = *r;
    s_found.valid = 1;
}

static size_t job_read(void *ctx, uint8_t *buf, size_t max, int *eof)
{
    (void)ctx;
    size_t left = s_job_len - s_job_pos;
    if (s_cancel_after && s_job_pos >= s_cancel_after) left = 0;
    size_t n = left < max ? left : max;
    if (s_cancel_after && s_job_pos + n > s_cancel_after) n = s_cancel_after - s_job_pos;
    memcpy(buf, s_job + s_job_pos, n);
    s_job_pos += n;
    *eof = s_job_pos == s_job_len;
    return n;
}

static void output(void *ctx, const uint8_t *d, size_t n)
{
    (void)ctx;
    if (s_query) lw_parse_feed(&s_parse, d, n);
    for (size_t k = 0; k < n; k++) {
        if (d[k] == '\r' || d[k] == '\n') {
            if (s_line_n) fprintf(stderr, "output %.*s\n", (int)s_line_n, s_line);
            s_line_n = 0;
        } else if (s_line_n < sizeof s_line) {
            s_line[s_line_n++] = (char)d[k];
        }
    }
}

static void status(void *ctx, const uint8_t *p)
{
    (void)ctx;
    fprintf(stderr, "status %.*s\n", p[0], (const char *)p + 1);
}

static void opened(void *ctx) { (void)ctx; fprintf(stderr, "opened\n"); }

static void ended(void *ctx, papc_end_t how)
{
    (void)ctx;
    static const char *const names[] = { "ok", "canceled", "busy", "dead", "refused", "dropped" };
    if (s_line_n) fprintf(stderr, "output %.*s\n", (int)s_line_n, s_line);
    s_line_n = 0;
    if (s_query) {
        const lw_info_t *i = &s_parse.info;
        fprintf(stderr, "query product=%s level=%u binary=%u page=%s complete=%u start_page=%s "
                "refused=%u\n", i->product, i->level, i->binary, lw_page_name(i->page), i->complete,
                i->start_page == LW_START_ON ? "on" : i->start_page == LW_START_OFF ? "off" : "unknown",
                s_parse.refused);
    }
    fprintf(stderr, "ended %s\n", names[how]);
    s_ended = (int)how;
}

// Frames in from stdin, the clock, until `done` says so or `ms` pass.
static int run(uint64_t ms, int (*done)(void))
{
    static uint8_t in[4096];
    static size_t have;
    uint64_t end = now_ms() + ms;
    while (now_ms() < end) {
        if (done && done()) return 1;
        fd_set rf;
        FD_ZERO(&rf);
        FD_SET(0, &rf);
        struct timeval tv = { 0, 10000 };
        if (select(1, &rf, NULL, NULL, &tv) > 0) {
            ssize_t n = read(0, in + have, sizeof in - have);
            if (n <= 0) return 0;                      // the fake went away
            have += (size_t)n;
            while (have >= 2) {
                size_t fl = (size_t)(in[0] << 8 | in[1]);
                if (have < 2 + fl) break;
                et_link_rx(&s_link, in + 2, fl, now_ms());
                memmove(in, in + 2 + fl, have - 2 - fl);
                have -= 2 + fl;
            }
        }
        et_link_tick(&s_link, now_ms());
        // --cancel-after: once that much has gone, as Cancel-Job would.
        if (s_cancel_after && s_job_pos >= s_cancel_after && !s_canceled) {
            s_canceled = 1;
            papc_cancel(s_pc, now_ms());
        }
    }
    return done ? done() : 1;
}

static int is_online(void) { return s_link.node.phase == LT_ONLINE; }
static int have_found(void) { return s_found.valid; }
static int is_ended(void) { return s_ended >= 0; }

static int find(void)
{
    for (int i = 0; i < 5 && !s_found.valid; i++) {
        lt_node_lookup(&s_link.node, "LaserWriter", now_ms());
        run(1000, have_found);
    }
    return s_found.valid;
}

static void usage(void)
{
    fputs("usage: lwhost lookup | query NAME | start-page NAME on|off | print NAME FILE"
          " [--cancel-after N] | status NAME [--dead-ms MS]\n", stderr);
    exit(2);
}

int main(int argc, char **argv)
{
    if (argc < 2) usage();
    const char *cmd = argv[1], *file = NULL;
    uint32_t dead_ms = 0;
    int k = 2;
    if (strcmp(cmd, "lookup")) {
        if (argc < 3) usage();
        s_want = argv[k++];
    }
    if (!strcmp(cmd, "print") || !strcmp(cmd, "start-page")) {
        if (argc < 4) usage();
        file = argv[k++];
    }
    for (; k < argc; k++) {
        if (!strcmp(argv[k], "--cancel-after") && k + 1 < argc) s_cancel_after = strtoul(argv[++k], NULL, 10);
        else if (!strcmp(argv[k], "--dead-ms") && k + 1 < argc) dead_ms = (uint32_t)strtoul(argv[++k], NULL, 10);
        else usage();
    }

    static const uint8_t mac[ETH_ALEN] = { 'B', '2', 0, 0, 0, 2 };
    et_cfg_t cfg = { .object = "lwhost", .type = "ImageWriter", .seed = (uint32_t)now_ms() | 1,
                     .idle_hold_ms = LT_IDLE_HOLD_MS };
    memcpy(cfg.mac, mac, ETH_ALEN);
    et_ops_t ops = { .send = out_frame, .log = log_line, .found = found };
    et_link_init(&s_link, &cfg, &ops, now_ms());
    lt_node_set_serve(&s_link.node, 0);                // no ImageWriter here

    s_pc = calloc(1, sizeof *s_pc);
    papc_ops_t po = { .read = job_read, .output = output, .status = status, .opened = opened,
                      .ended = ended, .log = log_line };
    papc_init(s_pc, &po, (uint32_t)now_ms() | 1);
    if (dead_ms) papc_set_dead_ms(s_pc, dead_ms);
    lt_node_attach(&s_link.node, s_pc);

    if (!run(10000, is_online)) { fprintf(stderr, "never online\n"); return 1; }
    if (!find()) { fprintf(stderr, "nothing found\n"); return 1; }
    if (!strcmp(cmd, "lookup")) return 0;

    if (!strcmp(cmd, "status")) {
        papc_status(s_pc, &s_found.r, now_ms());
        run(3000, NULL);
        return 0;
    }
    if (!strcmp(cmd, "query")) {
        static uint8_t q[4096];
        s_job_len = lw_query_job(q, sizeof q);
        s_job = q;
        s_query = 1;
        lw_parse_init(&s_parse);
    } else if (!strcmp(cmd, "start-page")) {
        static uint8_t q[1024];
        s_job_len = lw_start_page_job(q, sizeof q, !strcmp(file, "on"));
        s_job = q;
        s_query = 1;
        lw_parse_init(&s_parse);
    } else if (!strcmp(cmd, "print")) {
        FILE *f = fopen(file, "rb");
        if (!f) { perror(file); return 1; }
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        s_job = malloc((size_t)n + 1);
        s_job_len = fread(s_job, 1, (size_t)n, f);
        fclose(f);
    } else {
        usage();
    }
    if (papc_open(s_pc, &s_found.r, 600000, now_ms())) { fprintf(stderr, "cannot open\n"); return 1; }
    run(300000, is_ended);
    if (s_ended < 0) { fprintf(stderr, "ended timeout\n"); return 1; }
    return s_ended == PAPC_END_OK || (s_ended == PAPC_END_CANCELED && s_cancel_after) ? 0 : 1;
}
