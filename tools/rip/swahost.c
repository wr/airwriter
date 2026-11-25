// SPDX-License-Identifier: GPL-3.0-or-later
//
// The board's StyleWriter adapter on the host (W-1193): one EtherTalk link (et_link, its
// node) with the adapter a Mac's own StyleWriter driver prints through (sw_adapter, on the
// ADSP engine), the pipe (sw_pipe) and its pump, the port a serial device. With etpeer.py as
// the Mac and swfake.py's pty as the printer, the firmware's own C carries a whole job
// before anything goes to a board. Ethernet frames go over stdin and stdout as lwhost's do:
// a big-endian u16 length and the frame.
//
// It is built as the board is (at_adapter.c, main.c's swa_port_*): the link runs on one
// thread under a lock, and a thread made for each job, the pump, owns the port and reaches
// the link only to have the adapter end the job. One job at a time; from a job's accept to
// the end of its pump, the hold after a reset included, every request is answered busy.
//
//   swahost --name OBJECT TYPE ID DEV [--name OBJECT TYPE ID DEV]
//           [--busy-ms MS] [--hold-ms MS] [--baud N] [--stop-after BYTES]
//
//   --name        a StyleWriter to offer: its NBP object and type, its record id (1..255)
//                 and the serial device it is on (swfake's pty), opened raw at 57600 8N1.
//                 The first is on the adapter's first socket, the second on its second.
//   --busy-ms     the board prints something else for MS from the start: requests are
//                 answered busy until then.
//   --hold-ms     how far apart two resets of a printer are kept, the Mac's driver's (a
//                 job's first bytes) and the board's alike, and so how long the printer is
//                 kept after the board reset it (10000, the board's SW_RESET_GAP_MS).
//   --baud        the port takes bytes no faster than this (57600 is 5760 a second); 0, the
//                 default, as fast as the device does.
//   --stop-after  the board stops the job once BYTES of it have gone to the port, as the
//                 test button does.
//
// What happens goes to stderr, a line each:
//
//   online <net>.<node>
//   log <line>                        every log line of the link and the adapter
//   begin id=<id>                     a request took the printer
//   busy id=<id>                      one did not
//   end clean|unclean down=<n> up=<n> reached=<n> room_min=<n> stopped=<0|1> failed=<0|1>
//   reset                             the board's own 00 FF FF FF 49 went to the printer
//   free                              the pump is over: the next request is taken
//   renamed id=<id>                   a Mac's rename was applied to the offers
//
// tools/ethertalk/test_sw_adapter_interop.py runs it (make -C tools/rip swainterop).
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#define _DARWIN_C_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include "et_link.h"
#include "sw_adapter.h"
#include "sw_pipe.h"
#include "sw_session.h"

#define PUMP_CHUNK      256             // toward the port a round, as the board
#define WRITE_STUCK_MS  2000            // a device that takes no byte for this long has gone

static et_link_t       s_link;
static sw_adapter_t    s_swa;
static sw_pipe_t       s_pipe;
static pthread_mutex_t s_link_mx = PTHREAD_MUTEX_INITIALIZER;   // the link, the adapter, stdout
static pthread_mutex_t s_pipe_mx = PTHREAD_MUTEX_INITIALIZER;   // the pipe's rings

static swa_name_t s_names[SWA_NAMES];
static int        s_fd[SWA_NAMES];
static int        s_n;

static atomic_int  s_active;            // from a request taken to the pump's last line
static atomic_uint s_room_min;          // the least room the Mac was told this job
static int         s_job_fd = -1;
static int         s_job_k;             // which name's printer the job is on
static uint64_t    s_reset_ms[SWA_NAMES];   // when each printer was last reset, 0 for never: under s_pipe_mx
static uint64_t    s_busy_until;
static uint32_t    s_hold_ms = 10000, s_baud, s_stop_after;

// A Mac's rename, left by the link for the loop to apply, as the board leaves it for a task
// that may write flash.
static struct { uint8_t id, name[NBP_NAME_MAX + 1]; int set; } s_rename;

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static void sleep_ms(uint32_t ms)
{
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L }, rest;
    while (nanosleep(&ts, &rest) < 0 && errno == EINTR) ts = rest;
}

// ---- The link's side: under s_link_mx

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

static void t_send(void *ctx, const lt_route_t *to, uint8_t src_sock, const uint8_t *p, size_t n)
{
    (void)ctx;
    lt_node_send_ddp(&s_link.node, to, src_sock, DDP_ADSP, p, n);
}

