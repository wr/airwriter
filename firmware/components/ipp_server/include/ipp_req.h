// SPDX-License-Identifier: GPL-3.0-or-later
//
// IPP request parser. Pure C over a byte buffer with no ESP-IDF dependency, so
// the host tests in tools/ipp link it directly. The transport half of
// ipp_server (esp_http_server glue) is the part that cannot run on the host.
//
// It only lifts the attributes the firmware acts on: the operation group's, and
// from the job group the job's options (ipp_job_opts_t). Everything else is
// skipped, not stored -- a printer that ignores an attribute it does not
// implement is correct IPP, and iOS sends plenty we have no use for. Collections
// (media-col) are walked through, their members never read as attributes.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "ipp_attrs.h"

#define IPP_REQ_OK     0
#define IPP_REQ_SHORT -1   // ran off the end; read more and call again
#define IPP_REQ_BAD   -2   // malformed beyond recovery

// Long enough for what iOS sends, short enough to sit on the stack. A value
// that does not fit is truncated rather than rejected: an over-long job name
// is not a reason to fail a print.
#define IPP_URI_MAX  128
#define IPP_NAME_MAX 64
#define IPP_FMT_MAX  40

// What a job asks for, from its Job group (W-1103). A printer that has no use
// for an option ignores it.
typedef struct {
    // print-quality: 3 draft, 4 normal, 5 high. 0 when the client sent none,
    // or sent a value that is not one of these.
    int quality;
} ipp_job_opts_t;

typedef struct {
    uint16_t version;
    uint16_t op;
    uint32_t request_id;

    char printer_uri[IPP_URI_MAX];
    char job_name[IPP_NAME_MAX];
    char requesting_user[IPP_NAME_MAX];
    char document_format[IPP_FMT_MAX];

    int32_t job_id;          // -1 when absent; also read from job-uri

    ipp_job_opts_t opts;

    // requested-attributes, pointing into the parsed buffer: valid as long
    // as that buffer is. n == 0 when absent.
    ipp_names_t requested;
    int     truncated;       // a value was longer than its field

    // Offset of the first document byte, i.e. one past the end-of-attributes
    // tag. Print-Job data starts here.
    size_t  header_len;
} ipp_req_t;

int ipp_req_parse(ipp_req_t *r, const uint8_t *buf, size_t len);

// The printers one server answers for (W-1082, W-1088, W-1116): six slots, any
// mix of the printer port's records and printers on AppleTalk (PR_SHARE_MAX).
// Slot 0 is the oldest port record's, at every path it has always answered at
// and at its own ipp/p<id> (its legacy path, R9: the path it had before it was
// the oldest); the others each at its own (ipp/p<id>), and W-1082's LaserWriter
// also at ipp/laser.
#define IPP_PRINTERS       6
#define IPP_RESOURCE_LASER "ipp/laser"

// Which printer a request's HTTP path (or absolute URI) names: a slot at its
// resource or its legacy one (without the leading '/'), slot 0 included, then
// slot 0 for any other path while it is set, and -1 for none: a path of the
// ipp/p<n> or ipp/laser shape no slot holds is never slot 0's.
int ipp_route(const char *path, const char *const resources[IPP_PRINTERS],
              const char *const legacy[IPP_PRINTERS]);
