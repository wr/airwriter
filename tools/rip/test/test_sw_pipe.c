// SPDX-License-Identifier: GPL-3.0-or-later
//
// The pipe between a Mac's StyleWriter driver and the port (W-1193, sw_pipe.h):
// the two rings, the job's end, the capture, and the pump's round against a
// scripted port.
#include "sw_pipe.h"
#include "test_util.h"
#include <stdlib.h>

static sw_pipe_t P;

// The lock hooks: counted, and never taken twice or held across the port.
static int g_locked, g_locks, g_bad_lock;
static void lk(void *ctx)  { (void)ctx; if (g_locked) g_bad_lock++; g_locked = 1; g_locks++; }
static void ulk(void *ctx) { (void)ctx; if (!g_locked) g_bad_lock++; g_locked = 0; }

// ---- A scripted port
static struct {
    uint8_t  out[40000];        // what was written, in order
    size_t   n_out;
    int      n_flush;
    size_t   last_flush_out;
    int      fail_write, fail_flush;
    uint8_t  in[2048];          // what the printer will say
    size_t   n_in, in_pos;
    int      reads, idle_reads, max_read_cap, zero_cap_reads;
    int      drained_in_write;  // sw_pipe_drained while a write was in flight
} F;

static int f_write(void *ctx, const uint8_t *d, size_t n)
{
    (void)ctx;
    if (g_locked) g_bad_lock++;
    if (sw_pipe_drained(&P)) F.drained_in_write++;
    if (F.fail_write) return -1;
    memcpy(F.out + F.n_out, d, n);
    F.n_out += n;
    return 0;
}

static int f_flush(void *ctx)
{
    (void)ctx;
    if (g_locked) g_bad_lock++;
    if (sw_pipe_drained(&P)) F.drained_in_write++;
    F.n_flush++;
    F.last_flush_out = F.n_out;
    return F.fail_flush ? -1 : 0;
}

static int f_read(void *ctx, uint8_t *buf, size_t cap, int idle)
{
    (void)ctx;
    if (g_locked) g_bad_lock++;
    F.reads++;
    F.idle_reads += idle;
    if ((int)cap > F.max_read_cap) F.max_read_cap = (int)cap;
    if (!cap) { F.zero_cap_reads++; return 0; }
    size_t n = F.n_in - F.in_pos;
    if (n > cap) n = cap;
    memcpy(buf, F.in + F.in_pos, n);
    F.in_pos += n;
    return (int)n;
}

static const sw_pump_io_t IO = { f_write, f_flush, f_read, NULL };
static uint8_t g_buf[256];

static void fresh(void)
{
    memset(&F, 0, sizeof F);
    g_locked = g_locks = g_bad_lock = 0;
    sw_pipe_init(&P, lk, ulk, NULL);
    sw_pipe_begin(&P);
}

static sw_pump_t pump(void) { return sw_pipe_pump(&P, &IO, g_buf, sizeof g_buf); }

static void fill(uint8_t *d, size_t n, unsigned seed)
{
    for (size_t i = 0; i < n; i++) d[i] = (uint8_t)(seed + i * 7 + (i >> 8));
}

// ---- The rings

static void test_room_and_order(void)
{
    fresh();
    CHECK_EQ_INT(sw_pipe_room(&P), SW_PIPE_DOWN);
    CHECK(sw_pipe_drained(&P));                    // nothing put: nothing owed
    uint8_t a[100], got[100];
    fill(a, sizeof a, 1);
    sw_pipe_put(&P, a, 60);
    sw_pipe_put(&P, a + 60, 40);
    CHECK_EQ_INT(sw_pipe_room(&P), SW_PIPE_DOWN - 100);
    CHECK_EQ_INT(sw_pipe_down_total(&P), 100);
    CHECK(!sw_pipe_drained(&P));
    CHECK_EQ_INT(sw_pipe_take(&P, got, 30), 30);
    CHECK_EQ_INT(sw_pipe_take(&P, got + 30, 100), 70);
    CHECK_EQ_MEM(got, a, 100);
    CHECK_EQ_INT(sw_pipe_take(&P, got, 100), 0);
    CHECK_EQ_INT(sw_pipe_room(&P), SW_PIPE_DOWN);  // room is the ring's, taken or written
}

