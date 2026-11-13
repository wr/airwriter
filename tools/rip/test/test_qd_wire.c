// SPDX-License-Identifier: GPL-3.0-or-later
// W-1102: what goes down the wire to a QuickDraw LaserWriter: command and value packets, a row
// packet with its length and check, and the line image a packet becomes (BREAK, idle, 8N1
// bytes LSB first, marks), which the link clocks out one bit per printer clock.
#include "test_util.h"
#include "qd_wire.h"
#include <string.h>
#include <stdlib.h>

static int bit_at(const uint8_t *buf, size_t k) { return (buf[k >> 3] >> (k & 7)) & 1; }

static void test_cmd(void)
{
    uint8_t p[3];
    CHECK_EQ_INT(qd_cmd(0x83, 0x3E, p), 3);
    CHECK_EQ_MEM(p, ((uint8_t[]){0x83, 0x3E, 0xBD}), 3);
    qd_cmd(0x82, 0x00, p);
    CHECK_EQ_MEM(p, ((uint8_t[]){0x82, 0x00, 0x82}), 3);
    qd_cmd(0x8B, 0x8B, p);
    CHECK_EQ_MEM(p, ((uint8_t[]){0x8B, 0x8B, 0x00}), 3);
}

static void test_value(void)
{
    CHECK_EQ_INT(qd_value(0x21), 0x43);     // 0x42 has two ones: the parity bit makes three
    CHECK_EQ_INT(qd_value(32), 0x40);       // 0x40 already odd
}

static void test_reply_ok(void)
{
    CHECK(qd_reply_ok(0x01));
    CHECK(qd_reply_ok(0x29));
    CHECK(qd_reply_ok(0x20));
    CHECK(qd_reply_ok(0x61));
    CHECK(!qd_reply_ok(0x80));              // refused
    CHECK(!qd_reply_ok(0x03));              // even parity: garbled
    CHECK(!qd_reply_ok(0x83));              // refused, and garbled too
    CHECK(!qd_reply_ok(0x00));
}

static void test_row_packet(void)
{
    uint8_t out[QD_ROW_MAX + 8];
    uint8_t one[1] = {0x80};
    CHECK_EQ_INT(qd_row_packet(one, 1, out), 4);
    CHECK_EQ_MEM(out, ((uint8_t[]){0x4F, 0xFF, 0x80, 0x30}), 4);

    uint8_t codes[QD_ROW_MAX + 1];
    for (size_t i = 0; i < sizeof codes; i++) codes[i] = (uint8_t)(i * 7 + 3);
    CHECK_EQ_INT(qd_row_packet(codes, 257, out), 260);
    CHECK_EQ_INT(out[0], 0x4E); CHECK_EQ_INT(out[1], 0xFF);
    CHECK_EQ_MEM(out + 2, codes, 257);
    uint8_t x = 0;
    for (int i = 0; i < 259; i++) x ^= out[i];
    CHECK_EQ_INT(out[259], x);

    CHECK_EQ_INT(qd_row_packet(codes, QD_ROW_MAX, out), QD_ROW_MAX + 3);
    CHECK_EQ_INT(out[0], (0x5000 - QD_ROW_MAX) >> 8);
    CHECK_EQ_INT(qd_row_packet(codes, 0, out), 0);
    CHECK_EQ_INT(qd_row_packet(codes, QD_ROW_MAX + 1, out), 0);
}

static void test_bits_one_packet(void)
{
    uint8_t pkt[3] = {0x82, 0x00, 0x82};
    CHECK_EQ_INT(qd_bits_len(3), 430);
    uint8_t buf[64];
    memset(buf, 0x5A, sizeof buf);          // whatever was there before is overwritten
    size_t pos = 0;
    CHECK_EQ_INT(qd_bits_put(buf, 430, &pos, pkt, 3), 0);
    CHECK_EQ_INT(pos, 430);
    int ok = 1;
    for (size_t k = 0; k < 100; k++) ok &= bit_at(buf, k) == 0;
    CHECK(ok);                                  // BREAK
    ok = 1;
    for (size_t k = 100; k < 220; k++) ok &= bit_at(buf, k) == 1;
    CHECK(ok);                                  // idle
    static const int b82[10] = {0, 0, 1, 0, 0, 0, 0, 0, 1, 1};
    static const int b00[10] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
    ok = 1;
    for (int i = 0; i < 10; i++) {
        ok &= bit_at(buf, 220 + i) == b82[i];
        ok &= bit_at(buf, 230 + i) == b00[i];
        ok &= bit_at(buf, 240 + i) == b82[i];
    }
    CHECK(ok);                                  // the three bytes, 8N1 LSB first
    ok = 1;
    for (size_t k = 250; k < 430; k++) ok &= bit_at(buf, k) == 1;
    CHECK(ok);                                  // the marks after

    // A second packet starts where the first ended, not on a byte.
    uint8_t buf2[128];
    pos = 0;
    CHECK_EQ_INT(qd_bits_put(buf2, 1000, &pos, pkt, 3), 0);
    CHECK_EQ_INT(qd_bits_put(buf2, 1000, &pos, pkt, 3), 0);
    CHECK_EQ_INT(pos, 860);
    ok = 1;
    for (size_t k = 430; k < 530; k++) ok &= bit_at(buf2, k) == 0;
    for (int i = 0; i < 10; i++) ok &= bit_at(buf2, 430 + 220 + i) == b82[i];
    CHECK(ok);

    // Marks pad a chunk's tail.
    pos = 3;
    memset(buf, 0, sizeof buf);
    qd_bits_marks(buf, 64 * 8, &pos, 20);
    CHECK_EQ_INT(pos, 23);
    ok = bit_at(buf, 2) == 0 && bit_at(buf, 23) == 0;
    for (size_t k = 3; k < 23; k++) ok &= bit_at(buf, k) == 1;
    CHECK(ok);
    pos = 500;
    qd_bits_marks(buf, 64 * 8, &pos, 100);      // past the end: as many as fit
    CHECK_EQ_INT(pos, 512);
}

