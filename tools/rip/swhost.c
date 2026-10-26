// SPDX-License-Identifier: GPL-3.0-or-later
//
// The board's StyleWriter side on the host (W-1103): the firmware's own session, page stage
// and rip, built for this computer, printing to a serial device. With swfake.py the device is
// the pseudo-terminal it serves, so tools/stylewriter/test_swinterop.py can hold the
// conversation to a printer that checks every byte. With a USB serial adapter and the cable a
// Mac would use it is a real StyleWriter, and the tool is `swprint` for owners
// (tools/stylewriter/README.md): print a page, send back the log.
//
//   swhost --dev PATH [--model M | --ask] [--quality 3|4|5] [--log FILE]
//          [--dither NAME] [--dpi N] PAGE.urf|PAGE.png...
//
//   --dev PATH   the serial device (or swfake's pty). Opened raw at 57600 baud, 8N1, no flow
//                control, DTR and RTS on.
//   --ask        the model is the printer's answer to the plain question (one byte, `?`); the
//                default. The original StyleWriter does not answer it. The job that follows
//                begins with the reset, as every job does.
//   --model M    original, ii, 1200, 1500, 2200, 2400 or 2500: the printer is that model, and
//                the job begins with the reset, as it does from the board.
//   --quality    3 draft, 4 normal, 5 best (default normal).
//   --log FILE   every byte both ways, a line each: `MS > HEX` sent, `MS < HEX` heard,
//                `MS ! text` what happened. A write longer than 32 bytes is cut there, with
//                its length; what the printer says is whole.
//   --dither     the screen (rip's names; atkinson by default).
//   --dpi N      for a PNG that does not say how big a pixel is (360: one pixel a dot).
//
// Exit 0: every page printed. 1: the printer did not answer, held the job, was not the
// model, or the job was cancelled (SIGINT: the block being sent finishes, the printer is
// reset, and a second SIGINT stops at once). 2: a bad command line, device or page.
// Progress and errors go to stderr.
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#define _DARWIN_C_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/utsname.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include "png_read.h"
#include "ripjob.h"
#include "sw_page.h"
#include "sw_rip.h"
#include "sw_session.h"
#include "urf_decode.h"

#define LOG_CUT 32               // bytes of a long write kept in the log
#define WRITE_STUCK_MS 30000     // a device that takes no byte for this long has gone
#define RIP_FAILED (-100)

static volatile sig_atomic_t s_sigint;
static const char *s_prog = "swhost";      // argv[0]'s name: swprint, through the link the Makefile makes
static char s_stop_msg[96];

typedef struct {
    int      fd;
    FILE    *log;
    uint32_t t0;
    int      failed;             // the first thing that went wrong: a session result, or RIP_FAILED
    int      page_no, pages;     // the page being sent, and how many there are in all
    sw_session_t *sess;
} host_t;

static uint32_t clock_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000);
}

// ---- Log and talk

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

static void log_bytes(host_t *h, char arrow, const uint8_t *b, size_t n)
{
    if (!h->log || !n) return;
    fprintf(h->log, "%8u %c ", (unsigned)(clock_ms() - h->t0), arrow);
    size_t keep = arrow == '>' && n > LOG_CUT ? LOG_CUT : n;
    for (size_t i = 0; i < keep; i++) fprintf(h->log, "%02x", b[i]);
    if (keep < n) fprintf(h->log, "... (%zu bytes)", n);
    fputc('\n', h->log);
    fflush(h->log);
}

// A line to the person and to the log.
static void say(host_t *h, const char *fmt, ...)
{
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    fprintf(stderr, "%s: %s\n", s_prog, msg);
    log_line(h, '!', "%s", msg);
}

static const char *state_name(sw_state_t st)
{
    switch (st) {
    case SWS_READY:        return "ready";
    case SWS_PRINTING:     return "printing";
    case SWS_NO_PAPER:     return "out of paper";
    case SWS_JAM:          return "paper jam";
    case SWS_CART_MISSING: return "no cartridge";
    case SWS_CART_JAM:     return "cartridge jammed";
    case SWS_INK_OUT:      return "out of ink";
    case SWS_DOOR:         return "door open";
    case SWS_WASTE:        return "waste ink full";
    case SWS_OFFLINE:      return "not answering";
    default:               return "fault";
    }
}

