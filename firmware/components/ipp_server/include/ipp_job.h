// SPDX-License-Identifier: GPL-3.0-or-later
//
// The one job slot, as pure state. ipp_server owns the locks and the sink;
// this decides what a request may do to the job and what a client sees when it
// asks. No ESP-IDF dependency, so tools/ipp tests it on the host.
//
// A job arrives one of two ways. Print-Job carries its document, so the job is
// processing from the start -- but its response, and with it the job-id, only
// goes back once the whole document is in, so a client has nothing to poll
// while the upload runs. Create-Job answers with the job-id at once and the
// document follows in Send-Document; that is the path that lets an iPhone poll
// Get-Job-Attributes mid-upload and see a paused printer.
#pragma once
#include <stdint.h>
#include "ipp_req.h"

typedef struct {
    int32_t  id;
    uint8_t  state;          // IPP_JSTATE_*
    uint8_t  awaiting_doc;   // made by Create-Job, no Send-Document yet
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
    IPP_CANCEL_RELEASE,        // never got its document: free the printer
    IPP_CANCEL_STOP_PRINT,     // printing: stop the paper
} ipp_cancel_t;

// Puts a new job in the slot. `awaiting_doc` is 1 for Create-Job, 0 for
// Print-Job. The job-uri is the printer-uri with the id appended.
void ipp_job_start(ipp_job_rec_t *j, int32_t id, uint32_t now_s, int awaiting_doc,
                   const char *name, const char *user, const char *printer_uri);

// Whether `id` names the job in the slot. -1 (job-id absent) means the
// current job.
int ipp_job_matches(const ipp_job_rec_t *j, int32_t id);

// Pending or processing: the job still holds the printer.
int ipp_job_active(const ipp_job_rec_t *j);

// Send-Document for job `id`. Returns IPP_STATUS_OK and marks the job
// processing, or the status to refuse with.
uint16_t ipp_job_claim_doc(ipp_job_rec_t *j, int32_t id);

// A Create-Job whose document has not come within IPP_MULTI_OP_TIMEOUT_S is
// aborted. Returns 1 when it did that: the caller frees the printer.
int ipp_job_reap(ipp_job_rec_t *j, uint32_t now_s);

// The document ended; `ok` is 0 if it was cut short or failed to print.
void ipp_job_finish(ipp_job_rec_t *j, int ok);

ipp_cancel_t ipp_job_cancel(ipp_job_rec_t *j, int32_t id);

// The job-state and job-state-reasons a client is told. A printing job on a
// stopped printer reads processing-stopped / printer-stopped: the job is
// waiting on the printer, not failing.
void ipp_job_view(const ipp_job_rec_t *j, int printer_stopped,
                  uint8_t *state, const char **reason);
