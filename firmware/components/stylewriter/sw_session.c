// SPDX-License-Identifier: GPL-3.0-or-later
//
// The StyleWriter session (sw_session.h, W-1103). Pure C: the line and the clock are
// sw_ops_t's, so tools/rip's test_sw_session.c runs all of it against a scripted printer.
//
// References are to TailTalk (github.com/FeralFirmware/TailTalk at bf38658, GPL-3.0 with no
// "or later" grant, so read for facts and never copied): tailtalk/src/stylewriter/job.rs,
// session.rs and status.rs. The constants and the letters below are those facts, in our
// own words.
#include "sw_session.h"
#include "sw_page.h"
#include "bigmem.h"
#include <stdlib.h>
#include <string.h>

// ---- Times, in ms

#define SETTLE_MS        2000   // after the reset (session.rs:37)
#define SETTLE_LONG_MS   8000   // the original, and a printer that said nothing at 2 s (:41)
#define ID_BYTE_MS        250   // `?` is read to its CR at 250 ms a byte (design, "Recognising it")
#define ANSWER_MS        5000   // one try at a one-byte answer in a job (:48); asked again
#define IDLE_ANSWER_MS   1000   // one try at an idle check; asked twice
#define SILENT_MS       30000   // nothing heard since the last answer (Wells's rule)
#define POLL_MS          1000   // between looks at `1` and `2` while a fault holds (:483)
#define RESUME_MS        5000   // between `S` while a fault holds (:456)
#define ROOM_MS           100   // between `B` while the buffer has no room
#define ROOM_LOOK_MS     1000   // `1` and `2` are looked at this often while waiting for room
#define PRINTED_MS        250   // between looks at `1` for page done
#define GRACE_MS        10000   // a fault the design does not name may clear itself this long
#define CLEAN_START_MS   5000   // a cleaning that never reads busy is done after this (:56)
#define CLEAN_POLL_MS     500   // between looks at `1` during a cleaning (:921)
#define CLEAN_MS       600000   // the longest a cleaning may take (:60)
#define STALL_MS       600000   // sw_job_cfg_t.stall_ms of 0

enum { FRAME_HEAD = 3, RECT_LEN = 9, G_FRAMING = 4 };

static const uint8_t k_reset[5]  = { 0x00, 0xFF, 0xFF, 0xFF, 'I' };    // eject the page and reset
static const uint8_t k_resume[4] = { 0xFF, 0xFF, 0xFF, 'S' };
static const uint8_t k_zero      = 0x00;                               // ends a `G` block
static const uint8_t k_formfeed        = 0x0C;                               // ends a page

// ---- The letters, per model
//
// Per job (session.rs:599-603, 611): after `?` a Color StyleWriter (the 1500 too) takes `D`,
// the 1200 `N`, the II and the original nothing; then `B`; then `H` on every model but the
// original. Then the setup (job.rs:159-198), a string of letters that is sent whole, in
// which `*` is where one `P` goes for a cleaning and nowhere else:
//   Color StyleWriter 2200, 2400, 2500 (job.rs:159-174)  m2, then the quality: draft d, normal
//       nZ with a colour cartridge or b with a black one, best s; then A
//   Color StyleWriter 1500             (job.rs:163)      the same, but normal is Z alone
//   StyleWriter II and 1200            (job.rs:186-196)  u, the quality (d, n, b), D, then A
//   StyleWriter                        (job.rs:185)      n, u, A, at any quality
// Per page (session.rs:677-722): the 2200, 2400 and 2500 take their page mode, `H` for a
// colour page on a colour cartridge and `B` otherwise (job.rs:118-123), when it changes,
// and `L`; the 1500 and the 1200 take `L`; the II and the original nothing. After the last
// page (session.rs:813-826): wait for `1` to say printed, then `E`, `Y` more on the 1200,
// then `l`; the original does not wait.
// Plain paper only (`m2`); the narrow-paper `F` and the fast-clock requests are left out.
typedef struct {
    char    pre;                // after `?`: `D`, `N`, or 0
    uint8_t reads_h;            // `H` is asked
    uint8_t mode;               // a page mode letter ahead of `L`
    char    page_l;             // `L` ahead of each page, or 0
    char    end_y;              // `Y` after `E`, or 0
    uint8_t waits;              // waits for page done before `E`
    const char *setup[3][2];    // [draft, normal, best][black cartridge, colour cartridge]
} sw_letters_t;

