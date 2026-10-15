// SPDX-License-Identifier: GPL-3.0-or-later
#include "ipp_job.h"
#include "ipp_attrs.h"
#include "ipp_proto.h"
#include <stdio.h>
#include <string.h>

static void copy(char *dst, size_t cap, const char *src)
{
    strncpy(dst, src ? src : "", cap - 1);
    dst[cap - 1] = '\0';
}

void ipp_job_start(ipp_job_rec_t *j, int32_t id, uint32_t now_s, int awaiting_doc,
                   const char *name, const char *user, const char *printer_uri)
{
    memset(j, 0, sizeof *j);
    j->id           = id;
    j->state        = awaiting_doc ? IPP_JSTATE_PENDING : IPP_JSTATE_PROCESSING;
    j->awaiting_doc = awaiting_doc ? 1 : 0;
    j->created_s    = now_s;
    copy(j->name, sizeof j->name, name);
    copy(j->user, sizeof j->user, user);
    snprintf(j->uri, sizeof j->uri, "%s/%d", printer_uri, (int)id);
}

int ipp_job_matches(const ipp_job_rec_t *j, int32_t id)
{
    return id < 0 || id == j->id;
}

int ipp_job_active(const ipp_job_rec_t *j)
{
    return j->state == IPP_JSTATE_PENDING || j->state == IPP_JSTATE_PROCESSING;
}

uint16_t ipp_job_claim_doc(ipp_job_rec_t *j, int32_t id)
{
    if (!ipp_job_matches(j, id)) return IPP_STATUS_ERR_NOT_FOUND;
    // Already has its document, or was canceled or given up on: one document
    // per job (multiple-document-jobs-supported is false).
    if (!j->awaiting_doc) return IPP_STATUS_ERR_NOT_POSSIBLE;
    j->awaiting_doc = 0;
    j->state = IPP_JSTATE_PROCESSING;
    return IPP_STATUS_OK;
}

int ipp_job_reap(ipp_job_rec_t *j, uint32_t now_s)
{
    // Unsigned difference, so an uptime wrap does not age a job instantly.
    if (!j->awaiting_doc || now_s - j->created_s < IPP_MULTI_OP_TIMEOUT_S) return 0;
    j->awaiting_doc = 0;
    j->state = IPP_JSTATE_ABORTED;
    return 1;
}

void ipp_job_finish(ipp_job_rec_t *j, int ok)
{
    j->state = j->canceled ? IPP_JSTATE_CANCELED
             : ok          ? IPP_JSTATE_COMPLETED
                           : IPP_JSTATE_ABORTED;
}

ipp_cancel_t ipp_job_cancel(ipp_job_rec_t *j, int32_t id)
{
    if (!ipp_job_matches(j, id)) return IPP_CANCEL_NOT_FOUND;
    if (!ipp_job_active(j)) return IPP_CANCEL_NOT_POSSIBLE;

    j->canceled = 1;
    j->state = IPP_JSTATE_CANCELED;
    if (j->awaiting_doc) {
        j->awaiting_doc = 0;
        return IPP_CANCEL_RELEASE;
    }
    return IPP_CANCEL_STOP_PRINT;
}

void ipp_job_view(const ipp_job_rec_t *j, int printer_stopped,
                  uint8_t *state, const char **reason)
{
    *state = j->state;
    switch (j->state) {
    case IPP_JSTATE_PENDING:
        *reason = "job-incoming";
        break;
    case IPP_JSTATE_PROCESSING:
        // RFC 8011 5.3.7: processing-stopped, returning to processing once
        // the reasons clear; printer-stopped says the printer is why.
        if (printer_stopped) {
            *state  = IPP_JSTATE_STOPPED;
            *reason = "printer-stopped";
        } else {
            *reason = "job-printing";
        }
        break;
    case IPP_JSTATE_COMPLETED: *reason = "job-completed-successfully"; break;
    case IPP_JSTATE_CANCELED:  *reason = "job-canceled-by-user";       break;
    case IPP_JSTATE_ABORTED:   *reason = "aborted-by-system";          break;
    default:                   *reason = "none";                       break;
    }
}
