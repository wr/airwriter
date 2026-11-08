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
    uint32_t ms;
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

#define LB_RING_BYTES   32768u
#define LB_SECTOR_BYTES 4096u
#define LB_SECTORS      64u
#define LB_ERASED_AHEAD 2u
#define LB_REC_MAX      (16u + LB_TAG_MAX + LB_TEXT_MAX + 1u)

// The RAM ring: encoded records, its header at the start of the memory it's given. Survives a
// restart if the memory does; a push cut off part way loses that one line and no others.
typedef struct lb_ring lb_ring_t;

lb_ring_t *lb_ring_attach(void *mem, size_t bytes, int *kept);
void     lb_ring_begin_run(lb_ring_t *r, uint32_t min_seq, uint16_t last_run);  // next seq = max(its own, min_seq); run = max(its own, last_run) + 1
void     lb_ring_push(lb_ring_t *r, lb_line_t *l);                              // sets l->seq and l->run
uint32_t lb_ring_newest(const lb_ring_t *r);
uint16_t lb_ring_run(const lb_ring_t *r);
void     lb_ring_clear(lb_ring_t *r);

// The flash, by callback so the host can fake it. Each returns 0 for done.
typedef struct {
    void *ctx;
    int (*read)(void *ctx, uint32_t off, void *buf, uint32_t n);
    int (*write)(void *ctx, uint32_t off, const void *buf, uint32_t n);
    int (*erase)(void *ctx, uint32_t sector);
} lb_flash_t;

// Append-only records in a ring of sectors. A sector starts with "AWL", a format byte and a
// u32 sector number, written with its first record. Public so the board can place it in PSRAM.
typedef struct {
    lb_flash_t f;
    uint32_t   sector_no[LB_SECTORS];       // 0 = blank or unreadable
    uint32_t   first_seq[LB_SECTORS];
    uint8_t    ready[LB_SECTORS];           // known to be erased
    uint32_t   head, head_off, last_seq;    // head_off 0: the head sector isn't started
    uint16_t   last_run;
    lb_line_t  line;                        // working space, so nothing large is on a stack
    uint8_t    buf[8u + LB_REC_MAX];
} lb_store_t;

void     lb_store_open(lb_store_t *s, const lb_flash_t *f);
int      lb_store_append(lb_store_t *s, const lb_line_t *l);   // 0 when there's no erased room
void     lb_store_prepare(lb_store_t *s);                      // erases until LB_ERASED_AHEAD sectors are ready
unsigned lb_save(lb_store_t *s, const lb_ring_t *r);           // lines saved; moves last_seq past each, skipping ram_only
int      lb_save_next(lb_store_t *s, const lb_ring_t *r);      // lb_save a line at a time: the next one in s->line, for lb_store_append; 0 when caught up
int      lb_has_urgent(const lb_store_t *s, const lb_ring_t *r);  // a warning or error newer than last_seq
void     lb_store_clear(lb_store_t *s);

// Flash and RAM as one list, oldest first, no repeats. Safe against the ring moving under it.
typedef struct {
    const lb_ring_t  *r;
    const lb_store_t *s;                    // NULL: RAM only
    uint32_t  last;                         // the last seq handed out
    uint32_t  r_pos, r_gen;
    uint32_t  f_no, f_idx, f_off;           // the sector being read; f_no 0: none yet
    uint8_t   r_have, f_have;
    lb_line_t r_line, f_line;
    uint8_t   buf[LB_REC_MAX];
} lb_reader_t;

void lb_read_begin(lb_reader_t *rd, const lb_ring_t *r, const lb_store_t *s, uint32_t after);
int  lb_read_next(lb_reader_t *rd, lb_line_t *out);            // 1 and the next line, 0 at the end