#define SETUP_CS    { { "m2d*A", "m2d*A" }, { "m2b*A", "m2nZ*A" }, { "m2s*A", "m2s*A" } }
#define SETUP_1500  { { "m2d*A", "m2d*A" }, { "m2Z*A", "m2Z*A" },   { "m2s*A", "m2s*A" } }
#define SETUP_II    { { "udD*A", "udD*A" }, { "unD*A", "unD*A" },   { "ubD*A", "ubD*A" } }
#define SETUP_ORIG  { { "nuA",   "nuA"   }, { "nuA",   "nuA"   },   { "nuA",   "nuA"   } }

static const sw_letters_t k_letters[SW_MODELS] = {
    [SW_ORIGINAL] = { 0,   0, 0, 0,   0,   0, SETUP_ORIG },
    [SW_II]       = { 0,   1, 0, 0,   0,   1, SETUP_II },
    [SW_1200]     = { 'N', 1, 0, 'L', 'Y', 1, SETUP_II },
    [SW_C1500]    = { 'D', 1, 0, 'L', 0,   1, SETUP_1500 },
    [SW_C2200]    = { 'D', 1, 1, 'L', 0,   1, SETUP_CS },
    [SW_C2400]    = { 'D', 1, 1, 'L', 0,   1, SETUP_CS },
    [SW_C2500]    = { 'D', 1, 1, 'L', 0,   1, SETUP_CS },
};

static int is_cs_model(sw_model_t m) { return m >= SW_C1500 && m <= SW_C2500; }   // answers `?` with CS
static int is_2200_series(sw_model_t m) { return m >= SW_C2200 && m <= SW_C2500; }

// ---- What `1` and `2` say (status.rs:74-105)
//
// The 2200, 2400 and 2500 report in `1` an error (02), a hardware error (04), the door (40)
// and the waste ink (80); the original, II, 1200 and 1500 report a hardware error (04) and,
// on the 1500 only, the waste ink; their bit 02, in `1` or `2`, is the printer holding the
// job and clears itself. Of `2`: 01 cartridge jammed, 02 ink out (not the 1200 series: the
// holding bit), 04 no paper, 08 paper jam, 10 error, 20 cartridge missing, 40 paper too
// small, 80 error: every bit counts on every model but the 2400 and 2500, where 80 is
// spurious (lpstyl's testers; the design). Busy (01) and page done (08) of `1` are no
// fault. Beyond TailTalk, which treats every bit alike: the ones the design names
// (paper, jam, cartridge, ink, door, waste) are waited out and reported; the others end
// the job after GRACE_MS.
enum { K_NONE, K_HOLD, K_NAMED, K_UNNAMED };

static sw_state_t read_state(sw_model_t m, uint8_t s1, uint8_t s2, int *kind)
{
    int cs = is_2200_series(m);
    uint8_t e = s2;
    if (m == SW_C2400 || m == SW_C2500) e &= (uint8_t)~0x80;
    *kind = K_NAMED;
    if (e & 0x04) return SWS_NO_PAPER;
    if (e & 0x08) return SWS_JAM;
    if (e & 0x01) return SWS_CART_JAM;
    if (e & 0x20) return SWS_CART_MISSING;
    if (cs && (e & 0x02)) return SWS_INK_OUT;
    if (cs && (s1 & 0x40)) return SWS_DOOR;
    if ((cs || m == SW_C1500) && (s1 & 0x80)) return SWS_WASTE;
    *kind = K_UNNAMED;
    if ((e & 0xD0) || (s1 & (cs ? 0x06 : 0x04))) return SWS_FAULT;
    *kind = K_HOLD;
    if (!cs && ((s1 & 0x02) || (e & 0x02))) return SWS_PRINTING;
    *kind = K_NONE;
    return (s1 & 0x01) ? SWS_PRINTING : SWS_READY;
}

// ---- The line

