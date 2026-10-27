// SPDX-License-Identifier: GPL-3.0-or-later
//
// The StyleWriter session (W-1103, design "The page" and "The port"): the board's
// conversation with a StyleWriter over J2, as pure C behind a table of operations so
// the host tests run it against a scripted printer on a fake clock, and the board runs
// it over the UART. It finds the model (recognition), reads the printer's state while
// idle, and for a job sends the reset, the model's setup and page letters, then the
// rects, `G` blocks and form feeds sw_page.h makes, paced on the printer's free buffer
// and held for its faults. It also runs one cleaning.
//
// The letters per model, the order of a job and which bits of `1` and `2` stop it are
// TailTalk's reading of Apple's drivers (tailtalk/src/stylewriter/{job,session,status}.rs,
// FeralFirmware, read for facts and not copied); sw_session.c puts the line references
// beside the tables. Plain paper only, and none of the fast-clock requests (`x`, `h`).
// Only the Color StyleWriter 2200 has printed from TailTalk on a real printer.
//
// Time: a printer is silent when 30 s pass with nothing it says heard, counted from its
// last answer or from when the caller last gave the session something to do (sw_session_feed
// or _end), whichever is later: a job may wait a minute for its next frames while the rip
// runs. A wait for a fault to clear, for the buffer to drain or for the page to print ends
// at stall_ms (600 s) with no progress; silence during it is still silence. A block takes
// up to about 6 s to go out at 57600 baud, which counts as time.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "sw_model.h"

// What the printer's state is, for the panel and for IPP. During a job the session
// reports it through sw_ops_t.state as it changes: SWS_PRINTING from the start, a fault
// while it holds the job, SWS_PRINTING again once it clears, SWS_OFFLINE when the printer
// goes silent, SWS_READY at a job's end. Idle, sw_ask_status gives it.
//   SWS_NO_PAPER      `2` 04       SWS_JAM          `2` 08
//   SWS_CART_JAM      `2` 01       SWS_CART_MISSING `2` 20
//   SWS_INK_OUT       `2` 02 (the 2200, 2400 and 2500; on the others, the 1500 included,
//                     02 is the printer holding)
//   SWS_DOOR          `1` 40 (the 2200, 2400 and 2500)
//   SWS_WASTE         `1` 80 (the four colour models)
//   SWS_FAULT         a fault bit the design does not name: `1` 04 (and 02 on the 2200, 2400
//                     and 2500), `2` 10, 40 and 80 (80 not on the 2400 and 2500); a job that
//                     sees one ends in SW_E_FAULT
// Busy (`1` 01) and page done (`1` 08) are progress, not faults: SWS_PRINTING and SWS_READY.
typedef enum { SWS_READY, SWS_PRINTING, SWS_NO_PAPER, SWS_JAM, SWS_CART_MISSING, SWS_CART_JAM,
               SWS_INK_OUT, SWS_DOOR, SWS_WASTE, SWS_OFFLINE, SWS_FAULT } sw_state_t;

// Results. Zero is success; each other is what a caller does next (job_pipe_abort() is
// the board's way of failing the job whose bytes are still coming):
//   SW_E_SILENT  nothing the printer said was heard for 30 s, or a write failed. The state
//                callback has said SWS_OFFLINE. Fail the job; sw_session_abort() is safe
//                but the next job resets the printer anyway.
//   SW_E_MODEL   the printer answers `?` as another model than the job was made for (nothing
//                is sent after it), or its empty buffer is smaller than the model's blocks.
//                Fail the job and call sw_session_abort(); the port's record has the wrong model.
//   SW_E_STALL   a fault, a full buffer or an unprinted page held the job for stall_ms.
//                Fail the job and call sw_session_abort(): it ejects the page and resets.
//   SW_E_FAULT   a fault bit the design does not name held for 10 s, a frame that is not
//                one sw_page.h makes, or (sw_clean) a printer with no cleaning. Fail the
//                job and call sw_session_abort().
//   SW_E_CANCEL  sw_ops_t.cancelled said so. The session has already sent `E` (black-only
//                models) and the reset, and stops: calling sw_session_abort() as well is
//                harmless. Fail the job.
//   SW_E_CART    the cartridge cannot be read (a colour model that answers `H` with neither
//                81 nor 01, as with none fitted), or a colour page was sent to a black
//                cartridge or to a job not made for colour. Fail the job and call
//                sw_session_abort().
// After any error but SW_E_CANCEL the session is not used again but for sw_session_abort and
// sw_session_free. So every error ends the same way for a caller: sw_session_abort() (it
// sends `E` and the reset once, and only if the session ever wrote), then job_pipe_abort() so
// the rip task, which may be blocked in job_pipe_out(), is released.
enum { SW_OK = 0, SW_E_SILENT = -1, SW_E_MODEL = -2, SW_E_STALL = -3, SW_E_FAULT = -4,
       SW_E_CANCEL = -5, SW_E_CART = -6 };

