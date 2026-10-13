// SPDX-License-Identifier: GPL-3.0-or-later
#include "pic_sim.h"
#include <string.h>

#define KEY 0x4D434850u

void pic_sim_init(pic_sim_t *s, uint16_t devid)
{
    memset(s, 0, sizeof *s);
    for (unsigned i = 0; i < PIC_PROG_WORDS; i++) s->prog[i] = PIC_BLANK;
    for (unsigned i = 0; i < 11; i++) s->cfg[i] = PIC_BLANK;
    s->cfg[6]  = devid;
    s->cfg[9]  = 0x28D7;   // factory calibration words, as read off a real part
    s->cfg[10] = 0x1D48;
    s->mclr = 1;           // running, as on a powered board
    s->stuck_addr = UINT32_MAX;
}

int pic_sim_errors(const pic_sim_t *s)
{
    return s->err_contention + s->err_busy + s->err_tdly + s->err_tenth + s->err_unknown_cmd;
}

static uint16_t read_mem(const pic_sim_t *s, uint32_t pc)
{
    uint16_t v;
    if (pc < 0x8000u)            v = s->prog[pc & (PIC_PROG_WORDS - 1)];
    else if (pc - 0x8000u <= 10) v = s->cfg[pc - 0x8000u];
    else                         v = 0;
    if (pc == s->stuck_addr) v &= (uint16_t)~s->stuck_mask;
    return v;
}

static void erase(pic_sim_t *s, int user_ids)
{
    for (unsigned i = 0; i < PIC_PROG_WORDS; i++) s->prog[i] = PIC_BLANK;
    s->cfg[7] = s->cfg[8] = PIC_BLANK;
    if (user_ids) for (int i = 0; i < 4; i++) s->cfg[i] = PIC_BLANK;
}

static void execute(pic_sim_t *s, uint8_t cmd, uint16_t data)
{
    switch (cmd) {
    case 0x00:                                  // Load Configuration
        s->pc = 0x8000;
        s->latch[0] = data;
        break;
    case 0x02:                                  // Load Data For Program Memory
        s->latch[s->pc & (PIC_ROW_WORDS - 1)] = data;
        s->latched[s->pc & (PIC_ROW_WORDS - 1)] = 1;
        break;
    case 0x04:                                  // Read Data From Program Memory
        break;                                  // (handled while clocking)
    case 0x06:                                  // Increment Address
        s->pc = s->pc == 0x7FFFu ? 0 : s->pc == 0xFFFFu ? 0x8000u : s->pc + 1;
        break;
    case 0x16:                                  // Reset Address
        s->pc = 0;
        break;
    case 0x08:                                  // Begin Internally Timed Programming
        if (s->pc < 0x8000u) {
            uint32_t row = (s->pc & (PIC_PROG_WORDS - 1)) & ~(PIC_ROW_WORDS - 1);
            for (unsigned i = 0; i < PIC_ROW_WORDS; i++)
                if (s->latched[i]) s->prog[row + i] &= s->latch[i];  // no erase
            s->busy_until = s->now_us + 2500;
        } else {
            uint32_t a = s->pc - 0x8000u;
            uint16_t v = s->latch[s->pc & (PIC_ROW_WORDS - 1)];
            if (a == 8) v |= PIC_CONFIG2_LVP;   // LVP can't be cleared from LVP (Note 1, Reg 3-3)
            if (a <= 3 || a == 7 || a == 8) s->cfg[a] &= v;
            s->busy_until = s->now_us + 5000;
        }
        memset(s->latched, 0, sizeof s->latched);
        break;
    case 0x09:                                  // Bulk Erase Program Memory
        if (s->pc >= 0x8000u && s->pc <= 0x8008u) erase(s, 1);
        else if (s->pc < 0x8000u)                 erase(s, 0);
        s->busy_until = s->now_us + 5000;
        break;
    default:
        s->err_unknown_cmd++;
        break;
    }
}

static int has_payload(uint8_t cmd)
{
    return cmd == 0x00 || cmd == 0x02 || cmd == 0x04;
}

