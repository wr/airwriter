// SPDX-License-Identifier: GPL-3.0-or-later
#include "serial_writer.h"

int serial_writer_write(const serial_writer_ops_t *ops, const uint8_t *buf, size_t n)
{
    while (n) {
        if (ops->blocked(ops->ctx)) return -1;
        int w = ops->tx_write(ops->ctx, buf, n);
        if (w < 0) return -1;
        buf += w;
        n   -= (size_t)w;
        if (n) ops->delay(ops->ctx);
    }
    return 0;
}