static int t_busy(void *ctx)
{
    (void)ctx;
    return atomic_load(&s_active) || now_ms() < s_busy_until;
}

static void *pump_thread(void *arg);

// What is left of the hold since printer k was last reset, and that reset's time.
static uint32_t reset_wait(int k)
{
    pthread_mutex_lock(&s_pipe_mx);
    uint64_t last = s_reset_ms[k];
    pthread_mutex_unlock(&s_pipe_mx);
    return sw_reset_wait(last, now_ms(), s_hold_ms);
}

static void reset_stamp(int k)
{
    pthread_mutex_lock(&s_pipe_mx);
    s_reset_ms[k] = now_ms();
    pthread_mutex_unlock(&s_pipe_mx);
}

static void reset_hold(int k)
{
    uint32_t left;
    while ((left = reset_wait(k)) != 0) sleep_ms(left > 50 ? 50 : left);
}

static int t_begin(void *ctx, uint8_t id)
{
    int k = -1;
    for (int i = 0; i < s_n; i++) if (s_names[i].id == id) k = i;
    // Busy too while the printer's last reset is inside the hold, as main.c's claim.
    if (k < 0 || t_busy(ctx) || reset_wait(k)) {
        fprintf(stderr, "busy id=%u\n", id);
        return 1;
    }
    // The pipe before the 0: the adapter asks its room for the call-back at once.
    sw_pipe_begin(&s_pipe);
    atomic_store(&s_room_min, SW_PIPE_DOWN);
    s_job_fd = s_fd[k];
    s_job_k = k;
    atomic_store(&s_active, 1);
    pthread_t t;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&t, &at, pump_thread, NULL)) {
        atomic_store(&s_active, 0);
        fprintf(stderr, "busy id=%u\n", id);
        return 1;
    }
    fprintf(stderr, "begin id=%u\n", id);
    return 0;
}

static void t_up(void *ctx) { (void)ctx; }

static size_t t_room(void *ctx)
{
    (void)ctx;
    size_t r = atomic_load(&s_active) ? sw_pipe_room(&s_pipe) : 0;
    if (r < atomic_load(&s_room_min)) atomic_store(&s_room_min, (unsigned)r);
    return r;
}

static void t_data(void *ctx, const uint8_t *d, size_t n)
{
    (void)ctx;
    if (atomic_load(&s_active)) sw_pipe_put(&s_pipe, d, n);
}

static size_t t_output(void *ctx, uint8_t *buf, size_t max)
{
    (void)ctx;
    return atomic_load(&s_active) ? sw_pipe_output(&s_pipe, buf, max) : 0;
}

static int t_drained(void *ctx)
{
    (void)ctx;
    return !atomic_load(&s_active) || sw_pipe_drained(&s_pipe);
}

static void t_end(void *ctx, int clean)
{
    (void)ctx;
    if (atomic_load(&s_active)) sw_pipe_end(&s_pipe, clean);
}

static void t_rename(void *ctx, uint8_t id, const uint8_t *pname)
{
    (void)ctx;
    s_rename.id = id;
    memcpy(s_rename.name, pname, sizeof s_rename.name);
    s_rename.set = 1;
}

// ---- The port: the pump's

static void pipe_lock(void *ctx)   { (void)ctx; pthread_mutex_lock(&s_pipe_mx); }
static void pipe_unlock(void *ctx) { (void)ctx; pthread_mutex_unlock(&s_pipe_mx); }

static int wait_fd(int fd, int for_write, uint32_t ms)
{
    fd_set set;
    FD_ZERO(&set);
    FD_SET(fd, &set);
    struct timeval tv = { (time_t)(ms / 1000), (suseconds_t)(ms % 1000) * 1000 };
    return select(fd + 1, for_write ? NULL : &set, for_write ? &set : NULL, NULL, &tv);
}

static int port_write(void *ctx, const uint8_t *d, size_t n)
{
    int fd = *(int *)ctx;
    size_t off = 0;
    uint64_t last = now_ms();
    while (off < n) {
        ssize_t w = write(fd, d + off, n - off);
        if (w > 0) { off += (size_t)w; last = now_ms(); continue; }
        if (w < 0 && errno == EINTR) continue;
        if (w < 0 && errno == EAGAIN) {
            if (now_ms() - last > WRITE_STUCK_MS) return -1;
            wait_fd(fd, 1, 100);
            continue;
        }
        return -1;
    }
    if (s_baud) sleep_ms((uint32_t)((uint64_t)n * 10000 / s_baud));     // a byte is ten bits
    return 0;
}

