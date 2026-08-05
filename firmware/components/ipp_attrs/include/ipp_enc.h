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

// Response header: the version, status code, and request id. Both the version
// and the request id are echoed from the request -- a response carrying either
// one wrong is discarded by the client without an error, which is a miserable
// thing to debug. RFC 8011 section 4.1.8 for the version rule.
void ipp_resp_begin(ipp_enc_t *e, uint16_t version, uint16_t status,
                    uint32_t request_id);

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

// Collections (RFC 8010 section 3.1.6), which is how media-col carries a page
// size and its margins as one value. A member value is written with the
// ipp_addl_* helpers, since members carry no name of their own -- the
// preceding memberAttrName supplies it:
//
//   ipp_col_begin(e, "media-col-default");
//   ipp_col_member(e, "media-type");
//   ipp_addl_str(e, IPP_TAG_KEYWORD, "stationery");
//   ipp_col_end(e);
//
// Pass NULL as the name to open an additional collection value on the
// attribute just written, or a nested one after ipp_col_member.
void ipp_col_begin(ipp_enc_t *e, const char *name);
void ipp_col_member(ipp_enc_t *e, const char *member_name);
void ipp_col_end(ipp_enc_t *e);

// Adds one more value to the attribute just written, for the cases where the
// values do not come from a single array.
void ipp_addl_str(ipp_enc_t *e, uint8_t tag, const char *val);
void ipp_addl_int(ipp_enc_t *e, uint8_t tag, int32_t val);

static inline int ipp_enc_ok(const ipp_enc_t *e) { return !e->overflow; }
static inline size_t ipp_enc_len(const ipp_enc_t *e) { return e->len; }
