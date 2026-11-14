// SPDX-License-Identifier: GPL-3.0-or-later
//
// W-1102: the board's conversation with a QuickDraw LaserWriter over J2, as pure C behind a table
// of operations, as sw_session.h is for StyleWriters: the host tests run it against a scripted
// printer on a fake clock, tools/rip's qdhost against qdfake.py, and the board over its link
// (qd_link.h). It asks who the printer is and how it is while idle, and for a job sends each
// page's setup, its rows, and the print sequence, and sees each page through to done and through
// the printer's faults.
//
// W-1144: the next page loads while one prints, as Apple's driver had it. A page's print
// sequence goes once the page before it is done, and qd_session_print returns as soon as it
// has gone; the wait for its done is the next page's (or qd_session_end's). What board 1's
// printer allows, and so what the session keeps to:
//   - the next page's rows go in behind the page printing with only the paper source before
//     them; its offsets and width are the printing page's, so a page on other paper waits for
//     the done and then has the whole setup;
//   - the codes of the page printing and the page loading together stay inside `room`
//     (500,000 bytes; the printer took 513,287): past its memory while a page prints, both
//     sheets come out blank. A row that would pass it waits for the done, which frees the
//     printing page's room;
//   - 81 00 never goes between a page's 8A 01 and its done, unless to stop it: the sheet
//     comes out blank;
//   - after a jam or the cover, Apple's recovery prints again the page at the start of the
//     printer's memory: the first printed since the last 81 00. A page loaded behind it is
//     still there and prints after it. A later page can't be had back (jam_reprints in
//     qd_session.c holds that choice), and the job stops.
//
// The commands and their order are Apple's (the LaserWriter 300 driver 1.1, read for facts; the
// design's "What the printer and the board allow"): every packet is BREAK, idle, then its bytes
// (qd_wire.h); only 82, 83 (and a value after 83 9D or 83 A7) and 8E reply, one byte each.
//
// A question: drain, put, flush, then a byte within 1000 ms. An 83 whose reply is refused or
// garbled (qd_reply_ok) or does not come is sent again, five tries in all; 82 and 8E two. 8E's
// 00 says read one more byte: that is its status. While a page prints, the board asks every
// 500 ms (at once when the printer sends a BREAK), between the next page's rows too, and each
// question is tried once: two polls in a row with no answer are a printer switched off. 8E's
// page done is cleared by the read, so every 8E read counts: one that says done is the
// printing page's, wherever it was asked.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "qd_model.h"
#include "qd_status.h"

// Results. Zero is success. After any error the session is used for nothing but
// qd_session_abort and qd_session_free.
//   QD_E_OFFLINE  no clock, or the printer stopped answering. The state callback has said
//                 QDS_OFFLINE.
//   QD_E_MODEL    the engine is not the job's model. Nothing is sent after its 82 00.
//   QD_E_STALL    no paper, an empty manual slot, a jam or the cover held the job for stall_ms.
//   QD_E_FAULT    the page did not print: memory full or an error after its rows, an engine
//                 fault (named in the state as QDS_ENGINE), 85 s with no page done, or a jam
//                 the printer can't print again after (QD_EV_LOST names the page).
//   QD_E_CANCEL   cancelled said so. A sheet printing with nothing wrong came out first; then
//                 8A 00, 81 00 and 84 01 have gone; abort sends nothing more.
//   QD_E_LINK     what went out on the wire was not what was meant (the link read it back).
enum { QD_OK = 0, QD_E_OFFLINE = -1, QD_E_MODEL = -2, QD_E_STALL = -3, QD_E_FAULT = -4,
       QD_E_CANCEL = -5, QD_E_LINK = -6 };

// What happened to a page, for the log (qd_ops_t.event). Pages are numbered as qd_session_open
// was told.
typedef enum {
    QD_EV_PRINT,     // its 8A 01 went
    QD_EV_DONE,      // page done was read, ms after its 8A 01 (its last, after a jam or more
                     // paper); later than the done itself when no row came to ask between
    QD_EV_ROOM,      // its rows waited ms for the page before it to be done and free its room
    QD_EV_REPRINT,   // a jam or the cover stopped it, and the printer's recovery prints it again
    QD_EV_LOST,      // a jam or the cover, and the printer can't print it again: the job stops
} qd_event_t;