// One try at the one-byte answer to FF FF FF <q>: 1 it came, 0 it did not, -1 the write failed.
static int ask_once(const sw_ops_t *o, uint8_t q, uint8_t *ans, uint32_t first_ms)
{
    const uint8_t cmd[4] = { 0xFF, 0xFF, 0xFF, q };
    if (o->drain) o->drain(o->ctx);
    if (o->write(o->ctx, cmd, sizeof cmd)) return -1;
    return o->read(o->ctx, ans, 1, first_ms, 0) == 1;
}

// An idle question: two tries.
static int ask_idle(const sw_ops_t *o, uint8_t q, uint8_t *ans)
{
    for (int i = 0; i < 2; i++) {
        int r = ask_once(o, q, ans, IDLE_ANSWER_MS);
        if (r < 0) return SW_E_SILENT;
        if (r) return SW_OK;
    }
    return SW_E_SILENT;
}

// `?` and its reply read to the CR, a byte at a time, 250 ms after each; the first may take
// first_ms. A reply that stops without a CR counts. Returns the bytes (without the CR), 0 for
// none, or -1 when the write failed.
static int ask_ident(const sw_ops_t *o, uint8_t *r, size_t cap, uint32_t first_ms)
{
    static const uint8_t q = '?';
    if (o->drain) o->drain(o->ctx);
    if (o->write(o->ctx, &q, 1)) return -1;
    size_t n = 0;
    uint32_t wait = first_ms;
    while (n < cap) {
        uint8_t b;
        if (o->read(o->ctx, &b, 1, wait, ID_BYTE_MS) != 1) break;
        wait = ID_BYTE_MS;
        if (b == '\r') break;
        r[n++] = b;
    }
    return (int)n;
}

static int ops_ok(const sw_ops_t *o) { return o && o->write && o->read && o->sleep_ms && o->now_ms; }

// ---- Recognition and idle

sw_model_t sw_ask_plain(const sw_ops_t *o)
{
    if (!ops_ok(o)) return SW_NONE;
    uint8_t id[32];
    int n = ask_ident(o, id, sizeof id, ID_BYTE_MS);
    if (n <= 0) return SW_NONE;
    int cs = 0;
    sw_model_t m = sw_model_from_id(id, (size_t)n, &cs);
    return cs ? SW_C2200 : m;
}

sw_model_t sw_ask_full(const sw_ops_t *o)
{
    static const uint32_t settle[2] = { SETTLE_MS, SETTLE_LONG_MS };
    if (!ops_ok(o)) return SW_NONE;
    for (int i = 0; i < 2; i++) {
        if (o->write(o->ctx, k_reset, sizeof k_reset)) return SW_NONE;
        o->sleep_ms(o->ctx, settle[i]);
        uint8_t id[32];
        int n = ask_ident(o, id, sizeof id, ID_BYTE_MS);
        if (n < 0) return SW_NONE;
        if (n == 0) continue;                  // nothing at 2 s: the reset again, and 8 s
        int cs = 0;
        sw_model_t m = sw_model_from_id(id, (size_t)n, &cs);
        if (!cs) return m;
        // Only a CS may be asked `p`: any other model takes it as a fault and resets.
        for (int t = 0; t < 2; t++) {
            uint8_t p;
            int r = ask_once(o, 'p', &p, IDLE_ANSWER_MS);
            if (r < 0) return SW_NONE;
            if (r) return sw_model_from_p(p);
        }
        return SW_NONE;
    }
    return SW_NONE;
}

int sw_ask_status(const sw_ops_t *o, sw_model_t m, sw_state_t *st)
{
    if (!ops_ok(o) || !st || !sw_spec(m)) return SW_E_MODEL;
    uint8_t s1, s2;
    if (ask_idle(o, '1', &s1) || ask_idle(o, '2', &s2)) { *st = SWS_OFFLINE; return SW_E_SILENT; }
    int kind;
    *st = read_state(m, s1, s2, &kind);
    return SW_OK;
}

int sw_ask_cart(const sw_ops_t *o, sw_model_t m)
{
    if (!ops_ok(o) || !sw_spec(m)) return SW_E_MODEL;
    if (!k_letters[m].reads_h) return 0;       // the original has no `H`: black
    if (is_cs_model(m)) {                      // a Color StyleWriter reports its cartridge after `D` (session.rs:399-404)
        static const uint8_t d = 'D';
        if (o->write(o->ctx, &d, 1)) return SW_E_SILENT;
    }
    uint8_t h;
    if (ask_idle(o, 'H', &h)) return SW_E_SILENT;
    int c = sw_cart_colour(m, h);
    return c < 0 ? SW_E_CART : c;
}

