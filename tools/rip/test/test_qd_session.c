// SPDX-License-Identifier: GPL-3.0-or-later
// W-1102: the Personal LaserWriter 300's conversation (qd_session), run against a printer
// scripted in this file on a fake clock, so nothing sleeps. The fake answers 82, 83 (and the
// value after 83 9D and 83 A7) and 8E as the bench's printer did, keeps a page printing for a
// while after 8A 01, and can be told to jam, open its cover, run out of paper, refuse, go
// silent or stop. Its replies carry their parity in bit 0, as the printer's do. It records what
// it was sent as a trace of tokens:
//   8340   the packet 83 40 (its check is held to cmd ^ arg, and counted in bad when not)
//   =21    a value packet after 83 9D or 83 A7, with the value it carries
//   R*n    n row packets in a row
#include "test_util.h"
#include "qd_session.h"
#include "qd_wire.h"
#include <stdio.h>
#include <stdlib.h>

#define CHECK_STR(a, b) do { g_checks++; const char *_a = (a), *_b = (b); \
    if (strcmp(_a, _b)) { g_failed++; \
    fprintf(stderr, "FAIL %s:%d: %s\n   got  %s\n   want %s\n", __FILE__, __LINE__, #a, _a, _b); } } while (0)
#define CHECK_HAS(hay, needle) do { g_checks++; const char *_h = (hay), *_n = (needle); \
    if (!strstr(_h, _n)) { g_failed++; \
    fprintf(stderr, "FAIL %s:%d: trace lacks \"%s\"\n   trace %s\n", __FILE__, __LINE__, _n, _h); } } while (0)

enum { TRACE_N = 1 << 16 };

typedef struct { uint32_t from, to; uint8_t s01, s02, no_paper; } win_t;

typedef struct {
    uint64_t us;
    uint8_t engine, c82, s01, s02, s04, s07, s2a;
    int memfull;                 // 8E answers F9
    int zero_first;              // 8E answers 00 and then the status, this many times
    int refuse01;                // 83 01 answers 80, this many times
    int silent;                  // no replies at all
    int silent_after_print;      // no replies once a page is told to print
    int printing;
    uint32_t print_start, print_ms;
    int never_done, done_on_brk;
    int done;                    // 8E bit 2, until it is read
    win_t win[4];
    int nwin;
    int win_from_print;          // windows count from the first 8A 01
    uint32_t first_print;
    uint32_t brk_at;
    int brk_fired, brk_pending;
    int cancel_rows;
    uint32_t cancel_at_ms;
    int put_fail_row, put_rc, flush_rc;
    uint32_t put_us;             // a packet's time on the wire, overriding its bits
    uint8_t in[8];
    int in_n;
    int value_next;
    char trace[TRACE_N];
    size_t tn;
    int run;                     // rows in the last R token
    uint8_t raw[16][8];
    int nraw;
    uint32_t t8e[512];
    int n8e;
    int tries01;
    qd_state_t st[64];
    int nst;
    int rows, bad, puts;
} fake_t;

static fake_t F;

static uint32_t f_now(void) { return (uint32_t)(F.us / 1000); }

static uint32_t since_print(void) { return F.first_print ? f_now() - F.first_print : 0; }

static int in_win(uint8_t *s01, uint8_t *s02, int *no_paper)
{
    int any = 0;
    if (F.win_from_print && !F.first_print) return 0;
    uint32_t t = F.win_from_print ? since_print() : f_now();
    for (int i = 0; i < F.nwin; i++)
        if (t >= F.win[i].from && t < F.win[i].to) {
            *s01 |= F.win[i].s01;
            *s02 |= F.win[i].s02;
            *no_paper |= F.win[i].no_paper;
            any = 1;
        }
    return any;
}

// The page goes on printing while nothing holds it; a jam, the cover or no paper stops it.
static void tick(void)
{
    uint8_t a = 0, b = 0;
    int np = 0;
    in_win(&a, &b, &np);
    if (F.printing && (b & 0x0C || np)) F.printing = 0;
    if (F.printing && !F.never_done && !F.done_on_brk && f_now() - F.print_start >= F.print_ms) {
        F.printing = 0;
        F.done = 1;
    }
    if (F.brk_at && !F.brk_fired && f_now() >= F.brk_at) {
        F.brk_fired = 1;
        F.brk_pending = 1;
        if (F.done_on_brk && F.printing) { F.printing = 0; F.done = 1; }
    }
}

static void adv_us(uint64_t us) { F.us += us; tick(); }

static void tok(const char *s)
{
    size_t n = strlen(s);
    if (F.tn + n + 2 >= TRACE_N) return;
    if (F.tn) F.trace[F.tn++] = ' ';
    memcpy(F.trace + F.tn, s, n);
    F.tn += n;
    F.trace[F.tn] = 0;
}

static void row_tok(void)
{
    char t[16];
    if (F.run) {
        // Rewrite the last token in place.
        size_t back = (size_t)snprintf(t, sizeof t, "R*%d", F.run);
        F.tn -= back;
        F.trace[F.tn] = 0;
        if (F.tn) F.tn--;              // the space before it
        F.trace[F.tn] = 0;
    }
    F.run++;
    snprintf(t, sizeof t, "R*%d", F.run);
    tok(t);
}

static uint8_t odd(uint8_t v)
{
    v &= 0xFE;
    int n = 0;
    for (uint8_t x = v; x; x &= (uint8_t)(x - 1)) n++;
    return (n & 1) ? v : (uint8_t)(v | 1);
}

static void reply(uint8_t b)
{
    if (F.silent) return;
    if (F.in_n < (int)sizeof F.in) F.in[F.in_n++] = b;
}

static uint8_t reg(uint8_t which)
{
    uint8_t a = 0, b = 0;
    int np = 0;
    in_win(&a, &b, &np);
    switch (which) {
    case 0x01: return odd((uint8_t)(F.s01 | a | (F.printing ? 0x20 : 0)));
    case 0x02: return odd((uint8_t)(F.s02 | b));
    case 0x04: return odd(F.s04);
    case 0x07: return odd(F.s07);
    case 0x2A: return odd(np ? 0 : F.s2a);
    case 0x3E: return F.engine;
    default:   return odd(0);
    }
}

static int f_put(void *ctx, const uint8_t *p, size_t n)
{
    (void)ctx;
    F.puts++;
    if (F.put_fail_row && F.rows >= F.put_fail_row) return F.put_rc;
    adv_us(F.put_us ? F.put_us : (uint64_t)qd_bits_len(n) * 15 / 8);
    if (F.nraw < 16 && n <= 8) memcpy(F.raw[F.nraw++], p, n);
    char t[16];
    if (p[0] < 0x80) {
        unsigned len = 0x5000u - ((unsigned)p[0] << 8 | p[1]);
        uint8_t x = 0;
        for (size_t i = 0; i + 1 < n; i++) x ^= p[i];
        if (len + 3 != n || x != p[n - 1]) F.bad++;
        F.rows++;
        row_tok();
        return 0;
    }
    F.run = 0;
    if (n != 3 || (uint8_t)(p[0] ^ p[1]) != p[2]) F.bad++;
    if (F.value_next && p[0] == 0x83) {
        F.value_next = 0;
        uint8_t v = p[1];
        if (odd(v) != v) F.bad++;
        snprintf(t, sizeof t, "=%02X", v >> 1);
        tok(t);
        reply(odd(0));
        return 0;
    }
    snprintf(t, sizeof t, "%02X%02X", p[0], p[1]);
    tok(t);
    switch (p[0]) {
    case 0x82: reply(F.c82); break;
    case 0x83:
        if (p[1] == 0x9D || p[1] == 0xA7) F.value_next = 1;
        if (p[1] == 0x01) {
            F.tries01++;
            if (F.refuse01) { F.refuse01--; reply(0x80); break; }
        }
        reply(reg(p[1]));
        break;
    case 0x8E:
        if (F.n8e < 512) F.t8e[F.n8e++] = f_now();
        {
            uint8_t st = F.memfull ? 0xF9 : F.done ? 0xFC : 0xF8;
            if (F.done) F.done = 0;
            if (F.zero_first) { F.zero_first--; reply(0x00); }
            reply(st);
        }
        break;
    case 0x8A:
        if (p[1] == 0x01) {
            if (!F.first_print) F.first_print = f_now() ? f_now() : 1;
            F.printing = 1;
            F.print_start = f_now();
            if (F.silent_after_print) F.silent = 1;
        } else if (p[1] == 0x00) {
            F.printing = 0;
        }
        break;
    default: break;
    }
    return 0;
}

static int f_flush(void *ctx) { (void)ctx; return F.flush_rc; }

static int f_read(void *ctx, uint8_t *b, size_t cap, uint32_t first_ms, uint32_t gap_ms)
{
    (void)ctx; (void)gap_ms;
    if (!F.in_n) { adv_us((uint64_t)first_ms * 1000); return 0; }
    adv_us(1000);
    int n = F.in_n < (int)cap ? F.in_n : (int)cap;
    memcpy(b, F.in, (size_t)n);
    memmove(F.in, F.in + n, (size_t)(F.in_n - n));
    F.in_n -= n;
    return n;
}

static int f_brk(void *ctx)
{
    (void)ctx;
    tick();
    int b = F.brk_pending;
    F.brk_pending = 0;
    return b;
}
static void f_drain(void *ctx) { (void)ctx; F.in_n = 0; }
static void f_sleep(void *ctx, uint32_t ms) { (void)ctx; adv_us((uint64_t)ms * 1000); }
static uint32_t f_now_ms(void *ctx) { (void)ctx; return f_now(); }
static int f_cancelled(void *ctx)
{
    (void)ctx;
    return (F.cancel_rows && F.rows >= F.cancel_rows) || (F.cancel_at_ms && f_now() >= F.cancel_at_ms);
}
static void f_state(void *ctx, qd_state_t st)
{
    (void)ctx;
    if (F.nst < 64) F.st[F.nst++] = st;
}

static qd_ops_t g_ops = { f_put, f_flush, f_read, f_brk, f_drain, f_sleep, f_now_ms, f_cancelled,
                          f_state, NULL };

// The bench's printer, idle, paper in the cassette.
static void fake_reset(void)
{
    memset(&F, 0, sizeof F);
    F.us = 1000000;                     // the clock starts away from 0
    F.engine = 0x29;
    F.c82 = 0xA4;
    F.s2a = 0x20;
    F.print_ms = 13000;
    g_ops.brk = f_brk;
}

static const qd_media_t *letter(void) { return &qd_spec(QD_PLW300)->media[0]; }

static int count_tok(const char *t)
{
    int n = 0;
    size_t len = strlen(t);
    for (const char *p = F.trace; (p = strstr(p, t)); p += len)
        if ((p == F.trace || p[-1] == ' ') && (p[len] == ' ' || p[len] == 0)) n++;
    return n;
}

// The trace from the first token `from` on.
static const char *after(const char *from)
{
    const char *p = strstr(F.trace, from);
    return p ? p : "";
}

static int last_state(void) { return F.nst ? (int)F.st[F.nst - 1] : -1; }
static int saw_state(qd_state_t s)
{
    for (int i = 0; i < F.nst; i++) if (F.st[i] == s) return 1;
    return 0;
}

static qd_session_t *job(int manual, uint32_t stall_ms)
{
    qd_job_cfg_t c = { QD_PLW300, manual, stall_ms };
    return qd_session_new(&g_ops, &c);
}

// rows blank rows (80), returning the first error.
static int rows(qd_session_t *s, int n)
{
    static const uint8_t blank[1] = { 0x80 };
    for (int i = 0; i < n; i++) {
        int rc = qd_session_row(s, blank, 1);
        if (rc) return rc;
    }
    return 0;
}

// begin, open Letter, 3175 rows; the session is ready to print.
static qd_session_t *loaded(int manual, uint32_t stall_ms)
{
    qd_session_t *s = job(manual, stall_ms);
    CHECK(s != NULL);
    CHECK_EQ_INT(qd_session_begin(s), QD_OK);
    CHECK_EQ_INT(qd_session_open(s, letter()), QD_OK);
    CHECK_EQ_INT(rows(s, 3175), QD_OK);
    return s;
}

static const char *OPEN_LETTER =
    "8100 8340 833E 8200 87DD 85C2 86FF 839D =21 83A7 =20 8400 88D4 89FE 834C 8401";

static void test_identity_sequence(void)
{
    fake_reset();
    uint8_t e = 0, c = 0;
    CHECK_EQ_INT(qd_ask_identity(&g_ops, &e, &c), QD_OK);
    CHECK_EQ_INT(e, 0x29);
    CHECK_EQ_INT(c, 0xA4);
    CHECK_STR(F.trace, "8100 8340 833E 8200");
    CHECK_EQ_INT(F.nraw, 4);
    CHECK_EQ_MEM(F.raw[0], ((uint8_t[]){0x81, 0x00, 0x81}), 3);
    CHECK_EQ_MEM(F.raw[1], ((uint8_t[]){0x83, 0x40, 0xC3}), 3);
    CHECK_EQ_MEM(F.raw[2], ((uint8_t[]){0x83, 0x3E, 0xBD}), 3);
    CHECK_EQ_MEM(F.raw[3], ((uint8_t[]){0x82, 0x00, 0x82}), 3);
    CHECK_EQ_INT(F.bad, 0);

    // Nobody there: offline, said once.
    fake_reset();
    F.silent = 1;
    CHECK_EQ_INT(qd_ask_identity(&g_ops, &e, &c), QD_E_OFFLINE);
    CHECK_EQ_INT(last_state(), QDS_OFFLINE);
}

static void test_begin_refuses_other_engine(void)
{
    fake_reset();
    F.engine = 0x0D;                       // a Personal LaserWriter LS
    qd_session_t *s = job(0, 0);
    CHECK_EQ_INT(qd_session_begin(s), QD_E_MODEL);
    CHECK_STR(F.trace, "8100 8340 833E 8200");
    qd_session_free(s);
}

static void test_open_letter_is_apples(void)
{
    fake_reset();
    qd_session_t *s = job(0, 0);
    CHECK_EQ_INT(qd_session_begin(s), QD_OK);
    CHECK_STR(F.trace, "8100 8340 833E 8200 8301");
    CHECK_EQ_INT(last_state(), QDS_PRINTING);
    F.tn = 0;
    F.trace[0] = 0;
    CHECK_EQ_INT(qd_session_open(s, letter()), QD_OK);
    CHECK_STR(F.trace, OPEN_LETTER);
    CHECK_EQ_INT(F.bad, 0);
    qd_session_free(s);
}

static void test_open_manual(void)
{
    fake_reset();
    F.s2a = 0x40;
    qd_session_t *s = job(1, 0);
    CHECK_EQ_INT(qd_session_begin(s), QD_OK);
    F.tn = 0;
    F.trace[0] = 0;
    CHECK_EQ_INT(qd_session_open(s, letter()), QD_OK);
    CHECK_STR(F.trace,
              "8100 8340 833E 8200 87DD 85C2 86FF 839D =21 83A7 =20 8400 88D4 89FE 834F 8401");
    qd_session_free(s);
}

static void test_rows_blink_ready(void)
{
    fake_reset();
    qd_session_t *s = job(0, 0);
    CHECK_EQ_INT(qd_session_begin(s), QD_OK);
    CHECK_EQ_INT(qd_session_open(s, letter()), QD_OK);
    F.tn = 0;
    F.trace[0] = 0;
    F.put_us = 5000000 / 3175;             // the page's rows over 5 s
    CHECK_EQ_INT(rows(s, 3175), QD_OK);
    int off = count_tok("8400"), on = count_tok("8401");
    CHECK(off + on >= 4 && off + on <= 6);
    CHECK(off - on <= 1 && on - off <= 1);
    CHECK(strstr(F.trace, "8400") < strstr(F.trace, "8401"));   // the light was on: off first
    CHECK_EQ_INT(F.rows, 3175);
    CHECK_EQ_INT(F.bad, 0);
    qd_session_free(s);
}

static void test_print_sequence(void)
{
    fake_reset();
    qd_session_t *s = loaded(0, 0);
    int nst = F.nst;
    CHECK_EQ_INT(qd_session_print(s), QD_OK);
    CHECK(!strncmp(after("8E00"), "8E00 8200 832A 8401 8B8B 8B8B 8A01 ", 35));
    CHECK_EQ_INT(F.nst, nst);              // PRINTING since begin, and nothing more
    CHECK_EQ_INT(last_state(), QDS_PRINTING);
    CHECK_EQ_INT(count_tok("8A01"), 1);
    // Done on the 8E that read FC, about 13 s on.
    const char *t = F.trace + F.tn - 4;
    CHECK_STR(t, "8E00");
    F.tn = 0;
    F.trace[0] = 0;
    CHECK_EQ_INT(qd_session_end(s), QD_OK);
    CHECK_STR(F.trace, "8301 8100 8401");
    CHECK_EQ_INT(last_state(), QDS_READY);
    qd_session_free(s);
}

// W-1102 bench: page done comes while the sheet is still on its way out (83 01 bit 5 for about
// 15 s more on board 1). The job ends Printing, not Ready, so the state doesn't flick back to
// Printing at the next status; the status at rest says Ready once the sheet is out.
static void test_end_while_the_sheet_comes_out(void)
{
    fake_reset();
    qd_session_t *s = loaded(0, 0);
    F.win[0] = (win_t){ .from = 0, .to = 60000, .s01 = 0x20 };
    F.nwin = 1;
    F.win_from_print = 1;
    CHECK_EQ_INT(qd_session_print(s), QD_OK);
    F.tn = 0;
    F.trace[0] = 0;
    CHECK_EQ_INT(qd_session_end(s), QD_OK);
    CHECK_STR(F.trace, "8301 8100 8401");
    CHECK_EQ_INT(last_state(), QDS_PRINTING);
    qd_session_free(s);
}

// A page from the manual slot leaves the printer taking paper from the slot, which then says it
// has none and pulls in any sheet put there (board 1, 2026-11-13): the job's end sets the
// cassette back.
static void test_end_after_a_manual_page_sets_the_cassette_back(void)
{
    fake_reset();
    F.s2a = 0x60;                         // a sheet in the slot, paper in the cassette
    qd_session_t *s = loaded(1, 0);
    CHECK_EQ_INT(qd_session_print(s), QD_OK);
    F.tn = 0;
    F.trace[0] = 0;
    CHECK_EQ_INT(qd_session_end(s), QD_OK);
    CHECK_STR(F.trace, "8301 834C 8100 8401");
    qd_session_free(s);
}

static void test_done_by_break(void)
{
    fake_reset();
    qd_session_t *s = loaded(0, 0);
    F.done_on_brk = 1;
    F.brk_at = f_now() + 3200;
    uint32_t brk_at = F.brk_at;
    CHECK_EQ_INT(qd_session_print(s), QD_OK);
    CHECK(F.n8e >= 2);
    uint32_t last = F.t8e[F.n8e - 1];
    CHECK(last >= brk_at && last < brk_at + 100);   // asked at once, not at the next half second
    qd_session_free(s);
}

static void test_done_by_poll(void)
{
    fake_reset();
    g_ops.brk = NULL;
    qd_session_t *s = loaded(0, 0);
    int first = F.n8e;
    CHECK_EQ_INT(qd_session_print(s), QD_OK);
    int polls = F.n8e - first - 1;         // the first 8E is the check after the rows
    CHECK(polls >= 24 && polls <= 28);     // 13 s at one every 500 ms
    int bad = 0;
    for (int i = first + 2; i < F.n8e; i++) {
        uint32_t d = F.t8e[i] - F.t8e[i - 1];
        bad += d < 480 || d > 560;
    }
    CHECK_EQ_INT(bad, 0);
    qd_session_free(s);
    g_ops.brk = f_brk;
}

static void test_8e_zero_then_status(void)
{
    fake_reset();
    qd_session_t *s = loaded(0, 0);
    F.zero_first = 1;                      // the check after the rows reads 00, then F8
    CHECK_EQ_INT(qd_session_print(s), QD_OK);
    CHECK_EQ_INT(count_tok("8A01"), 1);
    // And 00 then FC is done.
    fake_reset();
    s = loaded(0, 0);
    F.print_ms = 1000;
    F.zero_first = 0;
    CHECK_EQ_INT(qd_session_print(s), QD_OK);
    qd_session_free(s);
}

static void test_done_timeout_85s(void)
{
    fake_reset();
    qd_session_t *s = loaded(0, 0);
    F.never_done = 1;
    uint32_t t0 = f_now();
    CHECK_EQ_INT(qd_session_print(s), QD_E_FAULT);
    uint32_t took = f_now() - t0;
    CHECK(took >= 85000 && took < 87000);
    qd_session_free(s);
}

static void test_paper_out_before_print(void)
{
    fake_reset();
    qd_session_t *s = loaded(0, 0);
    // No paper for the first 3 s after the rows.
    F.win[F.nwin++] = (win_t){ 0, f_now() + 3000, 0, 0, 1 };
    uint32_t t0 = f_now();
    CHECK_EQ_INT(qd_session_print(s), QD_OK);
    CHECK(saw_state(QDS_NO_PAPER));
    CHECK_HAS(F.trace, "832A 8302 8402");
    // 8A 01 only once the paper is back.
    const char *p8a = strstr(F.trace, "8A01");
    const char *last_out = NULL;
    for (const char *p = F.trace; (p = strstr(p, "8402")); p++) last_out = p;
    CHECK(p8a && last_out && last_out < p8a);
    CHECK(F.first_print - t0 >= 3000);
    CHECK_EQ_INT(F.st[F.nst - 1], QDS_PRINTING);
    qd_session_free(s);
}

static void test_paper_out_while_printing(void)
{
    fake_reset();
    qd_session_t *s = loaded(0, 0);
    F.win_from_print = 1;
    F.win[F.nwin++] = (win_t){ 2000, 6000, 0, 0, 1 };
    CHECK_EQ_INT(qd_session_print(s), QD_OK);
    CHECK(saw_state(QDS_NO_PAPER));
    CHECK_HAS(after("8A01"), "8A00");
    CHECK_EQ_INT(count_tok("8A01"), 2);
    CHECK_EQ_INT(F.st[F.nst - 1], QDS_PRINTING);
    qd_session_free(s);
}

// The cassette pulled out, as board 1's printer answered (2026-11-13): 83 01 says a fault is
// stored, 83 02 sets 10 (no paper at the source) and 83 2A has no paper. The page waits for
// paper.
static void test_cassette_out_while_printing(void)
{
    fake_reset();
    qd_session_t *s = loaded(0, 0);
    F.win_from_print = 1;
    F.win[F.nwin++] = (win_t){ 2000, 6000, 0x02, 0x10, 1 };
    CHECK_EQ_INT(qd_session_print(s), QD_OK);
    CHECK(saw_state(QDS_NO_PAPER));
    CHECK(!saw_state(QDS_ENGINE));
    CHECK_HAS(after("8A01"), "8A00");
    CHECK_EQ_INT(count_tok("8A01"), 2);
    CHECK_EQ_INT(F.st[F.nst - 1], QDS_PRINTING);
    qd_session_free(s);
}

static void test_cassette_out_before_print(void)
{
    fake_reset();
    qd_session_t *s = loaded(0, 0);
    F.win[F.nwin++] = (win_t){ 0, f_now() + 3000, 0x02, 0x10, 1 };
    CHECK_EQ_INT(qd_session_print(s), QD_OK);
    CHECK(saw_state(QDS_NO_PAPER));
    CHECK(!saw_state(QDS_ENGINE));
    CHECK_EQ_INT(count_tok("8A01"), 1);
    qd_session_free(s);
}

static void jam_or_cover(uint8_t bit, qd_state_t want)
{
    fake_reset();
    qd_session_t *s = loaded(0, 0);
    F.win_from_print = 1;
    F.win[F.nwin++] = (win_t){ 2000, 6000, 0, bit, 0 };
    CHECK_EQ_INT(qd_session_print(s), QD_OK);
    CHECK(saw_state(want));
    CHECK_HAS(F.trace, "8A00 8404");
    CHECK_HAS(F.trace, "8100 8408 8401 8340 833E 8200 835D 834C 8100 8A01");
    CHECK_EQ_INT(count_tok("8A01"), 2);
    CHECK_EQ_INT(F.st[F.nst - 1], QDS_PRINTING);
    qd_session_free(s);
}

static void test_jam_recovery_reprints(void)
{
    jam_or_cover(0x08, QDS_JAM);
    // 84 08 is held about 280 ms: the fake's clock moved at least that between it and 84 01.
}

static void test_cover_open(void)
{
    jam_or_cover(0x04, QDS_COVER);
}

static void test_engine_fault_fails(void)
{
    fake_reset();
    qd_session_t *s = loaded(0, 0);
    F.win_from_print = 1;
    F.win[F.nwin++] = (win_t){ 1000, 100000, 0x02, 0, 0 };
    F.s04 = 0x10;
    CHECK_EQ_INT(qd_session_print(s), QD_E_FAULT);
    CHECK_HAS(after("8A01"), "8304 8307");
    CHECK_EQ_INT(last_state(), QDS_ENGINE);
    qd_session_free(s);
}

static void test_memory_full_fails_page(void)
{
    fake_reset();
    qd_session_t *s = loaded(0, 0);
    F.memfull = 1;
    CHECK_EQ_INT(qd_session_print(s), QD_E_FAULT);
    CHECK_EQ_INT(count_tok("8A01"), 0);
    qd_session_free(s);
}

static void test_cancel(void)
{
    fake_reset();
    qd_session_t *s = job(0, 0);
    CHECK_EQ_INT(qd_session_begin(s), QD_OK);
    CHECK_EQ_INT(qd_session_open(s, letter()), QD_OK);
    F.cancel_rows = 64;
    CHECK_EQ_INT(rows(s, 3175), QD_E_CANCEL);
    CHECK_HAS(F.trace, "R*64 8A00 8100 8401");
    CHECK_EQ_INT(F.rows, 64);
    // Abort after a cancel sends nothing more.
    size_t tn = F.tn;
    qd_session_abort(s);
    CHECK_EQ_INT(F.tn, tn);
    qd_session_free(s);
}

static void test_cancel_while_waiting_for_paper(void)
{
    fake_reset();
    qd_session_t *s = loaded(0, 0);
    F.s2a = 0;
    F.cancel_at_ms = f_now() + 4000;
    CHECK_EQ_INT(qd_session_print(s), QD_E_CANCEL);
    CHECK(saw_state(QDS_NO_PAPER));
    const char *t = F.trace + F.tn - strlen("8A00 8100 8401");
    CHECK_STR(t, "8A00 8100 8401");
    CHECK_EQ_INT(count_tok("8A01"), 0);
    qd_session_free(s);
}

static void test_manual_never_fed_stalls_then_fails(void)
{
    fake_reset();
    qd_session_t *s = loaded(1, 0);       // the cassette has paper, the slot none
    uint32_t t0 = f_now();
    CHECK_EQ_INT(qd_session_print(s), QD_E_STALL);
    uint32_t took = f_now() - t0;
    CHECK(took >= 600000 && took < 602000);
    int only_manual = 1;
    for (int i = 1; i < F.nst; i++) only_manual &= F.st[i] == QDS_MANUAL;
    CHECK(only_manual && F.nst >= 2);
    CHECK_EQ_INT(count_tok("8A01"), 0);
    CHECK_HAS(F.trace, "832A 8402");
    qd_session_free(s);
}

static void test_clock_lost_mid_rows(void)
{
    fake_reset();
    qd_session_t *s = job(0, 0);
    CHECK_EQ_INT(qd_session_begin(s), QD_OK);
    CHECK_EQ_INT(qd_session_open(s, letter()), QD_OK);
    F.put_fail_row = 100;
    F.put_rc = QD_E_OFFLINE;
    CHECK_EQ_INT(rows(s, 3175), QD_E_OFFLINE);
    CHECK_EQ_INT(last_state(), QDS_OFFLINE);
    qd_session_free(s);
}

static void test_readback_mismatch(void)
{
    fake_reset();
    qd_session_t *s = loaded(0, 0);
    int nst = F.nst;
    F.flush_rc = QD_E_LINK;
    CHECK_EQ_INT(qd_session_print(s), QD_E_LINK);
    CHECK_EQ_INT(F.nst, nst);
    qd_session_free(s);
}

static void test_refused_reply_retries_5_then_offline(void)
{
    fake_reset();
    F.refuse01 = 5;
    qd_state_t st;
    qd_raw_t raw;
    CHECK_EQ_INT(qd_ask_status(&g_ops, QD_PLW300, 0, &st, &raw), QD_E_OFFLINE);
    CHECK_EQ_INT(F.tries01, 5);
    CHECK_EQ_INT(last_state(), QDS_OFFLINE);
    // Four refusals and then a good answer: fine.
    fake_reset();
    F.refuse01 = 4;
    CHECK_EQ_INT(qd_ask_status(&g_ops, QD_PLW300, 0, &st, &raw), QD_OK);
    CHECK_EQ_INT(F.tries01, 5);
    CHECK_EQ_INT(st, QDS_READY);
}

// W-1102 bench: a status that gets nothing names the question that went unanswered, for the
// log, so a printer that flickers offline on the bench says where.
static void test_status_names_the_unanswered_question(void)
{
    fake_reset();
    F.refuse01 = 5;
    qd_state_t st;
    qd_raw_t raw = { .miss = 0xFFFF };
    CHECK_EQ_INT(qd_ask_status(&g_ops, QD_PLW300, 0, &st, &raw), QD_E_OFFLINE);
    CHECK_EQ_INT(raw.miss, 0x8301);
    fake_reset();
    F.silent = 1;
    raw.miss = 0;
    CHECK_EQ_INT(qd_ask_status(&g_ops, QD_PLW300, 0, &st, &raw), QD_E_OFFLINE);
    CHECK_EQ_INT(raw.miss, 0x8301);
    // Answered: none.
    fake_reset();
    raw.miss = 0xFFFF;
    CHECK_EQ_INT(qd_ask_status(&g_ops, QD_PLW300, 0, &st, &raw), QD_OK);
    CHECK_EQ_INT(raw.miss, 0);
}

static void test_power_off_while_waiting_done(void)
{
    fake_reset();
    qd_session_t *s = loaded(0, 0);
    F.silent_after_print = 1;
    CHECK_EQ_INT(qd_session_print(s), QD_E_OFFLINE);
    CHECK(f_now() - F.first_print <= 3000);
    CHECK_EQ_INT(last_state(), QDS_OFFLINE);
    qd_session_free(s);
}

static void test_status_idle(void)
{
    fake_reset();
    qd_state_t st;
    qd_raw_t raw;
    CHECK_EQ_INT(qd_ask_status(&g_ops, QD_PLW300, 0, &st, &raw), QD_OK);
    CHECK_EQ_INT(st, QDS_READY);
    CHECK_EQ_INT(raw.s01, 0x01);
    CHECK_EQ_INT(raw.s2a, 0x20);
    CHECK_EQ_INT(raw.c82, 0xA4);
    CHECK_STR(F.trace, "8301 8302 832A 8200 8401");
    // Out of paper: the Paper light.
    fake_reset();
    F.s2a = 0;
    CHECK_EQ_INT(qd_ask_status(&g_ops, QD_PLW300, 0, &st, &raw), QD_OK);
    CHECK_EQ_INT(st, QDS_NO_PAPER);
    CHECK_STR(F.trace, "8301 8302 832A 8200 8402");
    // A stored fault: 83 04 and 83 07 too.
    fake_reset();
    F.s01 = 0x02;
    CHECK_EQ_INT(qd_ask_status(&g_ops, QD_PLW300, 0, &st, &raw), QD_OK);
    CHECK_EQ_INT(st, QDS_ENGINE);
    CHECK_STR(F.trace, "8301 8302 832A 8200 8304 8307 8400");
}

static void test_end_and_abort(void)
{
    fake_reset();
    qd_session_t *s = job(0, 0);
    qd_session_abort(s);                   // never wrote: nothing
    CHECK_EQ_INT(F.tn, 0);
    qd_session_free(s);

    fake_reset();
    s = loaded(0, 0);
    F.tn = 0;
    F.trace[0] = 0;
    qd_session_abort(s);
    CHECK_STR(F.trace, "8A00 8100 8401");
    qd_session_abort(s);                   // once
    CHECK_STR(F.trace, "8A00 8100 8401");
    qd_session_free(s);
}

int main(void)
{
    test_identity_sequence();
    test_begin_refuses_other_engine();
    test_open_letter_is_apples();
    test_open_manual();
    test_rows_blink_ready();
    test_print_sequence();
    test_done_by_break();
    test_done_by_poll();
    test_8e_zero_then_status();
    test_done_timeout_85s();
    test_paper_out_before_print();
    test_paper_out_while_printing();
    test_cassette_out_while_printing();
    test_cassette_out_before_print();
    test_jam_recovery_reprints();
    test_cover_open();
    test_engine_fault_fails();
    test_memory_full_fails_page();
    test_cancel();
    test_cancel_while_waiting_for_paper();
    test_manual_never_fed_stalls_then_fails();
    test_clock_lost_mid_rows();
    test_readback_mismatch();
    test_refused_reply_retries_5_then_offline();
    test_status_names_the_unanswered_question();
    test_end_while_the_sheet_comes_out();
    test_end_after_a_manual_page_sets_the_cassette_back();
    test_power_off_while_waiting_done();
    test_status_idle();
    test_end_and_abort();
    TEST_END;
}
