// SPDX-License-Identifier: GPL-3.0-or-later
//
// The serial_out write loop, off the hardware. The fake models what
// uart_tx_chars really gives us: a hardware FIFO that accepts what fits and
// reports the count -- the return value is ground truth, there is no free-
// space probe to go stale. (The first cut of this loop bounded writes with
// uart_get_tx_buffer_free_size, which only tracks the ring item currently in
// service; review traced a CTS stall re-wedging the write inside the driver.
// Ring gone, probe gone.) The watchdog verdict flips after a set number of
// poll periods.
#include <stdlib.h>
#include "test_util.h"
#include "serial_writer.h"

#define FAKE_FIFO 8

typedef struct {
    uint8_t fifo_taken[4096];  // every byte the "FIFO" accepted, in order
    size_t  taken;
    size_t  free;              // FIFO space right now
    size_t  drain_per_delay;   // bytes the "printer" absorbs each delay period
    int     blocked_after;     // delay periods until watchdog says BLOCKED, -1 never
    int     delays;
    int     writes;
    int     write_err;         // force tx_write to fail
} fake_t;

static int f_tx_write(void *ctx, const uint8_t *buf, size_t n)
{
    fake_t *f = (fake_t *)ctx;
    f->writes++;
    if (f->write_err) return -1;
    size_t take = n < f->free ? n : f->free;
    memcpy(f->fifo_taken + f->taken, buf, take);
    f->taken += take;
    f->free  -= take;
    return (int)take;
}

static int f_blocked(void *ctx)
{
    fake_t *f = (fake_t *)ctx;
    return f->blocked_after >= 0 && f->delays >= f->blocked_after;
}

static void f_delay(void *ctx)
{
    fake_t *f = (fake_t *)ctx;
    if (f->delays > 1000) {
        fprintf(stderr, "FAIL fake: writer is spinning forever on a printer "
                        "that will never drain\n");
        exit(1);
    }
    f->delays++;
    f->free += f->drain_per_delay;
    if (f->free > FAKE_FIFO) f->free = FAKE_FIFO;
}

static serial_writer_ops_t ops_for(fake_t *f)
{
    serial_writer_ops_t ops = {
        .ctx      = f,
        .tx_write = f_tx_write,
        .blocked  = f_blocked,
        .delay    = f_delay,
    };
    return ops;
}

int main(void)
{
    // A payload that fits the FIFO goes through in one write, no delays.
    {
        fake_t f = { .free = FAKE_FIFO, .blocked_after = -1 };
        serial_writer_ops_t ops = ops_for(&f);
        const uint8_t msg[] = "hello";
        CHECK_EQ_INT(serial_writer_write(&ops, msg, 5), 0);
        CHECK_EQ_INT(f.taken, 5);
        CHECK_EQ_MEM(f.fifo_taken, msg, 5);
        CHECK_EQ_INT(f.writes, 1);
        CHECK_EQ_INT(f.delays, 0);
    }

    // A payload bigger than the FIFO drains through in order as the printer
    // absorbs bytes between polls -- and every write that leaves a remainder
    // is followed by a delay, never a hot retry.
    {
        fake_t f = { .free = FAKE_FIFO, .blocked_after = -1, .drain_per_delay = 4 };
        serial_writer_ops_t ops = ops_for(&f);
        uint8_t msg[20];
        for (size_t i = 0; i < sizeof msg; i++) msg[i] = (uint8_t)i;
        CHECK_EQ_INT(serial_writer_write(&ops, msg, sizeof msg), 0);
        CHECK_EQ_INT(f.taken, sizeof msg);
        CHECK_EQ_MEM(f.fifo_taken, msg, sizeof msg);
        CHECK(f.delays > 0);
        CHECK(f.writes <= f.delays + 1);   // paced, not spinning
    }

    // The watchdog already called the printer blocked: fail before touching
    // the FIFO, so a dead printer costs nothing per call.
    {
        fake_t f = { .free = FAKE_FIFO, .blocked_after = 0 };
        serial_writer_ops_t ops = ops_for(&f);
        const uint8_t msg[] = "hello";
        CHECK_EQ_INT(serial_writer_write(&ops, msg, 5), -1);
        CHECK_EQ_INT(f.writes, 0);
        CHECK_EQ_INT(f.delays, 0);
    }

    // A full FIFO that never drains: poll at the watchdog cadence until the
    // watchdog flips, then give up. This is the stalled-printer brick from
    // the audit -- the old loop sat inside uart_write_bytes forever.
    {
        fake_t f = { .free = 0, .blocked_after = 3 };
        serial_writer_ops_t ops = ops_for(&f);
        const uint8_t msg[] = "hello";
        CHECK_EQ_INT(serial_writer_write(&ops, msg, 5), -1);
        CHECK_EQ_INT(f.taken, 0);
        CHECK_EQ_INT(f.delays, 3);
        CHECK(f.writes <= f.delays + 1);   // paced while stalled too
    }

    // The printer dies mid-stream: what fit in the FIFO is gone, the rest is
    // abandoned once the watchdog flips.
    {
        fake_t f = { .free = 4, .blocked_after = 2 };
        serial_writer_ops_t ops = ops_for(&f);
        uint8_t msg[20] = {0};
        CHECK_EQ_INT(serial_writer_write(&ops, msg, sizeof msg), -1);
        CHECK_EQ_INT(f.taken, 4);
        CHECK_EQ_INT(f.delays, 2);
    }

    // A stall shorter than the watchdog timeout is a head return, not paper
    // out: the FIFO drains slowly but steadily and the write rides it out.
    {
        fake_t f = { .free = 0, .blocked_after = 10, .drain_per_delay = 1 };
        serial_writer_ops_t ops = ops_for(&f);
        const uint8_t msg[] = "hello";
        CHECK_EQ_INT(serial_writer_write(&ops, msg, 5), 0);
        CHECK_EQ_INT(f.taken, 5);
        CHECK(f.delays < 10);
    }

    // Driver failure is fatal, not retried.
    {
        fake_t f = { .free = FAKE_FIFO, .blocked_after = -1, .write_err = 1 };
        serial_writer_ops_t ops = ops_for(&f);
        const uint8_t msg[] = "hello";
        CHECK_EQ_INT(serial_writer_write(&ops, msg, 5), -1);
        CHECK_EQ_INT(f.writes, 1);
    }

    // Nothing to send is success and touches nothing.
    {
        fake_t f = { .free = 0, .blocked_after = 0 };
        serial_writer_ops_t ops = ops_for(&f);
        CHECK_EQ_INT(serial_writer_write(&ops, NULL, 0), 0);
        CHECK_EQ_INT(f.writes, 0);
        CHECK_EQ_INT(f.delays, 0);
    }

    TEST_END;
}
