// SPDX-License-Identifier: GPL-3.0-or-later
// W-1102: the Personal LaserWriter 300's conversation (qd_session), run against a printer
// scripted in this file on a fake clock, so nothing sleeps. The fake answers 82, 83 (and the
// value after 83 9D and 83 A7) and 8E as the bench's printer did, keeps a page printing for a
// while after 8A 01, and can be told to jam, open its cover, run out of paper, refuse, go
// silent or stop. Its replies carry their parity in bit 0, as the printer's do. W-1144: it holds
// the page printing and the page loading behind it, counts their codes, and counts what board
// 1's printer answered with a blank sheet: 81 00 or a second 8A 01 while a page prints, the
// two pages past its memory; and the offsets changed while a page prints, which move it. It
// records what it was sent as a trace of tokens:
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
    int memfull_once;            // 8E's bit 0 once, cleared by that read
    int drop_8e_at_done;         // the first 8E after a page done gets no answer
    int late_8e_at_done;         // ... or its answer comes just after the wait for it
    int drop_8e_alt;             // every other 8E gets no answer
    int silent_4c;               // 83 4C gets no answer
    int put_fail_once;           // the next row's put fails with put_rc, once
    uint8_t late[2];
    int late_n;
    int told;                    // the last 83 02 answered said jam or cover
    int id_l, rows_l, id_p, rows_p;   // the loading and the printing page: its rows' first code, and how many
    int out_id[16], out_rows[16], nout;   // the sheets that came out
    int into_fault;              // rows, 81 00, 84 08 or 8A 01 sent after it said so
    int just_done;
    int err_once;                // 8E's bit 1 once, cleared by that read
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
    int st_rows[64];             // the rows heard when each state came
    int nst;
    int rows, bad, puts;
    // W-1144
    uint32_t mem;                // the printer's memory: 513287
    uint32_t mem_p, mem_l;       // codes of the page printing, and of the page loading
    uint32_t mem_peak;           // the most the two came to while a page printed
    int blanked;                 // 81 00 while a page printed, or the two pages past mem
    int twice;                   // 8A 01 while a page printed
    int moved;                   // 85 to 89 while a page printed
    int stops;                   // 8A 00 while a page printed
    int rows_while;              // rows heard while a page printed
    uint32_t t_print[16], t_done[16];
    int nprint, ndone;
    struct { qd_event_t ev; int page; uint32_t ms; } ev[64];
    int nev;
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

// Page done: its room is free.
static void page_out(void)
{
    F.printing = 0;
    F.done = 1;
    if (F.nout < 16) { F.out_id[F.nout] = F.id_p; F.out_rows[F.nout++] = F.rows_p; }
    F.just_done = 1;
    F.brk_pending = 1;                  // the printer says so with a BREAK
    F.mem_p = 0;
    if (F.ndone < 16) F.t_done[F.ndone++] = f_now();
}