static void test_bits_no_room(void)
{
    uint8_t pkt[3] = {0x82, 0x00, 0x82};
    uint8_t buf[64] = {0};
    size_t pos = 0;
    CHECK_EQ_INT(qd_bits_put(buf, 429, &pos, pkt, 3), -1);
    CHECK_EQ_INT(pos, 0);
    pos = 10;
    CHECK_EQ_INT(qd_bits_put(buf, 439, &pos, pkt, 3), -1);
    CHECK_EQ_INT(pos, 10);
    CHECK_EQ_INT(qd_bits_put(buf, 440, &pos, pkt, 3), 0);
    CHECK_EQ_INT(pos, 440);
}

/* W-1143: the packer builds the line a byte at a time; this reference is the rule bit by bit
 * (bit k of the line is bit k & 7 of byte k >> 3, 1 a mark; BREAK, idle, each byte 8N1 LSB
 * first, the gap), touching nothing else. */
static void ref_bit(uint8_t *buf, size_t k, int v)
{
    uint8_t m = (uint8_t)(1u << (k & 7));
    if (v) buf[k >> 3] |= m;
    else buf[k >> 3] &= (uint8_t)~m;
}

static void ref_put(uint8_t *buf, size_t *pos, const uint8_t *pkt, size_t n)
{
    size_t k = *pos;
    for (size_t i = 0; i < QD_BREAK_BITS; i++) ref_bit(buf, k++, 0);
    for (size_t i = 0; i < QD_IDLE_BITS; i++) ref_bit(buf, k++, 1);
    for (size_t i = 0; i < n; i++) {
        ref_bit(buf, k++, 0);
        for (int b = 0; b < 8; b++) ref_bit(buf, k++, (pkt[i] >> b) & 1);
        ref_bit(buf, k++, 1);
    }
    for (size_t i = 0; i < QD_GAP_BITS; i++) ref_bit(buf, k++, 1);
    *pos = k;
}

static void test_bits_match_the_rule(void)
{
    enum { CAP = 4400 };
    static uint8_t a[CAP / 8 + 1], b[CAP / 8 + 1], pkt[300];
    uint32_t seed = 1143;
    int same = 1;
    for (int run = 0; run < 400 && same; run++) {
        // Whatever was in the buffer around the packet stays as it was.
        for (size_t i = 0; i < sizeof a; i++) {
            seed = seed * 1103515245u + 12345u;
            a[i] = b[i] = (uint8_t)(seed >> 16);
        }
        size_t n = 1 + (size_t)run % 300;
        for (size_t i = 0; i < n; i++) {
            seed = seed * 1103515245u + 12345u;
            pkt[i] = (uint8_t)(seed >> 16);
        }
        size_t start = (size_t)run % 37, pa = start, pb = start;
        CHECK_EQ_INT(qd_bits_put(a, CAP, &pa, pkt, n), 0);
        ref_put(b, &pb, pkt, n);
        same = pa == pb && !memcmp(a, b, sizeof a);
    }
    CHECK(same);
}

/* The packet ends exactly at the buffer's last bit, from every starting bit of a byte: the last
 * byte, partial or whole, is written as the rule writes it and nothing past the buffer is touched.
 * One bit less room is refused with the buffer left as it was. */
static void test_bits_at_the_buffer_end(void)
{
    static const uint8_t pkt[5] = { 0x4F, 0xFF, 0x00, 0xA5, 0x5A };
    int same = 1, refused = 1;
    for (size_t n = 1; n <= sizeof pkt; n++) {
        for (size_t start = 0; start < 16; start++) {
            size_t cap = start + qd_bits_len(n), nb = (cap + 7) / 8;
            uint8_t *a = malloc(nb + 1), *b = malloc(nb + 1);
            if (!a || !b) { free(a); free(b); CHECK(0); return; }
            for (size_t i = 0; i <= nb; i++) a[i] = b[i] = (uint8_t)(0x6B ^ (i * 29) ^ start);
            size_t pa = start, pb = start;
            if (qd_bits_put(a, cap, &pa, pkt, n)) same = 0;
            ref_put(b, &pb, pkt, n);
            if (pa != pb || pa != cap || memcmp(a, b, nb + 1)) same = 0;
            memcpy(b, a, nb + 1);
            size_t pc = start;
            if (qd_bits_put(a, cap - 1, &pc, pkt, n) != -1 || pc != start || memcmp(a, b, nb + 1)) refused = 0;
            free(a);
            free(b);
        }
    }
    CHECK(same);
    CHECK(refused);
}

int main(void)
{
    test_cmd();
    test_value();
    test_reply_ok();
    test_row_packet();
    test_bits_one_packet();
    test_bits_no_room();
    test_bits_match_the_rule();
    test_bits_at_the_buffer_end();
    TEST_END;
}
