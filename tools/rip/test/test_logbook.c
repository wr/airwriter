// SPDX-License-Identifier: GPL-3.0-or-later
// The log's pure core: reading an esp_log line, taking names out of it, and the two rings.
#include "test_util.h"
#include "logbook_core.h"

static lb_line_t mk(uint32_t seq, char level, const char *tag, const char *text)
{
    lb_line_t l;
    memset(&l, 0, sizeof l);
    l.seq = seq;
    l.level = level;
    snprintf(l.tag, sizeof l.tag, "%s", tag);
    snprintf(l.text, sizeof l.text, "%s", text);
    return l;
}

static void test_parse(void)
{
    lb_line_t l;
    CHECK_EQ_INT(lb_parse("E (12345) airwriter: job failed\n", &l), 1);
    CHECK_EQ_INT(l.level, 'E');
    CHECK_EQ_INT(l.ms, 12345);
    CHECK(!strcmp(l.tag, "airwriter"));
    CHECK(!strcmp(l.text, "job failed"));

    CHECK_EQ_INT(lb_parse("W (7) ipp: slow\n", &l), 1);
    CHECK_EQ_INT(l.level, 'W');

    CHECK_EQ_INT(lb_parse("I (9) wifi:state: init -> auth\n", &l), 1);   // the Wi-Fi library's shape
    CHECK(!strcmp(l.tag, "wifi"));
    CHECK(!strcmp(l.text, "state: init -> auth"));

    CHECK_EQ_INT(lb_parse("not a log line\n", &l), 0);
    CHECK_EQ_INT(lb_parse("", &l), 0);
    CHECK_EQ_INT(lb_parse("X (1) tag: unknown level\n", &l), 0);
    CHECK_EQ_INT(lb_parse("I (abc) tag: no number\n", &l), 0);
    CHECK_EQ_INT(lb_parse("I (12", &l), 0);
    CHECK_EQ_INT(lb_parse("I (12) notagcolon\n", &l), 0);

    CHECK_EQ_INT(lb_parse("W (1) x: a\tb\rc\n", &l), 1);
    CHECK(!strcmp(l.text, "a b c"));                                     // never breaks a wire line

    char mid[64];
    snprintf(mid, sizeof mid, "I (1) x: first\nsecond\n");
    CHECK_EQ_INT(lb_parse(mid, &l), 1);
    CHECK(!strcmp(l.text, "first second"));

    char big[LB_TEXT_MAX + 100];
    int n = snprintf(big, sizeof big, "I (1) x: ");
    memset(big + n, 'y', LB_TEXT_MAX + 50);
    big[n + LB_TEXT_MAX + 50] = 0;
    CHECK_EQ_INT(lb_parse(big, &l), 1);
    CHECK_EQ_INT(strlen(l.text), LB_TEXT_MAX);

    char tag[64];
    snprintf(tag, sizeof tag, "I (1) a_very_long_tag_name_indeed: m\n");
    CHECK_EQ_INT(lb_parse(tag, &l), 1);
    CHECK_EQ_INT(strlen(l.tag), LB_TAG_MAX);
    CHECK(!strcmp(l.text, "m"));
}

static void test_feed_in_pieces(void)
{
    // The Wi-Fi library sends its "I (9) wifi:" and then its message, as two calls.
    lb_asm_t a;
    memset(&a, 0, sizeof a);
    lb_line_t l;
    int one = 1, two = 2;

    CHECK_EQ_INT(lb_feed(&a, "I (9) wifi:", &one, &l), 0);
    CHECK_EQ_INT(lb_feed(&a, "state: init -> auth (b0)\n", &one, &l), 1);
    CHECK_EQ_INT(l.level, 'I');
    CHECK_EQ_INT(l.ms, 9);
    CHECK(!strcmp(l.tag, "wifi"));
    CHECK(!strcmp(l.text, "state: init -> auth (b0)"));

    CHECK_EQ_INT(lb_feed(&a, "E (5) job: whole\n", &one, &l), 1);      // a whole line, as ever
    CHECK(!strcmp(l.text, "whole"));

    CHECK_EQ_INT(lb_feed(&a, "no line is waiting\n", &one, &l), 0);    // a piece of nothing

    // Another task's whole line in between doesn't disturb the waiting one.
    CHECK_EQ_INT(lb_feed(&a, "W (20) wifi:", &one, &l), 0);
    CHECK_EQ_INT(lb_feed(&a, "I (21) ipp: other\n", &two, &l), 1);
    CHECK(!strcmp(l.tag, "ipp"));
    CHECK_EQ_INT(lb_feed(&a, "the rest\n", &one, &l), 1);
    CHECK_EQ_INT(l.level, 'W');
    CHECK_EQ_INT(l.ms, 20);
    CHECK(!strcmp(l.text, "the rest"));

    // Another task's piece isn't taken for this one's.
    CHECK_EQ_INT(lb_feed(&a, "I (30) wifi:", &one, &l), 0);
    CHECK_EQ_INT(lb_feed(&a, "stray\n", &two, &l), 0);
    CHECK_EQ_INT(lb_feed(&a, "mine\n", &one, &l), 1);
    CHECK(!strcmp(l.text, "mine"));

    // The message can come in more than one piece.
    CHECK_EQ_INT(lb_feed(&a, "I (40) wifi:", &one, &l), 0);
    CHECK_EQ_INT(lb_feed(&a, "a\tb", &one, &l), 0);
    CHECK_EQ_INT(lb_feed(&a, " c\n", &one, &l), 1);
    CHECK(!strcmp(l.text, "a b c"));

    // An unfinished one gives way to the task's next line.
    CHECK_EQ_INT(lb_feed(&a, "I (50) wifi:", &one, &l), 0);
    CHECK_EQ_INT(lb_feed(&a, "W (51) wifi:", &one, &l), 0);
    CHECK_EQ_INT(lb_feed(&a, "x\n", &one, &l), 1);
    CHECK_EQ_INT(l.level, 'W');
    CHECK_EQ_INT(l.ms, 51);

    // A long message stays within the limit.
    CHECK_EQ_INT(lb_feed(&a, "I (60) wifi:", &one, &l), 0);
    char big[LB_TEXT_MAX + 60];
    memset(big, 'z', sizeof big - 2);
    big[sizeof big - 2] = '\n';
    big[sizeof big - 1] = 0;
    CHECK_EQ_INT(lb_feed(&a, big, &one, &l), 1);
    CHECK_EQ_INT(strlen(l.text), LB_TEXT_MAX);
}

