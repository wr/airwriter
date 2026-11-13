// SPDX-License-Identifier: GPL-3.0-or-later
//
// The board's Personal LaserWriter 300 side on the host (W-1102): the firmware's own session,
// page stage and rip, built for this computer, printing to tools/qdlaser/qdfake.py over the
// pseudo-terminal it serves, so tools/qdlaser/test_qdinterop.py can hold the conversation to a
// printer that checks every packet. The real printer's link runs on a clock the printer drives,
// which a pty cannot carry, so packets go framed: `P`, a u16 LE length, the packet; the printer
// answers `R` and a reply byte, or `B` for a BREAK of its own.
//
//   qdhost --dev PATH [--screen NAME] [--source main|manual] [--budget N] [--log FILE]
//          PAGE.urf|PAGE.png...
//
//   --dev PATH   qdfake's pty
//   --screen     the record's screen (rip's names; atkinson by default)
//   --source     main (the cassette, the default) or manual (the manual feed slot)
//   --budget N   bytes of codes a page may take (the model's 500,000 by default)
//   --log FILE   every packet both ways, a line each: `MS > HEX` sent, `MS < HEX` heard,
//                `MS ! text` what happened
//
// One line a page: `page N: atkinson, B bytes` (the record's screen, by name), `page N: apple,
// B bytes` (Apple's 300 screen), or `page N: too detailed for the printer's memory`. Exit 0:
// every page printed. 1: the printer did not answer, is not a Personal LaserWriter 300, held
// the job, faulted, a page fit neither screen, or the job was cancelled (SIGINT: the page
// stops, the printer is reset). 2: a bad command line, device or page.
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#define _DARWIN_C_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/time.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include "png_read.h"
#include "qd_page.h"
#include "qd_rip.h"
#include "qd_session.h"
#include "ripjob.h"
#include "urf_decode.h"

#define LOG_CUT 32
#define WRITE_STUCK_MS 30000
#define RIP_FAILED (-100)

static volatile sig_atomic_t s_sigint;

typedef struct {
    int      fd;
    FILE    *log;
    uint32_t t0;
    int      failed;             // the first thing that went wrong: a session result, or RIP_FAILED
    int      page_no, too_big;
    const char *screen;
    qd_session_t *sess;
    uint8_t  raw[512];           // what came off the line, not yet parsed
    size_t   raw_n;
    uint8_t  rep[64];            // reply bytes not yet read
    size_t   rep_n;
    int      brk;                // BREAKs not yet asked for
} host_t;

static uint32_t clock_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000);
}

static void log_line(host_t *h, char arrow, const char *fmt, ...)
{
    if (!h->log) return;
    fprintf(h->log, "%8u %c ", (unsigned)(clock_ms() - h->t0), arrow);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(h->log, fmt, ap);
    va_end(ap);
    fputc('\n', h->log);
    fflush(h->log);
}

