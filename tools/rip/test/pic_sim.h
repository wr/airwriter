// SPDX-License-Identifier: GPL-3.0-or-later
// A PIC12F1840 at the pin level, for host-testing pic_prog_core: LVP key entry,
// the 13-command ICSP state machine, row latches, internally timed writes and
// the timing rules a real part would punish (DS41439A).
#pragma once
#include <stdint.h>
#include "pic_prog_core.h"

typedef struct {
    // Memory
    uint16_t prog[PIC_PROG_WORDS];
    uint16_t cfg[11];             // 0x8000..0x800A
    uint16_t latch[PIC_ROW_WORDS];
    uint8_t  latched[PIC_ROW_WORDS];

    // Pins and time
    int mclr, clk, dat_prog, dat_driving, dat_dev, dev_driving;
    uint64_t now_us, busy_until, mclr_fell_at, last_fall_at;

    // ICSP state
    int      in_prog;
    uint32_t key_sr;
    int      key_bits;
    uint32_t pc;
    uint8_t  cmd, cmd_bits;      // command being clocked in
    uint8_t  pending;            // command whose data word is being clocked
    int      payload_clocks;      // >0 while a data word is being clocked
    uint16_t payload;
    int      reading;
    int      awaiting_payload_edge;

    // Fault injection
    int      absent;              // nothing on the wires: DAT reads low
    uint32_t stuck_addr;          // this address reads with stuck_mask bits low
    uint16_t stuck_mask;

    // Rule breaks seen; a well-behaved programmer leaves these at zero
    int err_contention;           // programmer drove DAT while the PIC did,
                                  // or while it was out of reset (RA0 = UART TX)
    int err_busy;                 // clocked during TPINT or TERAB
    int err_tdly;                 // payload started inside TDLY
    int err_tenth;                // key started inside TENTH
    int err_unknown_cmd;
    int lvp_entries;
} pic_sim_t;

void pic_sim_init(pic_sim_t *s, uint16_t devid);
pic_hal_t pic_sim_hal(pic_sim_t *s);
int pic_sim_errors(const pic_sim_t *s);
