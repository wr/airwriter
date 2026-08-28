// SPDX-License-Identifier: GPL-3.0-or-later
//
// IPP request parser. Pure C over a byte buffer with no ESP-IDF dependency, so
// the host tests in tools/ipp link it directly. The transport half of
// ipp_server (esp_http_server glue) is the part that cannot run on the host.
//
// It only lifts the operation attributes the firmware acts on. Everything else
// is skipped, not stored -- a printer that ignores an attribute it does not
// implement is correct IPP, and iOS sends plenty we have no use for.
#pragma once
#include <stdint.h>
#include <stddef.h>

#define IPP_REQ_OK     0
#define IPP_REQ_SHORT -1   // ran off the end; read more and call again
#define IPP_REQ_BAD   -2   // malformed beyond recovery

// Long enough for what iOS sends, short enough to sit on the stack. A value
// that does not fit is truncated rather than rejected: an over-long job name
// is not a reason to fail a print.
#define IPP_URI_MAX  128
#define IPP_NAME_MAX 64
#define IPP_FMT_MAX  40

typedef struct {
    uint16_t version;
    uint16_t op;
    uint32_t request_id;

    char printer_uri[IPP_URI_MAX];
    char job_name[IPP_NAME_MAX];
    char requesting_user[IPP_NAME_MAX];
    char document_format[IPP_FMT_MAX];

    int32_t job_id;          // -1 when absent
    int     truncated;       // a value was longer than its field

    // Offset of the first document byte, i.e. one past the end-of-attributes
    // tag. Print-Job data starts here.
    size_t  header_len;
} ipp_req_t;

int ipp_req_parse(ipp_req_t *r, const uint8_t *buf, size_t len);