static void clk_rise(pic_sim_t *s)
{
    if (!s->in_prog) {
        if (!s->mclr && s->key_bits == 0 && s->now_us - s->mclr_fell_at < 250)
            s->err_tenth++;
        return;
    }
    if (s->now_us < s->busy_until) s->err_busy++;
    if (s->awaiting_payload_edge) {
        if (s->now_us - s->last_fall_at < 1) s->err_tdly++;
        s->awaiting_payload_edge = 0;
    }
    if (s->payload_clocks && s->reading) {
        int k = 16 - s->payload_clocks + 1;      // this is rising edge k of 16
        if (k >= 2 && k <= 15) {
            s->dat_dev = (read_mem(s, s->pc) >> (k - 2)) & 1;
            s->dev_driving = 1;
        }
    }
}

static void clk_fall(pic_sim_t *s)
{
    int bit = s->dat_driving ? s->dat_prog : 0;
    s->last_fall_at = s->now_us;

    if (!s->in_prog) {
        if (s->mclr) return;
        if (s->key_bits < 32) {
            s->key_sr = (s->key_sr >> 1) | ((uint32_t)bit << 31);
            s->key_bits++;
        } else if (s->key_bits == 32) {
            s->key_bits++;
            if (s->key_sr == KEY) { s->in_prog = 1; s->pc = 0; s->lvp_entries++; }
        }
        return;
    }

    if (s->payload_clocks) {
        int c = 16 - s->payload_clocks;          // falling edge c+1 of 16
        if (s->reading) {
            if (c == 0) s->dev_driving = 1;      // output from the first falling edge
        } else {
            s->payload |= (uint16_t)(bit << c);
        }
        if (--s->payload_clocks == 0) {
            if (s->reading) s->dev_driving = 0;  // input again after the 16th
            else execute(s, s->pending, (s->payload >> 1) & 0x3FFF);
            s->awaiting_payload_edge = 1;
        }
        return;
    }

    s->cmd |= (uint8_t)(bit << s->cmd_bits);
    if (++s->cmd_bits < 6) return;
    uint8_t cmd = s->cmd;
    s->cmd = 0;
    s->cmd_bits = 0;
    s->awaiting_payload_edge = 1;
    if (has_payload(cmd)) {
        s->pending = cmd;
        s->payload_clocks = 16;
        s->payload = 0;
        s->reading = cmd == 0x04;
    } else {
        execute(s, cmd, 0);
    }
}

// ---- HAL ----------------------------------------------------------------------

static void h_mclr(void *ctx, int level)
{
    pic_sim_t *s = ctx;
    if (s->mclr == level) return;
    s->mclr = level;
    s->in_prog = 0;
    s->key_sr = 0;
    s->key_bits = 0;
    s->cmd = s->cmd_bits = 0;
    s->payload_clocks = 0;
    s->dev_driving = 0;
    if (!level) s->mclr_fell_at = s->now_us;
}

static void h_clk(void *ctx, int level)
{
    pic_sim_t *s = ctx;
    if (s->absent || s->clk == level) { s->clk = level; return; }
    s->clk = level;
    if (level) clk_rise(s); else clk_fall(s);
}

static void h_dat_out(void *ctx, int level)
{
    pic_sim_t *s = ctx;
    if (s->dev_driving) s->err_contention++;
    s->dat_driving = 1;
    s->dat_prog = level;
}

static void h_dat_release(void *ctx)
{
    ((pic_sim_t *)ctx)->dat_driving = 0;
}

static int h_dat_in(void *ctx)
{
    pic_sim_t *s = ctx;
    if (s->absent) return 0;
    if (s->dev_driving) return s->dat_dev;
    return s->dat_driving ? s->dat_prog : 0;
}

static void h_delay(void *ctx, uint32_t us)
{
    ((pic_sim_t *)ctx)->now_us += us;
}

pic_hal_t pic_sim_hal(pic_sim_t *s)
{
    pic_hal_t h = { h_mclr, h_clk, h_dat_out, h_dat_release, h_dat_in, h_delay, s };
    return h;
}
