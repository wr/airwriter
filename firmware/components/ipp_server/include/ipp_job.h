// SPDX-License-Identifier: GPL-3.0-or-later
//
// The one job slot, as pure state. ipp_server owns the locks and the sink;
// this decides what a request may do to the job and what a client sees when it
// asks. No ESP-IDF dependency, so tools/ipp tests it on the host.
//
// Jobs arrive by Print-Job only. Its response goes back once the whole
// document is in, but iOS and macOS do not wait for it: while the upload runs
// they find the job with Get-Jobs and poll Get-Job-Attributes, so the state
// below is what they show while a paused printer backs the upload up.
#pragma once
#include <stdint.h>
#include "ipp_req.h"

typedef struct {
    int32_t  id;
    uint8_t  state;          // IPP_JSTATE_*
    uint8_t  canceled;       // Cancel-Job arrived while the document streamed
    uint32_t created_s;
    char     name[IPP_NAME_MAX];
    char     user[IPP_NAME_MAX];
    char     uri[160];
} ipp_job_rec_t;

// What Cancel-Job has to do beyond the bookkeeping.
typedef enum {
    IPP_CANCEL_NOT_FOUND,      // no such job
    IPP_CANCEL_NOT_POSSIBLE,   // already completed, canceled or aborted
    IPP_CANCEL_STOP_PRINT,     // printing: stop the paper
} ipp_cancel_t;

// Puts a new, processing job in the slot. The job-uri is the printer-uri with
// the id appended.
void ipp_job_start(ipp_job_rec_t *j, int32_t id, uint32_t now_s,
                   const char *name, const char *user, const char *printer_uri);

// Whether `id` names the job in the slot. -1 (job-id absent) means the
// current job. An empty slot (id 0, before the first job) matches nothing.
int ipp_job_matches(const ipp_job_rec_t *j, int32_t id);

// Still printing: the job holds the printer.
int ipp_job_active(const ipp_job_rec_t *j);

// The document ended; `ok` is 0 if it was cut short or failed to print.
void ipp_job_finish(ipp_job_rec_t *j, int ok);

ipp_cancel_t ipp_job_cancel(ipp_job_rec_t *j, int32_t id);

// The job-state and job-state-reasons a client is told. A printing job on a
// stopped printer reads processing-stopped / printer-stopped: the job is
// waiting on the printer, not failing.
void ipp_job_view(const ipp_job_rec_t *j, int printer_stopped,
                  uint8_t *state, const char **reason);
