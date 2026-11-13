// SPDX-License-Identifier: GPL-3.0-or-later
//
// The board's side of a Personal LaserWriter 300 on J2 (W-1102): the questions the port asks,
// and a job's begin, pages and end, over the link (qd_link.h). qd_session.h is the
// conversation and qd_page.h the page, both pure and host-tested; this is the clock, the link
// and the job pipeline's hooks they run on. main.c holds the job lock and decides when;
// everything here that touches J2 is called with that lock held.
//
// A job's whole session runs on the rip task: the link opens and the session begins as the rip
// begins (job_pipe_target_t.colour_ok), each page is loaded and printed from the page stage as
// the rip makes it, and the job ends on the same task. Nothing here writes flash.
#pragma once
#include <stdint.h>
#include "qd_session.h"
#include "qd_model.h"
#include "qd_status.h"
#include "job_pipe_rip.h"
#include "halftone.h"

typedef struct {
    // The printer's state changed during a job or a test page. From whichever task runs the
    // session; keep it short, no flash. May be NULL.
    void (*state)(qd_state_t st);
    // The job (a test page) is to stop; asked between steps. NULL: never.
    int  (*cancelled)(void);
    // A page was settled (qd_sink_t.note): in Apple's screen or the record's, its bytes of codes,
    // or too detailed for the printer's memory in both. On the rip task. May be NULL.
    void (*page)(int page, int apple, uint32_t bytes, int too_big);
} qdlaser_cfg_t;

void qdlaser_start(const qdlaser_cfg_t *cfg);

// The board's own error beside qd_session.h's: no memory for the link, the session or the page.
enum { QD_E_NOMEM = -7 };

// ---- J2, idle, under the caller's job lock, with a clock on the line (hz from
// serial_out_clock). Each opens the link for a question (qd_link_open(hz, 0)) and closes it.

// 81 00, 83 40, 83 3E, 82 00: QD_OK with *engine (83 3E's answer), QD_E_OFFLINE, or
// QD_E_NOMEM when the link cannot open.
int  qdlaser_identity(int hz, uint8_t *engine);
// The printer's state at rest, for the cassette (qd_ask_status, which also sets its lights):
// QD_OK with *st and, when raw is not NULL, the replies it came from (an engine fault's 83 04
// and 83 07, for the log); QD_E_OFFLINE, or QD_E_NOMEM.
int  qdlaser_status(int hz, qd_model_t m, qd_state_t *st, qd_raw_t *raw);

// ---- A job

// From the sink's begin under the job lock: fills `t` for job_pipe_begin_to (the model's caps,
// the page stage, the screen ripjob is to use, the gain, keep when the record's screen is not
// Apple's, and colour_ok, the hook that opens the link and begins the session on the rip task).
// manual: take the paper from the manual feed slot. 0, or -1 with no clock on the line, no
// memory or no such model; nothing is held then.
int  qdlaser_job_begin(qd_model_t m, ht_algo_t screen, int gain, int manual, job_pipe_target_t *t);
// The first session error of the job so far, 0 for none.
int  qdlaser_job_error(void);
// Pages so far that fit neither screen (the job goes on past them).
int  qdlaser_job_failed_pages(void);
// The job is over, on the rip task. A job that was not aborted and had no error ends the
// session (81 00, Ready on); otherwise the session is aborted. Frees the job and closes the link.
// QD_OK if every page printed; the session's error; QD_E_FAULT for a page too detailed for the
// printer, or a job that ended short with no error of its own; QD_E_CANCEL for one cancelled.
int  qdlaser_job_end(int aborted);
// Stops the job or test page at the session's next step. Cleared as each starts and ends. Any
// task.
void qdlaser_cancel(void);
// The session's line with this module's cancelled op, for the test page.
const qd_ops_t *qdlaser_ops(void);
