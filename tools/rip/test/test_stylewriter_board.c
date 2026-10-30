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
//   - a job that ends short sends nothing it had not begun to send;
//   - the test page is one session at normal quality, made for the cartridge the printer
//     reports, cancelled through the session, and never cancelled by a flag a job left behind;
//   - the answer to the Clean Print Head key: 0 when a cleaning may begin, 409 while a job holds
//     the printer, 502 for a printer that is off or holds a fault, and nothing at all for the
//     original, which has none; one press is one `P`;
//   - with the port's last answer from an ImageWriter (the list's pr_port_other_family, the
//     cfg's swapped), no job, test page or cleaning sends it anything.
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
    int tap_on_block;              // the board's own cancel (the test button's second tap) is raised then
} fake_t;

static fake_t F;
static int g_tap;                  // what the board's cancelled callback answers

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
        if (F.tap_on_block && F.blocks == F.tap_on_block) g_tap = 1;
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
static int tapped(void) { return g_tap; }
static int g_swapped;
static int swapped(void) { return g_swapped; }

static void reset_all(sw_model_t model)
{
    uint8_t *w = F.wire;
    memset(&F, 0, sizeof F);
    F.wire = w;
    F.model = model;
    F.us = 100u * 1000000u;
    g_aborts = g_cart_calls = g_cart_val = g_nstates = 0;
    g_tap = 0;
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

static sw_tp_info_t tp_info(sw_model_t m)
{
    sw_tp_info_t i = { .printer = "Den StyleWriter", .board = "AirWriter v0.3.0 at 10.0.1.115",
                       .host = "airwriter-2434.local", .model = m, .colour_cart = 1,
                       .screen = HT_ATKINSON };
    return i;
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

    // ---------------------------------------------------------- a cancel left over from a job
    // A job that was cancelled leaves nothing behind: its end clears the flag (the test page's
    // session reads the same flag), so a page after it is not cancelled at once.
    reset_all(SW_C2500);
    F.cart_colour = 1;
    F.cancel_on_block = 1;
    CHECK_EQ_INT(stylewriter_job_begin(SW_C2500, 4, HT_APPLE, 0, &t), 0);
    g_target = &t;
    CHECK_EQ_INT(t.colour_ok(t.colour_ctx), 1);
    feed_page(t.stage, 1, 1);
    CHECK_EQ_INT(stylewriter_job_end(1), SW_E_CANCEL);
    CHECK_EQ_INT(stylewriter_ops()->cancelled(NULL), 0);
    // And a flag set while nothing runs (a cancel that lands as a job ends) does not stop the
    // next test page either: it clears the flag as it starts.
    reset_all(SW_C2500);
    F.cart_colour = 1;
    stylewriter_cancel();
    CHECK_EQ_INT(stylewriter_ops()->cancelled(NULL), 1);
    sw_tp_info_t ti = tp_info(SW_C2500);
    g_cart_calls = 0;
    CHECK_EQ_INT(stylewriter_test_page(SW_C2500, &ti), 0);
    CHECK_EQ_INT(F.pages, 1);
    CHECK_EQ_INT(stylewriter_ops()->cancelled(NULL), 0);

    // ---------------------------------------------------------- the test page
    // One session: the reset and setup at normal quality (m2 then n: no `d` draft, no `s` best),
    // the cartridge read and told to the board, the page's rects and blocks, its end.
    reset_all(SW_C2500);
    F.cart_colour = 1;
    start(1);
    stylewriter_link(0);                                      // (J2 was at the ImageWriter's link)
    ti = tp_info(SW_C2500);
    CHECK_EQ_INT(stylewriter_test_page(SW_C2500, &ti), 0);
    CHECK_EQ_INT(F.resets, 1);
    CHECK_EQ_MEM(F.wire, "\x00\xFF\xFF\xFF" "I", 5);
    CHECK(wire_has("m2nZA"));                                 // m2, normal on a colour cartridge, A
    CHECK(!wire_has("m2dA") && !wire_has("m2sA"));
    CHECK_EQ_INT(g_cart_calls, 1);
    CHECK_EQ_INT(g_cart_val, 1);
    CHECK_EQ_INT(F.pages, 1);
    CHECK(F.rects > 8 && F.blocks > 8);                       // a colour page is bands of blocks
    CHECK_EQ_INT(wire_count("El"), 1);                        // the page was waited out: E and l
    CHECK_EQ_INT(g_states[g_nstates - 1], SWS_READY);
    CHECK_EQ_INT(g_aborts, 0);                                // no job pipeline was involved
    CHECK_EQ_INT(F.nlinks, 2);                                // 9600, then 57600 for the page
    CHECK_EQ_INT(F.links[1].baud, 57600);                     // and the record's link stays after it

    // A black cartridge in a colour model: the page is the gray one (black rects only), whatever
    // the caller said, and the board is told black.
    reset_all(SW_C2500);
    F.cart_colour = 0;
    ti = tp_info(SW_C2500);
    ti.colour_cart = 1;
    CHECK_EQ_INT(stylewriter_test_page(SW_C2500, &ti), 0);
    CHECK_EQ_INT(g_cart_val, 0);
    CHECK_EQ_INT(F.pages, 1);
    CHECK(wire_has("m2bA"));                                  // normal on a black cartridge
    int gray_blocks = F.blocks;
    reset_all(SW_C2500);
    F.cart_colour = 1;
    CHECK_EQ_INT(stylewriter_test_page(SW_C2500, &ti), 0);
    CHECK(F.blocks > gray_blocks);                            // four planes a row against one

    // The models with no cartridge to read, and the original (no delta, no D, no H).
    reset_all(SW_II);
    ti = tp_info(SW_II);
    CHECK_EQ_INT(stylewriter_test_page(SW_II, &ti), 0);
    CHECK_EQ_INT(F.pages, 1);
    CHECK_EQ_INT(g_cart_val, 0);
    reset_all(SW_ORIGINAL);
    ti = tp_info(SW_ORIGINAL);
    CHECK_EQ_INT(stylewriter_test_page(SW_ORIGINAL, &ti), 0);
    CHECK_EQ_INT(F.pages, 1);

    // The second tap cancels it through the session: the block in flight finishes, then `E`
    // and the reset, nothing more, and the next page is not cancelled.
    reset_all(SW_C2500);
    F.cart_colour = 1;
    F.cancel_on_block = 3;
    ti = tp_info(SW_C2500);
    CHECK_EQ_INT(stylewriter_test_page(SW_C2500, &ti), SW_E_CANCEL);
    CHECK_EQ_INT(F.pages, 0);
    CHECK(F.blocks >= 3 && F.blocks <= 4);
    CHECK_EQ_INT(F.resets, 2);                                // the begin's and the abort's
    F.cancel_on_block = 0;
    CHECK_EQ_INT(stylewriter_ops()->cancelled(NULL), 0);
    F.blocks = F.pages = 0;
    CHECK_EQ_INT(stylewriter_test_page(SW_C2500, &ti), 0);
    CHECK_EQ_INT(F.pages, 1);

    // The test button's second tap is the board's own cancel, the cfg's callback: the same stop.
    {
        stylewriter_cfg_t cfg = { .record_is_sw = rec_is_sw, .state = on_state, .cart = on_cart,
                                  .cancelled = tapped };
        stylewriter_start(&cfg);
        reset_all(SW_C2500);
        F.cart_colour = 1;
        F.tap_on_block = 2;
        CHECK_EQ_INT(stylewriter_test_page(SW_C2500, &ti), SW_E_CANCEL);
        CHECK_EQ_INT(F.pages, 0);
        CHECK(F.blocks >= 2 && F.blocks <= 3);
        CHECK_EQ_INT(F.resets, 2);
        g_tap = 0;                                           // the board clears its own, as the page ends
        CHECK_EQ_INT(stylewriter_test_page(SW_C2500, &ti), 0);
        CHECK_EQ_INT(F.pages, 1);
        start(1);
    }

    // A printer that does not answer: nothing but the reset and its questions are sent, and
    // the page is never drawn.
    reset_all(SW_C2500);
    F.silent = 1;
    ti = tp_info(SW_C2500);
    CHECK_EQ_INT(stylewriter_test_page(SW_C2500, &ti), SW_E_SILENT);
    CHECK_EQ_INT(F.blocks, 0);
    CHECK_EQ_INT(F.pages, 0);
    // One that goes deaf partway: the page stops being drawn and sent, the session is aborted
    // once, and the error is the printer's.
    reset_all(SW_C2500);
    F.cart_colour = 1;
    F.silent_after_g = 2;
    CHECK_EQ_INT(stylewriter_test_page(SW_C2500, &ti), SW_E_SILENT);
    CHECK_EQ_INT(F.blocks, 2);
    CHECK_EQ_INT(F.resets, 2);
    // A model there is none of.
    reset_all(SW_NONE);
    CHECK(stylewriter_test_page(SW_NONE, &ti) != 0);
    CHECK_EQ_INT(F.wn, 0);

    // ---------------------------------------------------------- Clean Print Head, the answers
    // POST /api/printers {"id":N,"clean":true}: main.c takes the job lock without waiting and asks
    // whether a cleaning may begin; 0 starts it on its own task, which calls stylewriter_clean.
    reset_all(SW_II);
    start(1);
    CHECK_EQ_INT(stylewriter_clean_check(SW_II, 1), 409);     // a job holds the printer
    CHECK_EQ_INT(F.wn, 0);                                    // and the printer is not asked anything
    CHECK_EQ_INT(stylewriter_clean_check(SW_ORIGINAL, 0), 502);   // the original has no cleaning
    CHECK_EQ_INT(F.wn, 0);                                    // and is sent nothing
    CHECK_EQ_INT(stylewriter_clean_check(SW_NONE, 0), 502);
    CHECK_EQ_INT(F.wn, 0);
    F.silent = 1;
    CHECK_EQ_INT(stylewriter_clean_check(SW_II, 0), 502);     // switched off
    F.silent = 0;
    F.no_paper = 1;
    CHECK_EQ_INT(stylewriter_clean_check(SW_II, 0), 502);     // holding a fault: a cleaning would wait on it
    CHECK_EQ_INT(wire_count("P"), 0);                         // none was started by asking
    F.no_paper = 0;
    CHECK_EQ_INT(stylewriter_clean_check(SW_II, 0), 0);       // ready: it may begin
    CHECK_EQ_INT(wire_count("P"), 0);
    CHECK_EQ_INT(stylewriter_clean(SW_II), 0);                // and the one press is one cleaning
    CHECK_EQ_INT(wire_count("P"), 1);

    // ---------------------------------------------------------- another printer on the port
    // The printer list says an ImageWriter answered on the port last (pr_port_other_family,
    // the cfg's swapped): the StyleWriter's language must not reach it, so no job, test page
    // or cleaning starts a session,
    // and nothing at all is sent, not even the link switched. The port's own questions are the
    // policy's: the plain question is how the record's StyleWriter is found again.
    {
        stylewriter_cfg_t cfg = { .record_is_sw = rec_is_sw, .state = on_state, .cart = on_cart,
                                  .cancelled = no_cancel, .swapped = swapped };
        stylewriter_start(&cfg);
        g_swapped = 1;
        reset_all(SW_II);
        CHECK_EQ_INT(stylewriter_job_begin(SW_II, 4, HT_ATKINSON, 0, &t), -1);
        CHECK_EQ_INT(stylewriter_job_end(1), SW_E_CANCEL);        // nothing held, nothing sent
        ti = tp_info(SW_II);
        CHECK_EQ_INT(stylewriter_test_page(SW_II, &ti), SW_E_SWAPPED);
        CHECK_EQ_INT(stylewriter_clean_check(SW_II, 0), 409);
        CHECK_EQ_INT(stylewriter_clean(SW_II), SW_E_SWAPPED);
        CHECK_EQ_INT(F.wn, 0);
        CHECK_EQ_INT(F.nlinks, 0);
        CHECK_EQ_INT(g_nstates, 0);
        // It starts while a job's begin was taken: the rip's start refuses it too, before the reset.
        g_swapped = 0;
        CHECK_EQ_INT(stylewriter_job_begin(SW_II, 4, HT_ATKINSON, 0, &t), 0);
        g_target = &t;
        g_swapped = 1;
        CHECK_EQ_INT(t.colour_ok(t.colour_ctx), 0);
        CHECK_EQ_INT(g_aborts, 1);
        CHECK_EQ_INT(stylewriter_job_error(), SW_E_SWAPPED);
        CHECK_EQ_INT(stylewriter_job_end(0), SW_E_SWAPPED);
        CHECK_EQ_INT(F.wn, 0);
        // The plain question still goes, and once the record's own printer answers (the list
        // says so), a job runs as before.
        CHECK_EQ_INT(stylewriter_ask_plain(), SW_II);
        g_swapped = 0;
        reset_all(SW_II);
        CHECK_EQ_INT(stylewriter_job_begin(SW_II, 4, HT_ATKINSON, 0, &t), 0);
        g_target = &t;
        CHECK_EQ_INT(t.colour_ok(t.colour_ctx), 0);
        feed_page(t.stage, 0, 1);
        CHECK_EQ_INT(stylewriter_job_end(0), 0);
        CHECK_EQ_INT(F.pages, 1);
        start(1);
    }

    free(F.wire);
    TEST_END;
}
