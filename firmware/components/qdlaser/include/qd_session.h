// SPDX-License-Identifier: GPL-3.0-or-later
//
// W-1102: the board's conversation with a QuickDraw LaserWriter over J2, as pure C behind a table
// of operations, as sw_session.h is for StyleWriters: the host tests run it against a scripted
// printer on a fake clock, tools/rip's qdhost against qdfake.py, and the board over its link
// (qd_link.h). It asks who the printer is and how it is while idle, and for a job sends each
// page's setup, its rows, and the print sequence, then sees the page through to done and through
// the printer's faults. One page is in the printer at a time.
//
// The commands and their order are Apple's (the LaserWriter 300 driver 1.1, read for facts; the
// design's "What the printer and the board allow"): every packet is BREAK, idle, then its bytes
// (qd_wire.h); only 82, 83 (and a value after 83 9D or 83 A7) and 8E reply, one byte each.
//
// A question: drain, put, flush, then a byte within 1000 ms. An 83 whose reply is refused or
// garbled (qd_reply_ok) or does not come is sent again, five tries in all; 82 and 8E two. 8E's
// 00 says read one more byte: that is its status. While a page prints, the board asks every
// 500 ms (at once when the printer sends a BREAK) and each question is tried once: two polls
// in a row with no answer are a printer switched off.
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
//                 fault (named in the state as QDS_ENGINE), or 85 s with no page done.
//   QD_E_CANCEL   cancelled said so. 8A 00, 81 00 and 84 01 have gone; abort sends nothing more.
//   QD_E_LINK     what went out on the wire was not what was meant (the link read it back).
enum { QD_OK = 0, QD_E_OFFLINE = -1, QD_E_MODEL = -2, QD_E_STALL = -3, QD_E_FAULT = -4,
       QD_E_CANCEL = -5, QD_E_LINK = -6 };

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
} qd_job_cfg_t;

typedef struct qd_session qd_session_t;

// The session and its row buffer (bigmem.h). NULL for no memory or a model with no spec.
// ops must outlive it.
qd_session_t *qd_session_new(const qd_ops_t *o, const qd_job_cfg_t *c);
// The identity, which must be the model's (QD_E_MODEL); then waits out a page still moving
// (83 01 & 0x20). The state goes to QDS_PRINTING.
int  qd_session_begin(qd_session_t *s);
// A page's setup (qd_sink_t.open): 81 00 (which drops a page already loaded), the identity,
// the sheet's offsets and width, Apple's 83 9D and density, the paper source, Ready on.
int  qd_session_open(qd_session_t *s, const qd_media_t *md);
// One row's codes (qd_sink_t.row). The Ready light blinks about once a second.
int  qd_session_row(qd_session_t *s, const uint8_t *codes, size_t n);
// Every row is in (qd_sink_t.print): check the page arrived whole, wait for paper, print it,
// and wait until it is done, through out of paper, a jam and the cover.
int  qd_session_print(qd_session_t *s);
// The job is over: 81 00, Ready on, QDS_READY.
int  qd_session_end(qd_session_t *s);
// After an error: 8A 00, 81 00, 84 01, once, and only if the session ever wrote.
void qd_session_abort(qd_session_t *s);
void qd_session_free(qd_session_t *s);
