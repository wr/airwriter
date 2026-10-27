// SPDX-License-Identifier: GPL-3.0-or-later
//
// sw_session (W-1103): the StyleWriter conversation, run against a printer scripted in
// this file on a fake clock, so nothing sleeps. The fake follows the design's wire rules
// (and swfake.py's guesses): it answers `?`, `p`, `1`, `2`, `B` and `H`, takes the
// setup letters, rects, `G` blocks and `0C`, keeps a buffer that drains at a rate and
// notes an overflow, and goes deaf for a while after the reset. A write takes the time
// 57600 baud gives it. It records what it was sent as a trace of tokens:
//   [x]  the query FF FF FF x          00  the byte 00 (half the reset)
//   R c  a rect (the 8 bytes after it are counted, not shown)
//   G    a block, whole (its length in g_len)       0C  a form feed
//   any other single letter is itself; m2 is `m` and its digit.
// The expected orders of letters are TailTalk's (tailtalk-core/tests/stylewriter_cross.rs
// at bf38658, lines 379-382 for a 2200 colour job and 423-426 for a StyleWriter II), less
// the fast-clock `x` and `h`, and the queries, which that test does not list.
#include "test_util.h"
#include "sw_session.h"
#include "sw_page.h"
#include <stdio.h>
#include <stdlib.h>