static void test_scrub_names(void)
{
    lb_names_t n = { "Orchard", "imagewriter-2434" };
    lb_line_t l = mk(0, 'I', "wifi",
                     "connected with Orchard, aid = 1, channel 6, BW20, bssid = aa:bb:cc:dd:ee:ff");
    lb_scrub(&l, &n);
    CHECK(!strcmp(l.text, "connected with <network>, aid = 1, channel 6, BW20, bssid = <mac>"));

    l = mk(0, 'I', "wifi", "station: 11:22:33:44:55:66 join, AID=1");
    lb_scrub(&l, &n);
    CHECK(!strcmp(l.text, "station: <mac> join, AID=1"));

    l = mk(0, 'I', "airwriter", "settings at http://imagewriter-2434.local/");
    lb_scrub(&l, &n);
    CHECK(!strcmp(l.text, "settings at http://<host>.local/"));

    l = mk(0, 'I', "airwriter", "Orchards and Orchard1 stay");
    lb_scrub(&l, &n);
    CHECK(!strcmp(l.text, "Orchards and Orchard1 stay"));

    l = mk(0, 'I', "airwriter", "Orchard Orchard Orchard");           // every one goes
    lb_scrub(&l, &n);
    CHECK(!strcmp(l.text, "<network> <network> <network>"));

    lb_names_t tiny = { "a", "ab" };                                  // too short to tell apart
    l = mk(0, 'I', "airwriter", "a cab ab a");
    lb_scrub(&l, &tiny);
    CHECK(!strcmp(l.text, "a cab ab a"));

    lb_names_t blank = { "", NULL };
    l = mk(0, 'I', "airwriter", "nothing changes");
    lb_scrub(&l, &blank);
    CHECK(!strcmp(l.text, "nothing changes"));
    lb_scrub(&l, NULL);
    CHECK(!strcmp(l.text, "nothing changes"));

    l = mk(0, 'I', "airwriter", "not a mac here aa:bb:cc:dd:ee:ff");  // only the Wi-Fi library's lines
    lb_scrub(&l, &n);
    CHECK(!strcmp(l.text, "not a mac here aa:bb:cc:dd:ee:ff"));

    l = mk(0, 'I', "wifi", "almost aa:bb:cc:dd:ee nope");             // five pairs isn't a MAC
    lb_scrub(&l, &n);
    CHECK(!strcmp(l.text, "almost aa:bb:cc:dd:ee nope"));

    // A name that is "<network>" itself, or holds it, doesn't loop.
    lb_names_t self = { "<network>", "<host>" };
    l = mk(0, 'I', "airwriter", "x <network> y");
    lb_scrub(&l, &self);
    CHECK(!strcmp(l.text, "x <network> y"));

    // Growing past the end stays terminated, and within bounds.
    lb_names_t bee = { "bee", NULL };
    char full[LB_TEXT_MAX + 1];
    size_t i = 0;
    while (i + 4 <= LB_TEXT_MAX) { memcpy(full + i, "bee ", 4); i += 4; }
    full[i] = 0;
    l = mk(0, 'I', "airwriter", full);
    lb_scrub(&l, &bee);
    CHECK(strlen(l.text) <= LB_TEXT_MAX);
    CHECK(strstr(l.text, "bee") == NULL);
}

static void test_scrub_never_leaves_a_name_at_the_cut(void)
{
    // A name that would sit past the cut must not survive as a tail: truncating the growing
    // text drops it whole, so nothing of the name is left to read.
    lb_names_t n = { "Orchard", NULL };
    char text[LB_TEXT_MAX + 1];
    memset(text, 'x', LB_TEXT_MAX);
    text[LB_TEXT_MAX] = 0;
    memcpy(text + LB_TEXT_MAX - 8, " Orchard", 8);
    lb_line_t l = mk(0, 'I', "airwriter", text);
    lb_scrub(&l, &n);
    CHECK(strstr(l.text, "Orchard") == NULL);
    CHECK(strlen(l.text) <= LB_TEXT_MAX);

    // The cut at the end of a long line can leave the front of a name there.
    memset(text, 'x', LB_TEXT_MAX);
    memcpy(text + LB_TEXT_MAX - 5, " Orch", 5);
    l = mk(0, 'I', "airwriter", text);
    lb_scrub(&l, &n);
    CHECK(strstr(l.text, "Orch") == NULL);
    CHECK(strlen(l.text) <= LB_TEXT_MAX);

    // A line that merely ends short isn't touched.
    l = mk(0, 'I', "airwriter", "ends in Orch");
    lb_scrub(&l, &n);
    CHECK(!strcmp(l.text, "ends in Orch"));
}

