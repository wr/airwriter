// SPDX-License-Identifier: GPL-3.0-or-later
//
// The board's StyleWriter glue (stylewriter.c, W-1103) on the host: the real file, against
// stand-ins for the ESP-IDF headers (test/shim) and for serial_out and job_pipe, which the test
// defines, and a printer scripted below on a fake clock. What it holds is what the glue adds
// to the session, which test_sw_session.c holds on its own:
//   - the port's questions put the link at a StyleWriter's for the question and give it back
//     to the record's, and the plain question is the one byte `?`;
//   - a job's begin does not touch the printer: the printer is reset, the cartridge read and the
//     page stage made when the rip begins (job_pipe_target_t.colour_ok), so that nothing is
//     ripped before the cartridge is known, and the rip's colour is the cartridge's;
//   - the abort contract: any session error aborts the session and the job pipeline, once, and
//     the bytes that follow go nowhere;
//   - a job that ends short sends nothing it had not begun to send.
#include "test_util.h"
#include "stylewriter.h"
#include "sw_page.h"
#include "serial_out.h"
#include "job_pipe.h"
#include "esp_timer.h"
#include "freertos/task.h"
#include <stdlib.h>

// ---- the clock and the line (what stylewriter.c calls)

enum { WIRE_N = 1 << 20, LINKS = 32 };

typedef struct {
    // the printer
    sw_model_t model;
    int silent;                    // says nothing
    int silent_after_g;            // says nothing once it has been sent this many blocks (0: never)
    int no_paper;                  // `2` says 04
    int cart_colour;               // `H` says a colour cartridge
    int pages_done;                // `1` says page done once a 0C has come
    // the line
    uint64_t us;
    uint8_t  rx[256];
    int      rx_n, rx_pos;
    uint8_t *wire;
    size_t   wn;
    struct { int baud, cts; } links[LINKS];
    int      nlinks;
    // the parser
    int ps, nff, skip;
    uint32_t glen;
    // what it was sent
    int resets, blocks, pages, rects, aborts, draws;
    int cancel_on_block;           // stylewriter_cancel() is called as this block begins (0: never)
} fake_t;

static fake_t F;

int64_t esp_timer_get_time(void) { return (int64_t)F.us; }
void vTaskDelay(TickType_t ticks) { F.us += (uint64_t)ticks * 10000u; }
void serial_out_rx_drop(void) { F.rx_n = F.rx_pos = 0; }

void serial_out_set_link(int baud, int cts)
{
    if (F.nlinks < LINKS) { F.links[F.nlinks].baud = baud; F.links[F.nlinks].cts = cts; }
    F.nlinks++;
}

static void say(const char *s)
{
    if (F.silent) return;
    for (; *s; s++) if (F.rx_n < (int)sizeof F.rx) F.rx[F.rx_n++] = (uint8_t)*s;
}

static void say_byte(uint8_t b)
{
    if (F.silent) return;
    if (F.rx_n < (int)sizeof F.rx) F.rx[F.rx_n++] = b;
}

static const char *ident(void)
{
    switch (F.model) {
    case SW_ORIGINAL: return "IJ10\r";
    case SW_II:       return "SW\r";
    case SW_1200:     return "SW3\r";
    default:          return "CS\r";
    }
}

static uint8_t p_byte(void)
{
    switch (F.model) {
    case SW_C2400: return 0x01;
    case SW_C2200: return 0x02;
    case SW_C1500: return 0x04;
    default:       return 0x05;
    }
}

enum { P_CMD, P_Q, P_GLEN1, P_GLEN2, P_GDATA, P_SKIP };

static void query(uint8_t q)
{
    switch (q) {
    case '1': say_byte(F.pages_done ? 0x08 : 0x00); break;
    case '2': say_byte(F.no_paper ? 0x04 : 0x00); break;
    case 'B': say_byte(0xFF); break;                           // an empty, roomy buffer
    case 'H': say_byte(F.model >= SW_C2200 || F.model == SW_C1500
                       ? (F.cart_colour ? 0x81 : 0x01) : 0x00); break;
    case 'p': say_byte(p_byte()); break;
    case 'R': say_byte(0x00); break;
    case 'I': F.resets++; F.pages_done = 0; break;
    default: break;
    }
}

