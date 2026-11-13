// SPDX-License-Identifier: GPL-3.0-or-later
//
// The board's Personal LaserWriter 300 glue (qdlaser.c, W-1102) on the host: the real file,
// against stand-ins for the ESP-IDF headers (test/shim), for the link (qd_link.h), serial_out's
// clock and the job pipeline, which the test defines, and an idle printer scripted below that
// answers every question and prints a page the moment it is told to. What it holds is what the
// glue adds to the page stage and the session, which test_qd_page.c and test_qd_session.c hold
// on their own:
//   - a job's begin touches no printer and holds nothing without a clock; the link opens once,
//     for pages, when the rip begins, and closes once at the job's end;
//   - the page stage's sink is the session: a page's rows go to the printer and it prints;
//   - a page too detailed for the printer in both screens fails the job, which goes on;
//   - the abort contract: a session error aborts the session and the job pipeline, once;
//   - a cancel ends the job, and is not left for the next one;
//   - the port's questions open the link for a question and close it.
#include "test_util.h"
#include "qdlaser.h"
#include "qd_link.h"
#include "qd_wire.h"
#include "serial_out.h"
#include "job_pipe.h"
#include "esp_timer.h"
#include "freertos/task.h"
#include <stdlib.h>

// ---- the clock, the link and the printer (what qdlaser.c calls)

static struct {
    int      clocked, hz;
    int      opens, closes, open, job_open;
    uint8_t  asked[2];
    int      owed;
    uint32_t ms;
    int      puts, fail_put_at;    // the put that fails with QD_E_LINK (0: never)
    int      cancel_at_row;        // qdlaser_cancel() as this row goes (0: never)
    int      rows, prints, stops;  // row packets, 8A 01, 8A 00
    int      aborts;               // job_pipe_abort
    int      notes, note_apple, note_too_big;
    qd_state_t last_state;
} F;

int serial_out_clock(int *hz)
{
    if (hz) *hz = F.hz;
    return F.clocked;
}

int64_t esp_timer_get_time(void) { return (int64_t)F.ms * 1000; }
void vTaskDelay(TickType_t ticks) { F.ms += (uint32_t)ticks * 10u; }

void job_pipe_abort(void) { F.aborts++; }

static uint8_t *g_keep;
uint8_t *job_pipe_keep(size_t *cap)
{
    if (!g_keep) g_keep = malloc(512 * 1024);
    *cap = g_keep ? 512 * 1024 : 0;
    return g_keep;
}

static int f_put(void *ctx, const uint8_t *p, size_t n)
{
    (void)ctx;
    F.puts++;
    if (F.fail_put_at && F.puts == F.fail_put_at) return QD_E_LINK;
    if (p[0] < 0x80) {                                    // a row: 0x5000 - n, big-endian
        F.rows++;
        if (F.cancel_at_row && F.rows == F.cancel_at_row) qdlaser_cancel();
        return QD_OK;
    }
    if (n == 3 && p[0] == 0x8A) { if (p[1]) F.prints++; else F.stops++; }
    if (n == 3 && (p[0] == 0x82 || p[0] == 0x83 || p[0] == 0x8E)) {
        F.asked[0] = p[0];
        F.asked[1] = p[1];
        F.owed = 1;
    }
    return QD_OK;
}

static int f_flush(void *ctx) { (void)ctx; return QD_OK; }

static int f_read(void *ctx, uint8_t *b, size_t cap, uint32_t first_ms, uint32_t gap_ms)
{
    (void)ctx; (void)gap_ms;
    if (!F.owed || !cap) { F.ms += first_ms; return 0; }
    F.owed = 0;
    switch (F.asked[0]) {
    case 0x82: b[0] = 0xA4; break;                        // idle, memory empty
    case 0x8E: b[0] = 0xFC; break;                        // page done: it prints at once
    default:   b[0] = F.asked[1] == 0x3E ? 0x29           // a Personal LaserWriter 300
                    : F.asked[1] == 0x2A ? 0x61           // paper in the cassette and the slot
                    : 0x01; break;                        // nothing to say
    }
    return 1;
}