// Drained is the port's word: an empty ring is not enough while the pump still writes.
static void test_drained_only_after_wrote(void)
{
    fresh();
    uint8_t a[50], got[50];
    fill(a, sizeof a, 3);
    sw_pipe_put(&P, a, 50);
    CHECK_EQ_INT(sw_pipe_take(&P, got, 20), 20);
    CHECK(!sw_pipe_drained(&P));                   // 30 in the ring
    sw_pipe_wrote(&P, 20);
    CHECK(!sw_pipe_drained(&P));
    CHECK_EQ_INT(sw_pipe_take(&P, got, 50), 30);
    CHECK(!sw_pipe_drained(&P));                   // the ring is empty; 30 are in the pump's hands
    sw_pipe_wrote(&P, 10);
    CHECK(!sw_pipe_drained(&P));
    sw_pipe_wrote(&P, 20);
    CHECK(sw_pipe_drained(&P));
    sw_pipe_wrote(&P, 99);                         // more than was taken counts for nothing
    CHECK(sw_pipe_drained(&P));
    sw_pipe_put(&P, a, 1);
    CHECK(!sw_pipe_drained(&P));
}

// Both rings wrap, many times, with nothing lost or reordered.
static void test_rings_wrap(void)
{
    fresh();
    static uint8_t src[30000], dst[30000];
    fill(src, sizeof src, 9);
    size_t in = 0, out = 0;
    unsigned step = 1;
    while (out < sizeof src) {
        size_t room = sw_pipe_room(&P), n = 1 + step * 37 % 1500;
        if (n > room) n = room;
        if (n > sizeof src - in) n = sizeof src - in;
        sw_pipe_put(&P, src + in, n);
        in += n;
        size_t t = sw_pipe_take(&P, dst + out, 1 + step * 53 % 900);
        sw_pipe_wrote(&P, t);
        out += t;
        step++;
    }
    CHECK_EQ_INT(in, sizeof src);
    CHECK_EQ_MEM(dst, src, sizeof src);
    CHECK_EQ_INT(sw_pipe_down_total(&P), sizeof src);
    CHECK(sw_pipe_drained(&P));

    // The printer's ring, a few bytes at a time across its end.
    static uint8_t up[5000], back[5000];
    fill(up, sizeof up, 77);
    in = out = 0;
    step = 1;
    while (out < sizeof up) {
        size_t n = 1 + step * 11 % 200;
        if (n > sizeof up - in) n = sizeof up - in;
        // the pump never gives it more than it has room for
        size_t room = SW_PIPE_UP - (in - out);
        if (n > room) n = room;
        sw_pipe_from_printer(&P, up + in, n);
        in += n;
        out += sw_pipe_output(&P, back + out, 1 + step * 7 % 128);
        step++;
    }
    CHECK_EQ_MEM(back, up, sizeof up);
    CHECK_EQ_INT(sw_pipe_up_total(&P), sizeof up);
    CHECK_EQ_INT(P.up_lost, 0);
    CHECK_EQ_INT(g_bad_lock, 0);
    CHECK(g_locks > 0 && !g_locked);
}

static void test_full_rings(void)
{
    fresh();
    static uint8_t big[SW_PIPE_DOWN + 100], got[SW_PIPE_DOWN + 100];
    fill(big, sizeof big, 5);
    sw_pipe_put(&P, big, sizeof big);              // more than room: what fits, never a wrap over
    CHECK_EQ_INT(sw_pipe_room(&P), 0);
    CHECK_EQ_INT(sw_pipe_down_total(&P), SW_PIPE_DOWN);
    CHECK_EQ_INT(sw_pipe_take(&P, got, sizeof got), SW_PIPE_DOWN);
    CHECK_EQ_MEM(got, big, SW_PIPE_DOWN);

    static uint8_t u[SW_PIPE_UP + 40], ug[SW_PIPE_UP + 40];
    fill(u, sizeof u, 6);
    sw_pipe_from_printer(&P, u, sizeof u);
    CHECK_EQ_INT(sw_pipe_up_total(&P), SW_PIPE_UP);
    CHECK_EQ_INT(P.up_lost, 40);
    CHECK_EQ_INT(sw_pipe_output(&P, ug, sizeof ug), SW_PIPE_UP);
    CHECK_EQ_MEM(ug, u, SW_PIPE_UP);
    CHECK_EQ_INT(sw_pipe_output(&P, ug, sizeof ug), 0);
}