static int port_flush(void *ctx) { (void)ctx; return 0; }

static int port_read(void *ctx, uint8_t *buf, size_t cap, int idle)
{
    int fd = *(int *)ctx;
    if (idle && wait_fd(fd, 0, 2) <= 0) return 0;
    if (!cap) {
        if (idle) sleep_ms(2);                  // ready, and no room for it: no spin
        return 0;
    }
    ssize_t n = read(fd, buf, cap);
    return n > 0 ? (int)n : 0;
}

static int reset_write(void *ctx, const uint8_t *b, size_t n) { return port_write(ctx, b, n); }

// Owns the port for one job, as at_adapter.c's pump and main.c's close do.
static void *pump_thread(void *arg)
{
    (void)arg;
    int fd = s_job_fd, k = s_job_k, wrote = 0;
    uint8_t buf[PUMP_CHUNK];
    const sw_pump_io_t io = { port_write, port_flush, port_read, &fd };
    tcflush(fd, TCIFLUSH);
    sw_pump_t v;
    int stopped = 0;
    for (;;) {
        if (s_stop_after && !stopped && sw_pipe_reached(&s_pipe) >= s_stop_after) {
            stopped = 1;
            sw_pipe_stop(&s_pipe);
        }
        v = sw_pipe_pump(&s_pipe, &io, buf, sizeof buf);
        // The job's first bytes are the Mac's driver resetting the printer.
        if (!wrote && sw_pipe_reached(&s_pipe)) {
            wrote = 1;
            reset_stamp(k);
        }
        if (v == SW_PUMP_STOP) {                // under the link's lock, never from its thread
            pthread_mutex_lock(&s_link_mx);
            swa_stop(&s_swa, now_ms());
            pthread_mutex_unlock(&s_link_mx);
        } else if (v != SW_PUMP_GO) {
            break;
        }
    }
    int clean = v == SW_PUMP_CLEAN && !sw_pipe_stopped(&s_pipe) && !sw_pipe_failed(&s_pipe);
    fprintf(stderr, "end %s down=%u up=%u reached=%u room_min=%u stopped=%d failed=%d\n",
            clean ? "clean" : "unclean", (unsigned)sw_pipe_down_total(&s_pipe),
            (unsigned)sw_pipe_up_total(&s_pipe), (unsigned)sw_pipe_reached(&s_pipe),
            atomic_load(&s_room_min), sw_pipe_stopped(&s_pipe), sw_pipe_failed(&s_pipe));
    // Any end but a clean one with bytes at the printer leaves it partway through a page: it
    // is reset, no sooner than the hold after the driver's own reset. And the printer is kept
    // from the next job, whose first act is a reset too, until the hold after its last.
    if (!clean && sw_pipe_reached(&s_pipe)) {
        const sw_ops_t ops = { .write = reset_write, .ctx = &fd };
        reset_hold(k);
        tcflush(fd, TCIFLUSH);
        if (sw_reset(&ops) == SW_OK) fprintf(stderr, "reset\n");
        reset_stamp(k);
    }
    reset_hold(k);
    atomic_store(&s_active, 0);
    fprintf(stderr, "free\n");
    return NULL;
}

// The device, raw at 57600 8N1 with no flow control, as swhost opens it.
static int open_line(const char *path)
{
    int fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) return -1;
    struct termios t;
    if (tcgetattr(fd, &t)) { close(fd); return -1; }
    t.c_iflag = 0;
    t.c_oflag = 0;
    t.c_lflag = 0;
    t.c_cflag = (t.c_cflag & ~(tcflag_t)(CSIZE | PARENB | CSTOPB | CRTSCTS)) | CS8 | CREAD | CLOCAL;
    t.c_cc[VMIN] = 0;
    t.c_cc[VTIME] = 0;
    cfsetispeed(&t, B57600);
    cfsetospeed(&t, B57600);
    if (tcsetattr(fd, TCSANOW, &t)) { close(fd); return -1; }
    tcflush(fd, TCIOFLUSH);
    return fd;
}

static void usage(void)
{
    fputs("usage: swahost --name OBJECT TYPE ID DEV [--name OBJECT TYPE ID DEV]\n"
          "               [--busy-ms MS] [--hold-ms MS] [--baud N] [--stop-after BYTES]\n", stderr);
    exit(2);
}

