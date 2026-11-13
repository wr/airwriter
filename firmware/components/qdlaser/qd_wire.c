// SPDX-License-Identifier: GPL-3.0-or-later
// W-1102: QuickDraw LaserWriter packets and the line image they become (qd_wire.h).
#include "qd_wire.h"

size_t qd_cmd(uint8_t cmd, uint8_t arg, uint8_t out[3])
{
    out[0] = cmd;
    out[1] = arg;
    out[2] = (uint8_t)(cmd ^ arg);
    return 3;
}

static int ones(uint8_t v)
{
    int n = 0;
    for (; v; v &= (uint8_t)(v - 1)) n++;
    return n;
}

uint8_t qd_value(uint8_t v)
{
    uint8_t b = (uint8_t)(v << 1);
    return (ones(b) & 1) ? b : (uint8_t)(b | 1);
}

int qd_reply_ok(uint8_t r)
{
    return !(r & 0x80) && (ones(r) & 1);
}

size_t qd_row_packet(const uint8_t *codes, size_t n, uint8_t *out)
{
    if (n == 0 || n > QD_ROW_MAX) return 0;
    unsigned len = 0x5000u - (unsigned)n;
    out[0] = (uint8_t)(len >> 8);
    out[1] = (uint8_t)len;
    uint8_t x = out[0] ^ out[1];
    for (size_t i = 0; i < n; i++) {
        out[2 + i] = codes[i];
        x ^= codes[i];
    }
    out[2 + n] = x;
    return n + 3;
}

size_t qd_bits_len(size_t nbytes)
{
    return QD_BREAK_BITS + QD_IDLE_BITS + 10 * nbytes + QD_GAP_BITS;
}

static void put_bit(uint8_t *buf, size_t k, int v)
{
    uint8_t m = (uint8_t)(1u << (k & 7));
    if (v) buf[k >> 3] |= m;
    else buf[k >> 3] &= (uint8_t)~m;
}

// A run of one level, a byte at a time where it can.
static void put_run(uint8_t *buf, size_t *pos, size_t n, int v)
{
    size_t k = *pos, end = k + n;
    while (k < end && (k & 7)) put_bit(buf, k++, v);
    while (k + 8 <= end) {
        buf[k >> 3] = v ? 0xFF : 0x00;
        k += 8;
    }
    while (k < end) put_bit(buf, k++, v);
    *pos = end;
}

int qd_bits_put(uint8_t *buf, size_t cap_bits, size_t *pos, const uint8_t *pkt, size_t n)
{
    if (*pos > cap_bits || cap_bits - *pos < qd_bits_len(n)) return -1;
    put_run(buf, pos, QD_BREAK_BITS, 0);
    put_run(buf, pos, QD_IDLE_BITS, 1);
    // Each byte is ten line bits, LSB first: a start space, its eight bits, a stop mark. They
    // go into the line a whole byte at a time from an accumulator (W-1143), keeping the bits
    // of the first byte below *pos and of the last byte past the packet as they were.
    size_t k = *pos;
    uint8_t *p = buf + (k >> 3);
    unsigned have = (unsigned)(k & 7);
    uint32_t acc = *p & ((1u << have) - 1);
    for (size_t i = 0; i < n; i++) {
        acc |= (0x200u | (uint32_t)pkt[i] << 1) << have;
        have += 10;
        while (have >= 8) {
            *p++ = (uint8_t)acc;
            acc >>= 8;
            have -= 8;
        }
    }
    if (have) {
        uint8_t m = (uint8_t)((1u << have) - 1);
        *p = (uint8_t)((*p & ~m) | (acc & m));
    }
    *pos = k + 10 * n;
    put_run(buf, pos, QD_GAP_BITS, 1);
    return 0;
}

void qd_bits_marks(uint8_t *buf, size_t cap_bits, size_t *pos, size_t n)
{
    if (*pos >= cap_bits) return;
    if (n > cap_bits - *pos) n = cap_bits - *pos;
    put_run(buf, pos, n, 1);
}