static void parse(uint8_t b)
{
    switch (F.ps) {
    case P_CMD:
        if (b == 0xFF) {
            if (++F.nff == 3) { F.ps = P_Q; F.nff = 0; }
            return;
        }
        F.nff = 0;
        if (b == '?') say(ident());
        else if (b == 'G') F.ps = P_GLEN1;
        else if (b == 'R' || b == 'c') { F.ps = P_SKIP; F.skip = 8; F.rects++; }
        else if (b == 0x0C) { F.pages++; F.pages_done = 1; }
        return;
    case P_Q:
        F.ps = P_CMD;
        query(b);
        return;
    case P_GLEN1: F.glen = b; F.ps = P_GLEN2; return;
    case P_GLEN2:
        F.glen |= (uint32_t)b << 8;
        F.blocks++;
        if (F.cancel_on_block && F.blocks == F.cancel_on_block) stylewriter_cancel();
        if (F.silent_after_g && F.blocks >= F.silent_after_g) F.silent = 1;
        F.ps = F.glen ? P_GDATA : P_SKIP;
        F.skip = F.glen ? 0 : 1;
        return;
    case P_GDATA:
        if (--F.glen == 0) { F.ps = P_SKIP; F.skip = 1; }       // and the 00 that ends it
        return;
    case P_SKIP:
        if (--F.skip <= 0) F.ps = P_CMD;
        return;
    }
}

int serial_out_write_raw(const uint8_t *b, size_t n, uint32_t timeout_ms)
{
    (void)timeout_ms;
    for (size_t i = 0; i < n; i++) {
        if (F.wn < WIRE_N) F.wire[F.wn++] = b[i];
        parse(b[i]);
    }
    F.us += (uint64_t)n * 10u * 1000000u / 57600u;       // 57600 baud
    return 0;
}

int serial_out_read(uint8_t *b, size_t cap, uint32_t first_ms, uint32_t gap_ms)
{
    (void)gap_ms;
    if (F.rx_pos < F.rx_n) {
        size_t n = 0;
        while (n < cap && F.rx_pos < F.rx_n) b[n++] = F.rx[F.rx_pos++];
        return (int)n;
    }
    F.us += (uint64_t)first_ms * 1000u;                    // waited it out
    return 0;
}

// ---- the job pipeline (what stylewriter.c calls), and the printer task between them

static int g_record_sw;
static int g_aborts;                // job_pipe_abort calls
static int g_cart_calls, g_cart_val;
static int g_states[64], g_nstates;

void job_pipe_abort(void) { g_aborts++; }

// The ring's printer task hands the consumer up to 512 bytes at a time (job_pipe.c). Once the
// job is aborted it hands it nothing, which is job_pipe_abort's effect on the real thing.
static const job_pipe_target_t *g_target;
void job_pipe_out(const uint8_t *buf, size_t n)
{
    F.draws++;
    for (size_t off = 0; off < n && !g_aborts; ) {
        size_t k = n - off < 512 ? n - off : 512;
        g_target->prn(g_target->prn_ctx, buf + off, k);
        off += k;
    }
}

static int rec_is_sw(void) { return g_record_sw; }
static void on_state(sw_state_t st) { if (g_nstates < 64) g_states[g_nstates++] = (int)st; }
static void on_cart(int colour) { g_cart_calls++; g_cart_val = colour; }
static int no_cancel(void) { return 0; }

static void reset_all(sw_model_t model)
{
    uint8_t *w = F.wire;
    memset(&F, 0, sizeof F);
    F.wire = w;
    F.model = model;
    F.us = 100u * 1000000u;
    g_aborts = g_cart_calls = g_cart_val = g_nstates = 0;
    g_target = NULL;
}

static int wire_has(const char *s)
{
    size_t n = strlen(s);
    for (size_t i = 0; i + n <= F.wn; i++)
        if (!memcmp(F.wire + i, s, n)) return 1;
    return 0;
}

static int wire_count(const char *s)
{
    size_t n = strlen(s);
    int c = 0;
    for (size_t i = 0; i + n <= F.wn; i++)
        if (!memcmp(F.wire + i, s, n)) c++;
    return c;
}

// ---- a page through the stage, as the rip would feed it

static uint8_t g_rows[4][512];

