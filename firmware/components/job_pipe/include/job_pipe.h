// SPDX-License-Identifier: GPL-3.0-or-later
//
// Bridges the IPP document sink to the raster pipeline.
//
// The two halves want opposite control flow: ipp_server pushes bytes as they
// arrive off the socket, while urf_decode pulls them through a read callback.
// A stream buffer sits between them, with the pipeline on its own task.
//
// The buffer is 1MB of PSRAM, and an ImageWriter job splits it (W-1066): 512KB
// of URF ahead of the rip, and 512KB of the printer's bytes behind it, which a
// printer task feeds to cfg.out at 9600 baud. The rip keeps up with the
// upload, and what it makes is small: a full-page colour photo is ~5.7MB of
// URF but 250-350KB to print, a gray page at most 115KB. So the upload
// finishes at Wi-Fi speed whatever the printer is doing, the client gets its
// answer, and the printer prints from memory on its own time. That is also
// what makes a dropped upload visible: the socket is read to the end, so a
// client that hangs up mid-document is seen at once rather than sitting
// behind a zero window until paper comes back (W-1042).
//
// Another printer's job (job_pipe_begin_to) has rows, not bytes, and keeps
// the whole 1MB for URF: an iPhone Letter page is ~140KB of it.
//
// A job whose printer bytes outrun their 512KB (two colour photo pages, or
// five worst-case gray ones), a LaserWriter document bigger than 1MB, or any
// document on a board with no PSRAM, where the buffer is 8KB of internal RAM,
// streams as it always did: when the buffer is full the push blocks, the
// socket stops being read and TCP applies the backpressure, so the printer's
// own speed throttles the phone. So does a job sent while the ImageWriter is
// not ready, once its 512KB of URF is in: the rip asks the ribbon first.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#include "halftone.h"
#include "ripjob.h"
#include "job_pipe_rip.h"

typedef struct {
    size_t   doc_bytes;      // URF in
    size_t   printer_bytes;  // ImageWriter out
    int      pages;
    uint32_t dst_w, dst_h;
    uint32_t rip_ms;         // to the rip's end; the printer may be far behind
    uint32_t ms;             // to the last byte out
    int      rc;             // 0 if the job ripped cleanly and none of it was abandoned
} job_pipe_stats_t;

typedef struct {
    // Where the ImageWriter byte stream goes. Called from the printer task
    // (the pipeline task with no PSRAM), which may be minutes behind the rip.
    void     (*out)(void *ctx, const uint8_t *buf, size_t n);
    void      *out_ctx;
    ht_algo_t  dither;
    int        gain;         // dot gain at a 50% tint, percent (ripjob_cfg_t)
    int        top_margin;   // 1/144 in, signed (ripjob_cfg_t)
    // Asked on the pipeline task as each job starts, before its first byte
    // goes to the printer: may RGB pages print in colour? (W-1064: the
    // firmware asks the printer which ribbon is in.) NULL prints them gray.
    int      (*colour_ok)(void *ctx);
    void      *colour_ctx;
    // The document is finished with: ripped and its last byte given to `out`,
    // or abandoned. Called once per job_pipe_begin, on the pipeline task, some
    // time after job_pipe_end, with the printer task done writing. The
    // pipeline is already free for the next job_pipe_begin.
    void     (*done)(void *ctx, const job_pipe_stats_t *st);
    void      *done_ctx;
} job_pipe_cfg_t;

esp_err_t job_pipe_init(const job_pipe_cfg_t *cfg);
// The screen and gain the next job prints with (W-1068: the page's pick
// used to wait for a restart). A job already ripping keeps its own.
void      job_pipe_set_screen(ht_algo_t dither, int gain);
// The top margin the next job prints with, 1/144 in (ripjob_cfg_t).
void      job_pipe_set_top_margin(int top);

// Starts the pipeline task for one document.
esp_err_t job_pipe_begin(const char *job_name);

// W-1082: a document for another printer than the ImageWriter
// (job_pipe_target_t, in job_pipe_rip.h).
esp_err_t job_pipe_begin_to(const char *job_name, const job_pipe_target_t *t);

// Returns as soon as the bytes are in the buffer. Blocks only when the buffer
// is full -- a job bigger than it -- and that backpressure is the point.
esp_err_t job_pipe_data(const uint8_t *buf, size_t n);

// Abandons the page in flight. Safe to call from another task -- the pipeline
// notices at its next read and unwinds through the normal truncation path.
void job_pipe_abort(void);

// Signals end of document and returns at once; the pipeline prints what is
// buffered and then calls cfg.done. Pass ok = 0 when the transfer was
// truncated, so the pipeline abandons the page rather than emitting a short
// one.
void job_pipe_end(int ok);
