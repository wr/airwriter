// SPDX-License-Identifier: GPL-3.0-or-later
//
// The printer's attribute set. See SPEC.md 3.5 for why the margins and the
// UUID matter more than they look.
#pragma once
#include "ipp_enc.h"

// URF capability string, SPEC.md 3.3. iOS honoured RS72-144 in the capture at
// fixtures/ios-20260804-232652.urf -- it rendered at 144 dpi rather than
// sending 300 -- so no RS300 fallback is advertised.
#define IPP_URF_VERSION   "V1.4"
#define IPP_URF_STRING    IPP_URF_VERSION ",CP1,RS72-144,W8,DM1,IS1,MT1,FN3"

// How long a job made by Create-Job waits for its Send-Document before it is
// aborted and the printer freed for the next job. Advertised as
// multiple-operation-time-out, which RFC 8011 requires of a printer with
// Create-Job. A client sends the document straight after Create-Job.
#define IPP_MULTI_OP_TIMEOUT_S 60

typedef struct {
    const char *uri;        // ipp://host.local:631/ipp/print
    const char *name;       // queue name shown in the print sheet
    const char *info;
    const char *uuid;       // urn:uuid:...
    const char *adminurl;   // http://host.local/
    const char *location;
    uint8_t     state;              // IPP_PSTATE_*
    const char *state_reason;       // "none", "media-empty-error", ...
    const char *state_message;      // printer-state-message; NULL or "" omits it
    int         accepting_jobs;
    int32_t     queued_jobs;
    uint32_t    uptime_s;
} ipp_printer_t;

typedef struct {
    int32_t     id;
    const char *uri;
    const char *printer_uri;
    const char *name;
    const char *user;
    uint8_t     state;              // IPP_JSTATE_*
    const char *state_reason;
    int32_t     impressions_done;
    uint32_t    time_at_creation;
} ipp_job_t;

// Writes a printer-attributes group. `requested` is the client's
// requested-attributes list, honoured only to the extent of the all/none
// distinction: iOS asks for "all" in practice, and filtering attribute by
// attribute costs more code than sending the set.
void ipp_write_printer_attrs(ipp_enc_t *e, const ipp_printer_t *p);

// Writes a job-attributes group, the subset iOS polls during a job.
void ipp_write_job_attrs(ipp_enc_t *e, const ipp_job_t *j);

// The operation attributes every response opens with: charset first, then
// natural language. RFC 8011 requires that order.
void ipp_write_op_attrs(ipp_enc_t *e);