static void test_chatter(void)
{
    lb_line_t l = mk(0, 'I', "ipp", "op 0x000b id 1 asks printer-state");
    CHECK_EQ_INT(lb_is_chatter(&l), 1);
    l = mk(0, 'I', "ipp", "op 0x0009 id 1");
    CHECK_EQ_INT(lb_is_chatter(&l), 1);
    l = mk(0, 'I', "ipp", "op 0x000a id 1");
    CHECK_EQ_INT(lb_is_chatter(&l), 1);
    l = mk(0, 'I', "ipp", "  attributes of x: 1 bytes, state 3");
    CHECK_EQ_INT(lb_is_chatter(&l), 1);
    l = mk(0, 'I', "ipp", "op 0x0002 id 1");                       // Print-Job
    CHECK_EQ_INT(lb_is_chatter(&l), 0);
    l = mk(0, 'I', "airwriter", "op 0x000b");
    CHECK_EQ_INT(lb_is_chatter(&l), 0);
}

static void test_encode(void)
{
    lb_line_t a = mk(70000, 'W', "tag", "some text");
    a.run = 513;
    a.ms = 123456;
    uint8_t buf[512];
    size_t n = lb_encode(&a, buf, sizeof buf);
    CHECK(n > 0);

    lb_line_t b;
    CHECK_EQ_INT(lb_decode(buf, n, &b), n);
    CHECK_EQ_INT(b.seq, 70000);
    CHECK_EQ_INT(b.run, 513);
    CHECK_EQ_INT(b.ms, 123456);
    CHECK_EQ_INT(b.level, 'W');
    CHECK_EQ_INT(b.ram_only, 0);
    CHECK(!strcmp(b.tag, "tag"));
    CHECK(!strcmp(b.text, "some text"));

    a.ram_only = 1;
    n = lb_encode(&a, buf, sizeof buf);
    CHECK_EQ_INT(lb_decode(buf, n, &b), n);
    CHECK_EQ_INT(b.ram_only, 1);

    CHECK_EQ_INT(lb_decode(buf, n - 1, &b), 0);                    // cut short
    uint8_t bad[512];
    memcpy(bad, buf, n);
    bad[n - 3] ^= 0x01;                                            // a flipped text byte
    CHECK_EQ_INT(lb_decode(bad, n, &b), 0);
    memcpy(bad, buf, n);
    bad[0] = 0xFF;                                                 // blank flash
    CHECK_EQ_INT(lb_decode(bad, n, &b), 0);

    CHECK_EQ_INT(lb_encode(&a, buf, 4), 0);                        // won't fit

    lb_line_t empty = mk(1, 'I', "", "");                          // nothing to hold, still a record
    n = lb_encode(&empty, buf, sizeof buf);
    CHECK(n > 0);
    CHECK_EQ_INT(lb_decode(buf, n, &b), n);
    CHECK(b.tag[0] == 0 && b.text[0] == 0);
}

static void test_wire(void)
{
    lb_line_t l = mk(12, 'E', "job", "failed");
    l.run = 3;
    l.ms = 45;
    char buf[300];
    size_t n = lb_wire(&l, buf, sizeof buf);
    CHECK(!strcmp(buf, "12\t3\t45\tE\tjob\tfailed\n"));
    CHECK_EQ_INT(n, strlen(buf));
    CHECK_EQ_INT(lb_wire(&l, buf, 5), 0);                          // won't fit, nothing half-written
}

static void test_crash_text(void)
{
    lb_crash_t c = { .reason = "LoadProhibited", .task = "rip", .core = 1, .n = 2,
                     .pc = { 0x42001234, 0x42005678 } };
    char buf[LB_TEXT_MAX + 1];
    lb_crash_text(&c, buf, sizeof buf);
    CHECK(!strcmp(buf, "crashed in task rip on core 1 (LoadProhibited); backtrace 0x42001234 0x42005678"));

    lb_crash_t none = { .reason = "abort", .task = "main", .core = 0, .n = 0 };
    lb_crash_text(&none, buf, sizeof buf);
    CHECK(!strcmp(buf, "crashed in task main on core 0 (abort)"));
}

// The rings. A fake flash: writing can only clear bits, as on the real part.
#define FF_BYTES (LB_SECTORS * LB_SECTOR_BYTES)
typedef struct { uint8_t mem[FF_BYTES]; unsigned erases; int fail_writes; } fakeflash_t;
static fakeflash_t g_ff;
static uint32_t g_ring[LB_RING_BYTES / 4];

static unsigned g_ff_reads;

static int ff_read(void *c, uint32_t off, void *buf, uint32_t n)
{
    g_ff_reads++;
    memcpy(buf, ((fakeflash_t *)c)->mem + off, n);
    return 0;
}

static int ff_write(void *c, uint32_t off, const void *buf, uint32_t n)
{
    const uint8_t *d = buf;
    fakeflash_t *f = c;
    if (f->fail_writes) return 1;
    for (uint32_t i = 0; i < n; i++) f->mem[off + i] &= d[i];
    return 0;
}

static int ff_erase(void *c, uint32_t sector)
{
    fakeflash_t *f = c;
    memset(f->mem + sector * LB_SECTOR_BYTES, 0xFF, LB_SECTOR_BYTES);
    f->erases++;
    return 0;
}