#define CHECK_STR(a, b) do { g_checks++; const char *_a = (a), *_b = (b); \
    if (strcmp(_a, _b)) { g_failed++; \
    fprintf(stderr, "FAIL %s:%d: %s\n   got  %s\n   want %s\n", __FILE__, __LINE__, #a, _a, _b); } } while (0)

// ---- the scripted printer

enum { TRACE_N = 16384, EVENTS = 8192, RAW_N = 1 << 20, WINS = 4, MAXG = 512 };
enum { P_CMD, P_FF, P_GLEN, P_GDATA, P_GEND, P_SKIP, P_M };

typedef struct { uint32_t t; char kind; uint8_t arg; } ev_t;

typedef struct {
    uint64_t us;                           // the clock, in microseconds
    sw_model_t model;
    const char *ident;                     // the reply to `?`, with its CR if it has one
    uint8_t p, h;
    int awake;                             // answers `?` (the original does not until reset)
    int32_t cap;                           // bytes the empty buffer holds
    int bdiv;                              // 256, or the original's 64
    double drain;                          // bytes a second the engine eats; < 0 at once
    double fill;
    uint64_t fill_us;
    int page_pending;
    int overflow, bad;                     // bytes past the buffer; things a printer would fault on
    // the faults: windows on the clock, always-on bits
    struct { uint32_t from, to; uint8_t s1, s2; } win[WINS];
    int nwin;
    uint8_t s1, s2;
    uint32_t busy_until, clean_ms;
    int silent;
    uint32_t silent_at;                    // goes silent at this time (0: never)
    uint32_t deaf_until, settle_ms;
    int b_ov;                              // B reads this, within [b_ov_from, b_ov_to), when >= 0
    uint32_t b_ov_from, b_ov_to;
    int noise_on_reset;
    int fail_write;
    int no_p;                              // a CS that does not answer `p`
    int count_letters;                     // the letters after the first `B` take buffer bytes too
    int drain_from_b, nb;                  // drains at once from the drain_from_b-th `B` on (0: never)
    // a fault, or the end of its hearing, armed on the n-th block's end
    int arm_after_g;
    uint32_t arm_dur;                      // ms (0xFFFFFFFF: for good)
    uint8_t arm_s1, arm_s2;
    int arm_silent;
    int cancel_on_g;                       // `cancelled` turns true as the n-th block begins
    uint32_t cancel_at_ms;                 // or at this time (0: never)
    int cancel;
    // the line
    uint8_t in[64];
    int in_n;
    // the parser
    int ps, nff, skip, nlen;
    uint32_t glen, gleft;
    // the record
    char trace[TRACE_N];
    size_t tn;
    ev_t ev[EVENTS];
    int nev;
    uint8_t *raw;
    size_t rn;
    int nwrites;
    sw_state_t st[64];
    uint32_t st_t[64];
    int nst;
    uint32_t sleeps[256];
    int nsleeps;
    int g_n, g_complete;
    uint32_t g_len[MAXG];
    int resets, resumes, cleanings;
} fake_t;

static fake_t F;
static uint8_t g_raw[RAW_N];

static uint32_t now_ms(void) { return (uint32_t)(F.us / 1000); }

static void tok(const char *s)
{
    size_t n = strlen(s);
    if (F.tn + n + 2 >= TRACE_N) return;
    if (F.tn) F.trace[F.tn++] = ' ';
    memcpy(F.trace + F.tn, s, n);
    F.tn += n;
    F.trace[F.tn] = 0;
}

static void ev(char kind, uint8_t arg)
{
    if (F.nev < EVENTS) F.ev[F.nev++] = (ev_t){ now_ms(), kind, arg };
}

static void push(uint8_t b) { if (F.in_n < (int)sizeof F.in) F.in[F.in_n++] = b; }

static void tick(void)
{
    if (F.drain < 0) F.fill = 0;
    else {
        F.fill -= (double)(F.us - F.fill_us) / 1e6 * F.drain;
        if (F.fill < 0) F.fill = 0;
    }
    F.fill_us = F.us;
}

static void count(int n)
{
    tick();
    F.fill += n;
    if (F.fill > F.cap) F.overflow++;
}

static void in_window(uint8_t *s1, uint8_t *s2)
{
    uint32_t t = now_ms();
    for (int i = 0; i < F.nwin; i++)
        if (t >= F.win[i].from && t < F.win[i].to) { *s1 |= F.win[i].s1; *s2 |= F.win[i].s2; }
}

static void fault(uint32_t from, uint32_t to, uint8_t s1, uint8_t s2)
{
    F.win[F.nwin].from = from; F.win[F.nwin].to = to; F.win[F.nwin].s1 = s1; F.win[F.nwin].s2 = s2;
    F.nwin++;
}

static int is_cs(void) { return F.model >= SW_C1500; }

static uint8_t status1(void)
{
    tick();
    uint8_t a = F.s1, b = 0;
    in_window(&a, &b);
    if (now_ms() < F.busy_until) a |= 0x01;
    if (F.page_pending) a |= F.fill > 0 ? 0x01 : 0x08;
    return a;
}

static uint8_t status2(void)
{
    uint8_t a = 0, b = F.s2;
    in_window(&a, &b);
    return b;
}

static uint8_t b_now(void)
{
    uint32_t t = now_ms();
    if (F.b_ov >= 0 && t >= F.b_ov_from && t < F.b_ov_to) return (uint8_t)F.b_ov;
    tick();
    double free_b = F.cap - F.fill;
    long v = (long)((free_b + 2048) / F.bdiv);
    return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
}

static void do_reset(void)
{
    tok("[I]");
    ev('q', 'I');
    F.fill = 0;
    F.fill_us = F.us;
    F.page_pending = 0;
    F.awake = 1;
    F.resets++;
    F.deaf_until = now_ms() + F.settle_ms;
    F.ps = P_CMD;
    F.nff = 0;
    if (F.noise_on_reset) push('X');
}

static void query(uint8_t q)
{
    if (q == 'I') { do_reset(); return; }
    char t[8];
    snprintf(t, sizeof t, "[%c]", q);
    tok(t);
    ev('q', q);
    switch (q) {
    case '1': push(status1()); break;
    case '2': push(status2()); break;
    case 'B':
        if (F.drain_from_b && ++F.nb == F.drain_from_b) F.drain = -1;
        push(b_now());
        break;
    case 'H': push(F.h); break;
    case 'p': if (!is_cs()) F.bad++; else if (!F.no_p) push(F.p); break;
    case 'R': push(0x50); break;
    case 'S': F.resumes++; break;
    default: F.bad++;                      // D and N are letters, not queries
    }
}

static void g_done(void)
{
    F.g_complete++;
    if (F.arm_after_g && F.g_complete == F.arm_after_g) {
        uint32_t t = now_ms();
        if (F.arm_silent) F.silent = 1;
        else fault(t, F.arm_dur == 0xFFFFFFFFu ? 0xFFFFFFFFu : t + F.arm_dur, F.arm_s1, F.arm_s2);
    }
}

static void fake_byte(uint8_t b)
{
    uint32_t t = now_ms();
    if (F.silent || (F.silent_at && t >= F.silent_at)) return;
    if (t < F.deaf_until) {                // settling: only the reset gets through
        if (b == 0xFF) F.nff++;
        else { if (F.nff >= 3 && b == 'I') do_reset(); F.nff = 0; }
        return;
    }
    switch (F.ps) {
    case P_CMD:
        if (b == 0xFF) { F.nff = 1; F.ps = P_FF; }
        else if (b == 0x00) tok("00");
        else if (b == '?') { tok("?"); if (F.awake) for (const char *c = F.ident; *c; c++) push((uint8_t)*c); }
        else if (b == 'G') { count(1); F.ps = P_GLEN; F.nlen = 0; F.glen = 0; }
        else if (b == 'R' || b == 'c') { tok(b == 'R' ? "R" : "c"); ev('r', b); count(9); F.ps = P_SKIP; F.skip = 8; }
        else if (b == 'm') { if (F.count_letters && F.nb) count(1); F.ps = P_M; }
        else if (b == 0x0C) { tok("0C"); count(1); F.page_pending = 1; }
        else {
            char s[2] = { (char)b, 0 };
            tok(s);
            ev('k', b);
            if (F.count_letters && F.nb) count(1);
            if (b == 'P') { F.cleanings++; F.busy_until = t + F.clean_ms; }
            if (b == 'E') F.page_pending = 0;
        }
        break;
    case P_FF:
        if (b == 0xFF) F.nff++;
        else { if (F.nff >= 3) query(b); else F.bad++; if (F.ps == P_FF) F.ps = P_CMD; F.nff = 0; }
        break;
    case P_M: { char s[3] = { 'm', (char)b, 0 }; tok(s); if (F.count_letters && F.nb) count(1); F.ps = P_CMD; break; }
    case P_SKIP: if (--F.skip == 0) F.ps = P_CMD; break;
    case P_GLEN:
        count(1);
        F.glen |= (uint32_t)b << (8 * F.nlen);
        if (++F.nlen == 2) {
            if (F.g_n < MAXG) F.g_len[F.g_n] = F.glen;
            F.g_n++;
            tok("G");
            if (F.cancel_on_g && F.g_n == F.cancel_on_g) F.cancel = 1;
            F.gleft = F.glen;
            F.ps = F.glen ? P_GDATA : P_GEND;
        }
        break;
    case P_GDATA: count(1); if (--F.gleft == 0) F.ps = P_GEND; break;
    case P_GEND: count(1); if (b) F.bad++; F.ps = P_CMD; g_done(); break;
    }
}

// the printer, as a model
static void fake_init(sw_model_t m, int colour_cart)
{
    memset(&F, 0, sizeof F);
    F.raw = g_raw;
    F.model = m;
    F.us = 1000ull * 1000;                 // a job does not start at time 0
    F.drain = -1;
    F.b_ov = -1;
    F.settle_ms = 1500;
    F.awake = 1;
    F.bdiv = 256;
    F.h = colour_cart ? 0x81 : 0x01;
    uint8_t empty = 0xF8;
    switch (m) {
    case SW_ORIGINAL: F.ident = "IJ10\r"; empty = 0xA0; F.bdiv = 64; F.awake = 0; F.h = 0x01; break;
    case SW_II: F.ident = "SW\r"; F.h = 0x01; break;
    case SW_1200: F.ident = "SW3\r"; F.h = 0x01; break;
    case SW_C1500: F.ident = "CS\r"; F.p = 4; empty = colour_cart ? 0x87 : 0x9F; break;
    case SW_C2200: F.ident = "CS\r"; F.p = 2; empty = 0xA2; break;
    case SW_C2400: F.ident = "CS\r"; F.p = 1; F.s2 = 0x80; break;     // the spurious 80
    case SW_C2500: F.ident = "CS\r"; F.p = 5; empty = 0x80; F.s2 = 0x80; break;
    default: break;
    }
    F.cap = (int32_t)empty * F.bdiv - 2048;
}

// ---- the line, as sw_ops_t

static int o_write(void *c, const uint8_t *b, size_t n)
{
    (void)c;
    F.nwrites++;
    F.us += (uint64_t)((double)n * 10 * 1e6 / 57600);
    if (F.rn + n <= RAW_N) memcpy(F.raw + F.rn, b, n);
    F.rn += n;
    if (F.fail_write) return -1;
    for (size_t i = 0; i < n; i++) fake_byte(b[i]);
    return 0;
}

static int o_read(void *c, uint8_t *b, size_t cap, uint32_t first_ms, uint32_t gap_ms)
{
    (void)c; (void)gap_ms;
    if (!F.in_n) { F.us += (uint64_t)first_ms * 1000; return 0; }
    size_t k = (size_t)F.in_n < cap ? (size_t)F.in_n : cap;
    memcpy(b, F.in, k);
    memmove(F.in, F.in + k, (size_t)F.in_n - k);
    F.in_n -= (int)k;
    return (int)k;
}

static void o_drain(void *c) { (void)c; F.in_n = 0; }
static void o_sleep(void *c, uint32_t ms) { (void)c; F.us += (uint64_t)ms * 1000; if (F.nsleeps < 256) F.sleeps[F.nsleeps++] = ms; }
static uint32_t o_now(void *c) { (void)c; return now_ms(); }
static int o_cancelled(void *c)
{
    (void)c;
    if (F.cancel_at_ms && now_ms() >= F.cancel_at_ms) F.cancel = 1;
    return F.cancel;
}
static void o_state(void *c, sw_state_t st)
{
    (void)c;
    if (F.nst < 64) { F.st[F.nst] = st; F.st_t[F.nst] = now_ms(); F.nst++; }
}

static sw_ops_t ops(void)
{
    return (sw_ops_t){ o_write, o_read, o_drain, o_sleep, o_now, o_cancelled, o_state, NULL };
}

static sw_session_t *session(sw_model_t m, int quality, int colour_job, uint32_t stall_ms)
{
    sw_ops_t o = ops();
    sw_job_cfg_t c = { m, quality, colour_job, stall_ms };
    return sw_session_new(&o, &c);
}

// ---- frames, as sw_page.h makes them

static size_t fr(uint8_t *d, uint8_t type, const uint8_t *p, size_t n)
{
    d[0] = type; d[1] = (uint8_t)n; d[2] = (uint8_t)(n >> 8);
    if (n) memcpy(d + 3, p, n);
    return n + 3;
}

static size_t fr_page(uint8_t *d, int colour) { uint8_t p = (uint8_t)colour; return fr(d, SWF_PAGE, &p, 1); }
static size_t fr_end(uint8_t *d) { return fr(d, SWF_PAGE_END, NULL, 0); }

static size_t fr_rect(uint8_t *d, char kind, unsigned l, unsigned t, unsigned r, unsigned b)
{
    uint8_t p[9] = { (uint8_t)kind, (uint8_t)l, (uint8_t)(l >> 8), (uint8_t)t, (uint8_t)(t >> 8),
                     (uint8_t)r, (uint8_t)(r >> 8), (uint8_t)b, (uint8_t)(b >> 8) };
    return fr(d, SWF_RECT, p, 9);
}

static size_t fr_block(uint8_t *d, size_t n)
{
    uint8_t *p = d + 3;
    for (size_t i = 0; i < n; i++) p[i] = (uint8_t)(i * 7 + 3);
    d[0] = SWF_BLOCK; d[1] = (uint8_t)n; d[2] = (uint8_t)(n >> 8);
    return n + 3;
}

// A page: its start, `rects` rects of one block of `blen` each, its end.
static size_t page(uint8_t *d, int colour, int rects, size_t blen)
{
    size_t n = fr_page(d, colour);
    for (int i = 0; i < rects; i++) {
        n += fr_rect(d + n, colour ? 'c' : 'R', 0, (unsigned)i * 100, 99, (unsigned)i * 100 + 99);
        n += fr_block(d + n, blen);
    }
    return n + fr_end(d + n);
}

static uint8_t g_job[1 << 18];

// Runs a whole job: begin, the frames, end. Returns the first error.
static int run(sw_session_t *s, const uint8_t *d, size_t n)
{
    int rc = sw_session_begin(s);
    if (!rc) rc = sw_session_feed(s, d, n);
    if (!rc) rc = sw_session_end(s);
    return rc;
}

// The trace less its queries: what TailTalk's cross test lists.
static const char *less_queries(const char *tr)
{
    static char out[TRACE_N];
    size_t n = 0;
    for (const char *p = tr; *p;) {
        const char *e = strchr(p, ' ');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        if (!(len == 3 && p[0] == '[')) {
            if (n) out[n++] = ' ';
            memcpy(out + n, p, len);
            n += len;
        }
        p += len + (e ? 1 : 0);
    }
    out[n] = 0;
    return out;
}

// The letters sent from the last `H` query on (or the last `B` one for a model with none),
// run together: the model's setup, up to the first thing the page sends.
static const char *setup_of(const char *tr, const char *upto)
{
    static char out[64];
    const char *h = strstr(tr, "[H]");
    const char *b = strstr(tr, "[B]");
    const char *from = h ? h + 3 : b ? b + 3 : tr;
    const char *to = strstr(from, upto);
    size_t n = 0;
    for (const char *p = from; p < (to ? to : from + strlen(from)) && n < sizeof out - 1; p++)
        if (*p != ' ') out[n++] = *p;
    out[n] = 0;
    return out;
}

static int count_tok(const char *tr, const char *t)
{
    int n = 0;
    size_t len = strlen(t);
    for (const char *p = tr; *p;) {
        const char *e = strchr(p, ' ');
        size_t l = e ? (size_t)(e - p) : strlen(p);
        if (l == len && !memcmp(p, t, len)) n++;
        p += l + (e ? 1 : 0);
    }
    return n;
}

static int ends_with(const char *tr, const char *suffix)
{
    size_t a = strlen(tr), b = strlen(suffix);
    return a >= b && !strcmp(tr + a - b, suffix);
}

// ---- begin_order

static void test_begin_order(void)
{
    // The 2200, a colour job at normal quality, two pages: the second is gray, under the
    // same page mode (TailTalk's cross test, 379-382, without `x`).
    fake_init(SW_C2200, 1);
    sw_session_t *s = session(SW_C2200, 4, 1, 600000);
    CHECK(s != NULL);
    size_t n = page(g_job, 1, 1, 400);
    n += page(g_job + n, 0, 1, 400);
    CHECK_EQ_INT(run(s, g_job, n), SW_OK);
    CHECK_STR(less_queries(F.trace), "00 ? D m2 n Z A H L c G 0C L R G 0C E l");
    CHECK_STR(F.trace, "00 [I] [1] [2] ? D [B] [H] m2 n Z A H L [1] [2] c G 0C L [1] [2] R G 0C [1] E l");
    CHECK_EQ_INT(F.nsleeps, 1);
    CHECK_EQ_INT(F.sleeps[0], 2000);
    CHECK_EQ_INT(sw_session_cart(s), 1);
    CHECK_EQ_INT(F.bad, 0);
    CHECK_EQ_INT(F.overflow, 0);
    CHECK_EQ_INT(F.nst > 0 ? F.st[0] : -1, SWS_PRINTING);
    CHECK_EQ_INT(F.st[F.nst - 1], SWS_READY);
    // The reset is 00 FF FF FF 49, in one write.
    static const uint8_t reset[5] = { 0x00, 0xFF, 0xFF, 0xFF, 0x49 };
    CHECK_EQ_MEM(F.raw, reset, 5);
    sw_session_free(s);

    // A StyleWriter II, Best quality, two pages of the same band (TailTalk 423-426 without
    // `h`): nothing between pages.
    fake_init(SW_II, 0);
    s = session(SW_II, 5, 0, 0);
    n = page(g_job, 0, 1, 400);
    n += page(g_job + n, 0, 1, 400);
    CHECK_EQ_INT(run(s, g_job, n), SW_OK);
    CHECK_STR(less_queries(F.trace), "00 ? u b D A R G 0C R G 0C E l");
    sw_session_free(s);

    // The same at normal: u, n, D, A, with the queries where the design puts them.
    fake_init(SW_II, 0);
    s = session(SW_II, 4, 0, 600000);
    n = page(g_job, 0, 1, 400);
    CHECK_EQ_INT(run(s, g_job, n), SW_OK);
    CHECK_STR(F.trace, "00 [I] [1] [2] ? [B] [H] u n D A [1] [2] R G 0C [1] E l");
    sw_session_free(s);

    // The 1200: N after `?`, an L a page, Y after E.
    fake_init(SW_1200, 0);
    s = session(SW_1200, 4, 0, 600000);
    n = page(g_job, 0, 1, 400);
    n += page(g_job + n, 0, 1, 400);
    CHECK_EQ_INT(run(s, g_job, n), SW_OK);
    CHECK_STR(F.trace, "00 [I] [1] [2] ? N [B] [H] u n D A L [1] [2] R G 0C L [1] [2] R G 0C [1] E Y l");
    sw_session_free(s);

    // The 1500: no page mode, an L a page. Its black cartridge reads no colour.
    fake_init(SW_C1500, 1);
    s = session(SW_C1500, 4, 1, 600000);
    n = page(g_job, 1, 1, 400);
    CHECK_EQ_INT(run(s, g_job, n), SW_OK);
    CHECK_STR(F.trace, "00 [I] [1] [2] ? D [B] [H] m2 Z A L [1] [2] c G 0C [1] E l");
    sw_session_free(s);

    // The original: settles 8 s, has no H, nothing between pages, and does not wait for 08.
    fake_init(SW_ORIGINAL, 0);
    s = session(SW_ORIGINAL, 4, 0, 600000);
    n = page(g_job, 0, 1, 400);
    CHECK_EQ_INT(run(s, g_job, n), SW_OK);
    CHECK_STR(F.trace, "00 [I] [1] [2] ? [B] n u A [1] [2] R G 0C E l");
    CHECK_EQ_INT(F.sleeps[0], 8000);
    sw_session_free(s);

    // The 2400 and 2500 answer `2` with a spurious 80, which stops nothing.
    for (int k = 0; k < 2; k++) {
        sw_model_t m = k ? SW_C2500 : SW_C2400;
        fake_init(m, 1);
        s = session(m, 4, 1, 600000);
        n = page(g_job, 1, 1, 400);
        CHECK_EQ_INT(run(s, g_job, n), SW_OK);
        CHECK_STR(F.trace, "00 [I] [1] [2] ? D [B] [H] m2 n Z A H L [1] [2] c G 0C [1] E l");
        sw_session_free(s);
    }
}

// ---- quality

static void test_quality(void)
{
    // model, cartridge (1 colour), then the letters of draft, normal and best.
    static const struct { sw_model_t m; int cart; const char *q[3]; } t[] = {
        // job.rs 214-225
        { SW_C2200, 1, { "m2dA", "m2nZA", "m2sA" } },
        { SW_C2200, 0, { "m2dA", "m2bA",  "m2sA" } },
        { SW_C2400, 1, { "m2dA", "m2nZA", "m2sA" } },
        { SW_C2500, 0, { "m2dA", "m2bA",  "m2sA" } },
        // job.rs 227-232: Z alone at normal, whatever the cartridge
        { SW_C1500, 1, { "m2dA", "m2ZA",  "m2sA" } },
        { SW_C1500, 0, { "m2dA", "m2ZA",  "m2sA" } },
        // job.rs 252-257
        { SW_II,    0, { "udDA", "unDA",  "ubDA" } },
        { SW_1200,  0, { "udDA", "unDA",  "ubDA" } },
        // job.rs 257: the original takes n, u, A at any quality
        { SW_ORIGINAL, 0, { "nuA", "nuA", "nuA" } },
    };
    static const int qv[3] = { 3, 4, 5 };
    for (size_t i = 0; i < sizeof t / sizeof t[0]; i++)
        for (int q = 0; q < 3; q++) {
            fake_init(t[i].m, t[i].cart);
            sw_session_t *s = session(t[i].m, qv[q], 0, 600000);
            CHECK_EQ_INT(sw_session_begin(s), SW_OK);
            CHECK_STR(setup_of(F.trace, "\x01"), t[i].q[q]);
            CHECK_EQ_INT(sw_session_cart(s), t[i].cart && t[i].m != SW_II && t[i].m != SW_1200 && t[i].m != SW_ORIGINAL);
            sw_session_free(s);
        }
    // Print-quality 0, which the phone did not send, is normal; so is anything else.
    static const int odd[] = { 0, 1, 2, 6, 99, -1 };
    for (size_t i = 0; i < sizeof odd / sizeof odd[0]; i++) {
        fake_init(SW_C2200, 1);
        sw_session_t *s = session(SW_C2200, odd[i], 0, 0);
        CHECK_EQ_INT(sw_session_begin(s), SW_OK);
        CHECK_STR(setup_of(F.trace, "\x01"), "m2nZA");
        sw_session_free(s);
    }
    // A black cartridge at Best on a Color StyleWriter: the page mode `B` is followed by
    // `R` and then `s` (job.rs/session.rs 706-718; the page is a screen, never plain 1 bit).
    fake_init(SW_C2200, 0);
    sw_session_t *s = session(SW_C2200, 5, 0, 600000);
    size_t n = page(g_job, 0, 1, 100);
    CHECK_EQ_INT(run(s, g_job, n), SW_OK);
    CHECK_STR(F.trace, "00 [I] [1] [2] ? D [B] [H] m2 s A B [R] s L [1] [2] R G 0C [1] E l");
    sw_session_free(s);
    // and a colour cartridge on a gray job: mode B, nothing more
    fake_init(SW_C2200, 1);
    s = session(SW_C2200, 5, 0, 600000);
    n = page(g_job, 0, 1, 100);
    CHECK_EQ_INT(run(s, g_job, n), SW_OK);
    CHECK_STR(F.trace, "00 [I] [1] [2] ? D [B] [H] m2 s A B L [1] [2] R G 0C [1] E l");
    sw_session_free(s);
}

// ---- pacing

static void test_pacing(void)
{
    // The fake notices a buffer overflow: three 20000-byte blocks sent with no regard to
    // `B` overrun it (so the checks below can fail).
    fake_init(SW_C2200, 0);
    F.drain = 2000;
    uint8_t g[20004];
    g[0] = 'G'; g[1] = 20000 & 255; g[2] = 20000 >> 8; memset(g + 3, 0x55, 20000); g[20003] = 0;
    for (int i = 0; i < 3; i++) o_write(NULL, g, sizeof g);
    CHECK(F.overflow > 0);

    // The session: the same, in one rect, paced.
    fake_init(SW_C2200, 0);
    F.drain = 2000;
    sw_session_t *s = session(SW_C2200, 4, 0, 600000);
    size_t n = fr_page(g_job, 0);
    n += fr_rect(g_job + n, 'R', 0, 0, 99, 99);
    for (int i = 0; i < 3; i++) n += fr_block(g_job + n, 20000);
    n += fr_end(g_job + n);
    CHECK_EQ_INT(sw_session_begin(s), SW_OK);
    CHECK_EQ_INT(sw_session_feed(s, g_job, n), SW_OK);
    CHECK_EQ_INT(F.overflow, 0);
    CHECK_EQ_INT(F.bad, 0);
    CHECK_EQ_INT(F.g_complete, 3);
    for (int i = 0; i < 3; i++) CHECK_EQ_INT(F.g_len[i], 20000);
    // While short, `B` is read every 100 ms, and the block goes when the answer has room.
    int polls = 0, in_run = 0;
    uint32_t last = 0;
    for (int i = 0; i < F.nev; i++) {
        if (F.ev[i].kind != 'q' || F.ev[i].arg != 'B') continue;
        if (in_run && F.ev[i].t - last < 1000) { CHECK(F.ev[i].t - last >= 99 && F.ev[i].t - last <= 103); polls++; }
        in_run = 1;
        last = F.ev[i].t;
    }
    CHECK(polls >= 20 && polls <= 120);                     // a few seconds of waiting, 10 a second
    CHECK_EQ_INT(sw_session_end(s), SW_OK);
    sw_session_free(s);

    // B = 00 is no room: nothing goes while it reads so, and the block follows the first
    // better answer.
    uint32_t t0;
    fake_init(SW_C2200, 0);
    F.drain = 1e9;
    s = session(SW_C2200, 4, 0, 600000);
    CHECK_EQ_INT(sw_session_begin(s), SW_OK);
    n = fr_page(g_job, 0);
    n += fr_rect(g_job + n, 'R', 0, 0, 99, 99);
    n += fr_block(g_job + n, 30000);
    CHECK_EQ_INT(sw_session_feed(s, g_job, n), SW_OK);       // 30004 of 39424: 9420 free
    F.b_ov = 0;                                              // B lies for a second
    F.b_ov_from = now_ms();
    F.b_ov_to = now_ms() + 1000;
    t0 = now_ms();
    n = fr_block(g_job, 20000);
    CHECK_EQ_INT(sw_session_feed(s, g_job, n), SW_OK);
    CHECK_EQ_INT(F.g_complete, 2);
    int g_after = 0;
    for (int i = 0; i < F.nev; i++) if (F.ev[i].kind == 'q' && F.ev[i].arg == 'B' && F.ev[i].t >= t0) g_after++;
    CHECK(g_after >= 9 && g_after <= 14);                    // every 100 ms for a second
    CHECK(now_ms() >= t0 + 1000);
    CHECK_EQ_INT(F.overflow, 0);
    sw_session_free(s);

    // Before each rect, status is asked until clear: the door is open for 5 s.
    fake_init(SW_C2200, 0);
    s = session(SW_C2200, 4, 0, 600000);
    CHECK_EQ_INT(sw_session_begin(s), SW_OK);
    fault(now_ms(), now_ms() + 5000, 0x40, 0);
    t0 = now_ms();
    n = fr_page(g_job, 0);
    n += fr_rect(g_job + n, 'R', 0, 0, 99, 99);
    CHECK_EQ_INT(sw_session_feed(s, g_job, n), SW_OK);
    CHECK(now_ms() >= t0 + 5000);
    int rect_t = 0;
    for (int i = 0; i < F.nev; i++) if (F.ev[i].kind == 'r') rect_t = (int)F.ev[i].t;
    CHECK(rect_t >= (int)t0 + 5000);
    CHECK_EQ_INT(F.nst, 3);                                  // printing, door, printing
    CHECK_EQ_INT(F.st[1], SWS_DOOR);
    CHECK_EQ_INT(F.st[2], SWS_PRINTING);
    sw_session_free(s);
}

// ---- faults

static void test_paper_out(void)
{
    // `2` reads 04 once the first block is in, for 23 s: out of paper mid-page.
    fake_init(SW_C2200, 0);
    F.arm_after_g = 1;
    F.arm_dur = 23000;
    F.arm_s2 = 0x04;
    sw_session_t *s = session(SW_C2200, 4, 0, 600000);
    size_t n = fr_page(g_job, 0);
    n += fr_rect(g_job + n, 'R', 0, 0, 99, 99);
    n += fr_block(g_job + n, 5000);
    n += fr_rect(g_job + n, 'R', 0, 100, 199, 199);
    n += fr_block(g_job + n, 5000);
    n += fr_end(g_job + n);
    CHECK_EQ_INT(run(s, g_job, n), SW_OK);
    CHECK_EQ_INT(F.g_complete, 2);
    // the states: printing, no paper, printing, ready
    CHECK_EQ_INT(F.nst, 4);
    CHECK_EQ_INT(F.st[1], SWS_NO_PAPER);
    CHECK_EQ_INT(F.st[2], SWS_PRINTING);
    CHECK_EQ_INT(F.st[3], SWS_READY);
    // S every 5 s while it is out, and once more as it clears
    uint32_t ts[16];
    int ns = 0;
    for (int i = 0; i < F.nev; i++) if (F.ev[i].kind == 'q' && F.ev[i].arg == 'S' && ns < 16) ts[ns++] = F.ev[i].t;
    CHECK(ns >= 5 && ns <= 7);
    for (int i = 1; i + 1 < ns; i++) CHECK(ts[i] - ts[i - 1] >= 5000 && ts[i] - ts[i - 1] <= 6100);
    CHECK(ts[ns - 1] > ts[ns - 2]);
    // the second rect is not sent until the paper is back
    uint32_t second_rect = 0;
    int nr = 0;
    for (int i = 0; i < F.nev; i++) if (F.ev[i].kind == 'r' && ++nr == 2) second_rect = F.ev[i].t;
    CHECK(second_rect >= ts[ns - 1]);
    CHECK_EQ_INT(F.bad, 0);
    sw_session_free(s);

    // and never: it ends at stall_ms
    fake_init(SW_C2200, 0);
    F.arm_after_g = 1;
    F.arm_dur = 0xFFFFFFFFu;
    F.arm_s2 = 0x04;
    s = session(SW_C2200, 4, 0, 60000);
    uint32_t t_fault = 0;
    CHECK_EQ_INT(sw_session_begin(s), SW_OK);
    int rc = sw_session_feed(s, g_job, n);
    CHECK_EQ_INT(rc, SW_E_STALL);
    for (int i = 0; i < F.nev; i++) if (F.ev[i].kind == 'q' && F.ev[i].arg == 'S') { t_fault = F.ev[i].t; break; }
    CHECK(now_ms() - t_fault >= 60000 && now_ms() - t_fault <= 62500);
    CHECK_EQ_INT(F.st[F.nst - 1], SWS_NO_PAPER);
    sw_session_abort(s);
    CHECK(ends_with(F.trace, "[S] 00 [I]") || ends_with(F.trace, "[2] 00 [I]"));   // the reset, after no E (a colour model)
    CHECK_EQ_INT(count_tok(F.trace, "E"), 0);
    sw_session_free(s);

    // the default is 10 minutes
    fake_init(SW_C2200, 0);
    F.arm_after_g = 1;
    F.arm_dur = 0xFFFFFFFFu;
    F.arm_s2 = 0x04;
    s = session(SW_C2200, 4, 0, 0);
    CHECK_EQ_INT(sw_session_begin(s), SW_OK);
    t_fault = now_ms();
    CHECK_EQ_INT(sw_session_feed(s, g_job, n), SW_E_STALL);
    CHECK(now_ms() - t_fault >= 600000 && now_ms() - t_fault <= 605000);
    sw_session_free(s);
}

static void test_fault_bits(void)
{
    // Idle: each bit reports its state (status.rs 74-105).
    static const struct { sw_model_t m; uint8_t s1, s2; sw_state_t want; } t[] = {
        { SW_C2200, 0x00, 0x00, SWS_READY },
        { SW_C2200, 0x01, 0x00, SWS_PRINTING },                // busy
        { SW_C2200, 0x08, 0x00, SWS_READY },                   // page done
        { SW_C2200, 0x09, 0x00, SWS_PRINTING },
        { SW_C2200, 0x00, 0x01, SWS_CART_JAM },
        { SW_C2200, 0x00, 0x02, SWS_INK_OUT },
        { SW_C2200, 0x00, 0x04, SWS_NO_PAPER },
        { SW_C2200, 0x00, 0x08, SWS_JAM },
        { SW_C2200, 0x00, 0x10, SWS_FAULT },
        { SW_C2200, 0x00, 0x20, SWS_CART_MISSING },
        { SW_C2200, 0x00, 0x40, SWS_FAULT },                   // paper too small
        { SW_C2200, 0x00, 0x80, SWS_FAULT },                   // on the 2200, 80 of `2` counts
        { SW_C2200, 0x02, 0x00, SWS_FAULT },
        { SW_C2200, 0x04, 0x00, SWS_FAULT },                   // hardware error
        { SW_C2200, 0x40, 0x00, SWS_DOOR },
        { SW_C2200, 0x80, 0x00, SWS_WASTE },
        { SW_C2200, 0x40, 0x04, SWS_NO_PAPER },                // a named fault over the door
        { SW_C2200, 0x02, 0x04, SWS_NO_PAPER },                // and over an unnamed one
        // the 2400 and 2500 ignore 80 of `2`, only
        { SW_C2400, 0x00, 0x80, SWS_READY },
        { SW_C2500, 0x00, 0x80, SWS_READY },
        { SW_C2500, 0x00, 0x84, SWS_NO_PAPER },
        { SW_C2500, 0x00, 0x10, SWS_FAULT },
        { SW_C2400, 0x40, 0x00, SWS_DOOR },
        { SW_C2500, 0x80, 0x00, SWS_WASTE },
        // the 1200 series (status.rs 85-89, 96): no door, bit 02 is the printer holding
        // (and clears itself), a hardware error is 04, the waste container only the 1500's
        { SW_II,    0x40, 0x00, SWS_READY },
        { SW_1200,  0x40, 0x00, SWS_READY },
        { SW_ORIGINAL, 0x40, 0x00, SWS_READY },
        { SW_II,    0x80, 0x00, SWS_READY },
        { SW_C1500, 0x80, 0x00, SWS_WASTE },
        { SW_C1500, 0x40, 0x00, SWS_READY },
        { SW_II,    0x02, 0x00, SWS_PRINTING },
        { SW_II,    0x00, 0x02, SWS_PRINTING },                // not "ink out"
        { SW_C1500, 0x00, 0x02, SWS_PRINTING },
        { SW_1200,  0x04, 0x00, SWS_FAULT },
        { SW_II,    0x00, 0x80, SWS_FAULT },
        { SW_C1500, 0x00, 0x80, SWS_FAULT },
        { SW_II,    0x00, 0x04, SWS_NO_PAPER },
        { SW_ORIGINAL, 0x00, 0x08, SWS_JAM },
        { SW_1200,  0x00, 0x20, SWS_CART_MISSING },
        { SW_C1500, 0x00, 0x01, SWS_CART_JAM },
    };
    for (size_t i = 0; i < sizeof t / sizeof t[0]; i++) {
        fake_init(t[i].m, 0);
        F.s1 = t[i].s1;
        F.s2 = t[i].s2;
        sw_ops_t o = ops();
        sw_state_t st = SWS_OFFLINE;
        int rc = sw_ask_status(&o, t[i].m, &st);
        CHECK_EQ_INT(rc, SW_OK);
        if ((int)st != (int)t[i].want)
            fprintf(stderr, "   case %zu: model %d s1 %02X s2 %02X\n", i, (int)t[i].m, t[i].s1, t[i].s2);
        CHECK_EQ_INT(st, t[i].want);
        CHECK_STR(F.trace, "[1] [2]");
    }
    // Nothing answers: silent.
    fake_init(SW_C2200, 0);
    F.silent = 1;
    sw_ops_t o = ops();
    sw_state_t st = SWS_READY;
    CHECK_EQ_INT(sw_ask_status(&o, SW_C2200, &st), SW_E_SILENT);
    CHECK_EQ_INT(st, SWS_OFFLINE);

    // In a job, a named fault waits and clears; an unnamed one ends it after 10 s.
    fake_init(SW_C2200, 0);
    sw_session_t *s = session(SW_C2200, 4, 0, 600000);
    uint32_t t0 = now_ms();
    fault(t0, t0 + 30000, 0x04, 0);
    CHECK_EQ_INT(sw_session_begin(s), SW_E_FAULT);
    CHECK(now_ms() - t0 >= 10000 && now_ms() - t0 <= 20000);
    CHECK_EQ_INT(F.st[F.nst - 1], SWS_FAULT);
    CHECK(count_tok(F.trace, "[S]") >= 2);
    sw_session_free(s);

    // The 1200 series' holding bit is waited out with no state and no S.
    fake_init(SW_II, 0);
    s = session(SW_II, 4, 0, 600000);
    t0 = now_ms();
    fault(t0, t0 + 8000, 0x02, 0);
    CHECK_EQ_INT(sw_session_begin(s), SW_OK);
    CHECK(now_ms() - t0 >= 8000);
    CHECK_EQ_INT(F.nst, 1);
    CHECK_EQ_INT(F.resumes, 0);
    sw_session_free(s);

    // A door open at the start holds the job, then lets it go.
    fake_init(SW_C2200, 1);
    s = session(SW_C2200, 4, 1, 600000);
    t0 = now_ms();
    fault(t0, t0 + 4000, 0x40, 0);
    CHECK_EQ_INT(sw_session_begin(s), SW_OK);
    CHECK_EQ_INT(F.nst, 3);
    CHECK_EQ_INT(F.st[1], SWS_DOOR);
    CHECK_EQ_INT(F.st[2], SWS_PRINTING);
    CHECK(F.resumes >= 1);
    sw_session_free(s);
}

static void test_silent(void)
{
    // The printer stops answering after the first block of a page: 30 s of nothing.
    fake_init(SW_C2200, 0);
    F.arm_after_g = 1;
    F.arm_silent = 1;
    sw_session_t *s = session(SW_C2200, 4, 0, 600000);
    size_t n = fr_page(g_job, 0);
    n += fr_rect(g_job + n, 'R', 0, 0, 99, 99);
    n += fr_block(g_job + n, 5000);
    n += fr_rect(g_job + n, 'R', 0, 100, 199, 199);
    n += fr_block(g_job + n, 5000);
    CHECK_EQ_INT(sw_session_begin(s), SW_OK);
    CHECK_EQ_INT(sw_session_feed(s, g_job, n), SW_E_SILENT);
    uint32_t last_heard = 0;
    for (int i = 0; i < F.nev; i++)
        if (F.ev[i].kind == 'q' && F.ev[i].arg == '2' && F.ev[i].t <= now_ms()) last_heard = F.ev[i].t;   // the last it answered
    CHECK(now_ms() - last_heard >= 30000 && now_ms() - last_heard <= 37000);
    CHECK_EQ_INT(F.st[F.nst - 1], SWS_OFFLINE);
    sw_session_free(s);

    // Silence during a fault wait is still silence: out of paper for good, and the printer
    // stops answering 20 s into it. The wait is 600 s, but the job ends 30 s after the last
    // answer, as SW_E_SILENT with the printer offline. (cancel_at_ms is only a backstop, so
    // a session that kept waiting would end the test with SW_E_CANCEL and not hang it.)
    fake_init(SW_C2200, 0);
    F.arm_after_g = 1;
    F.arm_dur = 0xFFFFFFFFu;
    F.arm_s2 = 0x04;
    s = session(SW_C2200, 4, 0, 0);
    n = fr_page(g_job, 0);
    n += fr_rect(g_job + n, 'R', 0, 0, 99, 99);
    n += fr_block(g_job + n, 5000);
    n += fr_rect(g_job + n, 'R', 0, 100, 199, 199);
    n += fr_block(g_job + n, 5000);
    CHECK_EQ_INT(sw_session_begin(s), SW_OK);
    uint32_t t_wait = now_ms();
    F.silent_at = t_wait + 21000;                       // the fault begins about 1 s in
    F.cancel_at_ms = F.silent_at + 120000;
    CHECK_EQ_INT(sw_session_feed(s, g_job, n), SW_E_SILENT);
    CHECK(now_ms() >= F.silent_at + 29000 && now_ms() <= F.silent_at + 40000);
    CHECK(now_ms() - t_wait < 100000);              // far short of the 600 s a fault may be waited out
    CHECK_EQ_INT(F.st[F.nst - 1], SWS_OFFLINE);
    CHECK_EQ_INT(F.st[F.nst - 2], SWS_NO_PAPER);
    CHECK_EQ_INT(F.g_n, 1);                         // the second block never went
    sw_session_free(s);

    // Silent from the start: begin ends at 30 s.
    fake_init(SW_II, 0);
    F.silent = 1;
    s = session(SW_II, 4, 0, 600000);
    uint32_t t0 = now_ms();
    CHECK_EQ_INT(sw_session_begin(s), SW_E_SILENT);
    CHECK(now_ms() - t0 >= 30000 && now_ms() - t0 <= 36000);
    CHECK_EQ_INT(F.st[F.nst - 1], SWS_OFFLINE);
    sw_session_free(s);

    // A job that waits two minutes for its next frames (the rip is slow) has not found the
    // printer silent: the clock counts from when it was handed work. A printer that misses
    // one question in that first moment is asked again.
    fake_init(SW_II, 0);
    s = session(SW_II, 4, 0, 600000);
    CHECK_EQ_INT(sw_session_begin(s), SW_OK);
    F.us += 120ull * 1000 * 1000;
    F.deaf_until = now_ms() + 8000;
    t0 = now_ms();
    n = fr_rect(g_job, 'R', 16, 0, 99, 99);
    CHECK_EQ_INT(sw_session_feed(s, g_job, n), SW_OK);
    CHECK(now_ms() - t0 >= 8000 && now_ms() - t0 <= 12000);
    sw_session_free(s);

    // A write that fails is the same.
    fake_init(SW_II, 0);
    F.fail_write = 1;
    s = session(SW_II, 4, 0, 600000);
    CHECK_EQ_INT(sw_session_begin(s), SW_E_SILENT);
    sw_session_free(s);

    // A printer that answers late, after the first 5 s try, is asked again and answers.
    fake_init(SW_II, 0);
    F.settle_ms = 6500;
    s = session(SW_II, 4, 0, 600000);
    CHECK_EQ_INT(sw_session_begin(s), SW_OK);
    CHECK(F.resets == 2 && F.sleeps[1] == 8000);          // the reset again, with the long settle
    sw_session_free(s);
}

// ---- the cartridge

typedef struct { sw_session_t *s; int rc; } feed_t;

static void feed_out(void *ctx, const uint8_t *f, size_t n)
{
    feed_t *fd = ctx;
    if (!fd->rc) fd->rc = sw_session_feed(fd->s, f, n);
}

// A page of 64 x 300 dots through sw_page, a colour one given to ink_row.
static void rip_page(sw_page_t *p)
{
    rip_stage_t st;
    sw_page_stage(p, &st);
    uint8_t cmy[8], k[8];
    memset(cmy, 0xAA, sizeof cmy);
    memset(k, 0xCC, sizeof k);
    const uint8_t *planes[IW_INKS];
    planes[IW_CYAN] = cmy; planes[IW_MAGENTA] = cmy; planes[IW_YELLOW] = cmy; planes[IW_BLACK] = k;
    st.page_begin(st.ctx, 0, 64, 300);
    for (int y = 0; y < 300; y++) st.ink_row(st.ctx, planes);
    st.page_end(st.ctx);
}

static void test_cart_swapped(void)
{
    // A colour job, but the cartridge is black: begin sets up a black page, the cartridge
    // reads 0, and the page stage built from that prints R rects of K only.
    fake_init(SW_C2200, 0);
    sw_session_t *s = session(SW_C2200, 4, 1, 600000);
    CHECK_EQ_INT(sw_session_begin(s), SW_OK);
    CHECK_EQ_INT(sw_session_cart(s), 0);
    CHECK_STR(setup_of(F.trace, "\x01"), "m2bA");
    feed_t fd = { s, 0 };
    sw_page_cfg_t cfg = { SW_C2200, sw_session_cart(s), feed_out, &fd };
    sw_page_t *p = sw_page_new(&cfg);
    CHECK(p != NULL);
    rip_page(p);
    CHECK_EQ_INT(fd.rc, SW_OK);
    CHECK_EQ_INT(sw_session_end(s), SW_OK);
    CHECK_STR(F.trace, "00 [I] [1] [2] ? D [B] [H] m2 b A B L [1] [2] R G 0C [1] E l");
    CHECK_EQ_INT(count_tok(F.trace, "c"), 0);
    sw_page_free(p);
    sw_session_free(s);

    // The same job with the cartridge colour: c rects, the page mode H.
    fake_init(SW_C2200, 1);
    s = session(SW_C2200, 4, 1, 600000);
    CHECK_EQ_INT(sw_session_begin(s), SW_OK);
    CHECK_EQ_INT(sw_session_cart(s), 1);
    fd = (feed_t){ s, 0 };
    cfg = (sw_page_cfg_t){ SW_C2200, sw_session_cart(s), feed_out, &fd };
    p = sw_page_new(&cfg);
    rip_page(p);
    CHECK_EQ_INT(fd.rc, SW_OK);
    CHECK_EQ_INT(sw_session_end(s), SW_OK);
    CHECK_STR(F.trace, "00 [I] [1] [2] ? D [B] [H] m2 n Z A H L [1] [2] c G 0C [1] E l");
    sw_page_free(p);
    sw_session_free(s);

    // A stage built for colour against a black cartridge is refused before any colour rect.
    fake_init(SW_C2200, 0);
    s = session(SW_C2200, 4, 1, 600000);
    CHECK_EQ_INT(sw_session_begin(s), SW_OK);
    fd = (feed_t){ s, 0 };
    cfg = (sw_page_cfg_t){ SW_C2200, 1, feed_out, &fd };
    p = sw_page_new(&cfg);
    rip_page(p);
    CHECK_EQ_INT(fd.rc, SW_E_CART);
    CHECK_EQ_INT(count_tok(F.trace, "c"), 0);
    CHECK_EQ_INT(F.g_n, 0);
    sw_page_free(p);
    sw_session_free(s);

    // and a colour page for a job not made for colour
    fake_init(SW_C2200, 1);
    s = session(SW_C2200, 4, 0, 600000);
    CHECK_EQ_INT(sw_session_begin(s), SW_OK);
    uint8_t f[4];
    CHECK_EQ_INT(sw_session_feed(s, f, fr_page(f, 1)), SW_E_CART);
    sw_session_free(s);

    // H that is neither 81 nor 01 on a Color StyleWriter 2200: no cartridge, SW_E_CART
    fake_init(SW_C2200, 1);
    F.h = 0x00;
    s = session(SW_C2200, 4, 1, 600000);
    CHECK_EQ_INT(sw_session_begin(s), SW_E_CART);
    CHECK_STR(F.trace, "00 [I] [1] [2] ? D [B] [H]");
    sw_session_free(s);
    // the 1500 reads bit 80 alone
    fake_init(SW_C1500, 1);
    F.h = 0x83;
    s = session(SW_C1500, 4, 1, 600000);
    CHECK_EQ_INT(sw_session_begin(s), SW_OK);
    CHECK_EQ_INT(sw_session_cart(s), 1);
    sw_session_free(s);
}

static void test_ask_cart(void)
{
    sw_ops_t o = ops();
    fake_init(SW_C2200, 1);
    CHECK_EQ_INT(sw_ask_cart(&o, SW_C2200), 1);
    CHECK_STR(F.trace, "D [H]");
    fake_init(SW_C2500, 0);
    CHECK_EQ_INT(sw_ask_cart(&o, SW_C2500), 0);
    fake_init(SW_C2400, 1);
    F.h = 0x00;
    CHECK_EQ_INT(sw_ask_cart(&o, SW_C2400), SW_E_CART);
    fake_init(SW_C1500, 1);
    F.h = 0x80;
    CHECK_EQ_INT(sw_ask_cart(&o, SW_C1500), 1);
    fake_init(SW_II, 0);
    CHECK_EQ_INT(sw_ask_cart(&o, SW_II), 0);
    CHECK_STR(F.trace, "[H]");
    fake_init(SW_ORIGINAL, 0);
    CHECK_EQ_INT(sw_ask_cart(&o, SW_ORIGINAL), 0);
    CHECK_EQ_INT((long)F.rn, 0);
    fake_init(SW_C2200, 1);
    F.silent = 1;
    CHECK_EQ_INT(sw_ask_cart(&o, SW_C2200), SW_E_SILENT);
}

// ---- recognition

static void test_plain(void)
{
    // The plain question is the byte `?` and nothing else, unless the reply is CS (below).
    static const struct { const char *ident; int awake; sw_model_t want; } t[] = {
        { "SW\r",   1, SW_II },
        { "SW3\r",  1, SW_1200 },
        { "IJ10\r", 1, SW_ORIGINAL },          // an original that happens to answer
        { "IJ10\r", 0, SW_NONE },              // asleep until reset
        { "XYZ\r",  1, SW_NONE },
        { "\r",     1, SW_NONE },
    };
    for (size_t i = 0; i < sizeof t / sizeof t[0]; i++) {
        fake_init(SW_II, 0);
        F.ident = t[i].ident;
        F.awake = t[i].awake;
        sw_ops_t o = ops();
        CHECK_EQ_INT(sw_ask_plain(&o), t[i].want);
        CHECK_EQ_INT((long)F.rn, 1);
        CHECK_EQ_INT(F.raw[0], 0x3F);
        CHECK_EQ_INT(F.nwrites, 1);
        CHECK_EQ_INT(F.nsleeps, 0);
    }
    // A CS proves a colour StyleWriter, so `p` follows as a write of its own, `?` first: its
    // byte names the model (01 2400, 02 2200, 04 1500, 05 2500).
    static const struct { sw_model_t m; sw_model_t want; } cs[] = {
        { SW_C2400, SW_C2400 }, { SW_C2200, SW_C2200 }, { SW_C1500, SW_C1500 }, { SW_C2500, SW_C2500 },
    };
    for (size_t i = 0; i < sizeof cs / sizeof cs[0]; i++) {
        fake_init(cs[i].m, 1);
        sw_ops_t oc = ops();
        CHECK_EQ_INT(sw_ask_plain(&oc), cs[i].want);
        static const uint8_t q[5] = { 0x3F, 0xFF, 0xFF, 0xFF, 0x70 };
        CHECK_EQ_INT((long)F.rn, 5);
        CHECK_EQ_MEM(F.raw, q, 5);
        CHECK_EQ_INT(F.nwrites, 2);                          // `?` alone, then `p` alone
        CHECK_STR(F.trace, "? [p]");
        CHECK_EQ_INT(F.bad, 0);
        CHECK_EQ_INT(F.nsleeps, 0);
    }
    // a byte from p that the drivers do not know is the 2200, as in the full sequence
    fake_init(SW_C2500, 1);
    F.p = 0x09;
    sw_ops_t o = ops();
    CHECK_EQ_INT(sw_ask_plain(&o), SW_C2200);
    // a CS whose p goes unanswered is the family's stand-in, after 250 ms
    fake_init(SW_C2500, 1);
    F.no_p = 1;
    uint32_t t0 = now_ms();
    CHECK_EQ_INT(sw_ask_plain(&o), SW_C2200);
    CHECK_EQ_INT((long)F.rn, 5);
    CHECK_EQ_INT(F.nwrites, 2);
    CHECK(now_ms() - t0 >= 250 && now_ms() - t0 <= 260);
    // and no p goes to anything that is not a CS: SW, SW3, IJ10 and silence were one byte above
    // nobody home
    fake_init(SW_II, 0);
    F.silent = 1;
    CHECK_EQ_INT(sw_ask_plain(&o), SW_NONE);
    CHECK_EQ_INT((long)F.rn, 1);
    // a reply that ends without its CR counts
    fake_init(SW_II, 0);
    F.ident = "SW";
    CHECK_EQ_INT(sw_ask_plain(&o), SW_II);
    // a stray byte ahead of the reply is drained
    fake_init(SW_1200, 0);
    push('X');
    CHECK_EQ_INT(sw_ask_plain(&o), SW_1200);
}

static void test_ask_full(void)
{
    sw_ops_t o = ops();
    // `p` only after CS
    static const struct { sw_model_t m; sw_model_t want; } t[] = {
        { SW_ORIGINAL, SW_ORIGINAL }, { SW_II, SW_II }, { SW_1200, SW_1200 },
        { SW_C1500, SW_C1500 }, { SW_C2200, SW_C2200 }, { SW_C2400, SW_C2400 }, { SW_C2500, SW_C2500 },
    };
    for (size_t i = 0; i < sizeof t / sizeof t[0]; i++) {
        fake_init(t[i].m, 1);
        CHECK_EQ_INT(sw_ask_full(&o), t[i].want);
        CHECK_EQ_INT(F.bad, 0);
        if (t[i].m >= SW_C1500) CHECK_STR(F.trace, "00 [I] ? [p]");
        else CHECK_STR(F.trace, "00 [I] ?");
        CHECK_EQ_INT(F.nsleeps, 1);
        CHECK_EQ_INT(F.sleeps[0], 2000);
        static const uint8_t full[] = { 0x00, 0xFF, 0xFF, 0xFF, 0x49 };
        CHECK_EQ_MEM(F.raw, full, 5);
    }
    // a p byte it does not know is a 2200
    fake_init(SW_C2200, 1);
    F.p = 0x09;
    CHECK_EQ_INT(sw_ask_full(&o), SW_C2200);
    // a CS that does not answer p is not named
    fake_init(SW_C2200, 1);
    F.no_p = 1;
    CHECK_EQ_INT(sw_ask_full(&o), SW_NONE);
    // 2 s got nothing, so 8 s: the printer takes 5 s to come back from a reset
    fake_init(SW_II, 0);
    F.settle_ms = 5000;
    CHECK_EQ_INT(sw_ask_full(&o), SW_II);
    CHECK_EQ_INT(F.nsleeps, 2);
    CHECK_EQ_INT(F.sleeps[0], 2000);
    CHECK_EQ_INT(F.sleeps[1], 8000);
    CHECK_EQ_INT(F.resets, 2);
    // and the original, which would not have answered `?` before its reset
    fake_init(SW_ORIGINAL, 0);
    F.settle_ms = 5000;
    CHECK_EQ_INT(sw_ask_full(&o), SW_ORIGINAL);
    // nothing at all: two tries, then none
    fake_init(SW_II, 0);
    F.silent = 1;
    CHECK_EQ_INT(sw_ask_full(&o), SW_NONE);
    CHECK_EQ_INT(F.nsleeps, 2);
    // a reply that is not a StyleWriter's is not tried again
    fake_init(SW_II, 0);
    F.ident = "LW\r";
    CHECK_EQ_INT(sw_ask_full(&o), SW_NONE);
    CHECK_EQ_INT(F.nsleeps, 1);
    // noise on the line during the reset is thrown away before `?`
    fake_init(SW_II, 0);
    F.noise_on_reset = 1;
    CHECK_EQ_INT(sw_ask_full(&o), SW_II);
    // a stray byte also does not stop a job's `?`
    fake_init(SW_II, 0);
    F.noise_on_reset = 1;
    sw_session_t *s = session(SW_II, 4, 0, 600000);
    CHECK_EQ_INT(sw_session_begin(s), SW_OK);
    sw_session_free(s);
}

static void test_wrong_model(void)
{
    // The printer says SW3; the job was made for a StyleWriter II.
    fake_init(SW_1200, 0);
    sw_session_t *s = session(SW_II, 4, 0, 600000);
    CHECK_EQ_INT(sw_session_begin(s), SW_E_MODEL);
    CHECK_STR(F.trace, "00 [I] [1] [2] ?");                 // nothing after `?`
    sw_session_free(s);
    // a CS for a II, a II for a CS
    fake_init(SW_C2200, 0);
    s = session(SW_II, 4, 0, 600000);
    CHECK_EQ_INT(sw_session_begin(s), SW_E_MODEL);
    CHECK_STR(F.trace, "00 [I] [1] [2] ?");
    sw_session_free(s);
    fake_init(SW_II, 0);
    s = session(SW_C2500, 4, 0, 600000);
    CHECK_EQ_INT(sw_session_begin(s), SW_E_MODEL);
    sw_session_free(s);
    // `?` does not say which colour model: a 1500 job on a 2200 begins
    fake_init(SW_C2200, 1);
    s = session(SW_C1500, 4, 0, 600000);
    CHECK_EQ_INT(sw_session_begin(s), SW_OK);
    sw_session_free(s);
    // but one whose empty buffer cannot hold the model's blocks is not that model: a 2500
    // (30720 bytes) for a job made for a 2200 (blocks of up to 32375) ends before the setup
    fake_init(SW_C2500, 1);
    F.s2 = 0;                                  // (its spurious 80 would be a fault to a 2200 first)
    s = session(SW_C2200, 4, 0, 600000);
    CHECK_EQ_INT(sw_session_begin(s), SW_E_MODEL);
    CHECK_STR(F.trace, "00 [I] [1] [2] ? D [B] [H]");
    sw_session_free(s);
    // and the reverse is fine
    fake_init(SW_C2200, 1);
    s = session(SW_C2500, 4, 0, 600000);
    CHECK_EQ_INT(sw_session_begin(s), SW_OK);
    sw_session_free(s);
    // a block no empty buffer could take is not sent
    fake_init(SW_C2200, 1);
    s = session(SW_C2200, 4, 0, 600000);
    CHECK_EQ_INT(sw_session_begin(s), SW_OK);
    size_t n = fr_rect(g_job, 'R', 0, 0, 99, 99);
    n += fr_block(g_job + n, 0x8000);
    CHECK_EQ_INT(sw_session_feed(s, g_job, n), SW_E_FAULT);
    CHECK_EQ_INT(F.g_n, 0);
    sw_session_free(s);
}

// A printer whose `B` reads one step short of empty (256 bytes, 64 on the original) still
// begins the job and takes the largest block the model sends, at once: the quantisation
// varies (the 2200 reads A2 or A3 at empty), and an exact fit would fail every job while it
// read short, as the wrong model, or wait out the stall on a block.
static void test_b_one_short(void)
{
    for (int m = SW_ORIGINAL; m < SW_MODELS; m++)
        for (int cart = 0; cart <= 1; cart++) {
            const sw_spec_t *sp = sw_spec((sw_model_t)m);
            if (cart && !sp->colour) continue;
            int empty = -1;
            for (int b = 0; b < 256 && empty < 0; b++)
                if (sw_b_empty((sw_model_t)m, (uint8_t)b, cart)) empty = b;
            fake_init((sw_model_t)m, cart);
            F.b_ov = empty - 1;
            F.b_ov_from = 0;
            F.b_ov_to = 0xFFFFFFFFu;
            if (m == SW_C2400 || m == SW_C2500) F.s2 = 0;
            sw_session_t *s = session((sw_model_t)m, 4, cart, 600000);
            uint32_t cap = sw_block_cap((sw_model_t)m, cart ? 4 : 1);
            size_t n = page(g_job, cart, 1, cap);
            uint32_t t0 = now_ms();
            CHECK_EQ_INT(run(s, g_job, n), SW_OK);
            CHECK_EQ_INT(F.g_n, 1);
            CHECK_EQ_INT(F.g_len[0], cap);
            CHECK(now_ms() - t0 < 30000);
            sw_session_free(s);
        }
}

// The setup, page-mode and `L` letters are bytes in the printer's buffer as a rect is: the
// session counts them against what `B` last said. A printer that has not drained since `B`
// was read is never sent a block that only fits if they were not there.
static void test_letters_counted(void)
{
    fake_init(SW_C2500, 0);
    F.s2 = 0;
    F.drain = 0;                               // nothing prints until `B` is read again
    F.count_letters = 1;
    F.drain_from_b = 2;
    sw_session_t *s = session(SW_C2500, 4, 0, 600000);
    CHECK_EQ_INT(sw_session_begin(s), SW_OK);
    // What the session would believe free after a rect and a block if the letters took
    // nothing; the second block is sized to that exactly.
    int32_t empty = sw_free_bytes(SW_C2500, 0x80);
    uint32_t x1 = 20000, x2 = (uint32_t)(empty - 9 - (int32_t)(x1 + 4) - 4);
    size_t n = fr_page(g_job, 0);
    n += fr_rect(g_job + n, 'R', 0, 0, 99, 99);
    n += fr_block(g_job + n, x1);
    n += fr_block(g_job + n, x2);
    n += fr_end(g_job + n);
    CHECK_EQ_INT(sw_session_feed(s, g_job, n), SW_OK);
    CHECK_EQ_INT(F.g_n, 2);
    CHECK_EQ_INT(F.overflow, 0);
    CHECK_EQ_INT(F.nb, 2);                     // `B` was asked again before the second block
    sw_session_free(s);
}

// ---- cancel and abort

static void test_cancel(void)
{
    // A StyleWriter II: cancelled turns true as the first block begins. The block goes out
    // whole, then E and the reset.
    fake_init(SW_II, 0);
    F.cancel_on_g = 1;
    sw_session_t *s = session(SW_II, 4, 0, 600000);
    size_t n = fr_page(g_job, 0);
    n += fr_rect(g_job + n, 'R', 0, 0, 99, 99);
    n += fr_block(g_job + n, 8000);
    n += fr_block(g_job + n, 8000);
    n += fr_end(g_job + n);
    CHECK_EQ_INT(sw_session_begin(s), SW_OK);
    CHECK_EQ_INT(sw_session_feed(s, g_job, n), SW_E_CANCEL);
    CHECK_EQ_INT(F.g_n, 1);
    CHECK_EQ_INT(F.g_complete, 1);               // not cut off: its length and its 00 came
    CHECK_EQ_INT(F.g_len[0], 8000);
    CHECK_EQ_INT(F.bad, 0);
    CHECK(ends_with(F.trace, "G E 00 [I]"));   // black-only: E, then the reset
    size_t rn = F.rn;
    sw_session_abort(s);                          // once is enough
    CHECK_EQ_INT((long)F.rn, (long)rn);
    CHECK_EQ_INT(sw_session_feed(s, g_job, n), SW_E_CANCEL);
    CHECK_EQ_INT((long)F.rn, (long)rn);
    sw_session_free(s);

    // A colour model gets the reset and no E.
    fake_init(SW_C2200, 1);
    F.cancel_on_g = 1;
    s = session(SW_C2200, 4, 1, 600000);
    CHECK_EQ_INT(sw_session_begin(s), SW_OK);
    CHECK_EQ_INT(sw_session_feed(s, g_job, n), SW_E_CANCEL);
    CHECK_EQ_INT(count_tok(F.trace, "E"), 0);
    CHECK(ends_with(F.trace, "G 00 [I]"));
    CHECK_EQ_INT(F.g_complete, 1);
    sw_session_free(s);

    // Cancelled while out of paper: the wait gives way.
    fake_init(SW_C2200, 0);
    F.arm_after_g = 1;
    F.arm_dur = 0xFFFFFFFFu;
    F.arm_s2 = 0x04;
    s = session(SW_C2200, 4, 0, 600000);
    n = fr_page(g_job, 0);
    n += fr_rect(g_job + n, 'R', 0, 0, 99, 99);
    n += fr_block(g_job + n, 5000);
    n += fr_rect(g_job + n, 'R', 0, 100, 199, 199);
    n += fr_block(g_job + n, 5000);
    CHECK_EQ_INT(sw_session_begin(s), SW_OK);
    F.cancel_at_ms = now_ms() + 20000;
    uint32_t t0 = now_ms();
    CHECK_EQ_INT(sw_session_feed(s, g_job, n), SW_E_CANCEL);
    CHECK(now_ms() - t0 < 30000);
    CHECK_EQ_INT(F.g_n, 1);
    sw_session_free(s);

    // Cancelled before it has begun.
    fake_init(SW_II, 0);
    F.cancel = 1;
    s = session(SW_II, 4, 0, 600000);
    CHECK_EQ_INT(sw_session_begin(s), SW_E_CANCEL);
    sw_session_free(s);

    // sw_session_abort on its own: E (black-only) and the reset, once.
    fake_init(SW_1200, 0);
    s = session(SW_1200, 4, 0, 600000);
    CHECK_EQ_INT(sw_session_begin(s), SW_OK);
    sw_session_abort(s);
    CHECK(ends_with(F.trace, "E 00 [I]"));
    rn = F.rn;
    sw_session_abort(s);
    CHECK_EQ_INT((long)F.rn, (long)rn);
    sw_session_free(s);
}

// ---- cleaning

static void test_clean(void)
{
    sw_ops_t o = ops();
    // The original has none: nothing is sent.
    fake_init(SW_ORIGINAL, 0);
    CHECK_EQ_INT(sw_clean(&o, SW_ORIGINAL), SW_E_FAULT);
    CHECK_EQ_INT((long)F.rn, 0);
    CHECK_EQ_INT(sw_clean(&o, SW_NONE), SW_E_MODEL);

    // The others: the job's setup with one P, then until the busy has gone.
    static const struct { sw_model_t m; int cart; const char *trace; } t[] = {
        { SW_II,    0, "00 [I] [1] [2] ? [B] [H] u n D P A" },
        { SW_1200,  0, "00 [I] [1] [2] ? N [B] [H] u n D P A" },
        { SW_C1500, 1, "00 [I] [1] [2] ? D [B] [H] m2 Z P A" },
        { SW_C2200, 1, "00 [I] [1] [2] ? D [B] [H] m2 n Z P A" },
        { SW_C2400, 0, "00 [I] [1] [2] ? D [B] [H] m2 b P A" },
        { SW_C2500, 1, "00 [I] [1] [2] ? D [B] [H] m2 n Z P A" },
    };
    for (size_t i = 0; i < sizeof t / sizeof t[0]; i++) {
        fake_init(t[i].m, t[i].cart);
        F.clean_ms = 12000;
        uint32_t t0 = now_ms();
        CHECK_EQ_INT(sw_clean(&o, t[i].m), SW_OK);
        CHECK_EQ_INT(F.cleanings, 1);
        CHECK(now_ms() - t0 >= 12000);
        CHECK_EQ_INT(count_tok(F.trace, "P"), 1);
        char want[256];
        // the setup, then [1] polls, then E (Y on the 1200) and l
        char *p = strstr(F.trace, " A ");
        CHECK(p != NULL);
        size_t k = (size_t)(p - F.trace) + 2;
        memcpy(want, F.trace, k);
        want[k] = 0;
        CHECK_STR(want, t[i].trace);
        CHECK(ends_with(F.trace, t[i].m == SW_1200 ? "E Y l" : "E l"));
        CHECK(count_tok(F.trace, "[1]") > 3);
        CHECK_EQ_INT(F.bad, 0);
        CHECK_EQ_INT(F.st[F.nst - 1], SWS_READY);
    }
    // A printer that never reads busy is done after 5 s.
    fake_init(SW_II, 0);
    uint32_t t0 = now_ms();
    CHECK_EQ_INT(sw_clean(&o, SW_II), SW_OK);
    CHECK(now_ms() - t0 >= 5000 && now_ms() - t0 <= 12000);
    // One that stays busy is given 600 s, then reset.
    fake_init(SW_C2200, 1);
    F.clean_ms = 0x7FFFFFFF;
    t0 = now_ms();
    CHECK_EQ_INT(sw_clean(&o, SW_C2200), SW_E_STALL);
    CHECK(now_ms() - t0 >= 600000 && now_ms() - t0 <= 615000);
    CHECK(ends_with(F.trace, "00 [I]"));
    CHECK_EQ_INT(count_tok(F.trace, "E"), 0);
    // One that goes silent
    fake_init(SW_II, 0);
    F.silent = 1;
    CHECK_EQ_INT(sw_clean(&o, SW_II), SW_E_SILENT);
    CHECK_EQ_INT(count_tok(F.trace, "P"), 0);
    // Cancelled during the cleaning: E and the reset.
    fake_init(SW_II, 0);
    F.clean_ms = 100000;
    F.cancel_at_ms = now_ms() + 30000;
    CHECK_EQ_INT(sw_clean(&o, SW_II), SW_E_CANCEL);
    CHECK(ends_with(F.trace, "E 00 [I]"));
}

// ---- frames

static void test_frames(void)
{
    // The same job fed whole, a byte at a time and in odd pieces writes the same bytes.
    uint8_t whole[1 << 16];
    size_t wn = 0;
    size_t n = page(g_job, 1, 2, 3000);
    n += page(g_job + n, 0, 1, 700);
    static const size_t piece[] = { 0, 1, 2, 3, 7, 4096, 5 };
    for (size_t k = 0; k < sizeof piece / sizeof piece[0]; k++) {
        fake_init(SW_C2200, 1);
        sw_session_t *s = session(SW_C2200, 4, 1, 600000);
        CHECK_EQ_INT(sw_session_begin(s), SW_OK);
        if (piece[k] == 0) CHECK_EQ_INT(sw_session_feed(s, g_job, n), SW_OK);
        else for (size_t i = 0; i < n; i += piece[k]) {
            size_t c = n - i < piece[k] ? n - i : piece[k];
            int rc = sw_session_feed(s, g_job + i, c);
            CHECK_EQ_INT(rc, SW_OK);
            if (rc) break;
        }
        CHECK_EQ_INT(sw_session_end(s), SW_OK);
        CHECK_EQ_INT(F.g_complete, 3);
        CHECK_EQ_INT(F.bad, 0);
        if (k == 0) { wn = F.rn < sizeof whole ? F.rn : sizeof whole; memcpy(whole, F.raw, wn); }
        else { CHECK_EQ_INT((long)F.rn, (long)wn); CHECK_EQ_MEM(F.raw, whole, wn < F.rn ? wn : F.rn); }
        sw_session_free(s);
    }
    // The wire: a block is G, its length, its data, 00.
    fake_init(SW_II, 0);
    sw_session_t *s = session(SW_II, 4, 0, 600000);
    CHECK_EQ_INT(sw_session_begin(s), SW_OK);
    size_t base = F.rn;
    uint8_t b[16 + 3];
    size_t bn = fr_block(b, 16);
    CHECK_EQ_INT(sw_session_feed(s, b, bn), SW_OK);
    CHECK_EQ_INT((long)(F.rn - base), 16 + 4);
    CHECK_EQ_INT(F.raw[base], 'G');
    CHECK_EQ_INT(F.raw[base + 1], 16);
    CHECK_EQ_INT(F.raw[base + 2], 0);
    CHECK_EQ_MEM(F.raw + base + 3, b + 3, 16);
    CHECK_EQ_INT(F.raw[base + 3 + 16], 0);
    // a rect goes as it came
    uint8_t r[12];
    fr_rect(r, 'R', 16, 0, 2895, 799);
    CHECK_EQ_INT(sw_session_feed(s, r, 12), SW_OK);
    CHECK_EQ_MEM(F.raw + F.rn - 9, r + 3, 9);
    // frames that are not the stage's
    uint8_t bad[8] = { 9, 0, 0 };
    CHECK_EQ_INT(sw_session_feed(s, bad, 3), SW_E_FAULT);
    sw_session_free(s);
    fake_init(SW_II, 0);
    s = session(SW_II, 4, 0, 600000);
    CHECK_EQ_INT(sw_session_begin(s), SW_OK);
    uint8_t e[3] = { SWF_BLOCK, 0, 0 };
    CHECK_EQ_INT(sw_session_feed(s, e, 3), SW_E_FAULT);        // an empty block
    sw_session_free(s);
    fake_init(SW_II, 0);
    s = session(SW_II, 4, 0, 600000);
    CHECK_EQ_INT(sw_session_begin(s), SW_OK);
    uint8_t short_rect[12] = { SWF_RECT, 8, 0, 'R' };
    CHECK_EQ_INT(sw_session_feed(s, short_rect, 11), SW_E_FAULT);
    sw_session_free(s);
    // a block longer than the buffer would hold, announced across feeds, is refused when its
    // length is known
    fake_init(SW_II, 0);
    s = session(SW_II, 4, 0, 600000);
    CHECK_EQ_INT(sw_session_begin(s), SW_OK);
    uint8_t huge[3] = { SWF_BLOCK, 0xFF, 0xFF };
    CHECK_EQ_INT(sw_session_feed(s, huge, 2), SW_OK);
    CHECK_EQ_INT(sw_session_feed(s, huge + 2, 1), SW_E_FAULT);
    sw_session_free(s);
    // the job ends with half a frame in hand
    fake_init(SW_II, 0);
    s = session(SW_II, 4, 0, 600000);
    CHECK_EQ_INT(sw_session_begin(s), SW_OK);
    CHECK_EQ_INT(sw_session_feed(s, b, 5), SW_OK);
    CHECK_EQ_INT(sw_session_end(s), SW_E_FAULT);
    sw_session_free(s);
    // feeding before begin
    fake_init(SW_II, 0);
    s = session(SW_II, 4, 0, 600000);
    CHECK_EQ_INT(sw_session_feed(s, b, bn), SW_E_FAULT);
    CHECK_EQ_INT((long)F.rn, 0);
    sw_session_free(s);
    // a bad model or no ops
    sw_ops_t o = ops();
    sw_job_cfg_t c = { SW_NONE, 4, 0, 0 };
    CHECK(sw_session_new(&o, &c) == NULL);
    c.model = SW_II;
    CHECK(sw_session_new(NULL, &c) == NULL);
    o.write = NULL;
    CHECK(sw_session_new(&o, &c) == NULL);
}

static void test_end(void)
{
    // The end waits for 08: the page takes 20 s to print.
    fake_init(SW_C2200, 1);
    F.drain = 300;                               // the buffer holds 1000 bytes: ~3 s
    sw_session_t *s = session(SW_C2200, 4, 0, 600000);
    size_t n = page(g_job, 0, 1, 6000);
    CHECK_EQ_INT(sw_session_begin(s), SW_OK);
    CHECK_EQ_INT(sw_session_feed(s, g_job, n), SW_OK);
    uint32_t t0 = now_ms();
    CHECK_EQ_INT(sw_session_end(s), SW_OK);
    CHECK(now_ms() - t0 >= 10000);
    CHECK(ends_with(F.trace, "[1] E l"));
    CHECK_EQ_INT(F.st[F.nst - 1], SWS_READY);
    sw_session_free(s);

    // A page that never prints: stall_ms with no progress. The buffer does not empty.
    fake_init(SW_C2200, 1);
    F.drain = 0;
    s = session(SW_C2200, 4, 0, 90000);
    n = page(g_job, 0, 1, 1000);
    CHECK_EQ_INT(sw_session_begin(s), SW_OK);
    CHECK_EQ_INT(sw_session_feed(s, g_job, n), SW_OK);
    t0 = now_ms();
    CHECK_EQ_INT(sw_session_end(s), SW_E_STALL);
    CHECK(now_ms() - t0 >= 90000 && now_ms() - t0 <= 95000);
    sw_session_free(s);

    // A fault at the end holds it, then lets go.
    fake_init(SW_C2200, 1);
    F.drain = 1000;
    s = session(SW_C2200, 4, 0, 600000);
    n = page(g_job, 0, 1, 6000);
    CHECK_EQ_INT(sw_session_begin(s), SW_OK);
    CHECK_EQ_INT(sw_session_feed(s, g_job, n), SW_OK);
    fault(now_ms(), now_ms() + 15000, 0, 0x08);
    CHECK_EQ_INT(sw_session_end(s), SW_OK);
    CHECK_EQ_INT(F.st[1], SWS_JAM);
    CHECK_EQ_INT(F.st[2], SWS_PRINTING);
    CHECK(count_tok(F.trace, "[S]") >= 1);
    sw_session_free(s);

    // A job of no pages does not wait for a page that was never sent.
    fake_init(SW_II, 0);
    s = session(SW_II, 4, 0, 600000);
    CHECK_EQ_INT(sw_session_begin(s), SW_OK);
    CHECK_EQ_INT(sw_session_end(s), SW_OK);
    CHECK_STR(F.trace, "00 [I] [1] [2] ? [B] [H] u n D A E l");
    sw_session_free(s);

    // The clock wrapping in the middle of a job.
    fake_init(SW_C2200, 1);
    F.us = (0xFFFFFFFFull - 4000) * 1000;
    s = session(SW_C2200, 4, 1, 600000);
    n = page(g_job, 1, 1, 1000);
    CHECK_EQ_INT(run(s, g_job, n), SW_OK);
    CHECK_STR(F.trace, "00 [I] [1] [2] ? D [B] [H] m2 n Z A H L [1] [2] c G 0C [1] E l");
    sw_session_free(s);
}

int main(void)
{
    test_begin_order();
    test_quality();
    test_pacing();
    test_paper_out();
    test_fault_bits();
    test_silent();
    test_cart_swapped();
    test_ask_cart();
    test_plain();
    test_ask_full();
    test_wrong_model();
    test_b_one_short();
    test_letters_counted();
    test_cancel();
    test_clean();
    test_frames();
    test_end();
    TEST_END;
}