// The line and the clock. write, read, sleep_ms and now_ms are required; drain, cancelled
// and state may be NULL.
typedef struct {
    // Sends n bytes; 0 when they went, else the printer cannot be reached.
    int      (*write)(void *ctx, const uint8_t *b, size_t n);
    // Waits up to first_ms for a byte, then takes more while each comes within gap_ms of
    // the one before, up to cap. Returns how many came, 0 for none.
    int      (*read)(void *ctx, uint8_t *b, size_t cap, uint32_t first_ms, uint32_t gap_ms);
    // Throws away what the line has received and nobody has read: the session does this
    // before every question, so a late or stray byte never answers the next one.
    void     (*drain)(void *ctx);
    void     (*sleep_ms)(void *ctx, uint32_t ms);
    uint32_t (*now_ms)(void *ctx);           // may wrap; the session only subtracts
    // Non-zero when the job is to stop. Asked between steps, never in the middle of a
    // block: a block that has begun goes out whole.
    int      (*cancelled)(void *ctx);
    // The printer's state, when it changes (and once at a job's start).
    void     (*state)(void *ctx, sw_state_t st);
    void      *ctx;
} sw_ops_t;

// ---- Recognition and idle (no job; the caller holds the port and the baud rate)

// The plain question: writes the one byte `?` and reads the reply to its CR at 250 ms a
// byte (a reply that stops without a CR counts). Returns the model the reply names: SW_II,
// SW_1200, and SW_ORIGINAL if an original happens to answer it (it usually does not, until
// reset, so it is found by the full sequence or by being named). Nothing else is written
// for those, or for no reply or a reply that is not a StyleWriter's (SW_NONE): a port whose
// printer is unknown is asked one byte and no more. "CS" names the colour family but not a
// member; it proves a colour StyleWriter, which is what `p` is for, so `FF FF FF 70` follows
// as a write of its own and its byte (250 ms) names the model (sw_model_from_p). A CS that
// does not answer `p` names no model (SW_NONE), as in the full sequence.
sw_model_t sw_ask_plain(const sw_ops_t *o);

// The full sequence, for a printer the record or the person has named: `00 FF FF FF 49`
// (eject and reset), settle 2 s, drain, `?`; when that gets nothing, the reset again and
// 8 s (the original ignores `?` until reset). Only after a reply of "CS" is `FF FF FF 70`
// (`p`) sent, which any other model would take as a fault, and its byte names the model
// (sw_model_from_p). SW_NONE for no reply, a reply that is not a StyleWriter's, or a CS
// that does not answer `p`.
sw_model_t sw_ask_full(const sw_ops_t *o);

// `1` and `2`, for the idle check every 10 s. SW_OK with *st the state, or SW_E_SILENT
// (and SWS_OFFLINE) when either goes unanswered twice in a second each.
int sw_ask_status(const sw_ops_t *o, sw_model_t m, sw_state_t *st);

// The cartridge: 1 colour, 0 black, SW_E_SILENT, or SW_E_CART when the answer is
// unreadable. A colour model is sent `D` first, as a job does. The original has no `H`
// and reads 0 with nothing sent.
int sw_ask_cart(const sw_ops_t *o, sw_model_t m);

// One cleaning, as the Clean Print Head key asks: the job's reset and setup with a single
// `P` in it, then waits for the printer to stop being busy (up to 600 s), then `E` (and `Y`
// on the 1200) and `l`. SW_E_FAULT with nothing sent for the original, which has none.
// Otherwise the results of a job's begin, and SW_E_STALL if it is still busy at 600 s.
int sw_clean(const sw_ops_t *o, sw_model_t m);

// ---- A job

typedef struct {
    sw_model_t model;       // what the port's record says; `?` must agree
    int        quality;     // print-quality: 3 draft, 4 normal, 5 best; 0 (none sent) is normal
    int        colour_job;  // the job's pages are sRGB, to print in colour when the cartridge allows
    uint32_t   stall_ms;    // 600000 (0 also means that)
} sw_job_cfg_t;

typedef struct sw_session sw_session_t;

// Allocates the session and the buffer that holds a frame split across feeds (the model's
// largest block and 3 bytes, through bigmem.h: PSRAM on the board). NULL for a bad model
// or ops, or no memory.
sw_session_t *sw_session_new(const sw_ops_t *o, const sw_job_cfg_t *c);

// Resets the printer, lets it settle (2 s; 8 s for the original), waits for `1` and `2` to
// show no fault, asks `?` and holds its answer to the model, then `D` (the colour models)
// or `N` (the 1200), `B`, `H` (all but the original), and sends the model's setup for the
// quality and the cartridge. SW_OK, or one of the results above.
int sw_session_begin(sw_session_t *s);

// The frames of sw_page.h, split anywhere: a page's start, each rect (after `1` and `2`
// show no fault), each block (when the printer has room for it, tracked from `B`, which
// is read again every 100 ms while it does not), each page's end. It returns when all of
// buf has gone to the printer, so it blocks while the printer is full or holds a fault.
// The cartridge may have been read as black for a job made for colour: build the page
// stage with colour_cart = sw_session_cart(s) after sw_session_begin, and a colour page
// is refused (SW_E_CART).
int sw_session_feed(sw_session_t *s, const uint8_t *buf, size_t n);

// After the last page: waits for `1` to say the page is printed (all but the original), then
// `E` (`Y` more on the 1200) and `l`. SWS_READY is reported.
int sw_session_end(sw_session_t *s);

// For a job that stops short: `E` (the black-only models) and the reset, once, whatever has
// happened. The next job begins with its own reset.
void sw_session_abort(sw_session_t *s);

// What `H` said at begin: 1 colour, 0 black (and 0 before begin).
int sw_session_cart(const sw_session_t *s);

void sw_session_free(sw_session_t *s);