// The capture: the first bytes toward the printer, as the pump takes them, and no more than
// its size.
static void test_capture(void)
{
    fresh();
    size_t len = 99;
    CHECK(sw_pipe_captured(&P, &len) == NULL && len == 0);
    static uint8_t keep[1000], src[3000], got[3000];
    fill(src, sizeof src, 21);
    sw_pipe_capture(&P, keep, sizeof keep);
    sw_pipe_put(&P, src, 600);
    CHECK(sw_pipe_captured(&P, &len) == NULL);     // put is not taken
    sw_pipe_take(&P, got, 600);
    CHECK(sw_pipe_captured(&P, &len) == keep && len == 600);
    sw_pipe_put(&P, src + 600, 2400);
    sw_pipe_take(&P, got, 300);
    CHECK(sw_pipe_captured(&P, &len) == keep && len == 900);
    sw_pipe_take(&P, got, 2100);                   // across the cap
    CHECK(sw_pipe_captured(&P, &len) == keep && len == 1000);
    CHECK_EQ_MEM(keep, src, 1000);
    CHECK_EQ_INT(sw_pipe_down_total(&P), 3000);    // the totals count every byte
    // The next job starts with none until it is given one.
    sw_pipe_begin(&P);
    CHECK(sw_pipe_captured(&P, &len) == NULL && len == 0);
    sw_pipe_put(&P, src, 10);
    sw_pipe_take(&P, got, 10);
    CHECK(sw_pipe_captured(&P, &len) == NULL);
    CHECK_EQ_MEM(keep, src, 1000);                 // and the last one's is left alone
}

static void test_begin_clears(void)
{
    fresh();
    uint8_t a[10] = { 0 }, b[10];
    sw_pipe_put(&P, a, 10);
    sw_pipe_from_printer(&P, a, 5);
    sw_pipe_take(&P, b, 3);
    sw_pipe_stop(&P);
    sw_pipe_fail(&P);
    sw_pipe_end(&P, 1);
    CHECK_EQ_INT(sw_pipe_room(&P), 0);             // ended: nothing more is wanted
    sw_pipe_put(&P, a, 10);
    CHECK_EQ_INT(sw_pipe_down_total(&P), 10);
    sw_pipe_begin(&P);
    CHECK_EQ_INT(sw_pipe_room(&P), SW_PIPE_DOWN);
    CHECK_EQ_INT(sw_pipe_down_total(&P), 0);
    CHECK_EQ_INT(sw_pipe_up_total(&P), 0);
    CHECK_EQ_INT(sw_pipe_reached(&P), 0);
    CHECK(!sw_pipe_stopped(&P) && !sw_pipe_failed(&P));
    CHECK_EQ_INT(sw_pipe_output(&P, b, 10), 0);
    CHECK(sw_pipe_drained(&P));
    CHECK(P.lock == lk && P.unlock == ulk);        // the hooks are init's, kept
}

// ---- The pump

// A whole job: the Mac's bytes to the port in order, the printer's back, a clean end only
// once the last byte has left.
static void test_pump_clean_job(void)
{
    fresh();
    static uint8_t job[20000], keep[20000];
    fill(job, sizeof job, 40);
    sw_pipe_capture(&P, keep, sizeof keep);
    memcpy(F.in, "answer", 6);
    size_t in = 0;
    int rounds = 0;
    uint8_t back[32];
    size_t nback = 0;
    while (in < sizeof job) {
        size_t room = sw_pipe_room(&P), n = sizeof job - in;
        if (n > room) n = room;
        if (n > 572) n = 572;                      // an ADSP packet at a time
        sw_pipe_put(&P, job + in, n);
        in += n;
        if (rounds == 3) F.n_in = 6;
        CHECK_EQ_INT(pump(), SW_PUMP_GO);
        nback += sw_pipe_output(&P, back + nback, sizeof back - nback);
        rounds++;
    }
    CHECK_EQ_INT(nback, 6);
    CHECK_EQ_MEM(back, "answer", 6);
    sw_pipe_end(&P, 1);
    CHECK(!sw_pipe_drained(&P));
    sw_pump_t v;
    int tail = 0;
    while ((v = pump()) == SW_PUMP_GO && tail < 100) tail++;
    CHECK_EQ_INT(v, SW_PUMP_CLEAN);
    CHECK(tail > 0);                               // the ring held more than a round's worth
    CHECK_EQ_INT(F.n_out, sizeof job);
    CHECK_EQ_MEM(F.out, job, sizeof job);
    CHECK_EQ_MEM(keep, job, sizeof job);
    CHECK(sw_pipe_drained(&P));
    CHECK_EQ_INT(sw_pipe_reached(&P), sizeof job);
    CHECK_EQ_INT(sw_pipe_down_total(&P), sizeof job);
    CHECK_EQ_INT(sw_pipe_up_total(&P), 6);
    CHECK_EQ_INT(F.last_flush_out, sizeof job);    // the last byte was waited out
    CHECK_EQ_INT(F.drained_in_write, 0);           // never drained with a write or flush in flight
    CHECK_EQ_INT(g_bad_lock, 0);                   // and the lock never held across the port
    CHECK_EQ_INT(pump(), SW_PUMP_CLEAN);           // asked again: the same
}

