// SPDX-License-Identifier: GPL-3.0-or-later
#include "serial_writer.h"

int serial_writer_write(const serial_writer_ops_t *ops, const uint8_t *buf, size_t n)
{
    unsigned idle = 0;
    while (n) {
        int w = ops->tx_write(ops->ctx, buf, n);
        if (w < 0) return -1;
        buf += w;
        n   -= (size_t)w;
        if (!n) break;
        if (w > 0) idle = 0;
        else if (ops->stall_limit && ++idle > ops->stall_limit) return -1;
        if (ops->aborted && ops->aborted(ops->ctx)) return -1;
        ops->delay(ops->ctx);
    }
    return 0;
}
