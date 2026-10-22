// SPDX-License-Identifier: GPL-3.0-or-later
//
// The printer's attribute set. See SPEC.md 3.5 for why the margins and the
// UUID matter more than they look.
#pragma once
#include "ipp_enc.h"
#include "printer_caps.h"

// The ImageWriter's URF capability string, SPEC.md 3.3; printer_caps_urf
// builds it (and any other printer's) from printer_caps_imagewriter.
#define IPP_URF_VERSION   "V1.4"
#define IPP_URF_STRING    IPP_URF_VERSION ",CP1,RS72-144,W8,DM1,IS1,MT1,FN3"
// With the colour ribbon in (W-1064): 24-bit sRGB pages too.
#define IPP_URF_STRING_COLOUR IPP_URF_VERSION ",CP1,RS72-144,W8,SRGB24,DM1,IS1,MT1,FN3"

typedef struct {
    const char *uri;        // ipp://host.local:631/ipp/print
    const char *name;       // queue name shown in the print sheet
    const char *info;
    const char *uuid;       // urn:uuid:...
    const char *adminurl;   // http://host.local/
    const char *icons[3];   // printer-icons at 48, 128, 512 px; NULL ends the list
    const char *location;
    uint8_t     state;              // IPP_PSTATE_*
    const char *state_reason;       // "none", "media-empty-error", ...
    const char *state_message;      // printer-state-message; NULL or "" omits it
    int         accepting_jobs;
    int32_t     queued_jobs;
    uint32_t    uptime_s;
    int         colour;             // the colour ribbon is in: offer colour (W-1064)
    const printer_caps_t *caps;     // NULL: printer_caps_imagewriter (W-1082)
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

// A request's requested-attributes, left where the parser found it: `p`
// points at the attribute's first value-tag byte inside the request buffer
// (which must outlive this), `len` spans it and its additional values, `n`
// counts the values. n == 0 means the client did not send the attribute.
typedef struct {
    const uint8_t *p;
    size_t         len;
    uint16_t       n;
} ipp_names_t;

// Whether `name` is one of the requested names.
int ipp_names_has(const ipp_names_t *names, const char *name);

// Writes a printer-attributes group holding what `requested` asks for
// (RFC 8011 4.2.5.1): the groups 'all', 'printer-description' and
// 'job-template', explicit names, or 'none'. NULL or an empty list means
// 'all'. media-col-database is sent only when named: it is large, and
// IPP Everywhere clients that want it ask. The group delimiter is written
// only if at least one attribute is.
void ipp_write_printer_attrs(ipp_enc_t *e, const ipp_printer_t *p,
                             const ipp_names_t *requested);

// Writes a job-attributes group, the subset iOS polls during a job.
void ipp_write_job_attrs(ipp_enc_t *e, const ipp_job_t *j);

// The operation attributes every response opens with: charset first, then
// natural language. RFC 8011 requires that order.
void ipp_write_op_attrs(ipp_enc_t *e);
