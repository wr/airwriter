// SPDX-License-Identifier: GPL-3.0-or-later
#include "ipp_job.h"
#include "ipp_proto.h"
#include <stdio.h>
#include <string.h>

static void copy(char *dst, size_t cap, const char *src)
{
    strncpy(dst, src ? src : "", cap - 1);
    dst[cap - 1] = '\0';
}

void ipp_job_start(ipp_job_rec_t *j, int32_t id, uint32_t now_s,
                   const char *name, const char *user, const char *printer_uri)
{
    memset(j, 0, sizeof *j);
    j->id        = id;
    j->state     = IPP_JSTATE_PROCESSING;
    j->printing  = 1;
    j->created_s = now_s;
    copy(j->name, sizeof j->name, name);
    copy(j->user, sizeof j->user, user);
    snprintf(j->uri, sizeof j->uri, "%s/%d", printer_uri, (int)id);
}

int ipp_job_matches(const ipp_job_rec_t *j, int32_t id)
{
    // Ids start at 1; a slot still at 0 has never held a job.
    return j->id > 0 && (id < 0 || id == j->id);
}

int ipp_job_active(const ipp_job_rec_t *j)
{
    return j->state == IPP_JSTATE_PROCESSING;
}

void ipp_job_finish(ipp_job_rec_t *j, int ok)
{
    j->printing = 0;
    j->state = j->canceled ? IPP_JSTATE_CANCELED
             : ok          ? IPP_JSTATE_COMPLETED
                           : IPP_JSTATE_ABORTED;
}

int ipp_job_done(ipp_job_rec_t *j, int ok)
{
    if (!j->printing) return 0;
    ipp_job_finish(j, ok);
    return 1;
}

static ipp_cancel_t cancel(ipp_job_rec_t *j, int32_t id, uint8_t by)
{
    if (!ipp_job_matches(j, id)) return IPP_CANCEL_NOT_FOUND;
    // W-1165: macOS sends Cancel-Job when it reads job-canceled-at-device, and shows
    // a refusal (RFC 8011 4.3.3's not-possible) under the printer's name as "Unable to
    // cancel print job.". The job is already what it asks for.
    if (by == IPP_CANCELED_BY_USER && j->canceled == IPP_CANCELED_AT_DEVICE)
        return IPP_CANCEL_ALREADY;
    if (!ipp_job_active(j)) return IPP_CANCEL_NOT_POSSIBLE;
    j->canceled = by;
    j->state = IPP_JSTATE_CANCELED;
    return IPP_CANCEL_STOP_PRINT;
}

ipp_cancel_t ipp_job_cancel(ipp_job_rec_t *j, int32_t id)
{
    return cancel(j, id, IPP_CANCELED_BY_USER);
}

ipp_cancel_t ipp_job_cancel_at_device(ipp_job_rec_t *j)
{
    return cancel(j, -1, IPP_CANCELED_AT_DEVICE);
}

void ipp_job_pages(ipp_job_rec_t *j, int32_t pages)
{
    if (j->printing && pages > j->pages) j->pages = pages;
}

void ipp_job_view(const ipp_job_rec_t *j, int printer_stopped,
                  uint8_t *state, const char **reason)
{
    *state = j->state;
    switch (j->state) {
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
    case IPP_JSTATE_CANCELED:
        *reason = j->canceled == IPP_CANCELED_AT_DEVICE ? "job-canceled-at-device"
                                                        : "job-canceled-by-user";
        break;
    case IPP_JSTATE_ABORTED:   *reason = "aborted-by-system";          break;
    default:                   *reason = "none";                       break;
    }
}