static const char *why(int rc)
{
    switch (rc) {
    case SW_E_SILENT: return "printer stopped answering";
    case SW_E_MODEL:  return "the printer answers as another model than the one named";
    case SW_E_STALL:  return "the printer held the job for 10 minutes (paper, ink, a jam or the door), so it was given up";
    case SW_E_FAULT:  return "the printer reported a fault that did not clear";
    case SW_E_CANCEL: return "cancelled";
    case SW_E_CART:   return "the cartridge could not be read";
    case RIP_FAILED:  return "a page could not be read or made ready to print";
    default:          return "failed";
    }
}

// ---- The line (sw_ops_t over the device)

static int wait_fd(int fd, short events, uint32_t ms)
{
    uint32_t t0 = clock_ms();
    for (;;) {
        uint32_t used = clock_ms() - t0;
        int left = used >= ms ? 0 : (int)(ms - used);
        struct pollfd p = { fd, events, 0 };
        int r = poll(&p, 1, left);
        if (r < 0 && errno == EINTR) {
            if (!left) return 0;
            continue;
        }
        if (r < 0) return -1;
        if (!r) return 0;
        if (p.revents & events) return 1;
        return -1;                       // hung up, or an error, with nothing to read
    }
}

static int op_write(void *ctx, const uint8_t *b, size_t n)
{
    host_t *h = ctx;
    log_bytes(h, '>', b, n);
    size_t off = 0;
    uint32_t last = clock_ms();
    while (off < n) {
        ssize_t w = write(h->fd, b + off, n - off);
        if (w > 0) { off += (size_t)w; last = clock_ms(); continue; }
        if (w < 0 && errno == EINTR) continue;
        if (w < 0 && errno == EAGAIN) {
            if (clock_ms() - last > WRITE_STUCK_MS) return -1;
            if (wait_fd(h->fd, POLLOUT, 500) < 0) return -1;
            continue;
        }
        return -1;
    }
    return 0;
}

static int op_read(void *ctx, uint8_t *b, size_t cap, uint32_t first_ms, uint32_t gap_ms)
{
    host_t *h = ctx;
    size_t n = 0;
    uint32_t wait = first_ms;
    while (n < cap) {
        uint32_t t0 = clock_ms();
        int r = wait_fd(h->fd, POLLIN, wait);
        if (r < 0) {                     // a line that is gone: no byte, and no hurry to say so
            struct timespec ts = { wait / 1000, (long)(wait % 1000) * 1000000L };
            nanosleep(&ts, NULL);
            break;
        }
        if (!r) break;
        ssize_t got = read(h->fd, b + n, cap - n);
        if (got < 0 && (errno == EAGAIN || errno == EINTR)) {
            uint32_t used = clock_ms() - t0;
            wait = used < wait ? wait - used : 0;
            continue;
        }
        if (got <= 0) break;
        n += (size_t)got;
        wait = gap_ms;
    }
    log_bytes(h, '<', b, n);
    return (int)n;
}

static void op_drain(void *ctx)
{
    host_t *h = ctx;
    uint8_t junk[256];
    tcflush(h->fd, TCIFLUSH);
    ssize_t got;
    while ((got = read(h->fd, junk, sizeof junk)) > 0) log_bytes(h, '<', junk, (size_t)got);
}

static void op_sleep(void *ctx, uint32_t ms)
{
    (void)ctx;
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L }, rest;
    while (nanosleep(&ts, &rest) < 0 && errno == EINTR) ts = rest;
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

static sw_state_t s_last_state = (sw_state_t)-1;

static void op_state(void *ctx, sw_state_t st)
{
    if (st == s_last_state) return;
    s_last_state = st;
    say(ctx, "printer: %s", state_name(st));
}

static void on_sigint(int sig)
{
    (void)sig;
    if (s_sigint) {
        ssize_t ignored = write(2, s_stop_msg, strlen(s_stop_msg));
        (void)ignored;
        _exit(1);
    }
    s_sigint = 1;
}