static lb_flash_t ff_ops(void)
{
    memset(g_ff.mem, 0xFF, sizeof g_ff.mem);
    g_ff.erases = 0;
    g_ff.fail_writes = 0;
    return (lb_flash_t){ &g_ff, ff_read, ff_write, ff_erase };
}

static lb_store_t g_s, g_s2;
static lb_reader_t g_rd;

static lb_ring_t *fresh_ring(void)
{
    int kept = 1;
    memset(g_ring, 0, sizeof g_ring);
    lb_ring_t *r = lb_ring_attach(g_ring, sizeof g_ring, &kept);
    CHECK_EQ_INT(kept, 0);
    return r;
}

// Lines of mixed lengths, so records fall across the end of the ring in every way.
static void push_lines(lb_ring_t *r, unsigned n, char level, uint8_t ram_only)
{
    static unsigned salt;
    for (unsigned i = 0; i < n; i++) {
        lb_line_t l = mk(0, level, "tag", "");
        int w = snprintf(l.text, sizeof l.text, "line %u ", i);
        unsigned pad = (salt++ * 37u) % 120u;
        memset(l.text + w, 'p', pad);
        l.text[w + pad] = 0;
        l.ram_only = ram_only;
        lb_ring_push(r, &l);
    }
}

// Reads everything after `after`: the count, with the first and last seq, every step ascending.
static unsigned read_all(const lb_ring_t *r, const lb_store_t *s, uint32_t after,
                         uint32_t *first, uint32_t *last)
{
    lb_line_t l;
    unsigned n = 0;
    uint32_t prev = after;
    lb_read_begin(&g_rd, r, s, after);
    while (lb_read_next(&g_rd, &l)) {
        CHECK(l.seq > prev);
        if (!n && first) *first = l.seq;
        prev = l.seq;
        n++;
        if (n > 100000) { CHECK(0); break; }
    }
    if (last) *last = prev;
    return n;
}

static void test_ring_keeps_newest(void)
{
    lb_ring_t *r = fresh_ring();
    push_lines(r, 2000, 'I', 0);
    CHECK_EQ_INT(lb_ring_newest(r), 2000);
    uint32_t first = 0, last = 0;
    unsigned n = read_all(r, NULL, 0, &first, &last);
    CHECK_EQ_INT(last, 2000);
    CHECK_EQ_INT(n, last - first + 1);               // consecutive
    CHECK(n > 100 && n < 2000);                      // the ring holds some, not all
}

static void test_ring_attach(void)
{
    lb_ring_t *r = fresh_ring();
    lb_ring_begin_run(r, 1, 0);
    push_lines(r, 50, 'W', 0);
    int kept = 0;
    r = lb_ring_attach(g_ring, sizeof g_ring, &kept);
    CHECK_EQ_INT(kept, 1);
    CHECK_EQ_INT(lb_ring_newest(r), 50);
    CHECK_EQ_INT(read_all(r, NULL, 0, NULL, NULL), 50);
    push_lines(r, 1, 'I', 0);
    CHECK_EQ_INT(lb_ring_newest(r), 51);

    ((uint8_t *)g_ring)[0] ^= 0x40;                  // what power loss leaves
    r = lb_ring_attach(g_ring, sizeof g_ring, &kept);
    CHECK_EQ_INT(kept, 0);
    CHECK_EQ_INT(read_all(r, NULL, 0, NULL, NULL), 0);
}

static void test_ring_torn_tail(void)
{
    lb_ring_t *r = fresh_ring();
    for (int i = 0; i < 10; i++) { lb_line_t l = mk(0, 'I', "t", "same length"); lb_ring_push(r, &l); }
    lb_line_t probe = mk(0, 'I', "t", "same length");
    uint8_t rec[300];
    size_t one = lb_encode(&probe, rec, sizeof rec);

    // The ring's data ends one record after the last whole one: flip a byte of the last one.
    uint8_t *mem = (uint8_t *)g_ring;
    size_t i;
    for (i = sizeof g_ring - 1; i > 0 && !mem[i]; i--) {}   // the last byte ever written
    mem[i - one / 2] ^= 0x10;

    int kept = 0;
    r = lb_ring_attach(g_ring, sizeof g_ring, &kept);
    CHECK_EQ_INT(kept, 1);
    uint32_t last = 0;
    CHECK_EQ_INT(read_all(r, NULL, 0, NULL, &last), 9);
    CHECK_EQ_INT(last, 9);
}

static void test_store_blank(void)
{
    lb_flash_t f = ff_ops();
    lb_store_open(&g_s, &f);
    CHECK_EQ_INT(g_s.last_seq, 0);
    CHECK_EQ_INT(read_all(fresh_ring(), &g_s, 0, NULL, NULL), 0);
    lb_store_prepare(&g_s);
    CHECK_EQ_INT(g_ff.erases, 0);                    // blank needs no erase
    lb_line_t l = mk(1, 'I', "t", "x");
    CHECK_EQ_INT(lb_store_append(&g_s, &l), 1);

    uint32_t x = 0x12345678;                         // bytes of nothing in particular
    for (size_t i = 0; i < sizeof g_ff.mem; i++) {
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;
        g_ff.mem[i] = (uint8_t)x;
    }
    lb_store_open(&g_s, &f);
    CHECK_EQ_INT(read_all(fresh_ring(), &g_s, 0, NULL, NULL), 0);
    lb_store_prepare(&g_s);
    CHECK_EQ_INT(lb_store_append(&g_s, &l), 1);
    CHECK_EQ_INT(read_all(fresh_ring(), &g_s, 0, NULL, NULL), 1);
}