static int f_brk(void *ctx) { (void)ctx; return 0; }
static void f_drain(void *ctx) { (void)ctx; }
static void f_sleep(void *ctx, uint32_t ms) { (void)ctx; F.ms += ms; }
static uint32_t f_now(void *ctx) { (void)ctx; return F.ms; }

static const qd_ops_t k_link = {
    .put = f_put, .flush = f_flush, .read = f_read, .brk = f_brk, .drain = f_drain,
    .sleep_ms = f_sleep, .now_ms = f_now,
};

void qd_link_init(const qd_link_cfg_t *c) { (void)c; }
int qd_link_open(int hz, int job)
{
    (void)hz;
    F.opens++;
    F.open = 1;
    F.job_open = job;
    return 0;
}
void qd_link_close(void) { F.closes++; F.open = 0; }
int qd_link_is_open(void) { return F.open; }
const qd_ops_t *qd_link_ops(void) { return &k_link; }
void qd_link_stats(qd_link_stats_t *out, int reset) { (void)reset; if (out) memset(out, 0, sizeof *out); }

// ---- the board's callbacks

static void cb_state(qd_state_t st) { F.last_state = st; }
static void cb_page(int page, int apple, uint32_t bytes, int too_big)
{
    (void)page; (void)bytes;
    F.notes++;
    F.note_apple = apple;
    F.note_too_big = too_big;
}

static void reset(void)
{
    memset(&F, 0, sizeof F);
    F.clocked = 1;
    F.hz = 533000;
    F.last_state = QDS_OFFLINE;
}

// One Letter page through the target's stage, as ripjob would give it: 2,400 dots across and
// 3,300 rows, the gray first when the stage asks for it. noise: random dots and grays; else white.
static int page(const job_pipe_target_t *t, int noise)
{
    const rip_stage_t *st = t->stage;
    static uint8_t bits[300], gray[2400];
    if (st->page_begin(st->ctx, 0, 2400, 3300)) return -1;
    uint32_t r = 12345;
    for (int y = 0; y < 3300; y++) {
        for (int i = 0; i < 2400; i++) {
            r = r * 1103515245u + 12345u;
            gray[i] = noise ? (uint8_t)(r >> 16) : 255;
        }
        for (int i = 0; i < 300; i++) {
            r = r * 1103515245u + 12345u;
            bits[i] = noise ? (uint8_t)(r >> 16) : 0;
        }
        if (st->gray_row) st->gray_row(st->ctx, gray, 2400);
        st->row(st->ctx, bits);
    }
    return st->page_end(st->ctx);
}

static void test_job_prints_a_page(void)
{
    reset();
    job_pipe_target_t t;
    memset(&t, 0, sizeof t);
    CHECK_EQ_INT(qdlaser_job_begin(QD_PLW300, HT_ATKINSON, 0, 0, &t), 0);
    CHECK_EQ_INT(F.opens, 0);                             // begin touches no printer
    CHECK(t.caps == qd_caps(QD_PLW300));
    CHECK(t.stage != NULL);
    CHECK_EQ_INT(t.keep, 1);
    CHECK_EQ_INT(t.buffered, 0);
    CHECK_EQ_INT(t.dither, HT_ATKINSON);
    CHECK(t.colour_ok != NULL);
    CHECK_EQ_INT(t.colour_ok(t.colour_ctx), 0);           // gray only
    CHECK_EQ_INT(F.opens, 1);
    CHECK_EQ_INT(F.job_open, 1);
    CHECK(t.stage->gray_row != NULL);                     // Atkinson keeps an Apple page
    CHECK_EQ_INT(page(&t, 0), 0);
    CHECK_EQ_INT(F.rows, 3175);
    CHECK_EQ_INT(F.prints, 1);
    CHECK_EQ_INT(F.notes, 1);
    CHECK_EQ_INT(F.note_apple, 0);
    CHECK_EQ_INT(F.note_too_big, 0);
    CHECK_EQ_INT(qdlaser_job_failed_pages(), 0);
    CHECK_EQ_INT(qdlaser_job_error(), 0);
    CHECK_EQ_INT(qdlaser_job_end(0), QD_OK);
    CHECK_EQ_INT(F.opens, 1);
    CHECK_EQ_INT(F.closes, 1);
    CHECK_EQ_INT(F.open, 0);
    CHECK_EQ_INT(F.aborts, 0);
    CHECK_EQ_INT(F.last_state, QDS_READY);

    // Apple's screen keeps nothing: the rip screens in it and the stage sends its rows.
    reset();
    memset(&t, 0, sizeof t);
    CHECK_EQ_INT(qdlaser_job_begin(QD_PLW300, HT_APPLE, 0, 1, &t), 0);
    CHECK_EQ_INT(t.dither, HT_APPLE_LW);
    CHECK_EQ_INT(t.keep, 0);
    CHECK_EQ_INT(t.colour_ok(t.colour_ctx), 0);
    CHECK(t.stage->gray_row == NULL);
    CHECK_EQ_INT(page(&t, 0), 0);
    CHECK_EQ_INT(F.prints, 1);
    CHECK_EQ_INT(F.note_apple, 1);
    CHECK_EQ_INT(qdlaser_job_end(0), QD_OK);
    CHECK_EQ_INT(F.closes, 1);
}