// The flush comes when the ring empties, and only then do those bytes count as written.
static void test_pump_flush_when_empty(void)
{
    fresh();
    uint8_t a[600];
    fill(a, sizeof a, 2);
    sw_pipe_put(&P, a, 600);
    CHECK_EQ_INT(pump(), SW_PUMP_GO);              // 256 of 600: more is waiting, no flush
    CHECK_EQ_INT(F.n_out, 256);
    CHECK_EQ_INT(F.n_flush, 0);
    CHECK_EQ_INT(F.idle_reads, 0);                 // it wrote: the read does not wait
    CHECK_EQ_INT(pump(), SW_PUMP_GO);
    CHECK_EQ_INT(F.n_flush, 0);
    CHECK(!sw_pipe_drained(&P));
    CHECK_EQ_INT(pump(), SW_PUMP_GO);              // the last 88
    CHECK_EQ_INT(F.n_out, 600);
    CHECK_EQ_INT(F.n_flush, 1);
    CHECK(sw_pipe_drained(&P));
    CHECK_EQ_INT(pump(), SW_PUMP_GO);              // nothing to do: the read waits
    CHECK_EQ_INT(F.idle_reads, 1);
    CHECK_EQ_INT(F.n_flush, 1);
}

// The printer's bytes never overrun the up ring: the read asks for no more than its room.
static void test_pump_up_ring_holds(void)
{
    fresh();
    fill(F.in, sizeof F.in, 90);
    F.n_in = 1500;
    for (int i = 0; i < 20; i++) CHECK_EQ_INT(pump(), SW_PUMP_GO);
    CHECK_EQ_INT(sw_pipe_up_total(&P), SW_PIPE_UP);
    CHECK_EQ_INT(P.up_lost, 0);
    CHECK_EQ_INT(F.in_pos, SW_PIPE_UP);            // the rest waits in the port
    CHECK(F.max_read_cap <= (int)sizeof g_buf);
    CHECK(F.zero_cap_reads > 0);                   // full: asked for none, still given its wait
    uint8_t got[2000];
    size_t n = sw_pipe_output(&P, got, sizeof got);
    for (int i = 0; i < 20; i++) {
        pump();
        n += sw_pipe_output(&P, got + n, sizeof got - n);
    }
    CHECK_EQ_INT(n, 1500);
    CHECK_EQ_MEM(got, F.in, 1500);
}

// A Mac that vanishes: what is left is dropped, and the printer is owed a reset only if
// anything reached it (review focus 1).
static void test_pump_unclean(void)
{
    // before the pipe ever carried a byte
    fresh();
    CHECK_EQ_INT(pump(), SW_PUMP_GO);
    sw_pipe_end(&P, 0);
    CHECK_EQ_INT(pump(), SW_PUMP_UNCLEAN);
    CHECK_EQ_INT(sw_pipe_reached(&P), 0);
    CHECK_EQ_INT(F.n_out, 0);
    // bytes put, none taken yet: still nothing reached the printer
    fresh();
    uint8_t a[1000];
    fill(a, sizeof a, 8);
    sw_pipe_put(&P, a, 1000);
    sw_pipe_end(&P, 0);
    CHECK_EQ_INT(pump(), SW_PUMP_UNCLEAN);
    CHECK_EQ_INT(sw_pipe_reached(&P), 0);
    CHECK_EQ_INT(F.n_out, 0);
    // mid-page
    fresh();
    sw_pipe_put(&P, a, 1000);
    CHECK_EQ_INT(pump(), SW_PUMP_GO);
    sw_pipe_end(&P, 0);
    CHECK_EQ_INT(pump(), SW_PUMP_UNCLEAN);
    CHECK_EQ_INT(sw_pipe_reached(&P), 256);
    CHECK_EQ_INT(F.n_out, 256);                    // the other 744 never go
    CHECK(!sw_pipe_stopped(&P) && !sw_pipe_failed(&P));
    // the end is said once: a second job_end changes nothing
    sw_pipe_end(&P, 1);
    CHECK_EQ_INT(pump(), SW_PUMP_UNCLEAN);
}

// A clean end with nothing sent (a request withdrawn) is clean at once.
static void test_pump_clean_empty(void)
{
    fresh();
    sw_pipe_end(&P, 1);
    CHECK_EQ_INT(pump(), SW_PUMP_CLEAN);
    CHECK_EQ_INT(sw_pipe_reached(&P), 0);
}