// The device, raw at 57600 8N1 with no flow control and DTR and RTS asserted. Returns the fd,
// or -1 with errno set from what failed.
static int open_line(const char *path, const char **what)
{
    int fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) { *what = "open"; return -1; }
    struct termios t;
    if (tcgetattr(fd, &t)) { *what = "is not a serial device"; close(fd); return -1; }
    t.c_iflag = 0;
    t.c_oflag = 0;
    t.c_lflag = 0;
    t.c_cflag = (t.c_cflag & ~(tcflag_t)(CSIZE | PARENB | CSTOPB | CRTSCTS)) | CS8 | CREAD | CLOCAL;
    t.c_cc[VMIN] = 0;
    t.c_cc[VTIME] = 0;
    cfsetispeed(&t, B57600);
    cfsetospeed(&t, B57600);
    if (tcsetattr(fd, TCSANOW, &t)) { *what = "cannot be set to 57600 baud"; close(fd); return -1; }
    int bits = TIOCM_DTR | TIOCM_RTS;
    if (ioctl(fd, TIOCMBIS, &bits) && errno != ENOTTY) fprintf(stderr, "%s: could not raise DTR and RTS: %s\n", s_prog, strerror(errno));
    tcflush(fd, TCIOFLUSH);
    return fd;
}

// The last bytes are on the wire before the device is closed (bounded: a pty nobody reads
// would never empty).
static void settle_output(int fd)
{
    uint32_t t0 = clock_ms();
    int left;
    while (ioctl(fd, TIOCOUTQ, &left) == 0 && left > 0 && clock_ms() - t0 < 3000) {
        struct timespec ts = { 0, 10 * 1000000L };
        nanosleep(&ts, NULL);
    }
}

// ---- Pages

typedef struct { const uint8_t *b; size_t n, pos; } mem_t;

static int mem_read(void *ctx, uint8_t *buf, size_t n)
{
    mem_t *m = ctx;
    if (m->n - m->pos < n) return 0;
    memcpy(buf, m->b + m->pos, n);
    m->pos += n;
    return 1;
}

typedef struct {
    const char *name;
    uint8_t    *urf;                 // the file, or what the PNG decoded to
    size_t      n;
    int         pages;
} file_t;

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

// Reads a page file (a URF, or a PNG made into one), checks every page of it can be printed,
// and says whether any is in colour. 0, or -1 with the reason said.
static int load(const char *path, uint32_t dpi, file_t *f, int *rgb)
{
    uint8_t *raw;
    size_t n;
    if (read_file(path, &raw, &n)) { fprintf(stderr, "%s: cannot read %s: %s\n", s_prog, path, strerror(errno)); return -1; }
    f->name = path;
    f->urf = raw;
    f->n = n;
    if (png_is_png(raw, n)) {
        const char *reason;
        uint8_t *urf;
        size_t urf_n;
        int rc = png_to_urf(raw, n, dpi, &urf, &urf_n, &reason);
        free(raw);
        f->urf = NULL;
        if (rc) { fprintf(stderr, "%s: %s: %s\n", s_prog, path, reason); return -1; }
        f->urf = urf;
        f->n = urf_n;
    }
    mem_t m = { f->urf, f->n, 0 };
    urf_t u;
    int rc = urf_open(&u, mem_read, &m);
    if (rc != URF_OK) {
        if (rc == URF_ERR_UNSUPPORTED && u.is_pwg) fprintf(stderr, "%s: %s: PWG raster is not supported\n", s_prog, path);
        else fprintf(stderr, "%s: %s is neither a PNG nor a URF page\n", s_prog, path);
        return -1;
    }
    while ((rc = urf_next_page(&u)) == URF_OK) {
        const urf_page_t *pg = &u.page;
        int is_rgb = pg->bits_per_pixel == 24 && pg->num_colors == 3 &&
                     (pg->colorspace == 1 || pg->colorspace == 3 || pg->colorspace == 5);
        if (!is_rgb && (pg->bits_per_pixel != 8 || pg->num_colors != 1)) {
            fprintf(stderr, "%s: %s: page %d is not 8-bit gray or 24-bit RGB\n", s_prog, path, f->pages + 1);
            urf_free(&u);
            return -1;
        }
        *rgb |= is_rgb;
        f->pages++;
        const uint8_t *row;
        int r;
        while ((r = urf_next_row(&u, &row)) == URF_OK) {}
        if (r != URF_END) { rc = r; break; }
        fprintf(stderr, "%s: %s: %u x %u, %u dpi, %s\n", s_prog, path, pg->width, pg->height, pg->res_x,
                is_rgb ? "colour" : "gray");
    }
    urf_free(&u);
    if (rc != URF_END || !f->pages) {
        fprintf(stderr, "%s: %s: damaged or empty page\n", s_prog, path);
        return -1;
    }
    return 0;
}