static void store_lines(lb_store_t *s, unsigned from, unsigned n, uint16_t run)
{
    for (unsigned i = 0; i < n; i++) {
        lb_line_t l = mk(from + i, 'I', "tag", "a line of an ordinary length, saved");
        l.run = run;
        lb_store_prepare(s);
        CHECK_EQ_INT(lb_store_append(s, &l), 1);
    }
}

static void test_store_reopen(void)
{
    lb_flash_t f = ff_ops();
    lb_store_open(&g_s, &f);
    store_lines(&g_s, 1, 100, 4);
    lb_store_open(&g_s2, &f);
    CHECK_EQ_INT(g_s2.last_seq, 100);
    CHECK_EQ_INT(g_s2.last_run, 4);
    uint32_t last = 0;
    CHECK_EQ_INT(read_all(fresh_ring(), &g_s2, 0, NULL, &last), 100);
    CHECK_EQ_INT(last, 100);
    store_lines(&g_s2, 101, 1, 5);                   // and it carries on where it was
    CHECK_EQ_INT(read_all(fresh_ring(), &g_s2, 0, NULL, &last), 101);
}

static void test_store_wrap(void)
{
    lb_flash_t f = ff_ops();
    lb_store_open(&g_s, &f);
    lb_ring_t *r = fresh_ring();
    unsigned total = 15000;
    for (unsigned i = 0; i < total; i++) {
        push_lines(r, 1, 'I', 0);
        if (i % 10 == 9) { lb_store_prepare(&g_s); lb_save(&g_s, r); }
    }
    lb_store_prepare(&g_s);
    lb_save(&g_s, r);
    CHECK_EQ_INT(g_s.last_seq, total);

    unsigned live = 0;
    for (unsigned i = 0; i < LB_SECTORS; i++) live += g_s.sector_no[i] != 0;
    CHECK(live <= LB_SECTORS);

    uint32_t first = 0, last = 0;
    unsigned n = read_all(fresh_ring(), &g_s, 0, &first, &last);
    CHECK_EQ_INT(last, total);
    CHECK(first > 1);                                // the oldest are gone
    CHECK_EQ_INT(n, last - first + 1);               // nothing missing in between

    lb_store_open(&g_s2, &f);                        // the same from a cold start
    CHECK_EQ_INT(g_s2.last_seq, total);
    CHECK_EQ_INT(read_all(fresh_ring(), &g_s2, 0, NULL, NULL), n);
}

static void test_store_no_erased_room(void)
{
    lb_flash_t f = ff_ops();
    lb_store_open(&g_s, &f);
    lb_store_prepare(&g_s);
    unsigned stored = 0;
    for (unsigned i = 1; i < 10000; i++) {
        lb_line_t l = mk(i, 'I', "tag", "a line of an ordinary length, saved");
        if (!lb_store_append(&g_s, &l)) break;
        stored = i;
    }
    CHECK(stored > 100);
    CHECK_EQ_INT(g_s.last_seq, stored);
    uint32_t last = 0;
    CHECK_EQ_INT(read_all(fresh_ring(), &g_s, 0, NULL, &last), stored);

    // lb_save finds no room either, and doesn't pretend it saved.
    lb_ring_t *r = fresh_ring();
    lb_ring_begin_run(r, stored + 1, 1);
    push_lines(r, 3, 'E', 0);
    CHECK_EQ_INT(lb_has_urgent(&g_s, r), 1);
    CHECK_EQ_INT(lb_save(&g_s, r), 0);
    CHECK_EQ_INT(g_s.last_seq, stored);
    CHECK_EQ_INT(lb_has_urgent(&g_s, r), 1);

    lb_store_prepare(&g_s);                          // room again: those three go down
    CHECK_EQ_INT(lb_save(&g_s, r), 3);
    CHECK_EQ_INT(lb_has_urgent(&g_s, r), 0);
}

static void test_store_torn_record(void)
{
    lb_flash_t f = ff_ops();
    lb_store_open(&g_s, &f);
    store_lines(&g_s, 1, 5, 1);
    unsigned sector = g_s.head;

    lb_line_t l = mk(6, 'I', "tag", "this one loses power half way");
    uint8_t rec[300];
    size_t n = lb_encode(&l, rec, sizeof rec);
    ff_write(&g_ff, sector * LB_SECTOR_BYTES + g_s.head_off, rec, (uint32_t)(n / 2));

    lb_store_open(&g_s2, &f);
    uint32_t last = 0;
    CHECK_EQ_INT(read_all(fresh_ring(), &g_s2, 0, NULL, &last), 5);
    CHECK_EQ_INT(g_s2.last_seq, 5);

    store_lines(&g_s2, 6, 1, 1);                     // the next one goes in the next sector
    CHECK_EQ_INT(g_s2.head, (sector + 1) % LB_SECTORS);
    CHECK_EQ_INT(read_all(fresh_ring(), &g_s2, 0, NULL, &last), 6);
    CHECK_EQ_INT(last, 6);
}