static void say(host_t *h, const char *fmt, ...)
{
    char msg[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    printf("qdhost: %s\n", msg);
    fflush(stdout);
    log_line(h, '!', "%s", msg);
}

static const char *state_name(qd_state_t st)
{
    switch (st) {
    case QDS_READY:       return "ready";
    case QDS_PRINTING:    return "printing";
    case QDS_NO_PAPER:    return "out of paper";
    case QDS_NO_CASSETTE: return "the paper cassette is out";
    case QDS_MANUAL:      return "load the manual feed slot";
    case QDS_JAM:         return "paper jam";
    case QDS_COVER:       return "the cover is open";
    case QDS_ENGINE:      return "engine fault";
    default:              return "not answering";
    }
}

static const char *why(int rc)
{
    switch (rc) {
    case QD_E_OFFLINE: return "the printer stopped answering";
    case QD_E_MODEL:   return "the printer is not a Personal LaserWriter 300";
    case QD_E_STALL:   return "the printer held the job for 10 minutes (paper, a jam or the cover), so it was given up";
    case QD_E_FAULT:   return "the printer reported a fault, or a page did not reach it whole";
    case QD_E_CANCEL:  return "cancelled";
    case QD_E_LINK:    return "what went down the line was not what was sent";
    case RIP_FAILED:   return "a page could not be read or made ready to print";
    default:           return "failed";
    }
}

// ---- the line: qdfake's framing over the pty

static int wait_read(int fd, uint32_t ms)
{
    if (fd < 0 || fd >= FD_SETSIZE) return -1;
    fd_set rd;
    FD_ZERO(&rd);
    FD_SET(fd, &rd);
    struct timeval tv = { (time_t)(ms / 1000), (suseconds_t)(ms % 1000) * 1000 };
    int r = select(fd + 1, &rd, NULL, NULL, &tv);
    if (r < 0) return errno == EINTR ? 0 : -1;
    return r;
}

// Moves what has come in into replies and BREAKs.
static void parse(host_t *h)
{
    size_t i = 0;
    while (i < h->raw_n) {
        uint8_t c = h->raw[i];
        if (c == 'B') {
            h->brk++;
            log_line(h, '<', "BREAK");
            i++;
        } else if (c == 'R') {
            if (i + 1 >= h->raw_n) break;
            if (h->rep_n < sizeof h->rep) h->rep[h->rep_n++] = h->raw[i + 1];
            log_line(h, '<', "%02x", h->raw[i + 1]);
            i += 2;
        } else {
            i++;
        }
    }
    memmove(h->raw, h->raw + i, h->raw_n - i);
    h->raw_n -= i;
}

// Reads what the line has now (waiting up to ms for something). 0, or -1 for a line gone.
static int pull(host_t *h, uint32_t ms)
{
    int r = wait_read(h->fd, ms);
    if (r <= 0) return r;
    ssize_t got = read(h->fd, h->raw + h->raw_n, sizeof h->raw - h->raw_n);
    if (got < 0 && (errno == EAGAIN || errno == EINTR)) return 0;
    if (got <= 0) return -1;
    h->raw_n += (size_t)got;
    parse(h);
    return 0;
}

static int write_all(host_t *h, const uint8_t *b, size_t n)
{
    size_t off = 0;
    uint32_t last = clock_ms();
    while (off < n) {
        ssize_t w = write(h->fd, b + off, n - off);
        if (w > 0) { off += (size_t)w; last = clock_ms(); continue; }
        if (w < 0 && errno == EINTR) continue;
        if (w < 0 && errno == EAGAIN) {
            if (clock_ms() - last > WRITE_STUCK_MS) return -1;
            pull(h, 0);                      // keep what the printer says while the pty is full
            struct timespec ts = { 0, 2 * 1000000L };
            nanosleep(&ts, NULL);
            continue;
        }
        return -1;
    }
    return 0;
}

static int op_put(void *ctx, const uint8_t *p, size_t n)
{
    host_t *h = ctx;
    if (h->log) {
        fprintf(h->log, "%8u > ", (unsigned)(clock_ms() - h->t0));
        size_t keep = n > LOG_CUT ? LOG_CUT : n;
        for (size_t i = 0; i < keep; i++) fprintf(h->log, "%02x", p[i]);
        if (keep < n) fprintf(h->log, "... (%zu bytes)", n);
        fputc('\n', h->log);
        fflush(h->log);
    }
    uint8_t head[3] = { 'P', (uint8_t)n, (uint8_t)(n >> 8) };
    return write_all(h, head, 3) || write_all(h, p, n) ? QD_E_OFFLINE : 0;
}

static int op_flush(void *ctx)
{
    (void)ctx;
    return 0;
}

static int op_read(void *ctx, uint8_t *b, size_t cap, uint32_t first_ms, uint32_t gap_ms)
{
    host_t *h = ctx;
    size_t n = 0;
    uint32_t wait = first_ms;
    while (n < cap) {
        if (h->rep_n) {
            b[n++] = h->rep[0];
            memmove(h->rep, h->rep + 1, --h->rep_n);
            wait = gap_ms;
            continue;
        }
        uint32_t t0 = clock_ms();
        if (pull(h, wait) < 0) {             // a line that is gone: no byte, and no hurry to say so
            struct timespec ts = { wait / 1000, (long)(wait % 1000) * 1000000L };
            nanosleep(&ts, NULL);
            break;
        }
        if (!h->rep_n) {
            uint32_t used = clock_ms() - t0;
            if (used >= wait) break;
            wait -= used;
        }
    }
    return (int)n;
}

static int op_brk(void *ctx)
{
    host_t *h = ctx;
    pull(h, 0);
    if (!h->brk) return 0;
    h->brk--;
    return 1;
}

static void op_drain(void *ctx)
{
    host_t *h = ctx;
    while (pull(h, 0) == 0 && h->raw_n == 0 && wait_read(h->fd, 0) > 0) {}
    h->rep_n = 0;
}

static void op_sleep(void *ctx, uint32_t ms)
{
    (void)ctx;
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L }, rest;
    while (nanosleep(&ts, &rest) < 0 && errno == EINTR && !s_sigint) ts = rest;
}

static uint32_t op_now(void *ctx)
{
    (void)ctx;
    return clock_ms();
}