// A Letter page: ink in two places, 1400 rows apart (two bands on a Color StyleWriter 2500,
// whose bands are 400 rows). `ink` feeds four planes a row, else the K plane only.
static void feed_page(const rip_stage_t *st, int ink, int pages)
{
    const uint32_t W = 2916, H = 3960;
    for (int p = 0; p < pages; p++) {
        CHECK_EQ_INT(st->page_begin(st->ctx, p, W, H), 0);
        for (uint32_t y = 0; y < H && !g_aborts; y++) {
            int inked = (y >= 100 && y < 140) || (y >= 1500 && y < 1540);
            memset(g_rows, 0, sizeof g_rows);
            if (inked) {
                memset(g_rows[3], 0xAA, 64);
                if (ink) { memset(g_rows[0], 0x55, 64); memset(g_rows[1], 0xF0, 64); }
            }
            if (ink) {
                const uint8_t *planes[4] = { g_rows[0], g_rows[1], g_rows[2], g_rows[3] };
                st->ink_row(st->ctx, planes);
            } else {
                st->row(st->ctx, g_rows[3]);
            }
        }
        if (!g_aborts) CHECK_EQ_INT(st->page_end(st->ctx), 0);
    }
}

static void start(int record_sw)
{
    stylewriter_cfg_t cfg = { .record_is_sw = rec_is_sw, .state = on_state, .cart = on_cart,
                              .cancelled = no_cancel };
    stylewriter_start(&cfg);
    g_record_sw = record_sw;
}

