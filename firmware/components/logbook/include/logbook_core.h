// SPDX-License-Identifier: GPL-3.0-or-later
//
// The log's pure core (W-1136, docs/superpowers/specs/2026-11-08-log-design.md): reading one
// esp_log line, taking names out of it, and the record it's stored as. No writable statics
// and no allocation, because it runs on whichever task logged. tools/rip tests it on the host.
#pragma once
#include <stdint.h>
#include <stddef.h>

#define LB_TAG_MAX  15u
#define LB_TEXT_MAX 200u

typedef struct {
    uint32_t seq;
    uint16_t run;
    uint32_t ms;
    char     level;                 // 'S' a start, 'I', 'W', 'E'
    uint8_t  ram_only;              // kept in RAM, never saved to flash
    char     tag[LB_TAG_MAX + 1];
    char     text[LB_TEXT_MAX + 1];
} lb_line_t;

typedef struct { const char *network, *host; } lb_names_t;

typedef struct {
    char     reason[48];
    char     task[16];
    uint8_t  core, n;
    uint32_t pc[8];
} lb_crash_t;

// One esp_log v1 line ("E (123) tag: text\n") into *out: 1, or 0 if it isn't one. Leaves seq,
// run and ram_only 0. Tabs, returns and newlines in the text become spaces, so a line can't
// break the /api/log format.
int lb_parse(const char *s, lb_line_t *out);

// Replaces the saved network and host name wherever they stand as whole words, and, in lines
// tagged wifi, anything shaped like a MAC address. Grows in place, cut at LB_TEXT_MAX.
void lb_scrub(lb_line_t *l, const lb_names_t *n);

// A routine status poll: worth keeping in RAM, not worth pushing better lines out of flash.
int lb_is_chatter(const lb_line_t *l);

// The stored record, little-endian: 0xA5, level, flags, taglen, textlen(u16), run(u16),
// seq(u32), ms(u32), tag, text, crc8. Bytes used, 0 if it won't fit or isn't a whole valid
// record. A first byte of 0xFF is blank flash.
size_t lb_encode(const lb_line_t *l, uint8_t *buf, size_t cap);
size_t lb_decode(const uint8_t *buf, size_t avail, lb_line_t *out);

// One /api/log line, "<seq>\t<run>\t<ms>\t<level>\t<tag>\t<text>\n". Bytes used, 0 if it won't fit.
size_t lb_wire(const lb_line_t *l, char *buf, size_t cap);

// The line a crash leaves behind.
void lb_crash_text(const lb_crash_t *c, char *buf, size_t cap);
