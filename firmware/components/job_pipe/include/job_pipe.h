// SPDX-License-Identifier: GPL-3.0-or-later
//
// Bridges the IPP document sink to the raster pipeline.
//
// The two halves want opposite control flow: ipp_server pushes bytes as they
// arrive off the socket, while urf_decode pulls them through a read callback.
// A stream buffer sits between them, with the pipeline on its own task.
//
// That is not just plumbing. The printer runs at 9600 baud, so the pipeline
// drains far slower than WiFi fills it; when the buffer is full the push
// blocks, the socket stops being read and TCP applies the backpressure for us.
// The printer's own speed ends up throttling the phone, which is what the DTR
// handshake will extend once serial_out exists.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#include "halftone.h"

typedef struct {
    // Where the ImageWriter byte stream goes. Called from the pipeline task.
    void     (*out)(void *ctx, const uint8_t *buf, size_t n);
    void      *out_ctx;
    ht_algo_t  dither;
} job_pipe_cfg_t;

typedef struct {
    size_t   doc_bytes;      // URF in
    size_t   printer_bytes;  // ImageWriter out
    int      pages;
    uint32_t dst_w, dst_h;
    uint32_t ms;
    int      rc;             // 0 if the job ripped cleanly
} job_pipe_stats_t;

esp_err_t job_pipe_init(const job_pipe_cfg_t *cfg);

// Starts the pipeline task for one document.
esp_err_t job_pipe_begin(const char *job_name);

// Blocks while the pipeline is behind. That backpressure is the point.
esp_err_t job_pipe_data(const uint8_t *buf, size_t n);

// Abandons the page in flight. Safe to call from another task -- the pipeline
// notices at its next read and unwinds through the normal truncation path.
void job_pipe_abort(void);

// Signals end of document and waits for the pipeline to finish. Pass ok = 0
// when the transfer was truncated, so the pipeline abandons the page rather
// than emitting a short one.
void job_pipe_end(int ok, job_pipe_stats_t *st);
