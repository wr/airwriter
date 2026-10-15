// SPDX-License-Identifier: GPL-3.0-or-later
//
// IPP/1.1 wire constants. RFC 8010 section 3 for the encoding, RFC 8011
// section 5.2 for the enums, PWG 5100.14 (IPP Everywhere) for the rest.
//
// The encoding is: version(2) op-or-status(2) request-id(4), then a sequence
// of groups, each opened by a delimiter tag and holding attributes of the form
// value-tag(1) name-len(2) name value-len(2) value. A second value on the same
// attribute repeats with name-len 0. The whole thing closes with
// IPP_TAG_END, and any document data follows immediately after.
#pragma once
#include <stdint.h>

#define IPP_VERSION_1_1 0x0101

// Delimiter tags.
#define IPP_TAG_OPERATION   0x01
#define IPP_TAG_JOB         0x02
#define IPP_TAG_END         0x03
#define IPP_TAG_PRINTER     0x04
#define IPP_TAG_UNSUPPORTED_GROUP 0x05

// Value tags.
#define IPP_TAG_UNSUPPORTED 0x10
#define IPP_TAG_UNKNOWN     0x12
#define IPP_TAG_NOVALUE     0x13
#define IPP_TAG_INTEGER     0x21
#define IPP_TAG_BOOLEAN     0x22
#define IPP_TAG_ENUM        0x23
#define IPP_TAG_STRING      0x30   // octetString
#define IPP_TAG_DATE        0x31
#define IPP_TAG_RESOLUTION  0x32
#define IPP_TAG_RANGE       0x33
#define IPP_TAG_BEGIN_COLLECTION 0x34
#define IPP_TAG_END_COLLECTION   0x37
#define IPP_TAG_TEXT        0x41   // textWithoutLanguage
#define IPP_TAG_NAME        0x42   // nameWithoutLanguage
#define IPP_TAG_KEYWORD     0x44
#define IPP_TAG_URI         0x45
#define IPP_TAG_CHARSET     0x47
#define IPP_TAG_LANGUAGE    0x48
#define IPP_TAG_MIMETYPE    0x49
#define IPP_TAG_MEMBERNAME  0x4A

#define IPP_RES_DPI         3      // units code inside a resolution value

// Operations. Section 3.4 of SPEC.md lists the set iOS needs.
#define IPP_OP_PRINT_JOB            0x0002
#define IPP_OP_VALIDATE_JOB         0x0004
#define IPP_OP_CANCEL_JOB           0x0008
#define IPP_OP_GET_JOB_ATTRIBUTES   0x0009
#define IPP_OP_GET_JOBS             0x000A
#define IPP_OP_GET_PRINTER_ATTRIBUTES 0x000B

// Status codes.
#define IPP_STATUS_OK                       0x0000
#define IPP_STATUS_OK_IGNORED_ATTRS         0x0001
#define IPP_STATUS_ERR_BAD_REQUEST          0x0400
#define IPP_STATUS_ERR_NOT_POSSIBLE         0x0404
#define IPP_STATUS_ERR_NOT_FOUND            0x0406
#define IPP_STATUS_ERR_ENTITY_TOO_LARGE     0x0408
#define IPP_STATUS_ERR_FORMAT_NOT_SUPPORTED 0x040A
#define IPP_STATUS_ERR_ATTRS_NOT_SUPPORTED  0x040B
#define IPP_STATUS_ERR_INTERNAL             0x0500
#define IPP_STATUS_ERR_OP_NOT_SUPPORTED     0x0501
#define IPP_STATUS_ERR_NOT_ACCEPTING_JOBS   0x0506
#define IPP_STATUS_ERR_BUSY                 0x0507

// printer-state (RFC 8011 5.4.11) and job-state (5.3.7) enums.
#define IPP_PSTATE_IDLE       3
#define IPP_PSTATE_PROCESSING 4
#define IPP_PSTATE_STOPPED    5

#define IPP_JSTATE_PENDING    3
#define IPP_JSTATE_HELD       4
#define IPP_JSTATE_PROCESSING 5
#define IPP_JSTATE_STOPPED    6
#define IPP_JSTATE_CANCELED   7
#define IPP_JSTATE_ABORTED    8
#define IPP_JSTATE_COMPLETED  9