static int op_cancelled(void *ctx)
{
    (void)ctx;
    return s_sigint;
}

static void op_state(void *ctx, qd_state_t st)
{
    say(ctx, "printer: %s", state_name(st));
}

static void on_sigint(int sig)
{
    (void)sig;
    if (s_sigint) _exit(1);
    s_sigint = 1;
}

static int open_line(const char *path)
{
    int fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) return -1;
    struct termios t;
    if (tcgetattr(fd, &t)) { close(fd); errno = ENOTTY; return -1; }
    cfmakeraw(&t);
    t.c_cflag |= CREAD | CLOCAL;
    t.c_cc[VMIN] = 0;
    t.c_cc[VTIME] = 0;
    if (tcsetattr(fd, TCSANOW, &t)) { close(fd); return -1; }
    tcflush(fd, TCIOFLUSH);
    return fd;
}

// ---- pages

typedef struct { const uint8_t *b; size_t n, pos; } mem_t;

static int mem_read(void *ctx, uint8_t *buf, size_t n)
{
    mem_t *m = ctx;
    if (m->n - m->pos < n) return 0;
    memcpy(buf, m->b + m->pos, n);
    m->pos += n;
    return 1;
}

static int read_file(const char *path, uint8_t **buf, size_t *n)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    size_t cap = 1 << 20, len = 0;
    uint8_t *b = malloc(cap);
    while (b) {
        size_t got = fread(b + len, 1, cap - len, f);
        len += got;
        if (len < cap) break;
        uint8_t *nb = realloc(b, cap * 2);
        if (!nb) { free(b); b = NULL; break; }
        b = nb;
        cap *= 2;
    }
    int bad = ferror(f) || !b;
    fclose(f);
    if (bad) { free(b); return -1; }
    *buf = b;
    *n = len;
    return 0;
}

// A URF, or a PNG made into one at 300 dpi. 0, or -1 with the reason said.
static int load(const char *path, uint8_t **urf, size_t *n)
{
    uint8_t *raw;
    size_t len;
    if (read_file(path, &raw, &len)) {
        fprintf(stderr, "qdhost: cannot read %s: %s\n", path, strerror(errno));
        return -1;
    }
    if (png_is_png(raw, len)) {
        const char *reason;
        int rc = png_to_urf(raw, len, 300, urf, n, &reason);
        free(raw);
        if (rc) { fprintf(stderr, "qdhost: %s: %s\n", path, reason); return -1; }
        return 0;
    }
    *urf = raw;
    *n = len;
    return 0;
}

// ---- the job

static int sink_open(void *ctx, const qd_media_t *md)
{
    host_t *h = ctx;
    if (h->failed) return h->failed;
    say(h, "page %d: %s", h->page_no + 1, md->media);
    int rc = qd_session_open(h->sess, md);
    if (rc) h->failed = rc;
    return rc;
}

static int sink_row(void *ctx, const uint8_t *codes, size_t n)
{
    host_t *h = ctx;
    int rc = qd_session_row(h->sess, codes, n);
    if (rc) h->failed = rc;
    return rc;
}

static int sink_print(void *ctx)
{
    host_t *h = ctx;
    int rc = qd_session_print(h->sess);
    if (rc) h->failed = rc;
    return rc;
}

static void sink_note(void *ctx, int page, int apple, uint32_t bytes, int too_big)
{
    host_t *h = ctx;
    (void)page;
    h->page_no++;
    if (too_big) {
        h->too_big++;
        say(h, "page %d: too detailed for the printer's memory", h->page_no);
    } else {
        say(h, "page %d: %s, %u bytes", h->page_no, apple ? "apple" : h->screen, (unsigned)bytes);
    }
}

static void rip_log(void *ctx, const char *msg)
{
    say(ctx, "rip: %s", msg);
}

static void usage(void)
{
    fprintf(stderr, "usage: qdhost --dev PATH [--screen NAME] [--source main|manual] [--budget N]\n"
                    "              [--log FILE] PAGE.urf|PAGE.png...\n");
}