// ---- A job

struct sw_session {
    sw_ops_t            o;
    sw_job_cfg_t        cfg;
    const sw_spec_t    *spec;
    const sw_letters_t *let;
    int                 q;              // 0 draft, 1 normal, 2 best
    int                 clean;          // sw_clean: one P in the setup
    int                 cart;           // what `H` said
    int32_t             free_b;         // the printer's free buffer: `B`, less what went since
    int                 mode;           // the page mode letter last sent
    int                 pages_done;
    int                 begun, aborted, cancelled_out, touched;
    int                 state_set;
    sw_state_t          state;
    unsigned            faults;         // faults begun, to tell progress from waiting
    uint32_t            last_heard;     // when the printer last said anything
    uint32_t            active_from;    // when the caller last gave the session something to do
    uint32_t            last_s;         // when `S` was last sent
    uint32_t            maxcap;         // the most a block may carry
    size_t              have;           // bytes of a frame held in `frame`
    size_t              frame_cap;
    uint8_t            *frame;
};

static uint32_t ms(const sw_session_t *s) { return s->o.now_ms(s->o.ctx); }
static int cancelled(const sw_session_t *s) { return s->o.cancelled && s->o.cancelled(s->o.ctx); }

static void show(sw_session_t *s, sw_state_t st)
{
    if (s->state_set && s->state == st) return;
    s->state_set = 1;
    s->state = st;
    if (s->o.state) s->o.state(s->o.ctx, st);
}

static int put(sw_session_t *s, const void *b, size_t n)
{
    s->touched = 1;
    if (s->o.write(s->o.ctx, b, n)) { show(s, SWS_OFFLINE); return SW_E_SILENT; }
    return SW_OK;
}

static int put1(sw_session_t *s, char c)
{
    uint8_t b = (uint8_t)c;
    return put(s, &b, 1);
}

// `E` (the black-only models) and the reset, once, and only if the session ever wrote.
static void abort_seq(sw_session_t *s)
{
    if (s->aborted) return;
    s->aborted = 1;
    s->have = 0;
    if (!s->touched) return;
    if (!s->spec->colour) {                    // session.rs:872-874
        static const uint8_t e = 'E';
        s->o.write(s->o.ctx, &e, 1);
    }
    s->o.write(s->o.ctx, k_reset, sizeof k_reset);
}

static int cancel_out(sw_session_t *s)
{
    abort_seq(s);
    s->cancelled_out = 1;
    return SW_E_CANCEL;
}

// The printer has been silent for SILENT_MS: it has said nothing since it last answered, or
// since the caller last handed the session work (a job waits tens of seconds for its next
// frames while the rip runs, and that is not the printer's silence), whichever is later.
static int silent(sw_session_t *s)
{
    uint32_t t = ms(s), a = (uint32_t)(t - s->last_heard), b = (uint32_t)(t - s->active_from);
    if ((a < b ? a : b) < SILENT_MS) return 0;
    show(s, SWS_OFFLINE);
    return 1;
}

// The one-byte answer to a query, asked again until it comes or the printer is silent.
static int query(sw_session_t *s, uint8_t q, uint8_t *ans)
{
    for (;;) {
        s->touched = 1;
        int r = ask_once(&s->o, q, ans, ANSWER_MS);
        if (r < 0) { show(s, SWS_OFFLINE); return SW_E_SILENT; }
        if (r) { s->last_heard = ms(s); return SW_OK; }
        if (silent(s)) return SW_E_SILENT;
        if (cancelled(s)) return cancel_out(s);
    }
}

static int status12(sw_session_t *s, uint8_t *s1, uint8_t *s2)
{
    int rc = query(s, '1', s1);
    return rc ? rc : query(s, '2', s2);
}