static void test_store_torn_header(void)
{
    lb_flash_t f = ff_ops();
    lb_store_open(&g_s, &f);
    store_lines(&g_s, 1, 5, 1);
    unsigned next = (g_s.head + 1) % LB_SECTORS;

    static const uint8_t half[] = { 'A', 'W', 'L' };  // power lost inside a sector's first write
    ff_write(&g_ff, next * LB_SECTOR_BYTES, half, sizeof half);

    lb_store_open(&g_s2, &f);
    CHECK_EQ_INT(read_all(fresh_ring(), &g_s2, 0, NULL, NULL), 5);
    lb_store_prepare(&g_s2);
    CHECK_EQ_INT(g_ff.mem[next * LB_SECTOR_BYTES], 0xFF);   // made ready again
    for (unsigned i = 0; i < 300; i++) store_lines(&g_s2, 6 + i, 1, 1);
    uint32_t last = 0;
    CHECK_EQ_INT(read_all(fresh_ring(), &g_s2, 0, NULL, &last), 305);
    CHECK_EQ_INT(last, 305);
}

static void test_store_write_fails(void)
{
    lb_flash_t f = ff_ops();
    lb_store_open(&g_s, &f);
    lb_line_t probe = mk(1, 'I', "tag", "a line of an ordinary length, saved");
    uint8_t rec[300];
    size_t one = lb_encode(&probe, rec, sizeof rec);

    unsigned seq = 1;
    while (g_s.head < 3 || g_s.head_off + one <= LB_SECTOR_BYTES) store_lines(&g_s, seq++, 1, 1);

    // The first write into a new sector fails. The number sequence has to survive it.
    lb_store_prepare(&g_s);
    g_ff.fail_writes = 1;
    lb_line_t l = mk(seq, 'I', "tag", "a line of an ordinary length, saved");
    CHECK_EQ_INT(lb_store_append(&g_s, &l), 0);
    CHECK_EQ_INT(g_s.last_seq, seq - 1);
    g_ff.fail_writes = 0;
    store_lines(&g_s, seq, 1, 1);

    uint32_t last = 0;
    CHECK_EQ_INT(read_all(fresh_ring(), &g_s, 0, NULL, &last), seq);
    CHECK_EQ_INT(last, seq);
    lb_store_open(&g_s2, &f);
    CHECK_EQ_INT(g_s2.last_seq, seq);
    CHECK_EQ_INT(read_all(fresh_ring(), &g_s2, 0, NULL, &last), seq);
}

static void test_save_one_at_a_time(void)
{
    // The board holds the ring's lock only to take a line, never across a flash write.
    lb_flash_t f = ff_ops();
    lb_store_open(&g_s, &f);
    lb_ring_t *r = fresh_ring();
    push_lines(r, 2, 'I', 0);
    push_lines(r, 2, 'I', 1);
    push_lines(r, 1, 'I', 0);
    lb_store_prepare(&g_s);

    uint32_t seqs[8];
    unsigned n = 0;
    while (lb_save_next(&g_s, r) && n < 8) {
        seqs[n++] = g_s.line.seq;
        CHECK_EQ_INT(lb_store_append(&g_s, &g_s.line), 1);
    }
    CHECK_EQ_INT(n, 3);
    CHECK_EQ_INT(seqs[0], 1);
    CHECK_EQ_INT(seqs[1], 2);
    CHECK_EQ_INT(seqs[2], 5);
    CHECK_EQ_INT(g_s.last_seq, 5);
}

static void test_store_unknown_format(void)
{
    lb_flash_t f = ff_ops();
    uint8_t future[8] = { 'A', 'W', 'L', 2, 9, 0, 0, 0 };
    ff_write(&g_ff, 0, future, sizeof future);
    lb_store_open(&g_s, &f);
    CHECK_EQ_INT(g_s.sector_no[0], 0);
    lb_store_prepare(&g_s);
    CHECK_EQ_INT(g_ff.erases, 1);
    CHECK_EQ_INT(g_ff.mem[3], 0xFF);
}

static void test_save_skips_chatter(void)
{
    lb_flash_t f = ff_ops();
    lb_store_open(&g_s, &f);
    lb_ring_t *r = fresh_ring();
    push_lines(r, 2, 'I', 0);
    push_lines(r, 3, 'I', 1);                        // a phone asking how things are
    push_lines(r, 2, 'I', 0);
    lb_store_prepare(&g_s);
    CHECK_EQ_INT(lb_save(&g_s, r), 4);
    CHECK_EQ_INT(g_s.last_seq, 7);
    CHECK_EQ_INT(lb_save(&g_s, r), 0);               // nothing twice

    uint32_t last = 0;
    CHECK_EQ_INT(read_all(r, &g_s, 0, NULL, &last), 7);              // RAM beside it: all of them
    CHECK_EQ_INT(read_all(fresh_ring(), &g_s, 0, NULL, &last), 4);   // flash alone: not the chatter
    CHECK_EQ_INT(last, 7);
}

// Reads with s (NULL: RAM only, as during a job) and says whether line seq is there.
static int has_seq(const lb_ring_t *r, const lb_store_t *s, uint32_t seq)
{
    lb_line_t l;
    lb_read_begin(&g_rd, r, s, 0);
    while (lb_read_next(&g_rd, &l)) if (l.seq == seq) return 1;
    return 0;
}

static void push_poll(lb_ring_t *r, unsigned id)
{
    lb_line_t l = mk(0, 'I', "ipp", "");
    snprintf(l.text, sizeof l.text, "op 0x000b id %u v2.0 to /ipp/print/2 (1) asks printer-state,"
             "printer-state-reasons,printer-state-message,marker-levels,printer-is-accepting-jobs",
             id);
    l.ram_only = (uint8_t)lb_is_chatter(&l);
    CHECK_EQ_INT(l.ram_only, 1);
    lb_ring_push(r, &l);
    l = mk(0, 'I', "ipp", "  attributes of /ipp/print/2: 1834 bytes, state 4");
    l.ram_only = (uint8_t)lb_is_chatter(&l);
    lb_ring_push(r, &l);
}