int main(int argc, char **argv)
{
    uint32_t busy_ms = 0;
    for (int k = 1; k < argc; k++) {
        if (!strcmp(argv[k], "--name") && k + 4 < argc && s_n < SWA_NAMES) {
            swa_name_t *nm = &s_names[s_n];
            nbp_name(nm->obj, argv[k + 1]);
            nbp_name(nm->type, argv[k + 2]);
            long id = strtol(argv[k + 3], NULL, 10);
            if (id < 1 || id > 255) usage();
            nm->id = (uint8_t)id;
            s_fd[s_n] = open_line(argv[k + 4]);
            if (s_fd[s_n] < 0) { perror(argv[k + 4]); return 2; }
            s_n++;
            k += 4;
        }
        else if (!strcmp(argv[k], "--busy-ms") && k + 1 < argc) busy_ms = (uint32_t)strtoul(argv[++k], NULL, 10);
        else if (!strcmp(argv[k], "--hold-ms") && k + 1 < argc) s_hold_ms = (uint32_t)strtoul(argv[++k], NULL, 10);
        else if (!strcmp(argv[k], "--baud") && k + 1 < argc) s_baud = (uint32_t)strtoul(argv[++k], NULL, 10);
        else if (!strcmp(argv[k], "--stop-after") && k + 1 < argc) s_stop_after = (uint32_t)strtoul(argv[++k], NULL, 10);
        else usage();
    }
    if (!s_n) usage();

    static const uint8_t mac[ETH_ALEN] = { 'B', '2', 0, 0, 0, 2 };
    et_cfg_t cfg = { .object = "swahost", .type = "ImageWriter", .seed = (uint32_t)now_ms() | 1,
                     .idle_hold_ms = LT_IDLE_HOLD_MS };
    memcpy(cfg.mac, mac, ETH_ALEN);
    et_ops_t ops = { .send = out_frame, .log = log_line };
    et_link_init(&s_link, &cfg, &ops, now_ms());
    lt_node_set_serve(&s_link.node, 0);                // no ImageWriter here

    sw_pipe_init(&s_pipe, pipe_lock, pipe_unlock, NULL);
    const swa_ops_t so = {
        .send = t_send, .log = log_line, .busy = t_busy, .job_begin = t_begin, .job_up = t_up,
        .job_room = t_room, .job_data = t_data, .job_output = t_output, .job_drained = t_drained,
        .job_end = t_end, .rename = t_rename,
    };
    swa_init(&s_swa, &so, (uint32_t)now_ms() | 1);
    lt_node_attach_swa(&s_link.node, &s_swa);
    swa_offer(&s_swa, s_names, s_n, now_ms());
    s_busy_until = now_ms() + busy_ms;

    static uint8_t in[8192];
    size_t have = 0;
    int online = 0;
    for (;;) {
        fd_set rf;
        FD_ZERO(&rf);
        FD_SET(0, &rf);
        struct timeval tv = { 0, 5000 };
        int r = select(1, &rf, NULL, NULL, &tv);
        if (r > 0) {
            ssize_t n = read(0, in + have, sizeof in - have);
            if (n <= 0) return 0;                      // the Mac's side went away
            have += (size_t)n;
        }
        pthread_mutex_lock(&s_link_mx);
        while (have >= 2) {
            size_t fl = (size_t)(in[0] << 8 | in[1]);
            if (fl > sizeof in - 2) return 3;
            if (have < 2 + fl) break;
            et_link_rx(&s_link, in + 2, fl, now_ms());
            memmove(in, in + 2 + fl, have - 2 - fl);
            have -= 2 + fl;
        }
        et_link_tick(&s_link, now_ms());
        if (s_rename.set) {
            s_rename.set = 0;
            for (int i = 0; i < s_n; i++) {
                if (s_names[i].id != s_rename.id) continue;
                memcpy(s_names[i].obj, s_rename.name, sizeof s_names[i].obj);
                swa_offer(&s_swa, s_names, s_n, now_ms());
                fprintf(stderr, "renamed id=%u\n", s_rename.id);
            }
        }
        if (!online && s_link.node.phase == LT_ONLINE) {
            online = 1;
            fprintf(stderr, "online %u.%u\n", s_link.node.net, s_link.node.node);
        }
        pthread_mutex_unlock(&s_link_mx);
    }
}