int main(void)
{
    F.wire = malloc(WIRE_N);
    CHECK(F.wire != NULL);

    // ---------------------------------------------------------- the port's questions
    // An ImageWriter record: the plain question is the one byte `?` at 57600 with no flow
    // control, and the link goes back to the ImageWriter's (9600, CTS) when it is done.
    reset_all(SW_II);
    start(0);
    CHECK_EQ_INT(stylewriter_ask_plain(), SW_II);
    CHECK_EQ_INT(F.wn, 1);
    CHECK_EQ_INT(F.wire[0], '?');
    CHECK_EQ_INT(F.nlinks, 2);
    CHECK_EQ_INT(F.links[0].baud, 57600);
    CHECK_EQ_INT(F.links[0].cts, 0);
    CHECK_EQ_INT(F.links[1].baud, 9600);
    CHECK_EQ_INT(F.links[1].cts, 1);

    // A printer that says nothing is asked that one byte and no more.
    reset_all(SW_II);
    F.silent = 1;
    CHECK_EQ_INT(stylewriter_ask_plain(), SW_NONE);
    CHECK_EQ_INT(F.wn, 1);
    CHECK_EQ_INT(F.wire[0], '?');
    CHECK_EQ_INT(F.links[F.nlinks - 1].baud, 9600);

    // A colour model: `?` says CS, which earns `p` (FF FF FF 70) and names the model.
    reset_all(SW_C2500);
    CHECK_EQ_INT(stylewriter_ask_plain(), SW_C2500);
    CHECK(wire_has("?\xFF\xFF\xFF" "p"));

    // A StyleWriter record keeps its link: set once, never given back (the record wants it).
    reset_all(SW_II);
    start(1);
    CHECK_EQ_INT(stylewriter_ask_plain(), SW_II);
    CHECK_EQ_INT(F.nlinks, 1);
    CHECK_EQ_INT(F.links[0].baud, 57600);
    CHECK_EQ_INT(stylewriter_ask_plain(), SW_II);          // and a second question moves nothing
    CHECK_EQ_INT(F.nlinks, 1);

    // The ImageWriter-only traffic the board sends a record that stopped answering: the link
    // goes to 9600 with CTS for it and comes back to the record's after.
    stylewriter_link(0);
    CHECK_EQ_INT(F.links[F.nlinks - 1].baud, 9600);
    CHECK_EQ_INT(F.links[F.nlinks - 1].cts, 1);
    stylewriter_link_record();
    CHECK_EQ_INT(F.links[F.nlinks - 1].baud, 57600);
    CHECK_EQ_INT(F.links[F.nlinks - 1].cts, 0);

    // The full sequence, for a printer the record names: the reset, 2 s, then `?`.
    reset_all(SW_II);
    start(1);
    uint64_t t0 = F.us;
    CHECK_EQ_INT(stylewriter_ask_full(), SW_II);
    CHECK_EQ_INT(F.resets, 1);
    CHECK_EQ_MEM(F.wire, "\x00\xFF\xFF\xFF" "I?", 6);
    CHECK(F.us - t0 >= 2000u * 1000u);                      // it settled
    // The original ignores `?` until reset: found by the full sequence, as the design has it.
    reset_all(SW_ORIGINAL);
    CHECK_EQ_INT(stylewriter_ask_full(), SW_ORIGINAL);

    // Every question leaves J2 as the record wants it: for a record that is not a StyleWriter
    // (the person's pick is being changed, a notice not yet accepted) back to 9600 with CTS.
    reset_all(SW_II);
    start(0);
    sw_state_t any;
    stylewriter_status(SW_II, &any);
    CHECK_EQ_INT(F.links[F.nlinks - 1].baud, 9600);
    stylewriter_cart(SW_II);
    CHECK_EQ_INT(F.links[F.nlinks - 1].baud, 9600);
    stylewriter_ask_full();
    CHECK_EQ_INT(F.links[F.nlinks - 1].baud, 9600);
    CHECK_EQ_INT(F.links[F.nlinks - 1].cts, 1);
    stylewriter_clean(SW_II);
    CHECK_EQ_INT(F.links[F.nlinks - 1].baud, 9600);
    CHECK_EQ_INT(F.links[F.nlinks - 1].cts, 1);
    start(1);
    reset_all(SW_II);
    stylewriter_link_record();                               // (the record wants 57600 again)

    // `1` and `2` for the idle check; the cartridge by `D` and `H` for a colour model.
    reset_all(SW_II);
    sw_state_t st;
    CHECK_EQ_INT(stylewriter_status(SW_II, &st), SW_OK);
    CHECK_EQ_INT(st, SWS_READY);
    F.no_paper = 1;
    CHECK_EQ_INT(stylewriter_status(SW_II, &st), SW_OK);
    CHECK_EQ_INT(st, SWS_NO_PAPER);
    F.silent = 1;
    CHECK_EQ_INT(stylewriter_status(SW_II, &st), SW_E_SILENT);
    CHECK_EQ_INT(st, SWS_OFFLINE);
    reset_all(SW_C2500);
    F.cart_colour = 1;
    CHECK_EQ_INT(stylewriter_cart(SW_C2500), 1);
    CHECK(wire_has("D\xFF\xFF\xFF" "H"));
    F.cart_colour = 0;
    CHECK_EQ_INT(stylewriter_cart(SW_C2500), 0);

    // ---------------------------------------------------------- a colour job, colour cartridge
    reset_all(SW_C2500);
    F.cart_colour = 1;
    start(1);
    job_pipe_target_t t;
    CHECK_EQ_INT(stylewriter_job_begin(SW_C2500, 5, HT_APPLE, 3, &t), 0);
    g_target = &t;
    CHECK(t.caps == sw_caps(SW_C2500));
    CHECK(t.tone == sw_tone(SW_C2500, HT_APPLE));
    CHECK_EQ_INT(t.buffered, 1);
    CHECK(t.prn == stylewriter_prn);
    CHECK_EQ_INT(t.dither, HT_APPLE);
    CHECK_EQ_INT(t.gain, 3);
    CHECK(t.colour_ok != NULL && t.stage != NULL);
    // The sink's begin touched nothing: no byte reaches the printer, and no cartridge is known,
    // before the rip begins. A page offered to the stage until then is refused.
    CHECK_EQ_INT(F.wn, 0);
    CHECK_EQ_INT(g_cart_calls, 0);
    CHECK_EQ_INT(t.stage->page_begin(t.stage->ctx, 0, 100, 100), -1);

    // The rip begins: the printer is reset and the cartridge read, then the stage is made for
    // it, and the rip's colour is the cartridge's.
    CHECK_EQ_INT(t.colour_ok(t.colour_ctx), 1);
    CHECK_EQ_INT(g_cart_calls, 1);
    CHECK_EQ_INT(g_cart_val, 1);
    CHECK_EQ_INT(F.resets, 1);
    CHECK_EQ_MEM(F.wire, "\x00\xFF\xFF\xFF" "I", 5);
    CHECK(wire_has("m2sA"));                                 // quality 5: best, colour cartridge
    CHECK_EQ_INT(F.blocks, 0);
    CHECK(t.stage->ink_row != NULL);
    CHECK_EQ_INT(g_aborts, 0);
    CHECK_EQ_INT(F.draws, 0);

    feed_page(t.stage, 1, 1);
    CHECK(F.draws > 0);
    CHECK_EQ_INT(F.pages, 1);
    CHECK_EQ_INT(F.rects, 2);                                // two bands
    CHECK(F.blocks >= 2);
    CHECK_EQ_INT(g_aborts, 0);
    CHECK_EQ_INT(stylewriter_job_error(), 0);
    CHECK_EQ_INT(stylewriter_job_end(0), 0);                // waited for page done, then E and l
    CHECK_EQ_MEM(F.wire + F.wn - 2, "El", 2);
    CHECK_EQ_INT(F.resets, 1);                               // nothing but the job's own reset
    CHECK_EQ_INT(g_aborts, 0);
    CHECK_EQ_INT(g_states[g_nstates - 1], SWS_READY);

    // ---------------------------------------------------------- a black cartridge in a colour model
    reset_all(SW_C2500);
    F.cart_colour = 0;
    CHECK_EQ_INT(stylewriter_job_begin(SW_C2500, 0, HT_ATKINSON, 0, &t), 0);   // no quality: normal
    g_target = &t;
    CHECK_EQ_INT(t.colour_ok(t.colour_ctx), 0);              // the rip prints RGB pages as gray
    CHECK_EQ_INT(g_cart_val, 0);
    CHECK(wire_has("m2bA"));                                 // normal on a black cartridge
    feed_page(t.stage, 0, 1);
    CHECK_EQ_INT(stylewriter_job_end(0), 0);
    CHECK_EQ_INT(F.pages, 1);

    // A model with no colour cartridge reads black, and the rip is told so.
    reset_all(SW_II);
    CHECK_EQ_INT(stylewriter_job_begin(SW_II, 3, HT_ATKINSON, 0, &t), 0);
    g_target = &t;
    F.cart_colour = 1;
    CHECK_EQ_INT(t.colour_ok(t.colour_ctx), 0);
    CHECK_EQ_INT(g_cart_val, 0);
    CHECK_EQ_INT(stylewriter_job_end(1), SW_E_CANCEL);       // (stopped here: abort sequence)

    // ---------------------------------------------------------- the abort contract
    // The printer does not answer when the job begins: the pipeline is aborted at once (the rip
    // would otherwise start on a printer that is not there), the session is told to stop, the
    // stage stays one that refuses pages, and the job ends with the session's error.
    reset_all(SW_C2500);
    F.silent = 1;
    CHECK_EQ_INT(stylewriter_job_begin(SW_C2500, 4, HT_APPLE, 0, &t), 0);
    g_target = &t;
    CHECK_EQ_INT(t.colour_ok(t.colour_ctx), 0);
    CHECK_EQ_INT(g_aborts, 1);
    CHECK_EQ_INT(stylewriter_job_error(), SW_E_SILENT);
    CHECK_EQ_INT(t.stage->page_begin(t.stage->ctx, 0, 100, 100), -1);
    CHECK_EQ_INT(g_cart_calls, 0);
    size_t wn = F.wn;
    int resets = F.resets;
    CHECK(resets >= 2);                                      // begin's reset and its second try, then the abort's
    t.prn(t.prn_ctx, (const uint8_t *)"\x03\x01\x00\x00", 4);  // a late byte goes nowhere
    CHECK_EQ_INT(F.wn, wn);
    CHECK_EQ_INT(stylewriter_job_end(0), SW_E_SILENT);
    CHECK_EQ_INT(F.wn, wn);                                  // the abort was already sent: not twice
    CHECK_EQ_INT(F.resets, resets);

    // The printer is another model than the record says: the job fails before anything is
    // sent that is not a reset or a question, and the pipeline is aborted.
    reset_all(SW_II);
    F.cart_colour = 1;
    CHECK_EQ_INT(stylewriter_job_begin(SW_C2500, 4, HT_APPLE, 0, &t), 0);
    g_target = &t;
    CHECK_EQ_INT(t.colour_ok(t.colour_ctx), 0);
    CHECK_EQ_INT(g_aborts, 1);
    CHECK_EQ_INT(stylewriter_job_error(), SW_E_MODEL);
    CHECK(!wire_has("m2"));                                  // no setup
    CHECK_EQ_INT(F.blocks, 0);
    CHECK_EQ_INT(F.resets, 2);                               // begin's, and the abort's
    CHECK_EQ_INT(stylewriter_job_end(0), SW_E_MODEL);
    CHECK_EQ_INT(F.resets, 2);

    // The printer goes deaf in the middle of a page: the next thing the session asks it (before
    // the second band) gets no answer, it gives up after its silence, the pipeline is aborted
    // once, the session is aborted (a reset) once, and the bytes that follow go nowhere.
    reset_all(SW_C2500);
    F.cart_colour = 1;
    F.silent_after_g = 1;
    CHECK_EQ_INT(stylewriter_job_begin(SW_C2500, 4, HT_APPLE, 0, &t), 0);
    g_target = &t;
    CHECK_EQ_INT(t.colour_ok(t.colour_ctx), 1);
    feed_page(t.stage, 1, 1);
    CHECK_EQ_INT(g_aborts, 1);
    CHECK_EQ_INT(stylewriter_job_error(), SW_E_SILENT);
    CHECK_EQ_INT(F.blocks, 1);                               // only the first band's block was sent
    CHECK_EQ_INT(F.resets, 2);
    wn = F.wn;
    t.prn(t.prn_ctx, (const uint8_t *)"\x04\x00\x00", 3);
    CHECK_EQ_INT(F.wn, wn);
    CHECK_EQ_INT(stylewriter_job_end(0), SW_E_SILENT);       // a clean finish does not hide the error
    CHECK_EQ_INT(F.resets, 2);

    // A cancel (Cancel-Job, a cut upload): the session stops at its next step, sends the reset,
    // and the pipeline is aborted. The next job's begin clears it.
    reset_all(SW_C2500);
    F.cart_colour = 1;
    F.cancel_on_block = 1;
    CHECK_EQ_INT(stylewriter_job_begin(SW_C2500, 4, HT_APPLE, 0, &t), 0);
    g_target = &t;
    CHECK_EQ_INT(t.colour_ok(t.colour_ctx), 1);
    feed_page(t.stage, 1, 1);
    CHECK(g_aborts >= 1);
    CHECK_EQ_INT(stylewriter_job_error(), SW_E_CANCEL);
    CHECK_EQ_INT(stylewriter_job_end(1), SW_E_CANCEL);
    CHECK_EQ_INT(F.resets, 2);
    reset_all(SW_II);
    CHECK_EQ_INT(stylewriter_job_begin(SW_II, 4, HT_ATKINSON, 0, &t), 0);
    g_target = &t;
    CHECK_EQ_INT(t.colour_ok(t.colour_ctx), 0);              // not cancelled any more
    CHECK_EQ_INT(g_aborts, 0);
    CHECK_EQ_INT(stylewriter_job_end(0), 0);                 // (no pages: nothing to wait for)

    // A job that ends before the rip began (not a URF, a refused begin, a cut upload at once)
    // sends the printer nothing at all.
    reset_all(SW_C2500);
    CHECK_EQ_INT(stylewriter_job_begin(SW_C2500, 4, HT_APPLE, 0, &t), 0);
    CHECK_EQ_INT(stylewriter_job_end(1), SW_E_CANCEL);
    CHECK_EQ_INT(F.wn, 0);
    // And a begin for a model there is none of.
    CHECK_EQ_INT(stylewriter_job_begin(SW_NONE, 4, HT_ATKINSON, 0, &t), -1);
    CHECK_EQ_INT(stylewriter_job_end(1), SW_E_CANCEL);       // (nothing held: still safe to ask)

    // A page that goes through twice (two pages in the job) is one session: one reset, both
    // pages, the end once.
    reset_all(SW_C2500);
    F.cart_colour = 1;
    CHECK_EQ_INT(stylewriter_job_begin(SW_C2500, 4, HT_APPLE, 0, &t), 0);
    g_target = &t;
    CHECK_EQ_INT(t.colour_ok(t.colour_ctx), 1);
    feed_page(t.stage, 1, 2);
    CHECK_EQ_INT(stylewriter_job_end(0), 0);
    CHECK_EQ_INT(F.pages, 2);
    CHECK_EQ_INT(F.resets, 1);
    CHECK_EQ_INT(wire_count("El"), 1);

    // ---------------------------------------------------------- one cleaning
    reset_all(SW_II);
    start(1);
    CHECK_EQ_INT(stylewriter_clean(SW_II), 0);
    CHECK_EQ_INT(wire_count("P"), 1);                        // one `P` is one cleaning
    reset_all(SW_ORIGINAL);
    CHECK_EQ_INT(stylewriter_clean(SW_ORIGINAL), SW_E_FAULT);
    CHECK_EQ_INT(F.wn, 0);                                   // the original has none, and is sent nothing
    reset_all(SW_II);
    F.silent = 1;
    CHECK_EQ_INT(stylewriter_clean(SW_II), SW_E_SILENT);

    free(F.wire);
    TEST_END;
}