// W-1142: a Mac with a job queued asks after the printer about once a second (board 1,
// 2026-11-13: a Personal LaserWriter 300 job, its cover opened mid-page). Nothing is saved during
// a job until a warning, so the job's own lines wait in RAM: the asking mustn't push them out.
static void test_polls_keep_a_jobs_lines(void)
{
    lb_flash_t f = ff_ops();
    lb_store_open(&g_s, &f);
    lb_ring_t *r = fresh_ring();
    lb_ring_begin_run(r, 1, 0);
    push_lines(r, 30, 'I', 0);                       // before the job, saved while idle
    lb_store_prepare(&g_s);
    CHECK_EQ_INT(lb_save(&g_s, r), 30);

    lb_line_t l = mk(0, 'I', "airwriter", "job 12 for printer 2: started");
    lb_ring_push(r, &l);
    uint32_t start = l.seq;
    lb_line_t first;                                 // the screen, open from the start
    lb_read_begin(&g_rd, r, NULL, start - 1);
    CHECK_EQ_INT(lb_read_next(&g_rd, &first), 1);
    CHECK_EQ_INT(first.seq, start);
    lb_reader_t open_screen = g_rd;

    uint32_t page = 0, poll = 0;
    for (unsigned i = 0; i < 1200; i++) {            // 20 minutes of asking
        push_poll(r, 100 + i);
        if (i == 300) {
            l = mk(0, 'I', "lw300", "job 12: page 1 sent");
            lb_ring_push(r, &l);
            page = l.seq;
        }
        if (i == 30) poll = lb_ring_newest(r);
        CHECK_EQ_INT(lb_has_urgent(&g_s, r), 0);     // so nothing goes to flash
    }
    l = mk(0, 'W', "lw300", "printer 2: cover open");
    lb_ring_push(r, &l);
    uint32_t warn = l.seq;

    // What the screen shows during the job, from RAM alone: the job's lines, and the latest asking.
    CHECK(has_seq(r, NULL, start));
    CHECK(has_seq(r, NULL, page));
    CHECK(has_seq(r, NULL, warn));
    CHECK(has_seq(r, NULL, warn - 1));
    CHECK(!has_seq(r, NULL, poll));                  // the asking at the start has gone
    uint32_t last = 0;
    CHECK(read_all(r, NULL, 0, NULL, &last) > 100);
    CHECK_EQ_INT(last, warn);

    // The screen left open carries on in order, past everything the asking pushed out.
    g_rd = open_screen;
    uint32_t prev = start;
    int saw_page = 0;
    while (lb_read_next(&g_rd, &l)) {
        CHECK(l.seq > prev);
        prev = l.seq;
        saw_page |= l.seq == page;
    }
    CHECK(saw_page);
    CHECK_EQ_INT(prev, warn);

    // The warning saves the job's lines, and only those.
    CHECK_EQ_INT(lb_has_urgent(&g_s, r), 1);
    CHECK_EQ_INT(lb_save(&g_s, r), 3);
    CHECK(has_seq(fresh_ring(), &g_s, start));       // flash alone has them, after a power cut
    CHECK(has_seq(fresh_ring(), &g_s, page));
}

// The ring holds the asking apart from the rest; a restart keeps both, numbered as one.
static void test_ring_attach_keeps_polls(void)
{
    lb_ring_t *r = fresh_ring();
    lb_ring_begin_run(r, 1, 0);
    push_lines(r, 5, 'I', 0);
    push_poll(r, 1);                                 // the newest two are asking
    int kept = 0;
    r = lb_ring_attach(g_ring, sizeof g_ring, &kept);
    CHECK_EQ_INT(kept, 1);
    CHECK_EQ_INT(lb_ring_newest(r), 7);
    uint32_t first = 0, last = 0;
    CHECK_EQ_INT(read_all(r, NULL, 0, &first, &last), 7);
    CHECK_EQ_INT(first, 1);
    CHECK_EQ_INT(last, 7);
    lb_ring_begin_run(r, 1, 0);
    lb_line_t l = mk(0, 'S', "sys", "reset=panic");
    lb_ring_push(r, &l);
    CHECK_EQ_INT(l.seq, 8);
    CHECK_EQ_INT(l.run, 2);
}

static void test_reader_merge(void)
{
    lb_flash_t f = ff_ops();
    lb_store_open(&g_s, &f);
    lb_ring_t *r = fresh_ring();
    push_lines(r, 50, 'I', 0);
    lb_store_prepare(&g_s);
    lb_save(&g_s, r);
    push_lines(r, 50, 'I', 0);

    uint32_t first = 0, last = 0;
    CHECK_EQ_INT(read_all(r, &g_s, 0, &first, &last), 100);
    CHECK_EQ_INT(first, 1);
    CHECK_EQ_INT(last, 100);
    CHECK_EQ_INT(read_all(r, &g_s, 30, &first, &last), 70);
    CHECK_EQ_INT(first, 31);
    CHECK_EQ_INT(read_all(r, &g_s, 100, NULL, NULL), 0);
    CHECK_EQ_INT(read_all(r, NULL, 0, NULL, NULL), 100);             // RAM only, as during a job
}