// Looks at `1` and `2` (the reading given, if `have`, then fresh ones) until neither shows
// a fault bit (session.rs:474-485, wait_until_clear, the gate before every band at :731).
// While one does: its state is shown, `S` goes at once and every RESUME_MS (a printer taken
// out of paper or out of a jam waits for it), and the wait is stall_ms (SW_E_STALL), or
// GRACE_MS for a fault the design does not name (SW_E_FAULT). When it clears, one more `S`
// lets the printer go on.
static int clear_loop(sw_session_t *s, int have, uint8_t s1, uint8_t s2)
{
    uint32_t onset = 0;
    int held = 0, resumed = 0;
    for (;;) {
        if (cancelled(s)) return cancel_out(s);
        int rc;
        if (!have && (rc = status12(s, &s1, &s2))) return rc;
        have = 0;
        int kind;
        sw_state_t st = read_state(s->cfg.model, s1, s2, &kind);
        if (kind == K_NONE) {
            if (held) {
                if (resumed && (rc = put(s, k_resume, sizeof k_resume))) return rc;
                show(s, SWS_PRINTING);
            }
            return SW_OK;
        }
        uint32_t t = ms(s);
        if (!held) { held = 1; onset = t; s->faults++; }
        uint32_t waited = (uint32_t)(t - onset);
        if (kind == K_UNNAMED && waited >= GRACE_MS) return SW_E_FAULT;
        if (waited >= s->cfg.stall_ms) return SW_E_STALL;
        if (kind != K_HOLD) {
            show(s, st);
            if (!resumed || (uint32_t)(t - s->last_s) >= RESUME_MS) {
                if ((rc = put(s, k_resume, sizeof k_resume))) return rc;
                s->last_s = t;
                resumed = 1;
            }
        }
        s->o.sleep_ms(s->o.ctx, POLL_MS);
    }
}

static int wait_clear(sw_session_t *s) { return clear_loop(s, 0, 0, 0); }

// Waits until `need` bytes fit the printer's buffer (session.rs:779-801, make_room): reads
// `B` now and every ROOM_MS after, looks at `1` and `2` every ROOM_LOOK_MS (a fault stops
// the buffer draining), and gives up when the free count has not risen for stall_ms.
static int wait_room(sw_session_t *s, int32_t need)
{
    if (s->free_b >= need) return SW_OK;
    uint32_t t_prog = ms(s), t_look = t_prog;
    int32_t best = s->free_b;
    for (;;) {
        if (cancelled(s)) return cancel_out(s);
        uint8_t b;
        int rc = query(s, 'B', &b);
        if (rc) return rc;
        s->free_b = sw_free_bytes(s->cfg.model, b);
        uint32_t t = ms(s);
        if (s->free_b >= need) return SW_OK;
        if (s->free_b > best) { best = s->free_b; t_prog = t; }
        if ((uint32_t)(t - t_look) >= ROOM_LOOK_MS) {
            unsigned f0 = s->faults;
            if ((rc = wait_clear(s))) return rc;
            t = t_look = ms(s);
            if (s->faults != f0) t_prog = t;               // a fault that cleared is progress
        }
        if ((uint32_t)(t - t_prog) >= s->cfg.stall_ms) return SW_E_STALL;
        s->o.sleep_ms(s->o.ctx, ROOM_MS);
    }
}

// A block goes out whole: `G`, its length, the data, `00`. Cancelling is read before it
// begins and after it ends, never between.
static int send_block(sw_session_t *s, const uint8_t *d, size_t n)
{
    int32_t need = (int32_t)n + G_FRAMING;
    int rc = wait_room(s, need);
    if (rc) return rc;
    if (cancelled(s)) return cancel_out(s);
    const uint8_t head[3] = { 'G', (uint8_t)n, (uint8_t)(n >> 8) };
    if ((rc = put(s, head, sizeof head)) || (rc = put(s, d, n)) || (rc = put(s, &k_zero, 1))) return rc;
    s->free_b -= need;
    if (cancelled(s)) return cancel_out(s);
    return SW_OK;
}

