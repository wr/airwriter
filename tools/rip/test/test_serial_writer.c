// SPDX-License-Identifier: GPL-3.0-or-later
//
// The serial_out write loop, off the hardware. The fake models what
// uart_tx_chars really gives us: a hardware FIFO that accepts what fits and
// reports the count. The printer can pause (drain_after) and resume. The loop
// must ride out any pause shorter than stall_limit delays without progress,
// and give up only after that -- a paused job (paper out, SELECT) resumes.
#include <stdlib.h>
#include "test_util.h"
#include "serial_writer.h"

#define FAKE_FIFO 8

typedef struct {
    uint8_t fifo_taken[4096];  // every byte the "FIFO" accepted, in order
    size_t  taken;
    size_t  free;              // FIFO space right now
    size_t  drain_per_delay;   // bytes the "printer" absorbs each delay period
    int     drain_after;       // delay periods before the printer starts draining
    int     delays;
    int     writes;
    int     write_err;         // force tx_write to fail
    int     abort_after;       // delay periods until the job is cancelled, 0 never
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

static void f_delay(void *ctx)
{
    fake_t *f = (fake_t *)ctx;
    if (f->delays > 100000) {
        fprintf(stderr, "FAIL fake: writer is spinning forever\n");
        exit(1);
    }
    f->delays++;
    if (f->delays > f->drain_after) f->free += f->drain_per_delay;
    if (f->free > FAKE_FIFO) f->free = FAKE_FIFO;
}

static int f_aborted(void *ctx)
{
    fake_t *f = (fake_t *)ctx;
    return f->abort_after > 0 && f->delays >= f->abort_after;
}

static serial_writer_ops_t ops_for(fake_t *f, unsigned stall_limit)
{
    serial_writer_ops_t ops = {
        .ctx         = f,
        .tx_write    = f_tx_write,
        .delay       = f_delay,
        .aborted     = f_aborted,
        .stall_limit = stall_limit,
    };
    return ops;
}

int main(void)
{
    // A payload that fits the FIFO goes through in one write, no delays.
    {
        fake_t f = { .free = FAKE_FIFO };
        serial_writer_ops_t ops = ops_for(&f, 5);
        const uint8_t msg[] = "hello";
        CHECK_EQ_INT(serial_writer_write(&ops, msg, 5), 0);
        CHECK_EQ_INT(f.taken, 5);
        CHECK_EQ_MEM(f.fifo_taken, msg, 5);
        CHECK_EQ_INT(f.writes, 1);
        CHECK_EQ_INT(f.delays, 0);
    }

    // A payload bigger than the FIFO drains through in order, paced.
    {
        fake_t f = { .free = FAKE_FIFO, .drain_per_delay = 4 };
        serial_writer_ops_t ops = ops_for(&f, 5);
        uint8_t msg[20];
        for (size_t i = 0; i < sizeof msg; i++) msg[i] = (uint8_t)i;
        CHECK_EQ_INT(serial_writer_write(&ops, msg, sizeof msg), 0);
        CHECK_EQ_INT(f.taken, sizeof msg);
        CHECK_EQ_MEM(f.fifo_taken, msg, sizeof msg);
        CHECK(f.delays > 0);
        CHECK(f.writes <= f.delays + 1);
    }

    // A full FIFO that never drains: poll stall_limit times, then give up.
    {
        fake_t f = { .free = 0, .drain_after = 1 << 30 };
        serial_writer_ops_t ops = ops_for(&f, 3);
        const uint8_t msg[] = "hello";
        CHECK_EQ_INT(serial_writer_write(&ops, msg, 5), -1);
        CHECK_EQ_INT(f.taken, 0);
        CHECK_EQ_INT(f.delays, 3);
        CHECK_EQ_INT(f.writes, 4);
    }

    // A fresh write to a printer that has been dead a long time still waits
    // the full limit: nothing is remembered across calls, so a job sent while
    // the printer is off gets its whole pause, not an instant failure.
    {
        fake_t f = { .free = 0, .drain_after = 1 << 30 };
        serial_writer_ops_t ops = ops_for(&f, 3);
        const uint8_t msg[] = "hello";
        CHECK_EQ_INT(serial_writer_write(&ops, msg, 5), -1);
        f.delays = 0;
        f.writes = 0;
        CHECK_EQ_INT(serial_writer_write(&ops, msg, 5), -1);
        CHECK_EQ_INT(f.delays, 3);
    }

    // The printer dies mid-stream: the delay after real progress does not
    // count; then stall_limit delays with nothing taken, then give up.
    {
        fake_t f = { .free = 4, .drain_after = 1 << 30 };
        serial_writer_ops_t ops = ops_for(&f, 2);
        uint8_t msg[20] = {0};
        CHECK_EQ_INT(serial_writer_write(&ops, msg, sizeof msg), -1);
        CHECK_EQ_INT(f.taken, 4);
        CHECK_EQ_INT(f.delays, 3);
    }

    // Paper out and back: a 50-period pause inside a 100-period limit resumes
    // and delivers every byte in order.
    {
        fake_t f = { .free = 0, .drain_after = 50, .drain_per_delay = 4 };
        serial_writer_ops_t ops = ops_for(&f, 100);
        uint8_t msg[20];
        for (size_t i = 0; i < sizeof msg; i++) msg[i] = (uint8_t)(0xA0 + i);
        CHECK_EQ_INT(serial_writer_write(&ops, msg, sizeof msg), 0);
        CHECK_EQ_INT(f.taken, sizeof msg);
        CHECK_EQ_MEM(f.fifo_taken, msg, sizeof msg);
        CHECK(f.delays > 50);
    }

    // A pause longer than the limit fails.
    {
        fake_t f = { .free = 0, .drain_after = 50, .drain_per_delay = 4 };
        serial_writer_ops_t ops = ops_for(&f, 10);
        const uint8_t msg[] = "hello";
        CHECK_EQ_INT(serial_writer_write(&ops, msg, 5), -1);
        CHECK_EQ_INT(f.delays, 10);
    }

    // A slow trickle (head returns) never trips the limit: any progress resets it.
    {
        fake_t f = { .free = 0, .drain_per_delay = 1 };
        serial_writer_ops_t ops = ops_for(&f, 1);
        const uint8_t msg[] = "hello";
        CHECK_EQ_INT(serial_writer_write(&ops, msg, 5), 0);
        CHECK_EQ_INT(f.taken, 5);
    }

    // stall_limit 0 means never give up (with a printer that does come back).
    {
        fake_t f = { .free = 0, .drain_after = 500, .drain_per_delay = 8 };
        serial_writer_ops_t ops = ops_for(&f, 0);
        const uint8_t msg[] = "hello";
        CHECK_EQ_INT(serial_writer_write(&ops, msg, 5), 0);
    }

    // Cancel-Job on a paused printer: the write gives up as soon as the job
    // is aborted, not after the 10-minute stall limit.
    {
        fake_t f = { .free = 0, .drain_after = 1 << 30, .abort_after = 5 };
        serial_writer_ops_t ops = ops_for(&f, 6000);
        const uint8_t msg[] = "hello";
        CHECK_EQ_INT(serial_writer_write(&ops, msg, 5), -1);
        CHECK_EQ_INT(f.delays, 5);
    }

    // A cancel mid-stream stops a draining write too.
    {
        fake_t f = { .free = FAKE_FIFO, .drain_per_delay = 2, .abort_after = 3 };
        serial_writer_ops_t ops = ops_for(&f, 6000);
        uint8_t msg[64] = {0};
        CHECK_EQ_INT(serial_writer_write(&ops, msg, sizeof msg), -1);
        CHECK(f.taken < sizeof msg);
    }

    // Driver failure is fatal, not retried.
    {
        fake_t f = { .free = FAKE_FIFO, .write_err = 1 };
        serial_writer_ops_t ops = ops_for(&f, 5);
        const uint8_t msg[] = "hello";
        CHECK_EQ_INT(serial_writer_write(&ops, msg, 5), -1);
        CHECK_EQ_INT(f.writes, 1);
    }

    // Nothing to send is success and touches nothing.
    {
        fake_t f = { .free = 0 };
        serial_writer_ops_t ops = ops_for(&f, 5);
        CHECK_EQ_INT(serial_writer_write(&ops, NULL, 0), 0);
        CHECK_EQ_INT(f.writes, 0);
        CHECK_EQ_INT(f.delays, 0);
    }

    TEST_END;
}
