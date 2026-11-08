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

int main(void)
{
    test_parse();
    test_scrub_names();
    test_scrub_never_leaves_a_name_at_the_cut();
    test_chatter();
    test_encode();
    test_wire();
    test_crash_text();
    TEST_END;
}