static int frame(sw_session_t *s, uint8_t type, const uint8_t *p, size_t n)
{
    int rc;
    switch (type) {
    case SWF_PAGE: {
        if (n != 1) return SW_E_FAULT;
        // A colour page needs a job made for colour and a colour cartridge in.
        int may = s->cfg.colour_job && s->cart && s->spec->colour;
        if (p[0] && !may) return SW_E_CART;
        if (s->let->mode) {
            int want = may ? 'H' : 'B';
            if (s->mode != want) {
                if ((rc = put1(s, (char)want))) return rc;
                s->mode = want;
                if (want == 'B' && !s->cart && s->q == 2) {
                    // A black cartridge at Best asks `R`, then `s` (session.rs:709-718). Ours
                    // are screened pages, never plain 1 bit, so it is `s` whatever `R` says.
                    uint8_t r;
                    if ((rc = query(s, 'R', &r)) || (rc = put1(s, 's'))) return rc;
                }
            }
        }
        return s->let->page_l ? put1(s, s->let->page_l) : SW_OK;
    }
    case SWF_RECT:
        if (n != RECT_LEN || (p[0] != 'R' && p[0] != 'c')) return SW_E_FAULT;
        if (p[0] == 'c' && (!s->cart || !s->spec->colour)) return SW_E_CART;
        if ((rc = wait_clear(s))) return rc;
        if ((rc = put(s, p, n))) return rc;
        s->free_b -= RECT_LEN;
        return SW_OK;
    case SWF_BLOCK:
        if (n == 0 || n > s->maxcap) return SW_E_FAULT;
        return send_block(s, p, n);
    case SWF_PAGE_END:
        if (n != 0) return SW_E_FAULT;
        if ((rc = put(s, &k_formfeed, 1))) return rc;
        s->free_b -= 1;
        s->pages_done++;
        return SW_OK;
    default:
        return SW_E_FAULT;
    }
}

static uint8_t quality_index(int quality) { return quality == 3 ? 0 : quality == 5 ? 2 : 1; }

static void init(sw_session_t *s, const sw_ops_t *o, const sw_job_cfg_t *c, uint8_t *buf, size_t buf_cap)
{
    memset(s, 0, sizeof *s);
    s->o = *o;
    s->cfg = *c;
    if (!s->cfg.stall_ms) s->cfg.stall_ms = STALL_MS;
    s->spec = sw_spec(c->model);
    s->let = &k_letters[c->model];
    s->q = quality_index(c->quality);
    s->maxcap = sw_block_cap(c->model, 1);
    s->frame = buf;
    s->frame_cap = buf_cap;
}

sw_session_t *sw_session_new(const sw_ops_t *o, const sw_job_cfg_t *c)
{
    if (!ops_ok(o) || !c) return NULL;
    const sw_spec_t *sp = sw_spec(c->model);
    if (!sp) return NULL;
    // The most a frame can hold: the largest block the model's page stage makes, in a
    // gray page or a colour one.
    uint32_t cap = sw_block_cap(c->model, 1), cap4 = sp->colour ? sw_block_cap(c->model, 4) : 0;
    if (cap4 > cap) cap = cap4;
    if (cap < RECT_LEN) cap = RECT_LEN;
    size_t fcap = FRAME_HEAD + cap;
    sw_session_t *s = big_malloc(sizeof *s + fcap);
    if (!s) return NULL;
    init(s, o, c, (uint8_t *)(s + 1), fcap);
    return s;
}

void sw_session_free(sw_session_t *s)
{
    free(s);
}

int sw_session_cart(const sw_session_t *s)
{
    return s ? s->cart : 0;
}

// Resets the printer and waits for it to answer `1`: 2 s of settling (8 s for the original),
// one try of 5 s; a printer that said nothing is reset again with 8 s (session.rs:492-535,
// without the fast-clock steps), and then asked until it has been silent for SILENT_MS.
static int reset_and_status(sw_session_t *s, uint8_t *s1)
{
    for (int i = 0; i < 2; i++) {
        if (cancelled(s)) return cancel_out(s);
        int rc = put(s, k_reset, sizeof k_reset);
        if (rc) return rc;
        s->o.sleep_ms(s->o.ctx, i == 0 && s->cfg.model != SW_ORIGINAL ? SETTLE_MS : SETTLE_LONG_MS);
        int r = ask_once(&s->o, '1', s1, ANSWER_MS);
        if (r < 0) { show(s, SWS_OFFLINE); return SW_E_SILENT; }
        if (r) { s->last_heard = ms(s); return SW_OK; }
    }
    return query(s, '1', s1);
}