// The page goes on printing while nothing holds it; a jam, the cover or no paper stops it.
static void tick(void)
{
    uint8_t a = 0, b = 0;
    int np = 0;
    in_win(&a, &b, &np);
    if (F.printing && (b & 0x0C || np)) F.printing = 0;
    if (F.printing && !F.never_done && !F.done_on_brk && f_now() - F.print_start >= F.print_ms)
        page_out();
    if (F.brk_at && !F.brk_fired && f_now() >= F.brk_at) {
        F.brk_fired = 1;
        F.brk_pending = 1;
        if (F.done_on_brk && F.printing) page_out();
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
    if (F.put_fail_once && p[0] < 0x80) { F.put_fail_once = 0; return F.put_rc; }
    if (F.told && (p[0] < 0x80 || (n == 3 && ((p[0] == 0x81) || (p[0] == 0x84 && p[1] == 0x08) ||
                                              (p[0] == 0x8A && p[1] == 0x01)))))
        F.into_fault++;
    adv_us(F.put_us ? F.put_us : (uint64_t)qd_bits_len(n) * 15 / 8);
    if (F.nraw < 16 && n <= 8) memcpy(F.raw[F.nraw++], p, n);
    char t[16];
    if (p[0] < 0x80) {
        unsigned len = 0x5000u - ((unsigned)p[0] << 8 | p[1]);
        uint8_t x = 0;
        for (size_t i = 0; i + 1 < n; i++) x ^= p[i];
        if (len + 3 != n || x != p[n - 1]) F.bad++;
        F.rows++;
        if (!F.rows_l) F.id_l = p[2];
        F.rows_l++;
        F.mem_l += len;
        if (F.printing) {
            F.rows_while++;
            if (F.mem_p + F.mem_l > F.mem_peak) F.mem_peak = F.mem_p + F.mem_l;
            if (F.mem_p + F.mem_l > F.mem) F.blanked++;
        }
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
        if (p[1] == 0x4C && F.silent_4c) break;
        reply(reg(p[1]));
        if (p[1] == 0x02 && !F.silent) F.told = (reg(0x02) & 0x0C) != 0;
        break;
    case 0x8E:
        if (F.n8e < 512) F.t8e[F.n8e++] = f_now();
        {
            uint8_t st = (uint8_t)(0xF8 | (F.done ? 0x04 : 0) | (F.err_once ? 0x02 : 0) |
                                   (F.memfull || F.memfull_once ? 0x01 : 0));
            int drop = F.drop_8e_alt && (F.n8e & 1);
            int late = 0;
            if (F.just_done && (st & 0x04)) {
                F.just_done = 0;
                drop |= F.drop_8e_at_done;
                late = F.late_8e_at_done;
            }
            if (drop) break;                    // unheard: nothing is cleared
            F.done = F.memfull_once = F.err_once = 0;
            if (late) { F.late[0] = st; F.late_n = 1; break; }
            if (F.zero_first) { F.zero_first--; reply(0x00); }
            reply(st);
        }
        break;
    case 0x8A:
        if (p[1] == 0x01) {
            if (F.printing) { F.twice++; break; }
            if (!F.first_print) F.first_print = f_now() ? f_now() : 1;
            F.printing = 1;
            F.print_start = f_now();
            if (F.nprint < 16) F.t_print[F.nprint++] = f_now();
            if (F.mem_l) {                      // not a page printed again
                F.mem_p = F.mem_l;
                F.mem_l = 0;
                F.id_p = F.id_l;
                F.rows_p = F.rows_l;
                F.rows_l = 0;
            }
            if (F.silent_after_print) F.silent = 1;
        } else if (p[1] == 0x00) {
            if (F.printing) F.stops++;
            F.printing = 0;
        }
        break;
    case 0x81:
        if (F.printing) { F.blanked++; F.printing = 0; }
        F.mem_l = 0;                            // the next row starts the memory over
        F.rows_l = 0;
        break;
    case 0x85: case 0x86: case 0x87: case 0x88: case 0x89:
        if (F.printing) F.moved++;
        break;
    default: break;
    }
    return 0;
}

static int f_flush(void *ctx) { (void)ctx; return F.flush_rc; }

static int f_read(void *ctx, uint8_t *b, size_t cap, uint32_t first_ms, uint32_t gap_ms)
{
    (void)ctx; (void)gap_ms;
    if (!F.in_n) {
        adv_us((uint64_t)first_ms * 1000);
        if (F.late_n && first_ms) {             // it comes as the wait ends: too late for it
            memcpy(F.in, F.late, (size_t)F.late_n);
            F.in_n = F.late_n;
            F.late_n = 0;
        }
        return 0;
    }
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
    if (F.nst < 64) { F.st_rows[F.nst] = F.rows; F.st[F.nst++] = st; }
}
static void f_event(void *ctx, qd_event_t ev, int page, uint32_t ms)
{
    (void)ctx;
    if (F.nev < 64) {
        F.ev[F.nev].ev = ev;
        F.ev[F.nev].page = page;
        F.ev[F.nev++].ms = ms;
    }
}

static qd_ops_t g_ops = { .put = f_put, .flush = f_flush, .read = f_read, .brk = f_brk,
                          .drain = f_drain, .sleep_ms = f_sleep, .now_ms = f_now_ms,
                          .cancelled = f_cancelled, .state = f_state, .event = f_event };

// The bench's printer, idle, paper in the cassette.
static void fake_reset(void)
{
    memset(&F, 0, sizeof F);
    F.us = 1000000;                     // the clock starts away from 0
    F.engine = 0x29;
    F.c82 = 0xA4;
    F.s2a = 0x20;
    F.print_ms = 13000;
    F.mem = 513287;
    g_ops.brk = f_brk;
}

static void trace_clear(void)
{
    F.tn = 0;
    F.trace[0] = 0;
    F.run = 0;
}

// The nth event of a kind (from 0), or NULL.
static int ev_n(qd_event_t ev, int n, int *page, uint32_t *ms)
{
    for (int i = 0; i < F.nev; i++)
        if (F.ev[i].ev == ev && !n--) {
            if (page) *page = F.ev[i].page;
            if (ms) *ms = F.ev[i].ms;
            return 1;
        }
    return 0;
}

static int ev_count(qd_event_t ev)
{
    int n = 0;
    for (int i = 0; i < F.nev; i++) n += F.ev[i].ev == ev;
    return n;
}

static int ends_with(const char *tail)
{
    size_t n = strlen(tail);
    return F.tn >= n && !strcmp(F.trace + F.tn - n, tail);
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
    qd_job_cfg_t c = { .model = QD_PLW300, .manual = manual, .stall_ms = stall_ms };
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

// n rows of `size` bytes of codes each.
static int big_rows(qd_session_t *s, int n, size_t size)
{
    static const uint8_t lit[512] = { 0 };
    for (int i = 0; i < n; i++) {
        int rc = qd_session_row(s, lit, size);
        if (rc) return rc;
    }
    return 0;
}

// The next page: open, 3175 blank rows, print.
static int page(qd_session_t *s, int no)
{
    int rc = qd_session_open(s, no, letter());
    if (!rc) rc = rows(s, 3175);
    if (!rc) rc = qd_session_print(s);
    return rc;
}

// begin, open Letter, 3175 rows; the session is ready to print.
static qd_session_t *loaded(int manual, uint32_t stall_ms)
{
    qd_session_t *s = job(manual, stall_ms);
    CHECK(s != NULL);
    CHECK_EQ_INT(qd_session_begin(s), QD_OK);
    CHECK_EQ_INT(qd_session_open(s, 1, letter()), QD_OK);
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
    trace_clear();
    CHECK_EQ_INT(qd_session_open(s, 1, letter()), QD_OK);
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
    trace_clear();
    CHECK_EQ_INT(qd_session_open(s, 1, letter()), QD_OK);
    CHECK_STR(F.trace,
              "8100 8340 833E 8200 87DD 85C2 86FF 839D =21 83A7 =20 8400 88D4 89FE 834F 8401");
    qd_session_free(s);
}

static void test_rows_blink_ready(void)
{
    fake_reset();
    qd_session_t *s = job(0, 0);
    CHECK_EQ_INT(qd_session_begin(s), QD_OK);
    CHECK_EQ_INT(qd_session_open(s, 1, letter()), QD_OK);
    trace_clear();
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

// W-1144: the print sequence goes and print returns; the wait for page done is the job's end's.
static void test_print_sequence(void)
{
    fake_reset();
    qd_session_t *s = loaded(0, 0);
    int nst = F.nst;
    CHECK_EQ_INT(qd_session_print(s), QD_OK);
    CHECK(!strncmp(after("8E00"), "8E00 8200 832A 8302 8401 8B8B 8B8B 8A01", 39));
    CHECK(ends_with("8A01"));              // nothing after it: print did not wait
    CHECK_EQ_INT(F.printing, 1);
    CHECK_EQ_INT(F.nst, nst);              // PRINTING since begin, and nothing more
    CHECK_EQ_INT(last_state(), QDS_PRINTING);
    CHECK_EQ_INT(count_tok("8A01"), 1);
    int pg = 0;
    CHECK(ev_n(QD_EV_PRINT, 0, &pg, NULL) && pg == 1);
    CHECK_EQ_INT(ev_count(QD_EV_DONE), 0);
    // The end waits for its done, about 13 s on, then ends as before.
    trace_clear();
    uint32_t t0 = f_now(), ms = 0;
    CHECK_EQ_INT(qd_session_end(s), QD_OK);
    CHECK(f_now() - t0 >= 12500 && f_now() - t0 < 14000);
    CHECK(ends_with("8E00 8301 8100 8401"));
    CHECK_EQ_INT(last_state(), QDS_READY);
    CHECK(ev_n(QD_EV_DONE, 0, &pg, &ms) && pg == 1 && ms >= 12900 && ms < 13700);
    CHECK_EQ_INT(F.blanked + F.twice + F.moved + F.stops, 0);
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
    CHECK_EQ_INT(qd_session_end(s), QD_OK);
    CHECK(ends_with("8301 8100 8401"));
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
    CHECK_EQ_INT(qd_session_end(s), QD_OK);
    CHECK(ends_with("8301 834C 8100 8401"));
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
    CHECK_EQ_INT(qd_session_end(s), QD_OK);
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
    CHECK_EQ_INT(qd_session_print(s), QD_OK);
    int first = F.n8e;
    CHECK_EQ_INT(qd_session_end(s), QD_OK);
    int polls = F.n8e - first;
    CHECK(polls >= 25 && polls <= 29);     // 13 s at one every 500 ms, the first at once
    int bad = 0;
    for (int i = first + 1; i < F.n8e; i++) {
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
    qd_session_free(s);
    // And 00 then FC is done.
    fake_reset();
    s = loaded(0, 0);
    F.print_ms = 1000;
    CHECK_EQ_INT(qd_session_print(s), QD_OK);
    F.zero_first = 100;
    CHECK_EQ_INT(qd_session_end(s), QD_OK);
    qd_session_free(s);
}

static void test_done_timeout_85s(void)
{
    fake_reset();
    qd_session_t *s = loaded(0, 0);
    F.never_done = 1;
    uint32_t t0 = f_now();
    CHECK_EQ_INT(qd_session_print(s), QD_OK);
    CHECK_EQ_INT(qd_session_end(s), QD_E_FAULT);
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
    CHECK_EQ_INT(qd_session_end(s), QD_OK);
    CHECK(saw_state(QDS_NO_PAPER));
    CHECK_HAS(after("8A01"), "8A00");
    CHECK_EQ_INT(count_tok("8A01"), 2);
    CHECK_EQ_INT(last_state(), QDS_READY);
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
    CHECK_EQ_INT(qd_session_end(s), QD_OK);
    CHECK(saw_state(QDS_NO_PAPER));
    CHECK(!saw_state(QDS_ENGINE));
    CHECK_HAS(after("8A01"), "8A00");
    CHECK_EQ_INT(count_tok("8A01"), 2);
    CHECK_EQ_INT(last_state(), QDS_READY);
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
    CHECK_EQ_INT(qd_session_end(s), QD_OK);
    CHECK(saw_state(want));
    CHECK_HAS(F.trace, "8A00 8404");
    CHECK_HAS(F.trace, "8100 8408 8401 8340 833E 8200 835D 834C 8302 8100 8A01");
    CHECK_EQ_INT(count_tok("8A01"), 2);
    CHECK_EQ_INT(F.into_fault, 0);
    int pg = 0;
    CHECK(ev_n(QD_EV_REPRINT, 0, &pg, NULL) && pg == 1);
    CHECK_EQ_INT(ev_count(QD_EV_DONE), 1);
    CHECK_EQ_INT(last_state(), QDS_READY);
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
    CHECK_EQ_INT(qd_session_print(s), QD_OK);
    CHECK_EQ_INT(qd_session_end(s), QD_E_FAULT);
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
    CHECK_EQ_INT(qd_session_open(s, 1, letter()), QD_OK);
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
    // The job fails and its caller aborts: the cassette again, or the printer keeps the slot
    // and pulls in the next sheet put there (board 1, 2026-11-13).
    qd_session_abort(s);
    const char *t = F.trace + F.tn - strlen("8A00 834C 8100 8401");
    CHECK_STR(t, "8A00 834C 8100 8401");
    qd_session_free(s);
}

static void test_cancel_while_waiting_for_the_manual_slot(void)
{
    fake_reset();
    qd_session_t *s = loaded(1, 0);       // the cassette has paper, the slot none
    F.cancel_at_ms = f_now() + 4000;
    CHECK_EQ_INT(qd_session_print(s), QD_E_CANCEL);
    CHECK(saw_state(QDS_MANUAL));
    const char *t = F.trace + F.tn - strlen("8A00 834C 8100 8401");
    CHECK_STR(t, "8A00 834C 8100 8401");
    CHECK_EQ_INT(count_tok("8A01"), 0);
    qd_session_free(s);
}

static void test_clock_lost_mid_rows(void)
{
    fake_reset();
    qd_session_t *s = job(0, 0);
    CHECK_EQ_INT(qd_session_begin(s), QD_OK);
    CHECK_EQ_INT(qd_session_open(s, 1, letter()), QD_OK);
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
    CHECK_EQ_INT(qd_session_print(s), QD_OK);
    CHECK_EQ_INT(qd_session_end(s), QD_E_OFFLINE);
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
    trace_clear();
    qd_session_abort(s);
    CHECK_STR(F.trace, "8A00 8100 8401");
    qd_session_abort(s);                   // once
    CHECK_STR(F.trace, "8A00 8100 8401");
    qd_session_free(s);

    fake_reset();                          // a manual-slot job: the cassette again
    F.s2a = 0x60;
    s = loaded(1, 0);
    trace_clear();
    qd_session_abort(s);
    CHECK_STR(F.trace, "8A00 834C 8100 8401");
    qd_session_free(s);
}

// ---- W-1144: the next page loads while one prints

static const char *RECOVERY = "8100 8408 8401 8340 833E 8200 835D 834C 8302 8100 8A01";
static const char *PRINT_SEQ = "8E00 8200 832A 8302 8401 8B8B 8B8B 8A01";

static const qd_media_t *legal(void) { return &qd_spec(QD_PLW300)->media[1]; }

static int nothing_lost(void) { return F.blanked + F.twice + F.moved + F.stops + F.into_fault == 0; }

// Bench A1: only the paper source goes before the next page's rows, which go in while the
// page before prints; its print sequence goes at that page's done.
static void test_the_next_page_loads_behind_the_one_printing(void)
{
    fake_reset();
    qd_session_t *s = loaded(0, 0);
    CHECK_EQ_INT(qd_session_print(s), QD_OK);
    trace_clear();
    CHECK_EQ_INT(qd_session_open(s, 2, letter()), QD_OK);
    CHECK_STR(F.trace, "834C");
    CHECK_EQ_INT(rows(s, 3175), QD_OK);
    CHECK_EQ_INT(F.rows_while, 3175);          // every row while page 1 printed
    CHECK_EQ_INT(F.ndone, 0);
    CHECK_EQ_INT(qd_session_print(s), QD_OK);  // waits for page 1's done, then prints
    CHECK_EQ_INT(F.ndone, 1);
    CHECK_EQ_INT(F.nprint, 2);
    CHECK(F.t_print[1] >= F.t_done[0] && F.t_print[1] - F.t_done[0] < 200);
    CHECK_EQ_INT(count_tok("8100"), 0);        // nothing started the memory over
    CHECK_HAS(F.trace, PRINT_SEQ);
    CHECK(ends_with("8A01"));
    CHECK(F.nev == 3 && F.ev[0].ev == QD_EV_PRINT && F.ev[1].ev == QD_EV_DONE &&
          F.ev[1].page == 1 && F.ev[2].ev == QD_EV_PRINT && F.ev[2].page == 2);
    CHECK_EQ_INT(qd_session_end(s), QD_OK);
    CHECK_EQ_INT(F.ndone, 2);
    uint32_t apart = F.t_done[1] - F.t_done[0];
    CHECK(apart >= 13000 && apart < 13300);    // a print apart, not a load and a print
    CHECK(nothing_lost());
    CHECK_EQ_INT(F.bad, 0);
    qd_session_free(s);
}

// Five pages: each done a print's time after the one before, and nothing the printer answers
// with a blank or a moved sheet: no 81 00 and no offsets between a page's 8A 01 and its done.
static void test_five_pages_a_print_apart(void)
{
    fake_reset();
    qd_session_t *s = job(0, 0);
    CHECK_EQ_INT(qd_session_begin(s), QD_OK);
    for (int i = 1; i <= 5; i++) CHECK_EQ_INT(page(s, i), QD_OK);
    CHECK_EQ_INT(qd_session_end(s), QD_OK);
    CHECK_EQ_INT(F.ndone, 5);
    for (int i = 1; i < 5; i++) {
        uint32_t apart = F.t_done[i] - F.t_done[i - 1];
        CHECK(apart >= 13000 && apart < 13300);
    }
    CHECK_EQ_INT(count_tok("8100"), 3);        // the job's begin, the first page, the end
    CHECK_EQ_INT(count_tok("87DD"), 1);        // the offsets once
    CHECK_EQ_INT(count_tok("834C"), 5);
    CHECK(nothing_lost());
    int pg = 0;
    CHECK(ev_n(QD_EV_DONE, 4, &pg, NULL) && pg == 5);
    CHECK_EQ_INT(ev_count(QD_EV_ROOM), 0);
    CHECK_EQ_INT(last_state(), QDS_READY);
    qd_session_free(s);
}

// 8E's page done is cleared by the read: a done read between the next page's rows is the
// printing page's, and the print that follows does not wait for another.
static void test_a_done_between_rows_is_not_lost(void)
{
    fake_reset();
    qd_session_t *s = loaded(0, 0);
    CHECK_EQ_INT(qd_session_print(s), QD_OK);
    CHECK_EQ_INT(qd_session_open(s, 2, letter()), QD_OK);
    int n8e = F.n8e;
    F.put_us = 6000;                           // 19 s of rows: page 1 is done 13 s in
    CHECK_EQ_INT(rows(s, 3175), QD_OK);
    CHECK_EQ_INT(F.ndone, 1);
    CHECK_EQ_INT(F.done, 0);                   // read, between rows
    CHECK_EQ_INT(ev_count(QD_EV_DONE), 1);
    int polls = F.n8e - n8e;
    CHECK(polls >= 20 && polls <= 28);         // one about every 500 ms for 13 s, then none
    uint32_t t0 = f_now();
    CHECK_EQ_INT(qd_session_print(s), QD_OK);
    CHECK(f_now() - t0 < 100);                 // at once: nothing prints
    CHECK_EQ_INT(F.nprint, 2);
    CHECK_EQ_INT(qd_session_end(s), QD_OK);
    CHECK(nothing_lost());
    qd_session_free(s);
}

// Bench A2b: the two pages past the printer's memory while one prints, and both sheets are
// blank. A row that would take them past 500,000 bytes waits for the printing page's done.
static void test_a_row_waits_for_room(void)
{
    fake_reset();
    qd_session_t *s = job(0, 0);
    CHECK_EQ_INT(qd_session_begin(s), QD_OK);
    CHECK_EQ_INT(qd_session_open(s, 1, letter()), QD_OK);
    CHECK_EQ_INT(big_rows(s, 1000, 300), QD_OK);       // 300,000 bytes
    CHECK_EQ_INT(qd_session_print(s), QD_OK);
    CHECK_EQ_INT(qd_session_open(s, 2, letter()), QD_OK);
    CHECK_EQ_INT(big_rows(s, 1000, 300), QD_OK);
    CHECK(F.mem_peak <= 500000 && F.mem_peak > 499000); // right up to the room, and no further
    CHECK_EQ_INT(F.rows_while, 666);
    CHECK_EQ_INT(F.ndone, 1);
    int pg = 0;
    uint32_t ms = 0;
    CHECK(ev_n(QD_EV_ROOM, 0, &pg, &ms) && pg == 2);
    CHECK(ms > 7000 && ms < 10000);            // 13 s less the 4.3 s the first 666 rows took
    CHECK_EQ_INT(ev_count(QD_EV_ROOM), 1);
    CHECK_EQ_INT(qd_session_print(s), QD_OK);
    CHECK_EQ_INT(qd_session_end(s), QD_OK);
    CHECK(nothing_lost());
    qd_session_free(s);

    // The room is the job's to set (qdhost --room).
    fake_reset();
    qd_job_cfg_t c = { .model = QD_PLW300, .room = 5000 };
    s = qd_session_new(&g_ops, &c);
    CHECK_EQ_INT(qd_session_begin(s), QD_OK);
    CHECK_EQ_INT(page(s, 1), QD_OK);
    CHECK_EQ_INT(page(s, 2), QD_OK);
    CHECK_EQ_INT(ev_count(QD_EV_ROOM), 1);
    CHECK(F.mem_peak <= 5000);
    qd_session_free(s);
}

// Memory full said between rows (8E clears as it is read on some bits): the page never prints.
static void test_memory_full_between_rows_is_kept(void)
{
    for (int bit = 0; bit < 2; bit++) {
        fake_reset();
        qd_session_t *s = loaded(0, 0);
        CHECK_EQ_INT(qd_session_print(s), QD_OK);
        CHECK_EQ_INT(qd_session_open(s, 2, letter()), QD_OK);
        F.put_us = 2000;
        if (bit) F.err_once = 1; else F.memfull_once = 1;
        CHECK_EQ_INT(rows(s, 3175), QD_OK);
        CHECK_EQ_INT(F.memfull_once + F.err_once, 0);  // read between rows, and cleared
        CHECK_EQ_INT(qd_session_print(s), QD_E_FAULT);
        CHECK_EQ_INT(F.nprint, 1);
        qd_session_free(s);
    }
}

// Bench A3: a sheet stopped as it prints comes out blank. Cancel lets it come out, then stops.
static void test_cancel_lets_the_sheet_printing_come_out(void)
{
    fake_reset();
    qd_session_t *s = loaded(0, 0);
    CHECK_EQ_INT(qd_session_print(s), QD_OK);
    CHECK_EQ_INT(qd_session_open(s, 2, letter()), QD_OK);
    F.cancel_rows = F.rows + 640;
    CHECK_EQ_INT(rows(s, 3175), QD_E_CANCEL);
    CHECK_EQ_INT(F.rows, 3175 + 640);          // no more rows
    CHECK_EQ_INT(F.ndone, 1);                  // page 1 came out
    CHECK(f_now() - F.t_print[0] >= 13000 && f_now() - F.t_print[0] < 13300);
    CHECK(nothing_lost());                     // no 8A 00 and no 81 00 while it printed
    CHECK(ends_with("8E00 8A00 8100 8401"));
    CHECK_EQ_INT(F.nprint, 1);
    size_t tn = F.tn;
    qd_session_abort(s);                       // nothing more
    CHECK_EQ_INT(F.tn, tn);
    qd_session_free(s);

    // The same from the wait for the page before: page 2 is loaded and never prints.
    fake_reset();
    s = loaded(0, 0);
    CHECK_EQ_INT(qd_session_print(s), QD_OK);
    CHECK_EQ_INT(qd_session_open(s, 2, letter()), QD_OK);
    CHECK_EQ_INT(rows(s, 3175), QD_OK);
    F.cancel_at_ms = f_now() + 2000;
    CHECK_EQ_INT(qd_session_print(s), QD_E_CANCEL);
    CHECK_EQ_INT(F.ndone, 1);
    CHECK_EQ_INT(F.nprint, 1);
    CHECK(nothing_lost());
    CHECK(ends_with("8E00 8A00 8100 8401"));
    qd_session_free(s);
}

// A page printing that a jam has stopped has nothing to wait for: Cancel stops at once, and
// only the stop goes: no 81 00 into a jam (bench A4e).
static void test_cancel_with_the_page_printing_jammed_stops_at_once(void)
{
    fake_reset();
    qd_session_t *s = loaded(0, 0);
    CHECK_EQ_INT(qd_session_print(s), QD_OK);
    F.win_from_print = 1;
    F.win[F.nwin++] = (win_t){ 1000, 100000, 0, 0x08, 0 };
    CHECK_EQ_INT(qd_session_open(s, 2, letter()), QD_OK);
    F.put_us = 3000;
    F.cancel_at_ms = F.first_print + 4000;
    CHECK_EQ_INT(rows(s, 3175), QD_E_CANCEL);
    CHECK(f_now() - F.first_print < 4700);
    CHECK(saw_state(QDS_JAM));
    CHECK_EQ_INT(F.ndone, 0);
    CHECK(ends_with("8302 8A00"));
    CHECK_EQ_INT(count_tok("835D"), 0);        // no recovery
    CHECK_EQ_INT(F.into_fault, 0);
    qd_session_free(s);

    // And from the wait itself: the jam comes while Cancel waits for the sheet.
    fake_reset();
    s = loaded(0, 0);
    CHECK_EQ_INT(qd_session_print(s), QD_OK);
    F.win_from_print = 1;
    F.win[F.nwin++] = (win_t){ 5000, 100000, 0, 0x08, 0 };
    CHECK_EQ_INT(qd_session_open(s, 2, letter()), QD_OK);
    F.cancel_rows = F.rows + 64;
    CHECK_EQ_INT(rows(s, 3175), QD_E_CANCEL);
    CHECK(f_now() - F.first_print >= 5000 && f_now() - F.first_print < 6000);
    CHECK_EQ_INT(count_tok("835D"), 0);
    CHECK(ends_with("8302 8A00"));
    CHECK_EQ_INT(F.into_fault, 0);
    qd_session_free(s);
}

// Bench A5: the offsets are read as a page prints. A page on another sheet waits for the
// done, then has the whole setup.
static void test_another_sheet_waits_for_the_done(void)
{
    fake_reset();
    qd_session_t *s = loaded(0, 0);
    CHECK_EQ_INT(qd_session_print(s), QD_OK);
    trace_clear();
    CHECK_EQ_INT(qd_session_open(s, 2, legal()), QD_OK);
    CHECK_EQ_INT(F.ndone, 1);
    CHECK(nothing_lost());
    CHECK_HAS(F.trace, "8E00 8100 8340 833E 8200 87");
    CHECK_EQ_INT(count_tok("834C"), 1);
    CHECK_EQ_INT(rows(s, 3750), QD_OK);
    CHECK_EQ_INT(F.rows_while, 0);
    CHECK_EQ_INT(qd_session_print(s), QD_OK);
    // And the same sheet again after it loads behind.
    trace_clear();
    CHECK_EQ_INT(qd_session_open(s, 3, legal()), QD_OK);
    CHECK_STR(F.trace, "834C");
    CHECK_EQ_INT(rows(s, 3750), QD_OK);
    CHECK_EQ_INT(qd_session_print(s), QD_OK);
    CHECK_EQ_INT(qd_session_end(s), QD_OK);
    CHECK_EQ_INT(F.ndone, 3);
    CHECK(nothing_lost());
    qd_session_free(s);
}

// A page too big in the record's screen is opened again for Apple's (qd_page.h). Its first
// rows are behind the page printing: that page is done first, then 81 00 drops them.
static void test_a_page_opened_again_while_one_prints(void)
{
    fake_reset();
    qd_session_t *s = loaded(0, 0);
    CHECK_EQ_INT(qd_session_print(s), QD_OK);
    CHECK_EQ_INT(qd_session_open(s, 2, letter()), QD_OK);
    CHECK_EQ_INT(rows(s, 1000), QD_OK);
    trace_clear();
    CHECK_EQ_INT(qd_session_open(s, 2, letter()), QD_OK);
    CHECK_EQ_INT(F.ndone, 1);
    CHECK(nothing_lost());
    CHECK(ends_with(OPEN_LETTER));
    CHECK_EQ_INT(F.mem_l, 0);                  // the first rows are gone
    CHECK_EQ_INT(rows(s, 3175), QD_OK);
    CHECK_EQ_INT(qd_session_print(s), QD_OK);
    CHECK_EQ_INT(F.mem_p, 3175);
    int pg = 0;
    CHECK(ev_n(QD_EV_PRINT, 1, &pg, NULL) && pg == 2);
    // A page that fit neither screen is never printed; the next page's open drops its rows.
    CHECK_EQ_INT(qd_session_open(s, 3, letter()), QD_OK);
    CHECK_EQ_INT(rows(s, 500), QD_OK);
    trace_clear();
    CHECK_EQ_INT(qd_session_open(s, 4, letter()), QD_OK);
    CHECK_EQ_INT(F.ndone, 2);
    CHECK(ends_with(OPEN_LETTER));
    CHECK_EQ_INT(rows(s, 3175), QD_OK);
    CHECK_EQ_INT(qd_session_print(s), QD_OK);
    CHECK(ev_n(QD_EV_PRINT, 2, &pg, NULL) && pg == 4);
    CHECK_EQ_INT(qd_session_end(s), QD_OK);
    CHECK(nothing_lost());
    qd_session_free(s);
}

// Bench A4c: the cover opened as page 1 printed, with page 2 loaded behind it. Apple's
// recovery printed page 1 again, and page 2's own print sequence then printed page 2.
static void test_jam_on_the_first_page_with_the_next_loaded(void)
{
    fake_reset();
    qd_session_t *s = loaded(0, 0);
    CHECK_EQ_INT(qd_session_print(s), QD_OK);
    F.win_from_print = 1;
    F.win[F.nwin++] = (win_t){ 4000, 7000, 0x12, 0x0C, 0 };
    CHECK_EQ_INT(qd_session_open(s, 2, letter()), QD_OK);
    CHECK_EQ_INT(rows(s, 3175), QD_OK);        // 2.6 s: all in before the cover opens
    CHECK(!saw_state(QDS_JAM));
    CHECK_EQ_INT(qd_session_print(s), QD_OK);
    CHECK(saw_state(QDS_JAM));
    CHECK_HAS(F.trace, RECOVERY);
    CHECK_HAS(after(RECOVERY), PRINT_SEQ);     // page 2's own, after page 1 is done again
    CHECK_EQ_INT(F.nprint, 3);
    CHECK_EQ_INT(F.twice, 0);
    CHECK_EQ_INT(F.rows, 2 * 3175);            // no row sent again
    CHECK(F.nev == 4 && F.ev[1].ev == QD_EV_REPRINT && F.ev[1].page == 1 &&
          F.ev[2].ev == QD_EV_DONE && F.ev[2].page == 1 &&
          F.ev[3].ev == QD_EV_PRINT && F.ev[3].page == 2);
    CHECK_EQ_INT(last_state(), QDS_PRINTING);
    // The recovery left the printer at the start of its memory with page 2 behind: page 3
    // waits for page 2's done and starts the memory over.
    trace_clear();
    CHECK_EQ_INT(qd_session_open(s, 3, letter()), QD_OK);
    CHECK_EQ_INT(F.ndone, 2);
    CHECK(ends_with(OPEN_LETTER));
    CHECK_EQ_INT(rows(s, 3175), QD_OK);
    CHECK_EQ_INT(qd_session_print(s), QD_OK);
    // Page 3 is the first since that 81 00: a jam on it is recovered too, and page 4 loads behind.
    trace_clear();
    CHECK_EQ_INT(qd_session_open(s, 4, letter()), QD_OK);
    CHECK_STR(F.trace, "834C");
    CHECK_EQ_INT(qd_session_end(s), QD_OK);
    CHECK_EQ_INT(F.blanked + F.twice + F.moved, 0);
    qd_session_free(s);
}

// The jam comes while page 2's rows go in. The rows stop at the next status between rows
// (the printer was never tried taking rows with a jam showing), page 1 is recovered there,
// and page 2, whose first rows the recovery's 81 00 left stranded, is lost: the job stops.
static void test_jam_seen_between_rows(void)
{
    fake_reset();
    qd_session_t *s = loaded(0, 0);
    CHECK_EQ_INT(qd_session_print(s), QD_OK);
    F.win_from_print = 1;
    F.win[F.nwin++] = (win_t){ 2000, 9000, 0x12, 0x08, 0 };
    CHECK_EQ_INT(qd_session_open(s, 2, letter()), QD_OK);
    F.put_us = 4000;                           // 12.7 s of rows
    trace_clear();
    CHECK_EQ_INT(rows(s, 3175), QD_E_FAULT);
    int at = -1;
    for (int i = 0; i < F.nst; i++) if (F.st[i] == QDS_JAM) at = F.st_rows[i] - 3175;
    CHECK(at >= 440 && at <= 640);             // within 600 ms of the jam, 2 s (some 480 rows) in
    CHECK_EQ_INT(F.rows - 3175, at);           // and not a row after it
    CHECK_HAS(F.trace, "8E00 8301 8302 8A00 8404");
    CHECK_HAS(F.trace, RECOVERY);
    CHECK_EQ_INT(F.into_fault, 0);
    int pg = 0;
    CHECK(ev_n(QD_EV_REPRINT, 0, &pg, NULL) && pg == 1);
    CHECK(ev_n(QD_EV_LOST, 0, &pg, NULL) && pg == 2);
    qd_session_free(s);

    // Seen before any of page 2's rows went: nothing is stranded. Page 1 prints again, and
    // page 2 starts the memory over once it is done.
    fake_reset();
    s = loaded(0, 0);
    CHECK_EQ_INT(qd_session_print(s), QD_OK);
    F.win_from_print = 1;
    F.win[F.nwin++] = (win_t){ 0, 5000, 0x12, 0x08, 0 };
    CHECK_EQ_INT(qd_session_open(s, 2, letter()), QD_OK);
    f_sleep(NULL, 600);                        // the rip takes a while over its first row
    trace_clear();
    CHECK_EQ_INT(rows(s, 3175), QD_OK);
    CHECK_HAS(F.trace, RECOVERY);
    CHECK_EQ_INT(F.ndone, 1);
    CHECK_EQ_INT(F.rows, 2 * 3175);
    CHECK_HAS(F.trace, "8100 8340 833E 8200 87DD");
    CHECK_EQ_INT(qd_session_print(s), QD_OK);
    CHECK_EQ_INT(qd_session_end(s), QD_OK);
    CHECK_EQ_INT(F.ndone, 2);
    CHECK_EQ_INT(F.blanked + F.twice + F.moved + F.into_fault, 0);
    qd_session_free(s);
}

// The cover opened while a job's first page loads, nothing printing: the rows wait until it
// has stayed closed, then go on. No recovery: nothing was printing.
static void test_cover_while_the_first_page_loads(void)
{
    fake_reset();
    qd_session_t *s = job(0, 0);
    CHECK_EQ_INT(qd_session_begin(s), QD_OK);
    CHECK_EQ_INT(qd_session_open(s, 1, letter()), QD_OK);
    uint32_t t0 = f_now();
    F.win[F.nwin++] = (win_t){ t0 + 1000, t0 + 5000, 0x02, 0x04, 0 };
    F.put_us = 2000;
    CHECK_EQ_INT(rows(s, 3175), QD_OK);
    CHECK(saw_state(QDS_COVER));
    CHECK(f_now() - t0 >= 6350 + 4000 + 3000); // the rows, the cover, and 3 s closed
    CHECK_EQ_INT(F.into_fault, 0);
    CHECK_EQ_INT(count_tok("835D"), 0);
    CHECK_EQ_INT(count_tok("8100"), 2);        // begin's and the setup's
    CHECK_EQ_INT(last_state(), QDS_PRINTING);
    CHECK_EQ_INT(qd_session_print(s), QD_OK);
    CHECK_EQ_INT(qd_session_end(s), QD_OK);
    CHECK_EQ_INT(F.ndone, 1);
    qd_session_free(s);
}

// Bench A4d: with a second sheet in the path the printer read clear once after the cover
// closed, then jam again. The recovery waits until 83 02 has stayed clear 3 s and nothing
// moves; nothing but the question goes before that.
static void test_recovery_waits_for_the_printer_to_stay_clear(void)
{
    fake_reset();
    qd_session_t *s = loaded(0, 0);
    CHECK_EQ_INT(qd_session_print(s), QD_OK);
    F.win_from_print = 1;
    F.win[F.nwin++] = (win_t){ 2000, 6000, 0x12, 0x0C, 0 };
    F.win[F.nwin++] = (win_t){ 7000, 12000, 0x02, 0x08, 0 };    // clear for 1 s, then a jam again
    F.win[F.nwin++] = (win_t){ 12000, 18000, 0x20, 0, 0 };      // then the sheet goes out
    CHECK_EQ_INT(qd_session_end(s), QD_OK);
    CHECK_EQ_INT(count_tok("8408"), 1);        // one recovery, not one a clear read
    CHECK_EQ_INT(F.nprint, 2);
    CHECK(F.t_print[1] - F.first_print >= 18000 && F.t_print[1] - F.first_print < 19500);
    CHECK_EQ_INT(F.into_fault, 0);
    CHECK_EQ_INT(F.ndone, 1);
    qd_session_free(s);

    // The page's own print sequence waits out a jam too: 8A 01 sent into one is kept by the
    // printer and runs when the jam clears (bench A4e).
    fake_reset();
    s = loaded(0, 0);
    uint32_t t0 = f_now();
    F.win[F.nwin++] = (win_t){ t0, t0 + 4000, 0x02, 0x08, 0 };
    CHECK_EQ_INT(qd_session_print(s), QD_OK);
    CHECK(saw_state(QDS_JAM));
    CHECK(F.t_print[0] - t0 >= 7000);
    CHECK_EQ_INT(F.into_fault, 0);
    CHECK_EQ_INT(last_state(), QDS_PRINTING);
    qd_session_free(s);
}

// The printer's recovery prints from the start of its memory: a page that is not the first
// since the last 81 00 can't be had back. The job stops and the page is named.
static void test_jam_on_a_later_page_stops_the_job(void)
{
    fake_reset();
    qd_session_t *s = loaded(0, 0);
    CHECK_EQ_INT(qd_session_print(s), QD_OK);
    CHECK_EQ_INT(page(s, 2), QD_OK);           // prints at page 1's done, 13 s on
    F.win_from_print = 1;
    F.win[F.nwin++] = (win_t){ 17000, 40000, 0x12, 0x08, 0 };
    CHECK_EQ_INT(qd_session_open(s, 3, letter()), QD_OK);
    CHECK_EQ_INT(rows(s, 3175), QD_OK);
    CHECK_EQ_INT(qd_session_print(s), QD_E_FAULT);
    int pg = 0;
    CHECK(ev_n(QD_EV_LOST, 0, &pg, NULL) && pg == 2);
    CHECK_EQ_INT(last_state(), QDS_JAM);
    CHECK_EQ_INT(count_tok("835D"), 0);        // no recovery: it would print page 1 again
    CHECK_EQ_INT(F.nprint, 2);
    CHECK(ends_with("8A00 8404"));
    trace_clear();
    qd_session_abort(s);
    CHECK_STR(F.trace, "8A00");                // and nothing else into the jam
    CHECK_EQ_INT(F.into_fault, 0);
    qd_session_free(s);
}

// A jam on the first page while the next waits for room: the recovery's 81 00 takes the
// printer back to the start of its memory, so the rest of that page's rows can't follow the
// rows already in. The job stops, naming it.
static void test_jam_while_a_page_waits_for_room(void)
{
    fake_reset();
    qd_session_t *s = job(0, 0);
    CHECK_EQ_INT(qd_session_begin(s), QD_OK);
    CHECK_EQ_INT(qd_session_open(s, 1, letter()), QD_OK);
    CHECK_EQ_INT(big_rows(s, 1000, 300), QD_OK);
    CHECK_EQ_INT(qd_session_print(s), QD_OK);
    F.win_from_print = 1;
    F.win[F.nwin++] = (win_t){ 8000, 10000, 0x12, 0x08, 0 };
    CHECK_EQ_INT(qd_session_open(s, 2, letter()), QD_OK);
    CHECK_EQ_INT(big_rows(s, 1000, 300), QD_E_FAULT);
    CHECK_EQ_INT(F.rows, 1666);
    CHECK_EQ_INT(F.ndone, 1);                  // page 1 printed again, and is out
    int pg = 0;
    CHECK(ev_n(QD_EV_REPRINT, 0, &pg, NULL) && pg == 1);
    CHECK(ev_n(QD_EV_LOST, 0, &pg, NULL) && pg == 2);
    qd_session_free(s);
}

// The cassette runs out under the page printing while the next page's rows go in.
static void test_paper_out_with_the_next_page_loading(void)
{
    fake_reset();
    qd_session_t *s = loaded(0, 0);
    CHECK_EQ_INT(qd_session_print(s), QD_OK);
    F.win_from_print = 1;
    F.win[F.nwin++] = (win_t){ 2000, 8000, 0x02, 0x10, 1 };
    CHECK_EQ_INT(qd_session_open(s, 2, letter()), QD_OK);
    F.put_us = 4000;
    trace_clear();
    CHECK_EQ_INT(rows(s, 3175), QD_OK);
    int at = -1;
    for (int i = 0; i < F.nst; i++) if (F.st[i] == QDS_NO_PAPER) at = F.st_rows[i] - 3175;
    CHECK(at >= 440 && at <= 640);             // shown while the rows went in
    CHECK(!saw_state(QDS_ENGINE));
    CHECK_HAS(F.trace, "832A 8A00 832A 8302 8402");
    CHECK_EQ_INT(F.rows, 2 * 3175);
    CHECK_EQ_INT(F.nprint, 2);                 // page 1 again as soon as the paper was back
    CHECK_EQ_INT(qd_session_print(s), QD_OK);  // page 1 again with paper, its done, then page 2
    CHECK_EQ_INT(F.nprint, 3);
    CHECK_EQ_INT(F.ndone, 1);
    CHECK_EQ_INT(F.twice, 0);
    CHECK_EQ_INT(last_state(), QDS_PRINTING);
    CHECK_EQ_INT(qd_session_end(s), QD_OK);
    CHECK_EQ_INT(F.ndone, 2);
    CHECK_EQ_INT(count_tok("835D"), 0);
    qd_session_free(s);
}

static void test_engine_fault_between_rows(void)
{
    fake_reset();
    qd_session_t *s = loaded(0, 0);
    CHECK_EQ_INT(qd_session_print(s), QD_OK);
    F.win_from_print = 1;
    F.win[F.nwin++] = (win_t){ 2000, 100000, 0x02, 0, 0 };
    F.s04 = 0x10;
    CHECK_EQ_INT(qd_session_open(s, 2, letter()), QD_OK);
    F.put_us = 4000;
    CHECK_EQ_INT(rows(s, 3175), QD_E_FAULT);
    CHECK(F.rows - 3175 >= 440 && F.rows - 3175 <= 640);
    CHECK_EQ_INT(last_state(), QDS_ENGINE);
    // The printer's failure: abort stops at once, as before.
    trace_clear();
    uint32_t t0 = f_now();
    qd_session_abort(s);
    CHECK_STR(F.trace, "8A00 8100 8401");
    CHECK(f_now() - t0 < 100);
    qd_session_free(s);
}

static void test_switched_off_between_rows(void)
{
    fake_reset();
    qd_session_t *s = loaded(0, 0);
    CHECK_EQ_INT(qd_session_print(s), QD_OK);
    CHECK_EQ_INT(qd_session_open(s, 2, letter()), QD_OK);
    F.put_us = 4000;
    F.silent = 1;
    uint32_t t0 = f_now();
    CHECK_EQ_INT(rows(s, 3175), QD_E_OFFLINE);
    CHECK(f_now() - t0 < 4000);
    CHECK_EQ_INT(last_state(), QDS_OFFLINE);
    qd_session_free(s);
}

// The rip gives up (an upload cut off, a page size refused, no memory) with a page printing:
// the session has no error of its own, and abort is as a cancel.
static void test_abort_lets_the_sheet_printing_come_out(void)
{
    fake_reset();
    qd_session_t *s = loaded(0, 0);
    CHECK_EQ_INT(qd_session_print(s), QD_OK);
    CHECK_EQ_INT(qd_session_open(s, 2, letter()), QD_OK);
    CHECK_EQ_INT(rows(s, 100), QD_OK);
    trace_clear();
    qd_session_abort(s);
    CHECK_EQ_INT(F.ndone, 1);
    CHECK(nothing_lost());
    CHECK(ends_with("8E00 8A00 8100 8401"));
    size_t tn = F.tn;
    qd_session_abort(s);                       // once
    CHECK_EQ_INT(F.tn, tn);
    qd_session_free(s);

    // A jam as it waits: stopped there, once.
    fake_reset();
    s = loaded(0, 0);
    CHECK_EQ_INT(qd_session_print(s), QD_OK);
    F.win_from_print = 1;
    F.win[F.nwin++] = (win_t){ 3000, 100000, 0x12, 0x08, 0 };
    qd_session_abort(s);
    CHECK(f_now() - F.first_print < 4000);
    CHECK_EQ_INT(count_tok("8100"), 2);        // begin's and the page's setup: none into the jam
    CHECK(ends_with("8302 8A00"));
    CHECK_EQ_INT(F.into_fault, 0);
    qd_session_free(s);
}

// A chunk read back wrong, or the paper source's answer missed as a page opens, is the
// line's trouble: the sheet printing is sound and comes out before the job is given up.
static void test_a_line_error_lets_the_sheet_printing_come_out(void)
{
    fake_reset();
    qd_session_t *s = loaded(0, 0);
    CHECK_EQ_INT(qd_session_print(s), QD_OK);
    CHECK_EQ_INT(qd_session_open(s, 2, letter()), QD_OK);
    CHECK_EQ_INT(rows(s, 100), QD_OK);
    F.put_fail_once = 1;
    F.put_rc = QD_E_LINK;
    CHECK_EQ_INT(rows(s, 100), QD_E_LINK);
    qd_session_abort(s);
    CHECK_EQ_INT(F.ndone, 1);
    CHECK(nothing_lost());
    CHECK(ends_with("8E00 8A00 8100 8401"));
    qd_session_free(s);

    fake_reset();
    s = loaded(0, 0);
    CHECK_EQ_INT(qd_session_print(s), QD_OK);
    F.silent_4c = 1;
    int nst = F.nst;
    CHECK_EQ_INT(qd_session_open(s, 2, letter()), QD_E_OFFLINE);
    CHECK_EQ_INT(F.nst, nst);                  // the printer is not said to be offline
    qd_session_abort(s);
    CHECK_EQ_INT(F.ndone, 1);
    CHECK(nothing_lost());
    CHECK(ends_with("8E00 8A00 8100 8401"));
    qd_session_free(s);

    // No clock is the printer's: at once, as before.
    fake_reset();
    s = loaded(0, 0);
    CHECK_EQ_INT(qd_session_print(s), QD_OK);
    CHECK_EQ_INT(qd_session_open(s, 2, letter()), QD_OK);
    F.put_fail_once = 1;
    F.put_rc = QD_E_OFFLINE;
    CHECK_EQ_INT(rows(s, 100), QD_E_OFFLINE);
    uint32_t t0 = f_now();
    qd_session_abort(s);
    CHECK(f_now() - t0 < 100);
    CHECK_EQ_INT(F.ndone, 0);
    qd_session_free(s);
}

// One poll in two unanswered is a poor line, not a printer switched off: any answer ends a
// run of misses.
static void test_misses_are_counted_in_a_row(void)
{
    fake_reset();
    qd_session_t *s = loaded(0, 0);
    CHECK_EQ_INT(qd_session_print(s), QD_OK);
    F.drop_8e_alt = 1;
    CHECK_EQ_INT(qd_session_open(s, 2, letter()), QD_OK);
    F.put_us = 4000;
    CHECK_EQ_INT(rows(s, 3175), QD_OK);
    CHECK_EQ_INT(qd_session_print(s), QD_OK);
    CHECK_EQ_INT(qd_session_end(s), QD_OK);
    CHECK_EQ_INT(F.ndone, 2);
    CHECK(!saw_state(QDS_OFFLINE));
    qd_session_free(s);
}

// A Cancel that comes while the last page prints: its sheet comes out, and the job ends
// cancelled, not done.
static void test_cancel_as_the_last_page_prints(void)
{
    fake_reset();
    qd_session_t *s = loaded(0, 0);
    CHECK_EQ_INT(qd_session_print(s), QD_OK);
    F.cancel_at_ms = f_now() + 3000;
    CHECK_EQ_INT(qd_session_end(s), QD_E_CANCEL);
    CHECK_EQ_INT(F.ndone, 1);
    CHECK(nothing_lost());
    CHECK(ends_with("8E00 8A00 8100 8401"));
    qd_session_free(s);
}

// 8E's page done is said once. The printer's BREAK with no answer to the 8E after it: asked
// again at once. An answer that comes just after the wait for it is taken, not thrown away
// by the next question's drain.
static void test_a_done_is_not_lost_to_a_missed_answer(void)
{
    fake_reset();
    qd_session_t *s = loaded(0, 0);
    CHECK_EQ_INT(qd_session_print(s), QD_OK);
    F.drop_8e_at_done = 1;
    CHECK_EQ_INT(qd_session_end(s), QD_OK);
    uint32_t d = F.t8e[F.n8e - 1] - F.t8e[F.n8e - 2];
    CHECK(d >= 1000 && d < 1100);              // the wait for the answer, and no half second more
    CHECK(!saw_state(QDS_OFFLINE));
    qd_session_free(s);

    fake_reset();
    s = loaded(0, 0);
    CHECK_EQ_INT(qd_session_print(s), QD_OK);
    F.late_8e_at_done = 1;
    uint32_t t0 = f_now();
    CHECK_EQ_INT(qd_session_end(s), QD_OK);    // not 85 s later: the late answer was the done
    CHECK(f_now() - t0 < 15000);
    CHECK_EQ_INT(ev_count(QD_EV_DONE), 1);
    qd_session_free(s);

    // Between rows too.
    fake_reset();
    s = loaded(0, 0);
    CHECK_EQ_INT(qd_session_print(s), QD_OK);
    F.late_8e_at_done = 1;
    CHECK_EQ_INT(qd_session_open(s, 2, letter()), QD_OK);
    F.put_us = 6000;
    CHECK_EQ_INT(rows(s, 3175), QD_OK);
    CHECK_EQ_INT(ev_count(QD_EV_DONE), 1);
    uint32_t t1 = f_now();
    CHECK_EQ_INT(qd_session_print(s), QD_OK);
    CHECK(f_now() - t1 < 100);
    qd_session_free(s);
}

// ---- W-1144: the copy of what is in the printer, sent again after a jam

static uint8_t g_copy[QD_COPY_CAP];

static qd_session_t *copy_job(size_t cap)
{
    memset(g_copy, 0xEE, sizeof g_copy);
    qd_job_cfg_t c = { .model = QD_PLW300, .copy = g_copy, .copy_cap = cap };
    qd_session_t *s = qd_session_new(&g_ops, &c);
    CHECK(s != NULL);
    CHECK_EQ_INT(qd_session_begin(s), QD_OK);
    return s;
}

// n rows of `size` codes, each the page's number.
static int rows_of(qd_session_t *s, int no, int n, size_t size)
{
    uint8_t codes[512];
    memset(codes, no, sizeof codes);
    for (int i = 0; i < n; i++) {
        int rc = qd_session_row(s, codes, size);
        if (rc) return rc;
    }
    return 0;
}

static int page_of(qd_session_t *s, int no, int n, size_t size)
{
    int rc = qd_session_open(s, no, letter());
    if (!rc) rc = rows_of(s, no, n, size);
    if (!rc) rc = qd_session_print(s);
    return rc;
}

// The sheets that came out: "1:100 2:100".
static const char *sheets(void)
{
    static char t[256];
    size_t n = 0;
    t[0] = 0;
    for (int i = 0; i < F.nout; i++)
        n += (size_t)snprintf(t + n, sizeof t - n, "%s%d:%d", i ? " " : "", F.out_id[i], F.out_rows[i]);
    return t;
}

static int resent(int page)
{
    for (int i = 0; i < F.nev; i++)
        if (F.ev[i].ev == QD_EV_RESENT && F.ev[i].page == page) return 1;
    return 0;
}

// A jam as page 2 prints, page 3 part loaded, page 1's sheet long out: pages 2 and 3 go again
// from the copy and the rip carries on with page 3. The printer's own recovery, which would
// print page 1, is not used: its last 8A 01 never goes.
static void test_copy_jam_on_page_2_with_page_3_part_loaded(void)
{
    fake_reset();
    F.print_ms = 30000;
    qd_session_t *s = copy_job(QD_COPY_CAP);
    CHECK_EQ_INT(page_of(s, 1, 100, 4), QD_OK);
    CHECK_EQ_INT(page_of(s, 2, 100, 4), QD_OK);        // prints 30 s on, at page 1's done
    F.win_from_print = 1;
    F.win[F.nwin++] = (win_t){ 52000, 56000, 0x12, 0x0C, 0 };   // 22 s after page 1's done
    CHECK_EQ_INT(qd_session_open(s, 3, letter()), QD_OK);
    F.put_us = 50000;                                  // 30 s of rows
    CHECK_EQ_INT(rows_of(s, 3, 600, 4), QD_OK);
    F.put_us = 0;
    CHECK(saw_state(QDS_JAM));
    CHECK(!resent(1) && resent(2) && resent(3));
    CHECK_HAS(F.trace, "8100 8408 8401 8340 833E 8200 835D 834C 8302 8100 8340 833E 8200 87DD");
    CHECK_EQ_INT(qd_session_print(s), QD_OK);
    CHECK_EQ_INT(qd_session_end(s), QD_OK);
    CHECK_STR(sheets(), "1:100 2:100 3:600");
    CHECK_EQ_INT(F.blanked + F.twice + F.moved + F.into_fault, 0);
    CHECK_EQ_INT(ev_count(QD_EV_LOST) + ev_count(QD_EV_REPRINT), 0);
    CHECK_EQ_INT(F.bad, 0);
    qd_session_free(s);
}

// The jam comes 5 s after page 1's done: its sheet may be the one that jammed on its way out,
// so it goes again too, and may come out twice.
static void test_copy_jam_soon_after_a_done_sends_that_page_too(void)
{
    fake_reset();
    F.print_ms = 30000;
    qd_session_t *s = copy_job(QD_COPY_CAP);
    CHECK_EQ_INT(page_of(s, 1, 100, 4), QD_OK);
    CHECK_EQ_INT(page_of(s, 2, 100, 4), QD_OK);
    F.win_from_print = 1;
    F.win[F.nwin++] = (win_t){ 35000, 39000, 0x12, 0x08, 0 };
    CHECK_EQ_INT(qd_session_open(s, 3, letter()), QD_OK);
    F.put_us = 50000;
    CHECK_EQ_INT(rows_of(s, 3, 600, 4), QD_OK);
    F.put_us = 0;
    CHECK(resent(1) && resent(2) && resent(3));
    CHECK_EQ_INT(qd_session_print(s), QD_OK);
    CHECK_EQ_INT(qd_session_end(s), QD_OK);
    CHECK_STR(sheets(), "1:100 1:100 2:100 3:600");
    CHECK_EQ_INT(F.blanked + F.twice + F.moved + F.into_fault, 0);
    qd_session_free(s);
}

// A jam on page 1 while page 2 part loads: with the copy nothing is lost.
static void test_copy_jam_on_page_1_while_page_2_loads(void)
{
    fake_reset();
    qd_session_t *s = copy_job(QD_COPY_CAP);
    CHECK_EQ_INT(page_of(s, 1, 100, 4), QD_OK);
    F.win_from_print = 1;
    F.win[F.nwin++] = (win_t){ 3000, 6000, 0x12, 0x08, 0 };
    CHECK_EQ_INT(qd_session_open(s, 2, letter()), QD_OK);
    F.put_us = 20000;                                  // 12 s of rows
    CHECK_EQ_INT(rows_of(s, 2, 600, 4), QD_OK);
    F.put_us = 0;
    CHECK(resent(1) && resent(2));
    CHECK_EQ_INT(qd_session_print(s), QD_OK);
    CHECK_EQ_INT(qd_session_end(s), QD_OK);
    CHECK_STR(sheets(), "1:100 2:600");
    CHECK_EQ_INT(F.blanked + F.twice + F.moved + F.into_fault, 0);
    CHECK_EQ_INT(ev_count(QD_EV_LOST), 0);
    qd_session_free(s);
}

// A jam again while the pages go again: from the top, once it has cleared.
static void test_copy_jam_during_the_resend(void)
{
    fake_reset();
    qd_session_t *s = copy_job(QD_COPY_CAP);
    CHECK_EQ_INT(page_of(s, 1, 100, 4), QD_OK);
    F.win_from_print = 1;
    F.win[F.nwin++] = (win_t){ 3000, 6000, 0x12, 0x08, 0 };
    F.win[F.nwin++] = (win_t){ 13000, 16000, 0x12, 0x08, 0 };   // as page 2's rows go again
    CHECK_EQ_INT(qd_session_open(s, 2, letter()), QD_OK);
    F.put_us = 20000;
    CHECK_EQ_INT(rows_of(s, 2, 600, 4), QD_OK);
    F.put_us = 0;
    CHECK_EQ_INT(count_tok("8408"), 2);
    CHECK_EQ_INT(qd_session_print(s), QD_OK);
    CHECK_EQ_INT(qd_session_end(s), QD_OK);
    CHECK_STR(sheets(), "1:100 2:600");
    CHECK_EQ_INT(F.blanked + F.twice + F.moved + F.into_fault, 0);
    qd_session_free(s);
}

// A jam as the last page prints, and a Cancel while the pages go again.
static void test_copy_jam_at_the_end_and_cancel(void)
{
    fake_reset();
    F.print_ms = 30000;
    qd_session_t *s = copy_job(QD_COPY_CAP);
    CHECK_EQ_INT(page_of(s, 1, 100, 4), QD_OK);
    CHECK_EQ_INT(page_of(s, 2, 100, 4), QD_OK);
    F.win_from_print = 1;
    F.win[F.nwin++] = (win_t){ 55000, 58000, 0x12, 0x08, 0 };   // page 2 printing, page 1 long out
    CHECK_EQ_INT(qd_session_end(s), QD_OK);
    CHECK_STR(sheets(), "1:100 2:100");
    CHECK(resent(2) && !resent(1));
    qd_session_free(s);

    fake_reset();
    s = copy_job(QD_COPY_CAP);
    CHECK_EQ_INT(page_of(s, 1, 100, 4), QD_OK);
    F.win_from_print = 1;
    F.win[F.nwin++] = (win_t){ 3000, 6000, 0x12, 0x08, 0 };
    F.cancel_at_ms = F.first_print + 7000;             // the jam just gone, not yet stayed clear
    CHECK_EQ_INT(qd_session_end(s), QD_E_CANCEL);
    CHECK_EQ_INT(F.nout, 0);
    CHECK_EQ_INT(F.into_fault, 0);
    qd_session_free(s);
}

// A page near the budget, then another: the second's rows wait for room in the printer, and
// its copy takes the first's place only once the first is done.
static void test_copy_of_a_page_near_the_budget_then_another(void)
{
    fake_reset();
    qd_session_t *s = copy_job(QD_COPY_CAP);
    CHECK_EQ_INT(page_of(s, 1, 1660, 300), QD_OK);     // 498,000 bytes of codes
    CHECK_EQ_INT(qd_session_open(s, 2, letter()), QD_OK);
    CHECK_EQ_INT(rows_of(s, 2, 6, 300), QD_OK);        // all that fits beside it
    CHECK_EQ_INT(F.ndone, 0);
    int same = 1;
    for (int i = 0; i < 302 * 1660; i++) {             // page 1's copy is whole: [2C 01][300 x 01]
        int at = i % 302;
        same &= g_copy[i] == (at == 0 ? 0x2C : 1);
    }
    CHECK(same);
    CHECK_EQ_INT(rows_of(s, 2, 1654, 300), QD_OK);     // the rest, once page 1 is done
    CHECK_EQ_INT(F.ndone, 1);
    CHECK_EQ_INT(qd_session_print(s), QD_OK);
    F.win_from_print = 1;
    uint32_t at = F.t_print[1] - F.first_print;
    F.win[F.nwin++] = (win_t){ at + 5000, at + 8000, 0x12, 0x08, 0 };
    CHECK_EQ_INT(qd_session_end(s), QD_OK);
    CHECK(resent(2) && !resent(1));                    // page 1's copy made way for page 2's
    CHECK_STR(sheets(), "1:1660 2:1660");
    CHECK(F.mem_peak <= 500000);
    CHECK_EQ_INT(F.blanked + F.twice + F.into_fault, 0);
    qd_session_free(s);
}

// A copy too small for two pages: a row never goes in over a page still in the printer. The
// page loading has no copy then, and a jam has the rules of a job with none.
static void test_copy_never_overwrites_a_page_in_the_printer(void)
{
    fake_reset();
    qd_session_t *s = copy_job(1000);
    CHECK_EQ_INT(page_of(s, 1, 100, 4), QD_OK);        // 600 bytes of the copy
    F.win_from_print = 1;
    F.win[F.nwin++] = (win_t){ 6000, 9000, 0x12, 0x08, 0 };
    CHECK_EQ_INT(qd_session_open(s, 2, letter()), QD_OK);
    F.put_us = 20000;
    CHECK_EQ_INT(rows_of(s, 2, 200, 4), QD_OK);        // 4 s: page 1 still printing
    int same = 1;
    for (int i = 0; i < 600; i++) same &= g_copy[i] == (i % 6 == 0 ? 4 : i % 6 == 1 ? 0 : 1);
    CHECK(same);                                       // page 1's copy untouched
    CHECK_EQ_INT(g_copy[1000], 0xEE);                  // and nothing past the ring
    CHECK_EQ_INT(rows_of(s, 2, 400, 4), QD_E_FAULT);   // the jam: no copy of page 2
    CHECK_EQ_INT(ev_count(QD_EV_RESENT), 0);
    int pg = 0;
    CHECK(ev_n(QD_EV_REPRINT, 0, &pg, NULL) && pg == 1);
    CHECK(ev_n(QD_EV_LOST, 0, &pg, NULL) && pg == 2);
    qd_session_free(s);

    // The ring wraps: five pages through a copy that holds two and a bit, a jam on the last.
    fake_reset();
    s = copy_job(1500);
    for (int i = 1; i <= 4; i++) CHECK_EQ_INT(page_of(s, i, 100, 4), QD_OK);
    CHECK_EQ_INT(page_of(s, 5, 100, 4), QD_OK);
    uint32_t at = F.t_print[4] - F.first_print;
    F.win[F.nwin++] = (win_t){ at + 3000, at + 6000, 0x12, 0x08, 0 };
    F.win_from_print = 1;
    CHECK_EQ_INT(qd_session_end(s), QD_OK);
    CHECK_STR(sheets(), "1:100 2:100 3:100 4:100 4:100 5:100");
    CHECK_EQ_INT(g_copy[1500], 0xEE);
    qd_session_free(s);
}

// A page opened again for Apple's screen: its copy starts over with it.
static void test_copy_of_a_page_opened_again(void)
{
    fake_reset();
    qd_session_t *s = copy_job(QD_COPY_CAP);
    CHECK_EQ_INT(page_of(s, 1, 100, 4), QD_OK);
    CHECK_EQ_INT(qd_session_open(s, 2, letter()), QD_OK);
    CHECK_EQ_INT(rows_of(s, 9, 300, 4), QD_OK);        // the rows that will not print
    CHECK_EQ_INT(qd_session_open(s, 2, letter()), QD_OK);
    CHECK_EQ_INT(rows_of(s, 2, 50, 4), QD_OK);
    CHECK_EQ_INT(qd_session_print(s), QD_OK);
    F.win_from_print = 1;
    uint32_t at = F.t_print[1] - F.first_print;
    F.win[F.nwin++] = (win_t){ at + 3000, at + 6000, 0x12, 0x08, 0 };
    CHECK_EQ_INT(qd_session_end(s), QD_OK);
    CHECK(resent(2));
    CHECK_EQ_INT(F.out_id[F.nout - 1], 2);
    CHECK_EQ_INT(F.out_rows[F.nout - 1], 50);
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
    test_cancel_while_waiting_for_the_manual_slot();
    test_clock_lost_mid_rows();
    test_readback_mismatch();
    test_refused_reply_retries_5_then_offline();
    test_status_names_the_unanswered_question();
    test_end_while_the_sheet_comes_out();
    test_end_after_a_manual_page_sets_the_cassette_back();
    test_power_off_while_waiting_done();
    test_status_idle();
    test_end_and_abort();
    test_the_next_page_loads_behind_the_one_printing();
    test_five_pages_a_print_apart();
    test_a_done_between_rows_is_not_lost();
    test_a_row_waits_for_room();
    test_memory_full_between_rows_is_kept();
    test_cancel_lets_the_sheet_printing_come_out();
    test_cancel_with_the_page_printing_jammed_stops_at_once();
    test_another_sheet_waits_for_the_done();
    test_a_page_opened_again_while_one_prints();
    test_jam_on_the_first_page_with_the_next_loaded();
    test_jam_seen_between_rows();
    test_cover_while_the_first_page_loads();
    test_recovery_waits_for_the_printer_to_stay_clear();
    test_jam_on_a_later_page_stops_the_job();
    test_jam_while_a_page_waits_for_room();
    test_paper_out_with_the_next_page_loading();
    test_engine_fault_between_rows();
    test_switched_off_between_rows();
    test_abort_lets_the_sheet_printing_come_out();
    test_a_line_error_lets_the_sheet_printing_come_out();
    test_misses_are_counted_in_a_row();
    test_cancel_as_the_last_page_prints();
    test_a_done_is_not_lost_to_a_missed_answer();
    test_copy_jam_on_page_2_with_page_3_part_loaded();
    test_copy_jam_soon_after_a_done_sends_that_page_too();
    test_copy_jam_on_page_1_while_page_2_loads();
    test_copy_jam_during_the_resend();
    test_copy_jam_at_the_end_and_cancel();
    test_copy_of_a_page_near_the_budget_then_another();
    test_copy_never_overwrites_a_page_in_the_printer();
    test_copy_of_a_page_opened_again();
    TEST_END;
}
