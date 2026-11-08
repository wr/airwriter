// SPDX-License-Identifier: GPL-3.0-or-later
#include "logbook_core.h"
#include <stdio.h>
#include <string.h>

enum { REC_MAGIC = 0xA5, REC_HEAD = 16, FLAG_RAM_ONLY = 1, NAME_MIN = 3, MAC_LEN = 17 };

static int is_alnum(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static int is_hex(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static int level_ok(char c)
{
    return c == 'S' || c == 'I' || c == 'W' || c == 'E';
}

int lb_parse(const char *s, lb_line_t *out)
{
    memset(out, 0, sizeof *out);
    if ((s[0] != 'I' && s[0] != 'W' && s[0] != 'E') || s[1] != ' ' || s[2] != '(') return 0;
    out->level = s[0];

    const char *p = s + 3;
    if (*p < '0' || *p > '9') return 0;
    uint32_t ms = 0;
    for (; *p >= '0' && *p <= '9'; p++) ms = ms * 10 + (uint32_t)(*p - '0');
    if (p[0] != ')' || p[1] != ' ') return 0;
    out->ms = ms;
    p += 2;

    const char *colon = strchr(p, ':');
    if (!colon || colon == p) return 0;
    size_t tag = (size_t)(colon - p);
    if (tag > LB_TAG_MAX) tag = LB_TAG_MAX;
    memcpy(out->tag, p, tag);
    out->tag[tag] = 0;

    p = colon + 1;
    if (*p == ' ') p++;                     // the Wi-Fi library leaves no space after the colon

    size_t n = 0;
    for (; *p && n < LB_TEXT_MAX; p++) out->text[n++] = (unsigned char)*p < 0x20 ? ' ' : *p;
    while (n && out->text[n - 1] == ' ' && p[0] == 0) n--;
    out->text[n] = 0;
    return 1;
}

static int starts_a_line(const char *s)
{
    return (s[0] == 'I' || s[0] == 'W' || s[0] == 'E') && s[1] == ' ' && s[2] == '('
        && s[3] >= '0' && s[3] <= '9';
}

int lb_feed(lb_asm_t *a, const char *s, const void *who, lb_line_t *out)
{
    if (a->open && a->who == who && !starts_a_line(s)) {
        size_t n = strlen(a->line.text), have = strlen(s);
        int ended = have && s[have - 1] == '\n';
        for (const char *p = s; *p && n < LB_TEXT_MAX; p++)
            a->line.text[n++] = (unsigned char)*p < 0x20 ? ' ' : *p;
        a->line.text[n] = 0;
        if (!ended) return 0;
        a->open = 0;
        while (n && a->line.text[n - 1] == ' ') n--;
        a->line.text[n] = 0;
        if (!n) return 0;
        *out = a->line;
        return 1;
    }
    if (!lb_parse(s, out)) return 0;
    if (out->text[0]) return 1;
    a->line = *out;                         // only the front of a line so far
    a->who = who;
    a->open = 1;
    return 0;
}

// Swaps text[at, at+len) for `with`, in place. What grows past the end falls off.
static size_t splice(char *text, size_t at, size_t len, const char *with)
{
    size_t have = strlen(text), add = strlen(with);
    size_t tail = have - at - len;
    size_t to = at + add;
    if (to > LB_TEXT_MAX) { add = LB_TEXT_MAX - at; to = LB_TEXT_MAX; tail = 0; }
    if (to + tail > LB_TEXT_MAX) tail = LB_TEXT_MAX - to;
    memmove(text + to, text + at + len, tail);
    memcpy(text + at, with, add);
    text[to + tail] = 0;
    return to;
}

static void scrub_name(char *text, const char *name, const char *with)
{
    if (!name) return;
    size_t nl = strlen(name);
    if (nl < NAME_MIN) return;

    size_t i = 0;
    while (text[i]) {
        size_t have = strlen(text + i);
        if (have >= nl && !memcmp(text + i, name, nl)
            && (i == 0 || !is_alnum(text[i - 1])) && !is_alnum(text[i + nl])) {
            i = splice(text, i, nl, with);
        } else {
            i++;
        }
    }

    // A line cut at the limit can end on the front of a name.
    size_t have = strlen(text);
    if (have < LB_TEXT_MAX) return;
    for (size_t k = nl - 1; k >= NAME_MIN; k--) {
        size_t at = have - k;
        if (!memcmp(text + at, name, k) && (at == 0 || !is_alnum(text[at - 1]))) {
            splice(text, at, k, with);
            return;
        }
    }
}

static int is_mac(const char *s)
{
    for (int i = 0; i < MAC_LEN; i++) {
        if (i % 3 == 2) { if (s[i] != ':') return 0; }
        else if (!is_hex(s[i])) return 0;
    }
    return !is_alnum(s[MAC_LEN]) && s[MAC_LEN] != ':';
}

void lb_scrub(lb_line_t *l, const lb_names_t *n)
{
    if (n) {
        scrub_name(l->text, n->network, "<network>");
        scrub_name(l->text, n->host, "<host>");
    }
    if (strcmp(l->tag, "wifi")) return;
    size_t i = 0;
    while (l->text[i]) {
        if (strlen(l->text + i) >= MAC_LEN && is_mac(l->text + i)
            && (i == 0 || (!is_alnum(l->text[i - 1]) && l->text[i - 1] != ':'))) {
            i = splice(l->text, i, MAC_LEN, "<mac>");
        } else {
            i++;
        }
    }
}

int lb_is_chatter(const lb_line_t *l)
{
    if (strcmp(l->tag, "ipp")) return 0;
    return !strncmp(l->text, "op 0x000b", 9) || !strncmp(l->text, "op 0x0009", 9)
        || !strncmp(l->text, "op 0x000a", 9) || !strncmp(l->text, "  attributes of ", 16);
}

static uint8_t crc8(const uint8_t *p, size_t n)
{
    uint8_t c = 0;
    while (n--) {
        c ^= *p++;
        for (int i = 0; i < 8; i++) c = (uint8_t)((c & 0x80) ? (c << 1) ^ 0x07 : c << 1);
    }
    return c;
}

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { put16(p, (uint16_t)v); put16(p + 2, (uint16_t)(v >> 16)); }
static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t get32(const uint8_t *p) { return get16(p) | (uint32_t)get16(p + 2) << 16; }

size_t lb_encode(const lb_line_t *l, uint8_t *buf, size_t cap)
{
    size_t tl = strnlen(l->tag, LB_TAG_MAX), xl = strnlen(l->text, LB_TEXT_MAX);
    size_t n = REC_HEAD + tl + xl + 1;
    if (n > cap) return 0;
    buf[0] = REC_MAGIC;
    buf[1] = (uint8_t)l->level;
    buf[2] = l->ram_only ? FLAG_RAM_ONLY : 0;
    buf[3] = (uint8_t)tl;
    put16(buf + 4, (uint16_t)xl);
    put16(buf + 6, l->run);
    put32(buf + 8, l->seq);
    put32(buf + 12, l->ms);
    memcpy(buf + REC_HEAD, l->tag, tl);
    memcpy(buf + REC_HEAD + tl, l->text, xl);
    buf[n - 1] = crc8(buf, n - 1);
    return n;
}

// A whole, valid record's length, or 0.
static size_t rec_check(const uint8_t *buf, size_t avail)
{
    if (avail < REC_HEAD + 1 || buf[0] != REC_MAGIC || !level_ok((char)buf[1])) return 0;
    size_t tl = buf[3], xl = get16(buf + 4);
    if (tl > LB_TAG_MAX || xl > LB_TEXT_MAX) return 0;
    size_t n = REC_HEAD + tl + xl + 1;
    if (n > avail || crc8(buf, n - 1) != buf[n - 1]) return 0;
    return n;
}

size_t lb_decode(const uint8_t *buf, size_t avail, lb_line_t *out)
{
    size_t n = rec_check(buf, avail);
    if (!n) return 0;
    size_t tl = buf[3], xl = get16(buf + 4);
    out->level = (char)buf[1];
    out->ram_only = (buf[2] & FLAG_RAM_ONLY) != 0;
    out->run = get16(buf + 6);
    out->seq = get32(buf + 8);
    out->ms = get32(buf + 12);
    memcpy(out->tag, buf + REC_HEAD, tl);
    out->tag[tl] = 0;
    memcpy(out->text, buf + REC_HEAD + tl, xl);
    out->text[xl] = 0;
    return n;
}

size_t lb_wire(const lb_line_t *l, char *buf, size_t cap)
{
    int n = snprintf(buf, cap, "%lu\t%u\t%lu\t%c\t%s\t%s\n", (unsigned long)l->seq, l->run,
                     (unsigned long)l->ms, l->level, l->tag, l->text);
    if (n < 0 || (size_t)n >= cap) {
        if (cap) buf[0] = 0;
        return 0;
    }
    return (size_t)n;
}

void lb_crash_text(const lb_crash_t *c, char *buf, size_t cap)
{
    int n = snprintf(buf, cap, "crashed in task %s on core %u (%s)", c->task, c->core, c->reason);
    if (n < 0 || (size_t)n >= cap || !c->n) return;
    size_t at = (size_t)n;
    for (int i = 0; i < c->n && i < 8; i++) {
        int w = snprintf(buf + at, cap - at, "%s0x%08lx", i ? " " : "; backtrace ", (unsigned long)c->pc[i]);
        if (w < 0 || (size_t)w >= cap - at) { buf[at] = 0; return; }
        at += (size_t)w;
    }
}

// ---- the RAM ring ----

enum { RING_WRAP = 0x00 };                  // in place of a record: the next one is at offset 0
#define RING_MAGIC 0x4C4F4731u

struct lb_ring {
    uint32_t magic, cap;
    uint32_t tail, head;                    // equal: empty. Never run into each other full.
    uint32_t next_seq;
    uint32_t gen;                           // moves when a record is dropped, for readers
    uint16_t run;
    uint16_t pad;
    uint8_t  data[];
};

static size_t rec_len(const uint8_t *p) { return REC_HEAD + (size_t)p[3] + get16(p + 4) + 1; }
static uint32_t rec_seq(const uint8_t *p) { return get32(p + 8); }

static void ring_init(lb_ring_t *r, uint32_t cap)
{
    r->magic = RING_MAGIC;
    r->cap = cap;
    r->tail = r->head = 0;
    r->next_seq = 1;
    r->gen = 0;
    r->run = 0;
    r->pad = 0;
}

// Goes round the end if a wrap marker is what's at pos.
static uint32_t ring_norm(const lb_ring_t *r, uint32_t pos)
{
    return pos != r->head && r->data[pos] == RING_WRAP ? 0 : pos;
}

lb_ring_t *lb_ring_attach(void *mem, size_t bytes, int *kept)
{
    lb_ring_t *r = mem;
    uint32_t cap = (uint32_t)(bytes - sizeof *r);
    *kept = 0;
    if (r->magic != RING_MAGIC || r->cap != cap || r->tail >= cap || r->head >= cap) {
        ring_init(r, cap);
        return r;
    }

    // Trust only what reads back whole: the end moves to the first record that doesn't.
    uint32_t pos = r->tail, last_seq = 0;
    uint16_t last_run = 0;
    int wrapped = 0;
    while (pos != r->head) {
        if (r->data[pos] == RING_WRAP) {
            if (wrapped) break;
            wrapped = 1;
            pos = 0;
            continue;
        }
        size_t n = rec_check(r->data + pos, cap - pos);
        if (!n || pos + n >= cap) break;
        last_seq = rec_seq(r->data + pos);
        last_run = get16(r->data + pos + 6);
        pos += (uint32_t)n;
    }
    if (pos != r->head) {
        if (r->data[r->tail] != RING_WRAP && !rec_check(r->data + r->tail, cap - r->tail)) {
            ring_init(r, cap);              // not even the oldest reads back
            return r;
        }
        r->head = pos;
    }
    if (last_seq) {
        r->next_seq = last_seq + 1;
        r->run = last_run;
    }
    r->gen++;
    *kept = 1;
    return r;
}

void lb_ring_begin_run(lb_ring_t *r, uint32_t min_seq, uint16_t last_run)
{
    if (r->next_seq < min_seq) r->next_seq = min_seq;
    r->run = (uint16_t)((r->run > last_run ? r->run : last_run) + 1);
}

static void ring_drop_oldest(lb_ring_t *r)
{
    uint32_t t = r->tail + (uint32_t)rec_len(r->data + r->tail);
    r->gen++;
    r->tail = ring_norm(r, t);
}

void lb_ring_push(lb_ring_t *r, lb_line_t *l)
{
    l->seq = r->next_seq;
    l->run = r->run;
    size_t n = REC_HEAD + strnlen(l->tag, LB_TAG_MAX) + strnlen(l->text, LB_TEXT_MAX) + 1;

    for (;;) {
        if (r->tail > r->head) {            // live data runs round the end
            if (r->head + n < r->tail) break;
            ring_drop_oldest(r);
        } else if (r->head + n < r->cap) {
            break;
        } else if (r->tail == r->head) {    // empty, with no room before the end
            r->tail = r->head = 0;
            break;
        } else if (r->tail > n) {           // room at the front
            r->data[r->head] = RING_WRAP;
            r->head = 0;
            break;
        } else {
            ring_drop_oldest(r);
        }
    }

    lb_encode(l, r->data + r->head, r->cap - r->head);
    r->head += (uint32_t)n;
    r->next_seq++;
}

uint32_t lb_ring_newest(const lb_ring_t *r) { return r->next_seq - 1; }
uint16_t lb_ring_run(const lb_ring_t *r) { return r->run; }

void lb_ring_clear(lb_ring_t *r)
{
    uint32_t gen = r->gen;
    uint16_t run = r->run;
    ring_init(r, r->cap);
    r->gen = gen + 1;
    r->run = run;
}

// ---- the flash ring ----

enum { SEC_HEAD = 8, SEC_FORMAT = 1 };

static uint32_t sec_off(uint32_t sector) { return sector * LB_SECTOR_BYTES; }

static int is_blank(lb_store_t *s, uint32_t sector, uint32_t from)
{
    while (from < LB_SECTOR_BYTES) {
        uint32_t n = LB_SECTOR_BYTES - from;
        if (n > sizeof s->buf) n = sizeof s->buf;
        if (s->f.read(s->f.ctx, sec_off(sector) + from, s->buf, n)) return 0;
        for (uint32_t i = 0; i < n; i++) if (s->buf[i] != 0xFF) return 0;
        from += n;
    }
    return 1;
}

// A sector's number, with its first record's seq in *first; 0 if it holds no readable lines.
static uint32_t sec_read_header(lb_store_t *s, uint32_t sector, uint32_t *first)
{
    if (s->f.read(s->f.ctx, sec_off(sector), s->buf, sizeof s->buf)) return 0;
    uint32_t no = get32(s->buf + 4);
    if (memcmp(s->buf, "AWL", 3) || s->buf[3] != SEC_FORMAT || no == 0 || no == 0xFFFFFFFFu) return 0;
    if (!rec_check(s->buf + SEC_HEAD, sizeof s->buf - SEC_HEAD)) return 0;
    *first = rec_seq(s->buf + SEC_HEAD);
    return no;
}

void lb_store_open(lb_store_t *s, const lb_flash_t *f)
{
    memset(s, 0, sizeof *s);
    s->f = *f;

    uint32_t best = 0;
    for (uint32_t i = 0; i < LB_SECTORS; i++) {
        s->sector_no[i] = sec_read_header(s, i, &s->first_seq[i]);
        if (s->sector_no[i] > best) { best = s->sector_no[i]; s->head = i; }
    }
    if (!best) return;

    // Find the end of the newest sector.
    uint32_t off = SEC_HEAD;
    for (;;) {
        uint32_t n = LB_SECTOR_BYTES - off;
        if (n > LB_REC_MAX) n = LB_REC_MAX;
        size_t used = 0;
        if (n && !s->f.read(s->f.ctx, sec_off(s->head) + off, s->buf, n)) used = rec_check(s->buf, n);
        if (!used) break;
        s->last_seq = rec_seq(s->buf);
        s->last_run = get16(s->buf + 6);
        off += (uint32_t)used;
    }
    // Anything but blank after the last whole record is a write that was cut off: leave the
    // rest of this sector alone.
    s->head_off = is_blank(s, s->head, off) ? off : LB_SECTOR_BYTES;
}

void lb_store_prepare(lb_store_t *s)
{
    for (uint32_t i = 0; i <= LB_ERASED_AHEAD; i++) {
        if (i == 0 && s->head_off != 0) continue;
        uint32_t x = (s->head + i) % LB_SECTORS;
        if (s->ready[x]) continue;
        if (s->sector_no[x] == 0 && is_blank(s, x, 0)) {
            s->ready[x] = 1;
        } else if (s->f.erase(s->f.ctx, x) == 0) {
            s->sector_no[x] = 0;
            s->first_seq[x] = 0;
            s->ready[x] = 1;
        }
    }
}

int lb_store_append(lb_store_t *s, const lb_line_t *l)
{
    size_t n = lb_encode(l, s->buf + SEC_HEAD, LB_REC_MAX);
    if (!n) return 0;

    if (s->head_off && s->head_off + n > LB_SECTOR_BYTES) {
        uint32_t next = (s->head + 1) % LB_SECTORS;
        if (!s->ready[next]) return 0;
        s->head = next;
        s->head_off = 0;
    }
    uint32_t off = s->head_off;
    const uint8_t *from = s->buf + SEC_HEAD;
    uint32_t len = (uint32_t)n, no = 0;
    if (off == 0) {
        if (!s->ready[s->head]) return 0;
        for (uint32_t i = 0; i < LB_SECTORS; i++) if (s->sector_no[i] > no) no = s->sector_no[i];
        no++;
        memcpy(s->buf, "AWL", 3);
        s->buf[3] = SEC_FORMAT;
        put32(s->buf + 4, no);
        from = s->buf;
        len += SEC_HEAD;
    }
    if (s->f.write(s->f.ctx, sec_off(s->head) + off, from, len)) {
        s->ready[s->head] = 0;              // what's there now is unknown: move on from it
        s->head_off = LB_SECTOR_BYTES;
        return 0;
    }
    if (off == 0) {
        s->sector_no[s->head] = no;
        s->first_seq[s->head] = l->seq;
        s->ready[s->head] = 0;
    }
    s->head_off = off + len;
    s->last_seq = l->seq;
    s->last_run = l->run;
    return 1;
}

// The first record in the ring newer than seq, or the ring's head.
static uint32_t ring_seek(const lb_ring_t *r, uint32_t seq)
{
    uint32_t pos = ring_norm(r, r->tail);
    while (pos != r->head && rec_seq(r->data + pos) <= seq)
        pos = ring_norm(r, pos + (uint32_t)rec_len(r->data + pos));
    return pos;
}

int lb_save_next(lb_store_t *s, const lb_ring_t *r)
{
    uint32_t pos = ring_seek(r, s->last_seq);
    while (pos != r->head) {
        size_t n = lb_decode(r->data + pos, r->cap - pos, &s->line);
        if (!n) return 0;
        if (!s->line.ram_only) return 1;
        s->last_seq = s->line.seq;
        pos = ring_norm(r, pos + (uint32_t)n);
    }
    return 0;
}

unsigned lb_save(lb_store_t *s, const lb_ring_t *r)
{
    unsigned saved = 0;
    while (lb_save_next(s, r) && lb_store_append(s, &s->line)) saved++;
    return saved;
}

int lb_has_urgent(const lb_store_t *s, const lb_ring_t *r)
{
    uint32_t pos = ring_seek(r, s->last_seq);
    while (pos != r->head) {
        const uint8_t *p = r->data + pos;
        if ((p[1] == 'W' || p[1] == 'E') && !(p[2] & FLAG_RAM_ONLY)) return 1;
        pos = ring_norm(r, pos + (uint32_t)rec_len(p));
    }
    return 0;
}

void lb_store_clear(lb_store_t *s)
{
    for (uint32_t i = 0; i < LB_SECTORS; i++) {
        s->sector_no[i] = 0;
        s->first_seq[i] = 0;
        if (is_blank(s, i, 0) || s->f.erase(s->f.ctx, i) == 0) s->ready[i] = 1;
        else s->ready[i] = 0;
    }
    s->head = s->head_off = s->last_seq = 0;
    s->last_run = 0;
}

// ---- reading both ----

void lb_read_begin(lb_reader_t *rd, const lb_ring_t *r, const lb_store_t *s, uint32_t after)
{
    rd->r = r;
    rd->s = s;
    rd->last = after;
    rd->r_pos = ring_seek(r, after);
    rd->r_gen = r->gen;
    rd->f_no = rd->f_idx = rd->f_off = 0;
    rd->r_have = rd->f_have = 0;
}

static void r_fill(lb_reader_t *rd)
{
    const lb_ring_t *r = rd->r;
    if (rd->r_have) return;
    if (rd->r_gen != r->gen) {              // records have gone: find our place again
        rd->r_pos = ring_seek(r, rd->last);
        rd->r_gen = r->gen;
    }
    uint32_t pos = ring_norm(r, rd->r_pos);
    while (pos != r->head && rec_seq(r->data + pos) <= rd->last)
        pos = ring_norm(r, pos + (uint32_t)rec_len(r->data + pos));
    if (pos != r->head) {
        size_t n = lb_decode(r->data + pos, r->cap - pos, &rd->r_line);
        if (n) {
            rd->r_have = 1;
            pos = ring_norm(r, pos + (uint32_t)n);
        }
    }
    rd->r_pos = pos;
}

// Moves to the next sector worth reading. 0 when there are no more.
static int f_next_sector(lb_reader_t *rd)
{
    const lb_store_t *s = rd->s;
    for (;;) {
        uint32_t best = 0, idx = 0;
        for (uint32_t i = 0; i < LB_SECTORS; i++)
            if (s->sector_no[i] > rd->f_no && (!best || s->sector_no[i] < best)) {
                best = s->sector_no[i];
                idx = i;
            }
        if (!best) { rd->f_no = 0xFFFFFFFFu; return 0; }
        rd->f_no = best;
        rd->f_idx = idx;
        rd->f_off = SEC_HEAD;

        // A sector can be skipped when the one after it starts at or before where we want.
        int skip = 0;
        for (uint32_t i = 0; i < LB_SECTORS; i++)
            if (s->sector_no[i] == best + 1 && s->first_seq[i] <= rd->last + 1) skip = 1;
        if (!skip) return 1;
    }
}

static void f_fill(lb_reader_t *rd)
{
    const lb_store_t *s = rd->s;
    if (!s || rd->f_have || rd->f_no == 0xFFFFFFFFu) return;
    if (!rd->f_no && !f_next_sector(rd)) return;
    for (;;) {
        // The sector may have been erased under us: the number says so.
        size_t used = 0;
        uint32_t n = LB_SECTOR_BYTES - rd->f_off;
        if (n > sizeof rd->buf) n = sizeof rd->buf;
        if (s->sector_no[rd->f_idx] == rd->f_no && n
            && !s->f.read(s->f.ctx, sec_off(rd->f_idx) + rd->f_off, rd->buf, n))
            used = lb_decode(rd->buf, n, &rd->f_line);
        if (!used) {
            if (!f_next_sector(rd)) return;
            continue;
        }
        rd->f_off += (uint32_t)used;
        if (rd->f_line.seq > rd->last) { rd->f_have = 1; return; }
    }
}

int lb_read_next(lb_reader_t *rd, lb_line_t *out)
{
    r_fill(rd);
    f_fill(rd);
    if (rd->r_have && rd->f_have && rd->r_line.seq == rd->f_line.seq) rd->f_have = 0;
    if (!rd->r_have && !rd->f_have) return 0;

    int use_ram = rd->r_have && (!rd->f_have || rd->r_line.seq < rd->f_line.seq);
    *out = use_ram ? rd->r_line : rd->f_line;
    if (use_ram) rd->r_have = 0; else rd->f_have = 0;
    rd->last = out->seq;
    return 1;
}