static void test_job_no_clock_refused(void)
{
    reset();
    F.clocked = 0;
    job_pipe_target_t t;
    memset(&t, 0, sizeof t);
    CHECK_EQ_INT(qdlaser_job_begin(QD_PLW300, HT_ATKINSON, 0, 0, &t), -1);
    CHECK_EQ_INT(F.opens, 0);
    CHECK_EQ_INT(qdlaser_job_error(), 0);
    // Nothing was held: the next job, with the clock, prints.
    F.clocked = 1;
    CHECK_EQ_INT(qdlaser_job_begin(QD_PLW300, HT_ATKINSON, 0, 0, &t), 0);
    CHECK_EQ_INT(t.colour_ok(t.colour_ctx), 0);
    CHECK_EQ_INT(page(&t, 0), 0);
    CHECK_EQ_INT(qdlaser_job_end(0), QD_OK);
    CHECK_EQ_INT(F.prints, 1);
    // A job no model is for is refused too.
    CHECK_EQ_INT(qdlaser_job_begin(QD_NONE, HT_ATKINSON, 0, 0, &t), -1);
}

static void test_too_big_page_fails_job(void)
{
    reset();
    job_pipe_target_t t;
    memset(&t, 0, sizeof t);
    CHECK_EQ_INT(qdlaser_job_begin(QD_PLW300, HT_ATKINSON, 0, 0, &t), 0);
    CHECK_EQ_INT(t.colour_ok(t.colour_ctx), 0);
    page(&t, 1);
    CHECK_EQ_INT(F.notes, 1);
    CHECK_EQ_INT(F.note_too_big, 1);
    CHECK_EQ_INT(F.prints, 0);                            // nothing printed
    CHECK_EQ_INT(qdlaser_job_failed_pages(), 1);
    CHECK_EQ_INT(qdlaser_job_error(), 0);                 // not the printer's error
    CHECK_EQ_INT(F.aborts, 0);                            // the job goes on
    // The next page prints.
    CHECK_EQ_INT(page(&t, 0), 0);
    CHECK_EQ_INT(F.prints, 1);
    CHECK_EQ_INT(qdlaser_job_failed_pages(), 1);
    CHECK(qdlaser_job_end(0) != QD_OK);
    CHECK_EQ_INT(F.closes, 1);
}