// The test button: the owner is asked once to have the adapter end the job, and the job then
// ends unclean whatever the adapter says of it.
static void test_pump_stop(void)
{
    fresh();
    uint8_t a[2000];
    fill(a, sizeof a, 12);
    sw_pipe_put(&P, a, 2000);
    CHECK_EQ_INT(pump(), SW_PUMP_GO);
    sw_pipe_stop(&P);
    CHECK_EQ_INT(pump(), SW_PUMP_STOP);
    CHECK_EQ_INT(F.n_out, 256);                    // nothing more goes to the port
    sw_pipe_end(&P, 0);                            // swa_stop's job_end
    CHECK_EQ_INT(pump(), SW_PUMP_UNCLEAN);
    CHECK(sw_pipe_stopped(&P));
    CHECK_EQ_INT(sw_pipe_reached(&P), 256);
    CHECK_EQ_INT(F.n_out, 256);

    // the adapter had no job left to end: over all the same, never a second ask
    fresh();
    sw_pipe_stop(&P);
    CHECK_EQ_INT(pump(), SW_PUMP_STOP);
    CHECK_EQ_INT(pump(), SW_PUMP_UNCLEAN);

    // a stop that lands on a job the Mac just ended cleanly: its tail is dropped
    fresh();
    sw_pipe_put(&P, a, 2000);
    sw_pipe_end(&P, 1);
    CHECK_EQ_INT(pump(), SW_PUMP_GO);
    sw_pipe_stop(&P);
    CHECK_EQ_INT(pump(), SW_PUMP_UNCLEAN);
    CHECK_EQ_INT(F.n_out, 256);
}

// A port that will not take the bytes: the adapter is told, as for a stop, and the job ends
// unclean with the reset still owed for what a write was tried for.
static void test_pump_port_fails(void)
{
    fresh();
    uint8_t a[100];
    fill(a, sizeof a, 4);
    sw_pipe_put(&P, a, 100);
    F.fail_write = 1;
    CHECK_EQ_INT(pump(), SW_PUMP_GO);
    CHECK(sw_pipe_failed(&P));
    CHECK(!sw_pipe_drained(&P));                   // taken, never written
    CHECK_EQ_INT(pump(), SW_PUMP_STOP);
    sw_pipe_end(&P, 0);
    CHECK_EQ_INT(pump(), SW_PUMP_UNCLEAN);
    CHECK_EQ_INT(sw_pipe_reached(&P), 100);

    // the last byte never left
    fresh();
    sw_pipe_put(&P, a, 100);
    F.fail_flush = 1;
    CHECK_EQ_INT(pump(), SW_PUMP_GO);
    CHECK(sw_pipe_failed(&P) && !sw_pipe_drained(&P));
    CHECK_EQ_INT(pump(), SW_PUMP_STOP);

    // a port that could not be had at all (sw_pipe_fail before the first round): nothing is
    // written, and a clean end from the Mac is not one
    fresh();
    sw_pipe_put(&P, a, 100);
    sw_pipe_fail(&P);
    CHECK_EQ_INT(pump(), SW_PUMP_STOP);
    sw_pipe_end(&P, 1);
    CHECK_EQ_INT(pump(), SW_PUMP_UNCLEAN);
    CHECK_EQ_INT(F.n_out, 0);
    CHECK_EQ_INT(sw_pipe_reached(&P), 0);
    CHECK_EQ_INT(g_bad_lock, 0);
}

// With no flush hook a write is done when it returns; with no lock hooks it runs on one task.
static void test_pump_no_hooks(void)
{
    memset(&F, 0, sizeof F);
    g_locked = 0;
    sw_pipe_init(&P, NULL, NULL, NULL);
    sw_pipe_begin(&P);
    const sw_pump_io_t io = { f_write, NULL, f_read, NULL };
    uint8_t a[10] = "123456789";
    sw_pipe_put(&P, a, 10);
    sw_pipe_end(&P, 1);
    CHECK_EQ_INT(sw_pipe_pump(&P, &io, g_buf, sizeof g_buf), SW_PUMP_CLEAN);
    CHECK_EQ_INT(F.n_out, 10);
    CHECK_EQ_MEM(F.out, a, 10);
}

int main(void)
{
    test_room_and_order();
    test_drained_only_after_wrote();
    test_rings_wrap();
    test_full_rings();
    test_capture();
    test_begin_clears();
    test_pump_clean_job();
    test_pump_flush_when_empty();
    test_pump_up_ring_holds();
    test_pump_unclean();
    test_pump_clean_empty();
    test_pump_stop();
    test_pump_port_fails();
    test_pump_no_hooks();
    TEST_END;
}