int sw_session_begin(sw_session_t *s)
{
    if (!s || s->begun || s->aborted) return SW_E_FAULT;
    sw_model_t m = s->cfg.model;
    s->last_heard = s->active_from = ms(s);
    show(s, SWS_PRINTING);

    // Reset, settle, and the first look at the printer: `1` and `2` clear.
    uint8_t s1, s2, b, h = 0;
    int rc = reset_and_status(s, &s1);
    if (rc) return rc;
    if ((rc = query(s, '2', &s2))) return rc;
    if ((rc = clear_loop(s, 1, s1, s2))) return rc;

    // `?` must name the model the job was made for. Nothing is sent after a mismatch.
    uint8_t id[32];
    int n;
    for (;;) {
        n = ask_ident(&s->o, id, sizeof id, ANSWER_MS);
        if (n < 0) { show(s, SWS_OFFLINE); return SW_E_SILENT; }
        if (n > 0) break;
        if (silent(s)) return SW_E_SILENT;
        if (cancelled(s)) return cancel_out(s);
    }
    s->last_heard = ms(s);
    int cs = 0;
    sw_model_t got = sw_model_from_id(id, (size_t)n, &cs);
    if (!(cs ? is_cs_model(m) : got == m)) return SW_E_MODEL;

    // D or N, the free buffer with the printer empty, the cartridge (session.rs:599-619).
    if (s->let->pre && (rc = put1(s, s->let->pre))) return rc;
    if ((rc = query(s, 'B', &b))) return rc;
    s->free_b = sw_free_bytes(m, b);
    if (s->let->reads_h) {
        if ((rc = query(s, 'H', &h))) return rc;
        s->cart = sw_cart_colour(m, h);
        if (s->cart < 0) { s->cart = 0; return SW_E_CART; }
    }

    // The largest block this job can send has to fit the empty buffer, or this is not the
    // model the record says (the 2500's holds a little less than the 2200's blocks).
    uint32_t cap = sw_block_cap(m, 1), cap4 = s->cart && s->spec->colour ? sw_block_cap(m, 4) : 0;
    s->maxcap = cap4 > cap ? cap4 : cap;
    if (s->free_b < (int32_t)s->maxcap + G_FRAMING) return SW_E_MODEL;

    // The setup for the quality and the cartridge.
    char setup[16];
    size_t k = 0;
    for (const char *t = s->let->setup[s->q][s->cart ? 1 : 0]; *t; t++) {
        if (*t != '*') setup[k++] = *t;
        else if (s->clean) setup[k++] = 'P';
    }
    if ((rc = put(s, setup, k))) return rc;
    s->begun = 1;
    return SW_OK;
}

int sw_session_feed(sw_session_t *s, const uint8_t *buf, size_t n)
{
    if (!s) return SW_E_FAULT;
    if (s->aborted) return s->cancelled_out ? SW_E_CANCEL : SW_E_FAULT;
    if (!s->begun || !s->frame) return SW_E_FAULT;
    s->active_from = ms(s);
    size_t pos = 0;
    while (pos < n) {
        if (cancelled(s)) return cancel_out(s);
        int rc;
        // A whole frame in what came, with none half held: used where it lies.
        if (!s->have && n - pos >= FRAME_HEAD) {
            size_t len = (size_t)buf[pos + 1] | ((size_t)buf[pos + 2] << 8);
            if (n - pos >= FRAME_HEAD + len) {
                if ((rc = frame(s, buf[pos], buf + pos + FRAME_HEAD, len))) return rc;
                pos += FRAME_HEAD + len;
                continue;
            }
        }
        // Otherwise it is gathered: the head first, then as much of the body as is here.
        size_t want = FRAME_HEAD;
        if (s->have >= FRAME_HEAD) want += (size_t)s->frame[1] | ((size_t)s->frame[2] << 8);
        size_t take = want - s->have;
        if (take > n - pos) take = n - pos;
        memcpy(s->frame + s->have, buf + pos, take);
        s->have += take;
        pos += take;
        if (s->have < FRAME_HEAD) continue;
        size_t len = (size_t)s->frame[1] | ((size_t)s->frame[2] << 8);
        if (len > s->frame_cap - FRAME_HEAD) { s->have = 0; return SW_E_FAULT; }
        if (s->have == FRAME_HEAD + len) {
            s->have = 0;
            if ((rc = frame(s, s->frame[0], s->frame + FRAME_HEAD, len))) return rc;
        }
    }
    return SW_OK;
}