int main(int argc, char **argv)
{
    const char *dev = NULL, *logpath = NULL, *screen = "atkinson", *source = "main";
    long budget = 0;
    int first_page = argc;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--dev") && i + 1 < argc)          dev = argv[++i];
        else if (!strcmp(argv[i], "--screen") && i + 1 < argc)  screen = argv[++i];
        else if (!strcmp(argv[i], "--source") && i + 1 < argc) source = argv[++i];
        else if (!strcmp(argv[i], "--budget") && i + 1 < argc)  budget = atol(argv[++i]);
        else if (!strcmp(argv[i], "--log") && i + 1 < argc)     logpath = argv[++i];
        else if (argv[i][0] == '-' && argv[i][1] == '-')        { usage(); return 2; }
        else { first_page = i; break; }
    }
    ht_algo_t algo;
    if (!dev || first_page >= argc || ht_algo_from_name(screen, &algo) || budget < 0 ||
        (strcmp(source, "main") && strcmp(source, "manual"))) {
        usage();
        return 2;
    }

    host_t h;
    memset(&h, 0, sizeof h);
    h.fd = -1;
    h.t0 = clock_ms();
    h.screen = screen;
    if (logpath && !(h.log = fopen(logpath, "w"))) {
        fprintf(stderr, "qdhost: cannot write %s: %s\n", logpath, strerror(errno));
        return 2;
    }

    int nfiles = argc - first_page, rc = 2;
    uint8_t **urf = calloc((size_t)nfiles, sizeof *urf);
    size_t *urf_n = calloc((size_t)nfiles, sizeof *urf_n);
    uint8_t *keep = malloc(QD_KEEP_CAP);
    qd_page_t *page = NULL;
    if (!urf || !urf_n || !keep) goto out;
    for (int i = 0; i < nfiles; i++)
        if (load(argv[first_page + i], &urf[i], &urf_n[i])) goto out;

    if ((h.fd = open_line(dev)) < 0) {
        fprintf(stderr, "qdhost: %s: %s\n", dev, strerror(errno));
        goto out;
    }
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_sigint;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);

    const qd_ops_t ops = { .put = op_put, .flush = op_flush, .read = op_read, .brk = op_brk,
                           .drain = op_drain, .sleep_ms = op_sleep, .now_ms = op_now,
                           .cancelled = op_cancelled, .state = op_state, .ctx = &h };
    rc = 1;
    uint8_t engine = 0, ctrl = 0;
    if (qd_ask_identity(&ops, &engine, &ctrl)) {
        say(&h, "no answer from the printer");
        goto out;
    }
    qd_model_t model = qd_model_from_engine(engine);
    if (model == QD_NONE) {
        const char *name = qd_engine_name(engine);
        say(&h, "found: %s (engine %02x), which AirWriter does not support yet",
            name ? name : "a QuickDraw LaserWriter", engine);
        goto out;
    }
    say(&h, "found: %s", qd_spec(model)->name);

    const qd_job_cfg_t jc = { .model = model, .manual = !strcmp(source, "manual") };
    if (!(h.sess = qd_session_new(&ops, &jc))) { say(&h, "out of memory"); goto out; }
    qd_page_cfg_t pc = { .model = model, .screen = algo, .keep = keep, .keep_cap = QD_KEEP_CAP,
                         .budget = (uint32_t)budget,
                         .sink = { sink_open, sink_row, sink_print, sink_note, &h } };
    if (!(page = qd_page_new(&pc))) { say(&h, "out of memory"); goto out; }
    int err = qd_session_begin(h.sess);
    if (err) {
        h.failed = err;
    } else {
        rip_stage_t stage;
        memset(&stage, 0, sizeof stage);
        ripjob_cfg_t cfg = { .log = rip_log, .log_ctx = &h };
        qd_rip_cfg(&cfg, &stage, page, &pc);
        for (int i = 0; i < nfiles && !h.failed; i++) {
            mem_t m = { urf[i], urf_n[i], 0 };
            urf_t u;
            if (urf_open(&u, mem_read, &m) != URF_OK) { h.failed = RIP_FAILED; break; }
            int r = ripjob_run(&u, &cfg, NULL);
            urf_free(&u);
            if (!h.failed && r < 0) h.failed = RIP_FAILED;
        }
        if (!h.failed) h.failed = qd_session_end(h.sess);
    }
    if (h.failed) {
        qd_session_abort(h.sess);
        say(&h, "%s", why(h.failed));
    } else if (h.too_big) {
        say(&h, "%d page%s did not fit the printer's memory", h.too_big, h.too_big == 1 ? "" : "s");
    } else {
        say(&h, "printed %d page%s", h.page_no, h.page_no == 1 ? "" : "s");
        rc = 0;
    }
out:
    qd_page_free(page);
    if (h.sess) qd_session_free(h.sess);
    for (int i = 0; urf && i < nfiles; i++) free(urf[i]);
    free(urf);
    free(urf_n);
    free(keep);
    if (h.fd >= 0) close(h.fd);
    if (h.log) fclose(h.log);
    return rc;
}
