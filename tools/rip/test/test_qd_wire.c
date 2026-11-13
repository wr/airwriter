// SPDX-License-Identifier: GPL-3.0-or-later
// W-1102: what goes down the wire to a QuickDraw LaserWriter: command and value packets, a row
// packet with its length and check, and the line image a packet becomes (BREAK, idle, 8N1
// bytes LSB first, marks), which the link clocks out one bit per printer clock.
#include "test_util.h"
#include "qd_wire.h"

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

int main(void)
{
    test_cmd();
    test_value();
    test_reply_ok();
    test_row_packet();
    test_bits_one_packet();
    test_bits_no_room();
    TEST_END;
}