static void test_session_error_aborts_pipe(void)
{
    reset();
    F.fail_put_at = 60;                                   // among the first page's rows
    job_pipe_target_t t;
    memset(&t, 0, sizeof t);
    CHECK_EQ_INT(qdlaser_job_begin(QD_PLW300, HT_ATKINSON, 0, 0, &t), 0);
    CHECK_EQ_INT(t.colour_ok(t.colour_ctx), 0);
    CHECK(page(&t, 0) != 0);
    CHECK_EQ_INT(F.aborts, 1);                            // once, however many rows followed
    CHECK_EQ_INT(qdlaser_job_error(), QD_E_LINK);
    CHECK_EQ_INT(F.prints, 0);
    CHECK_EQ_INT(qdlaser_job_end(1), QD_E_LINK);
    CHECK_EQ_INT(F.aborts, 1);
    CHECK_EQ_INT(F.closes, 1);
    CHECK_EQ_INT(qdlaser_job_error(), 0);                 // freed for the next job

    // A session that will not begin (the printer silent) aborts the pipe before any row.
    reset();
    F.fail_put_at = 1;
    memset(&t, 0, sizeof t);
    CHECK_EQ_INT(qdlaser_job_begin(QD_PLW300, HT_ATKINSON, 0, 0, &t), 0);
    t.colour_ok(t.colour_ctx);
    CHECK_EQ_INT(F.aborts, 1);
    CHECK(qdlaser_job_error() != 0);
    CHECK(qdlaser_job_end(1) != QD_OK);
    CHECK_EQ_INT(F.closes, 1);
}

static void test_cancel(void)
{
    reset();
    F.cancel_at_row = 100;
    job_pipe_target_t t;
    memset(&t, 0, sizeof t);
    CHECK_EQ_INT(qdlaser_job_begin(QD_PLW300, HT_ATKINSON, 0, 0, &t), 0);
    CHECK_EQ_INT(t.colour_ok(t.colour_ctx), 0);
    CHECK(page(&t, 0) != 0);
    CHECK_EQ_INT(qdlaser_job_error(), QD_E_CANCEL);
    CHECK_EQ_INT(F.aborts, 1);
    CHECK_EQ_INT(F.prints, 0);
    CHECK(F.stops >= 1);                                  // 8A 00 went
    CHECK_EQ_INT(qdlaser_job_end(1), QD_E_CANCEL);
    CHECK_EQ_INT(F.closes, 1);

    // A cancel that lands after the job is not the next job's.
    qdlaser_cancel();
    reset();
    memset(&t, 0, sizeof t);
    CHECK_EQ_INT(qdlaser_job_begin(QD_PLW300, HT_ATKINSON, 0, 0, &t), 0);
    CHECK_EQ_INT(t.colour_ok(t.colour_ctx), 0);
    CHECK_EQ_INT(page(&t, 0), 0);
    CHECK_EQ_INT(qdlaser_job_end(0), QD_OK);
    CHECK_EQ_INT(F.prints, 1);
}

static void test_questions_open_the_link(void)
{
    reset();
    uint8_t engine = 0;
    CHECK_EQ_INT(qdlaser_identity(533000, &engine), QD_OK);
    CHECK_EQ_INT(engine, 0x29);
    CHECK_EQ_INT(F.opens, 1);
    CHECK_EQ_INT(F.job_open, 0);
    CHECK_EQ_INT(F.closes, 1);
    qd_state_t st = QDS_OFFLINE;
    qd_raw_t raw;
    memset(&raw, 0xEE, sizeof raw);
    CHECK_EQ_INT(qdlaser_status(533000, QD_PLW300, &st, &raw), QD_OK);
    CHECK_EQ_INT(st, QDS_READY);
    CHECK_EQ_INT(raw.c82, 0xA4);                          // the replies it came from
    CHECK_EQ_INT(raw.s2a, 0x61);
    CHECK_EQ_INT(qdlaser_status(533000, QD_PLW300, &st, NULL), QD_OK);
    CHECK_EQ_INT(F.opens, 3);
    CHECK_EQ_INT(F.closes, 3);
    CHECK_EQ_INT(F.open, 0);
}

int main(void)
{
    static const qdlaser_cfg_t cfg = { .state = cb_state, .page = cb_page };
    qdlaser_start(&cfg);
    test_job_prints_a_page();
    test_job_no_clock_refused();
    test_too_big_page_fails_job();
    test_session_error_aborts_pipe();
    test_cancel();
    test_questions_open_the_link();
    free(g_keep);
    TEST_END;
}