// ---- The job

static void frame_out(void *ctx, const uint8_t *f, size_t n)
{
    host_t *h = ctx;
    if (h->failed) return;
    int rc = sw_session_feed(h->sess, f, n);
    if (rc) h->failed = rc;
}

// ripjob_cfg_t.page: each page as it starts; non-zero stops the job at a page that follows a failure.
static int on_page(void *ctx, int index, uint32_t w, uint32_t hgt)
{
    host_t *h = ctx;
    (void)index;
    if (h->failed) return -1;
    say(h, "page %d of %d (%u x %u)", ++h->page_no, h->pages, w, hgt);
    return 0;
}

static void rip_log(void *ctx, const char *msg)
{
    say(ctx, "rip: %s", msg);
}

static void usage(void)
{
    fprintf(stderr, "usage: %s --dev PATH [--model original|ii|1200|1500|2200|2400|2500 | --ask]\n"
                    "       %*s [--quality 3|4|5] [--log FILE] [--dither NAME] [--dpi N] PAGE.urf|PAGE.png...\n",
            s_prog, (int)strlen(s_prog), "");
}

int main(int argc, char **argv)
{
    const char *dev = NULL, *logpath = NULL, *model_arg = NULL, *dither = "atkinson";
    int ask = 0, quality = 0;
    if (argc > 0 && argv[0]) {
        const char *slash = strrchr(argv[0], '/');
        s_prog = slash ? slash + 1 : argv[0];
    }
    snprintf(s_stop_msg, sizeof s_stop_msg, "%s: stopped at once; the printer was not reset\n", s_prog);
    uint32_t dpi = 360;
    int first_page = argc;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--dev") && i + 1 < argc)           dev = argv[++i];
        else if (!strcmp(argv[i], "--model") && i + 1 < argc)    model_arg = argv[++i];
        else if (!strcmp(argv[i], "--ask"))                      ask = 1;
        else if (!strcmp(argv[i], "--quality") && i + 1 < argc)  quality = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--log") && i + 1 < argc)      logpath = argv[++i];
        else if (!strcmp(argv[i], "--dither") && i + 1 < argc)   dither = argv[++i];
        else if (!strcmp(argv[i], "--dpi") && i + 1 < argc)      dpi = (uint32_t)atoi(argv[++i]);
        else if (argv[i][0] == '-' && argv[i][1] == '-')         { usage(); return 2; }
        else { first_page = i; break; }
    }
    sw_model_t named = SW_NONE;
    ht_algo_t algo;
    if (!dev || first_page >= argc || (ask && model_arg) || (quality && (quality < 3 || quality > 5)) ||
        !dpi || (model_arg && sw_rip_model(model_arg, &named)) || ht_algo_from_name(dither, &algo)) {
        usage();
        return 2;
    }
    if (!model_arg) ask = 1;

    host_t h;
    memset(&h, 0, sizeof h);
    h.fd = -1;
    h.t0 = clock_ms();
    if (logpath && !(h.log = fopen(logpath, "w"))) {
        fprintf(stderr, "%s: cannot write %s: %s\n", s_prog, logpath, strerror(errno));
        return 2;
    }
    if (h.log) {
        struct utsname un;
        fprintf(h.log, "# %s", s_prog);
        for (int i = 1; i < argc; i++) fprintf(h.log, " %s", argv[i]);
        fprintf(h.log, "\n");
        if (!uname(&un)) fprintf(h.log, "# %s %s %s\n", un.sysname, un.release, un.machine);
        fprintf(h.log, "# MS > HEX is what was sent (a write over %d bytes is cut there), MS < HEX what the printer said,"
                " MS ! text what happened\n", LOG_CUT);
        fflush(h.log);
    }

    // Every page is read and checked before the printer is touched; one in colour makes the
    // job a colour job.
    int nfiles = argc - first_page, rgb = 0, rc = 2;
    file_t *files = calloc((size_t)nfiles, sizeof *files);
    if (!files) return 2;
    for (int i = 0; i < nfiles; i++) {
        if (load(argv[first_page + i], dpi, &files[i], &rgb)) goto out;
        h.pages += files[i].pages;
    }

    const char *what = NULL;
    h.fd = open_line(dev, &what);
    if (h.fd < 0) {
        fprintf(stderr, "%s: %s: %s\n", s_prog, dev, what && what[0] == 'o' ? strerror(errno) : what);
        goto out;
    }
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_sigint;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);          // no SA_RESTART: a wait ends when the signal comes

    const sw_ops_t ops = { .write = op_write, .read = op_read, .drain = op_drain, .sleep_ms = op_sleep,
                           .now_ms = op_now, .cancelled = op_cancelled, .state = op_state, .ctx = &h };
    rc = 1;
    sw_model_t model = named;
    if (ask) {
        model = sw_ask_plain(&ops);
        if (model == SW_NONE) {
            say(&h, "no answer to the plain question: a printer just reset takes 2 seconds to settle, and the original StyleWriter gives none (name it: --model original)");
            goto out;
        }
        say(&h, "found: %s", sw_spec(model)->name);
    } else {
        say(&h, "model: %s (named)", sw_spec(model)->name);
    }

    // The cartridge is read before the page stage is built, so a colour job on a black
    // cartridge prints gray through the screen and not only the K separation.
    const sw_job_cfg_t jc = { .model = model, .quality = quality, .colour_job = rgb };
    h.sess = sw_session_new(&ops, &jc);
    if (!h.sess) { say(&h, "out of memory"); goto out; }
    int err = sw_session_begin(h.sess);
    if (err) { h.failed = err; goto failed; }
    int cart = sw_session_cart(h.sess);
    say(&h, "cartridge: %s", cart ? "colour" : "black");

    sw_page_cfg_t pc = { .model = model, .colour_cart = cart, .out = frame_out, .ctx = &h };
    sw_page_t *page = sw_page_new(&pc);
    if (!page) { say(&h, "out of memory"); h.failed = RIP_FAILED; goto failed; }
    rip_stage_t stage;
    memset(&stage, 0, sizeof stage);
    ripjob_cfg_t cfg = {
        .dither = algo, .page = on_page, .page_ctx = &h, .log = rip_log, .log_ctx = &h,
    };
    sw_rip_cfg(&cfg, &stage, page, model, algo, rgb && cart, 1);

    for (int i = 0; i < nfiles && !h.failed; i++) {
        mem_t m = { files[i].urf, files[i].n, 0 };
        urf_t u;
        if (urf_open(&u, mem_read, &m) != URF_OK) { h.failed = RIP_FAILED; break; }
        ripjob_stats_t st;
        int r = ripjob_run(&u, &cfg, &st);
        urf_free(&u);
        if (!h.failed && r < 0) h.failed = RIP_FAILED;
    }
    sw_page_free(page);
    if (!h.failed) h.failed = sw_session_end(h.sess);

failed:
    if (h.failed) {
        sw_session_abort(h.sess);
        say(&h, "%s", why(h.failed));
        rc = 1;
    } else {
        say(&h, "printed %d page%s", h.pages, h.pages == 1 ? "" : "s");
        rc = 0;
    }
    settle_output(h.fd);
out:
    sw_session_free(h.sess);
    for (int i = 0; i < nfiles; i++) free(files[i].urf);
    free(files);
    if (h.fd >= 0) close(h.fd);
    if (h.log) fclose(h.log);
    return rc;
}
