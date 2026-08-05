// SPDX-License-Identifier: GPL-3.0-or-later
//
// IPP response encoder. Writes into a caller-supplied buffer and never
// allocates. Overflow is sticky: once the buffer is full every later call is a
// no-op and ipp_enc_ok() reports the failure, so a caller can emit a whole
// response and check once at the end rather than after every attribute.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "ipp_proto.h"

typedef struct {
    uint8_t *buf;
    size_t   cap;
    size_t   len;
    int      overflow;
} ipp_enc_t;

void ipp_enc_init(ipp_enc_t *e, uint8_t *buf, size_t cap);

// Response header: version 1.1, the status code, and the request id echoed
// back from the request. A response with a different request id is discarded
// by the client without an error, which is a miserable thing to debug.
void ipp_resp_begin(ipp_enc_t *e, uint16_t status, uint32_t request_id);

void ipp_group(ipp_enc_t *e, uint8_t delimiter_tag);
void ipp_end(ipp_enc_t *e);

void ipp_attr_str(ipp_enc_t *e, uint8_t tag, const char *name, const char *val);
void ipp_attr_int(ipp_enc_t *e, uint8_t tag, const char *name, int32_t val);
void ipp_attr_bool(ipp_enc_t *e, const char *name, int val);
void ipp_attr_range(ipp_enc_t *e, const char *name, int32_t lo, int32_t hi);
void ipp_attr_res(ipp_enc_t *e, const char *name, int32_t x, int32_t y);

// Multi-value attributes. A second and later value repeats the value tag with
// an empty name; these helpers emit the whole set from one array.
void ipp_attr_strs(ipp_enc_t *e, uint8_t tag, const char *name,
                   const char *const *vals, size_t n);
void ipp_attr_ints(ipp_enc_t *e, uint8_t tag, const char *name,
                   const int32_t *vals, size_t n);

// Adds one more value to the attribute just written, for the cases where the
// values do not come from a single array.
void ipp_addl_str(ipp_enc_t *e, uint8_t tag, const char *val);
void ipp_addl_int(ipp_enc_t *e, uint8_t tag, int32_t val);

static inline int ipp_enc_ok(const ipp_enc_t *e) { return !e->overflow; }
static inline size_t ipp_enc_len(const ipp_enc_t *e) { return e->len; }
