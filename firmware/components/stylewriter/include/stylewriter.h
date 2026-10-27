// SPDX-License-Identifier: GPL-3.0-or-later
//
// The board's side of a StyleWriter on J2 (W-1103, design "The port" and "The page"): the
// session's line over serial_out, the questions the port asks, a job's begin, bytes and end,
// and one cleaning. sw_session.h is the conversation, pure and host-tested; this is the
// UART, the clock and the job pipeline's hooks it runs on. main.c holds the job lock and
// decides when; everything here that touches J2 is called with that lock held.
//
// A StyleWriter's link is 57600 baud and no flow control (serial_out_set_link); the
// ImageWriter's is 9600 with CTS. The questions here put the link back to the one the port's
// record wants when they are done, so an ImageWriter never hears 57600 for longer than a
// question takes, and a StyleWriter's record keeps its link.
//
// Nothing in here may write flash from the printer task: it runs on a PSRAM stack
// (job_pipe.c), and a flash write with such a task runnable crashes. The callbacks main.c
// gives are held to that.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "sw_session.h"
#include "sw_model.h"
#include "job_pipe_rip.h"
#include "halftone.h"

typedef struct {
    // The port's record is a StyleWriter: the link J2 goes back to after a question.
    // NULL: it is not.
    int  (*record_is_sw)(void);
    // The printer's state changed (sw_session.h's sw_state_t), during a job, a cleaning or
    // any session the board runs. From whichever task runs the session: the rip task for a
    // job's begin and end, the printer task for its pages, the cleaning's own. Keep it short.
    void (*state)(sw_state_t st);
    // A job read the cartridge: 1 colour, 0 black. On the rip task, once per job.
    void (*cart)(int colour);
    // The job (a cleaning, a test page) is to stop; asked between steps. NULL: never.
    int  (*cancelled)(void);
} stylewriter_cfg_t;

void stylewriter_start(const stylewriter_cfg_t *cfg);

// A job error of the board's own, beside sw_session.h's: no memory for the page stage.
enum { SW_E_NOMEM = -7 };

// ---- J2, under the caller's job lock and with no job in the pipeline

// The link: 1 is a StyleWriter's (57600, no flow control), 0 an ImageWriter's (9600, CTS).
// Cheap when it is the link J2 already has. Before any ImageWriter-only traffic (ESC ?,
// CTRL-X, the resync, the test page) call stylewriter_link(0), and
// stylewriter_link_record() once it is done.
void stylewriter_link(int sw);
// The link the port's record wants.
void stylewriter_link_record(void);

// The plain question: the link to 57600, one `?`, then the link the record wants.
// The model the printer named (sw_ask_plain), or SW_NONE.
sw_model_t stylewriter_ask_plain(void);
// The full sequence, for a printer the person or the record named (sw_ask_full): SW_NONE if
// it does not answer. The link is the record's again at the end.
sw_model_t stylewriter_ask_full(void);
// `1` and `2`: SW_OK with *st, or SW_E_SILENT (sw_ask_status).
int        stylewriter_status(sw_model_t m, sw_state_t *st);
// The cartridge: 1 colour, 0 black, or SW_E_SILENT / SW_E_CART (sw_ask_cart).
int        stylewriter_cart(sw_model_t m);
// One cleaning (sw_clean): 0, or the session's error. Takes as long as the printer does,
// up to ten minutes; the state callback follows it.
int        stylewriter_clean(sw_model_t m);

// The session's line, for whoever else sends a StyleWriter bytes under the lock (the test
// page). Its cancelled op is the callback's and stylewriter_cancel's.
const sw_ops_t *stylewriter_ops(void);

// Stops what the session is doing at its next step (a cancel, a cut upload). Cleared by
// each stylewriter_job_begin and stylewriter_clean. Any task.
void stylewriter_cancel(void);

// ---- A job

// Called from the sink's begin, under the job lock, with the port's record's model, the
// job's print-quality (0: none sent), and the record's screen. Allocates the session and
// fills `t` for job_pipe_begin_to: the model's caps, Apple's tone curve for the screen, the
// printer-bytes ring (`buffered`), stylewriter_prn as its consumer, and the stage the page
// is made into. The page itself is made, and the printer reset, on the pipeline task as the
// rip begins (job_pipe_target_t.colour_ok): the cartridge decides what the page stage is, so
// nothing is ripped before it is known. 0, or -1 for no memory or a bad model (nothing is
// held then).
int  stylewriter_job_begin(sw_model_t m, int quality, ht_algo_t dither, int gain,
                           job_pipe_target_t *t);

// The job's printer bytes (the frames of sw_page.h), on the printer task: fed to the
// session, which blocks while the printer is full or holds a fault. An error ends the job:
// the session is aborted, the rip task released (job_pipe_abort), and later bytes go nowhere.
void stylewriter_prn(void *ctx, const uint8_t *buf, size_t n);

// The first session error of the job so far, 0 for none.
int  stylewriter_job_error(void);

// The job is over, on the pipeline task, with the printer task done. `aborted` is 0 for a rip
// that finished every page of a whole transfer that was not cancelled: the last page is
// waited out and `E` and `l` are sent (sw_session_end). Otherwise, or after any error of the
// job's own, the session is aborted (`E` and the reset). Frees the job. 0 if the pages
// printed; else the session's error (sw_session.h), or SW_E_CANCEL / SW_E_FAULT for a job that
// ended short with no error of its own.
int  stylewriter_job_end(int aborted);
