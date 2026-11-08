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

size_t lb_decode(const uint8_t *buf, size_t avail, lb_line_t *out)
{
    if (avail < REC_HEAD + 1 || buf[0] != REC_MAGIC || !level_ok((char)buf[1])) return 0;
    size_t tl = buf[3], xl = get16(buf + 4);
    if (tl > LB_TAG_MAX || xl > LB_TEXT_MAX) return 0;
    size_t n = REC_HEAD + tl + xl + 1;
    if (n > avail || crc8(buf, n - 1) != buf[n - 1]) return 0;
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