typedef struct {
    // Queues one packet (the bytes after the BREAK). 0, or <0 (a QD_E_) when the line cannot
    // carry it.
    int      (*put)(void *ctx, const uint8_t *pkt, size_t n);
    // Everything queued is on the wire and was read back as sent. 0; QD_E_OFFLINE no clock;
    // QD_E_LINK a mismatch.
    int      (*flush)(void *ctx);
    // Waits up to first_ms for a byte, then takes more while each comes within gap_ms. Returns
    // the bytes read, 0 for none.
    int      (*read)(void *ctx, uint8_t *b, size_t cap, uint32_t first_ms, uint32_t gap_ms);
    // 1 once for each BREAK the printer sent since the last call. May be NULL.
    int      (*brk)(void *ctx);
    // Throws away what the line has received and nobody has read. May be NULL.
    void     (*drain)(void *ctx);
    void     (*sleep_ms)(void *ctx, uint32_t ms);
    uint32_t (*now_ms)(void *ctx);           // may wrap; the session only subtracts
    // Non-zero when the job is to stop. May be NULL.
    int      (*cancelled)(void *ctx);
    // The printer's state, when it changes during a job. May be NULL.
    void     (*state)(void *ctx, qd_state_t st);
    // A page's print, done, wait for room, or jam (qd_event_t). May be NULL.
    void     (*event)(void *ctx, qd_event_t ev, int page, uint32_t ms);
    void      *ctx;
} qd_ops_t;

// ---- Idle, under the caller's job lock

// 81 00, 83 40, 83 3E, 82 00. *engine is 83 3E's answer, *ctrl 82 00's. QD_OK or QD_E_OFFLINE.
int qd_ask_identity(const qd_ops_t *o, uint8_t *engine, uint8_t *ctrl);
// 83 01, 83 02, 83 2A, 82 00 (then 83 04 and 83 07 when 83 01 & 0x12), then 84 with the
// state's lights. QD_OK with *st and *raw, or QD_E_OFFLINE.
int qd_ask_status(const qd_ops_t *o, qd_model_t m, int manual, qd_state_t *st, qd_raw_t *raw);

// ---- A job

typedef struct {
    qd_model_t model;
    int        manual;       // take paper from the manual slot (83 4F), not the cassette
    uint32_t   stall_ms;     // the longest a fault holds the job; 0: 600000
    uint32_t   room;         // bytes of codes the printing and the loading page may take
                             // together; 0: the model's budget (500000)
} qd_job_cfg_t;

typedef struct qd_session qd_session_t;

// The session and its row buffer (bigmem.h). NULL for no memory or a model with no spec.
// ops must outlive it.
qd_session_t *qd_session_new(const qd_ops_t *o, const qd_job_cfg_t *c);
// The identity, which must be the model's (QD_E_MODEL); then waits out a page still moving
// (83 01 & 0x20). The state goes to QDS_PRINTING.
int  qd_session_begin(qd_session_t *s);
// A page's setup (qd_sink_t.open); `page` is its number, for the events.
//   - Nothing printing (a job's first page): 81 00, the identity, the sheet's offsets and
//     width, Apple's 83 9D and density, the paper source, Ready on.
//   - A page printing, on the same sheet: the paper source alone. The rows that follow go in
//     behind the page printing.
//   - A page printing, and this one on another sheet: waits for its done (as qd_session_end
//     does), then the whole setup.
//   - Opened again before it printed (qd_page.h: a page too big goes again in Apple's screen),
//     or after a page that was loaded and never printed, or after a jam's recovery: waits for
//     the done of any page printing, then the whole setup, whose 81 00 drops the rows.
int  qd_session_open(qd_session_t *s, int page, const qd_media_t *md);
// One row's codes (qd_sink_t.row). The Ready light blinks about once a second. While a page
// prints: a row that would take the two pages past `room` first waits for its done
// (QD_EV_ROOM); otherwise, at the printer's BREAK or every 500 ms, one 8E 00, and 83 01 and
// 83 02 when that is not done. A jam, the cover or no paper seen there stops the page (8A 00)
// and shows in the state and the lights at once; the rows go on, and the next wait for the
// done puts it right. An engine fault ends the job there.
int  qd_session_row(qd_session_t *s, const uint8_t *codes, size_t n);
// Every row is in (qd_sink_t.print): waits for the page before it to be done, through out of
// paper, a jam and the cover; checks this page arrived whole; waits for paper; sends its print
// sequence, and returns. It is the page printing from then on.
int  qd_session_print(qd_session_t *s);
// The job is over: waits for the last page's done, as qd_session_print does for the page
// before; then 83 01 once, 81 00, Ready on; QDS_PRINTING while the last sheet is still on
// its way out (83 01 bit 5), else QDS_READY.
int  qd_session_end(qd_session_t *s);
// The job is given up, once, and only if the session ever wrote. After an error of the
// session's own (the printer's, or a cancel): 8A 00, 81 00, 84 01 at once. With none (the rip
// failed, the upload was cut off): as a cancel, so a sheet printing with nothing wrong comes
// out first.
void qd_session_abort(qd_session_t *s);
void qd_session_free(qd_session_t *s);