// The screen asks every second for the lines after the last it has. RAM holds those.
static void test_reader_leaves_flash_alone_when_ram_has_it_all(void)
{
    lb_flash_t f = ff_ops();
    lb_store_open(&g_s, &f);
    store_lines(&g_s, 1, 50, 7);
    lb_ring_t *r = fresh_ring();
    lb_ring_begin_run(r, g_s.last_seq + 1, g_s.last_run);
    push_lines(r, 50, 'I', 0);

    uint32_t first = 0, last = 0;
    g_ff_reads = 0;
    CHECK_EQ_INT(read_all(r, &g_s, 100, NULL, NULL), 0);
    CHECK_EQ_INT(read_all(r, &g_s, 90, &first, &last), 10);
    CHECK_EQ_INT(first, 91);
    CHECK_EQ_INT(last, 100);
    CHECK_EQ_INT(read_all(r, &g_s, 50, &first, NULL), 50);           // the line after is RAM's oldest
    CHECK_EQ_INT(first, 51);
    CHECK_EQ_INT(g_ff_reads, 0);

    CHECK_EQ_INT(read_all(r, &g_s, 30, &first, &last), 70);          // further back needs flash
    CHECK_EQ_INT(first, 31);
    CHECK_EQ_INT(last, 100);
    CHECK(g_ff_reads > 0);
    CHECK_EQ_INT(read_all(fresh_ring(), &g_s, 0, NULL, &last), 50);  // and so does an empty ring
    CHECK_EQ_INT(last, 50);
}

static void test_reader_overwrite(void)
{
    lb_ring_t *r = fresh_ring();
    push_lines(r, 300, 'I', 0);
    lb_line_t l;
    lb_read_begin(&g_rd, r, NULL, 0);
    CHECK_EQ_INT(lb_read_next(&g_rd, &l), 1);
    uint32_t prev = l.seq;
    push_lines(r, 3000, 'I', 0);                     // the ring laps the reader's place
    unsigned n = 0;
    while (lb_read_next(&g_rd, &l)) {
        CHECK(l.seq > prev);
        prev = l.seq;
        if (++n > 5000) { CHECK(0); break; }
    }
    CHECK_EQ_INT(prev, 3300);
    CHECK(n > 0);
}

static void test_ids_after_power_cut(void)
{
    lb_flash_t f = ff_ops();
    lb_store_open(&g_s, &f);
    store_lines(&g_s, 500, 1, 7);
    lb_ring_t *r = fresh_ring();
    lb_ring_begin_run(r, g_s.last_seq + 1, g_s.last_run);
    CHECK_EQ_INT(lb_ring_run(r), 8);
    lb_line_t l = mk(0, 'S', "sys", "reset=power-on");
    lb_ring_push(r, &l);
    CHECK_EQ_INT(l.seq, 501);
    CHECK_EQ_INT(l.run, 8);
}

static void test_ring_carries_on_after_restart(void)
{
    // RAM survived: its numbers are ahead of flash's, and win.
    lb_ring_t *r = fresh_ring();
    lb_ring_begin_run(r, 1, 0);
    push_lines(r, 40, 'I', 0);
    int kept = 0;
    r = lb_ring_attach(g_ring, sizeof g_ring, &kept);
    lb_ring_begin_run(r, 11, 0);
    CHECK_EQ_INT(lb_ring_run(r), 2);
    lb_line_t l = mk(0, 'S', "sys", "reset=panic");
    lb_ring_push(r, &l);
    CHECK_EQ_INT(l.seq, 41);
}

static void test_clear(void)
{
    lb_flash_t f = ff_ops();
    lb_store_open(&g_s, &f);
    lb_ring_t *r = fresh_ring();
    lb_ring_begin_run(r, 1, 0);
    push_lines(r, 200, 'E', 0);
    lb_store_prepare(&g_s);
    lb_save(&g_s, r);
    lb_ring_clear(r);
    lb_store_clear(&g_s);
    CHECK_EQ_INT(read_all(r, &g_s, 0, NULL, NULL), 0);
    CHECK_EQ_INT(g_s.last_seq, 0);
    CHECK_EQ_INT(lb_ring_newest(r), 0);

    lb_line_t l = mk(0, 'W', "airwriter", "factory reset: settings erased");
    lb_ring_push(r, &l);
    CHECK_EQ_INT(l.seq, 1);                          // the page sees the number fall, and reloads

    lb_store_open(&g_s2, &f);                        // and it stays gone
    CHECK_EQ_INT(read_all(fresh_ring(), &g_s2, 0, NULL, NULL), 0);
}

int main(void)
{
    test_parse();
    test_feed_in_pieces();
    test_scrub_names();
    test_scrub_never_leaves_a_name_at_the_cut();
    test_chatter();
    test_encode();
    test_wire();
    test_crash_text();
    test_ring_keeps_newest();
    test_ring_attach();
    test_ring_torn_tail();
    test_store_blank();
    test_store_reopen();
    test_store_wrap();
    test_store_no_erased_room();
    test_store_torn_record();
    test_store_torn_header();
    test_store_write_fails();
    test_save_one_at_a_time();
    test_store_unknown_format();
    test_save_skips_chatter();
    test_polls_keep_a_jobs_lines();
    test_ring_attach_keeps_polls();
    test_reader_merge();
    test_reader_leaves_flash_alone_when_ram_has_it_all();
    test_reader_overwrite();
    test_ids_after_power_cut();
    test_ring_carries_on_after_restart();
    test_clear();
    TEST_END;
}