// Waits for `1` to say the page is printed (session.rs:832-858): checks `2` for faults
// meanwhile, and gives up when the buffer's free count has not risen, and no fault has
// cleared, for stall_ms.
static int wait_printed(sw_session_t *s)
{
    uint32_t t_prog = ms(s);
    int32_t best = -1;
    for (;;) {
        if (cancelled(s)) return cancel_out(s);
        uint8_t s1, s2, b;
        int rc = query(s, '1', &s1);
        if (rc) return rc;
        if (s1 & 0x08) return SW_OK;
        if ((rc = query(s, '2', &s2))) return rc;
        int kind;
        read_state(s->cfg.model, s1, s2, &kind);
        if (kind != K_NONE) {
            if ((rc = clear_loop(s, 1, s1, s2))) return rc;
            t_prog = ms(s);
            continue;
        }
        if ((rc = query(s, 'B', &b))) return rc;
        int32_t free_b = sw_free_bytes(s->cfg.model, b);
        if (free_b > best) { best = free_b; t_prog = ms(s); }
        if ((uint32_t)(ms(s) - t_prog) >= s->cfg.stall_ms) return SW_E_STALL;
        s->o.sleep_ms(s->o.ctx, PRINTED_MS);
    }
}

// `E`, `Y` on the 1200, `l` (session.rs:820-824).
static int finish(sw_session_t *s)
{
    char tail[3];
    size_t k = 0;
    tail[k++] = 'E';
    if (s->let->end_y) tail[k++] = s->let->end_y;
    tail[k++] = 'l';
    int rc = put(s, tail, k);
    if (!rc) show(s, SWS_READY);
    return rc;
}

int sw_session_end(sw_session_t *s)
{
    if (!s) return SW_E_FAULT;
    if (s->aborted) return s->cancelled_out ? SW_E_CANCEL : SW_E_FAULT;
    if (!s->begun || s->have) return SW_E_FAULT;           // not begun, or half a frame left
    s->active_from = ms(s);
    if (cancelled(s)) return cancel_out(s);
    if (s->let->waits && s->pages_done) {
        int rc = wait_printed(s);
        if (rc) return rc;
    }
    return finish(s);
}

void sw_session_abort(sw_session_t *s)
{
    if (s) abort_seq(s);
}

// A cleaning: busy has CLEAN_START_MS to come on (a printer that never reads busy is taken
// to have cleaned), then CLEAN_MS to go off (session.rs:902-923).
static int wait_cleaned(sw_session_t *s)
{
    uint32_t t0 = ms(s);
    int seen_busy = 0;
    for (;;) {
        if (cancelled(s)) return cancel_out(s);
        uint8_t s1;
        int rc = query(s, '1', &s1);
        if (rc) return rc;
        uint32_t el = (uint32_t)(ms(s) - t0);
        if (s1 & 0x01) seen_busy = 1;
        else if (seen_busy || el >= CLEAN_START_MS) return SW_OK;
        if (el >= CLEAN_MS) return SW_E_STALL;
        s->o.sleep_ms(s->o.ctx, CLEAN_POLL_MS);
    }
}

int sw_clean(const sw_ops_t *o, sw_model_t m)
{
    const sw_spec_t *sp = sw_spec(m);
    if (!ops_ok(o) || !sp) return SW_E_MODEL;
    if (!sp->cleans) return SW_E_FAULT;                    // the original has none (job.rs:70-78)
    sw_session_t s;                                        // no frames to hold: a few hundred bytes
    const sw_job_cfg_t c = { m, 4, 0, STALL_MS };
    init(&s, o, &c, NULL, 0);
    s.clean = 1;
    int rc = sw_session_begin(&s);
    if (!rc) rc = wait_cleaned(&s);
    if (!rc) rc = finish(&s);
    if (rc && rc != SW_E_SILENT) abort_seq(&s);
    return rc;
}
