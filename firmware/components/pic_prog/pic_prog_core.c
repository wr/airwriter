// SPDX-License-Identifier: GPL-3.0-or-later
#include "pic_prog_core.h"
#include <stdio.h>
#include <string.h>

// ---- Intel HEX -------------------------------------------------------------

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

// Stores one byte at a hex byte address. Word = address / 2, low byte first.
static int put_byte(pic_image_t *img, uint32_t baddr, uint8_t b, uint8_t *seen_cfg)
{
    uint32_t w = baddr >> 1;
    int hi = baddr & 1;
    uint16_t *dst;

    if (w < PIC_PROG_WORDS)                      dst = &img->prog[w];
    else if (w == PIC_ADDR_CONFIG1)              { dst = &img->config1; *seen_cfg |= 1; }
    else if (w == PIC_ADDR_CONFIG2)              { dst = &img->config2; *seen_cfg |= 2; }
    else if (w >= 0x8000u && w <= PIC_ADDR_DEVID) return 0;  // user IDs, devid
    else                                         return -1;

    if (hi) *dst = (uint16_t)((*dst & 0x00FF) | ((b & 0x3F) << 8));
    else    *dst = (uint16_t)((*dst & 0x3F00) | b);
    return 0;
}

pic_hex_err_t pic_hex_parse(const char *hex, size_t len, pic_image_t *img, int *line)
{
    for (size_t i = 0; i < PIC_PROG_WORDS; i++) img->prog[i] = PIC_BLANK;
    img->config1 = img->config2 = PIC_BLANK;
    img->has_config = 0;

    uint8_t seen_cfg = 0;
    uint32_t base = 0;
    size_t p = 0;
    int ln = 0;

    while (p < len) {
        while (p < len && (hex[p] == '\r' || hex[p] == '\n' || hex[p] == ' ')) p++;
        if (p >= len) break;
        ln++;
        if (line) *line = ln;
        if (hex[p++] != ':') return PIC_HEX_SYNTAX;

        uint8_t rec[255 + 5];
        size_t n = 0;
        while (p < len && hex[p] != '\r' && hex[p] != '\n') {
            if (p + 1 >= len || n >= sizeof rec) return PIC_HEX_SYNTAX;
            int a = hexval(hex[p]), b = hexval(hex[p + 1]);
            if (a < 0 || b < 0) return PIC_HEX_SYNTAX;
            rec[n++] = (uint8_t)(a << 4 | b);
            p += 2;
        }
        if (n < 5 || n != (size_t)rec[0] + 5) return PIC_HEX_SYNTAX;

        uint8_t sum = 0;
        for (size_t i = 0; i < n; i++) sum += rec[i];
        if (sum != 0) return PIC_HEX_CHECKSUM;

        uint8_t count = rec[0], type = rec[3];
        uint32_t off = (uint32_t)rec[1] << 8 | rec[2];
        switch (type) {
        case 0x00:
            for (uint8_t i = 0; i < count; i++)
                if (put_byte(img, base + off + i, rec[4 + i], &seen_cfg) < 0)
                    return PIC_HEX_RANGE;
            break;
        case 0x01:
            img->has_config = seen_cfg == 3;
            return PIC_HEX_OK;
        case 0x04:
            if (count != 2) return PIC_HEX_SYNTAX;
            base = ((uint32_t)rec[4] << 8 | rec[5]) << 16;
            break;
        default:   // 02/03/05 never appear in a PIC hex; tolerate rather than guess
            break;
        }
    }
    return PIC_HEX_NO_EOF;
}

void pic_image_enable_lvp(pic_image_t *img)
{
    img->config2 |= PIC_CONFIG2_LVP;
}

// ---- ICSP wire protocol ------------------------------------------------------
//
// Commands are six bits, data words sixteen clocks (start, 14 bits, stop), all
// LSb first. The programmer changes DAT while CLK is high and the PIC latches
// it on the falling edge (section 4.0). Every half-clock gets a microsecond,
// ten times TCKH/TCKL, so slow GPIO and the 100R series resistors on the board
// have nothing to argue about.

enum {
    CMD_LOAD_CONFIG   = 0x00,
    CMD_LOAD_PROG     = 0x02,
    CMD_READ_PROG     = 0x04,
    CMD_INC_ADDR      = 0x06,
    CMD_RESET_ADDR    = 0x16,
    CMD_BEGIN_INT     = 0x08,
    CMD_BULK_ERASE    = 0x09,
};

#define T_HALF_US   1
#define T_DLY_US    2       // TDLY >= 1 us between command and data
#define T_ENTH_US   500     // TENTH >= 250 us after MCLR falls
#define T_PINT_PROG 3000    // TPINT 2.5 ms max, program memory
#define T_PINT_CFG  6000    // TPINT 5 ms max, configuration words
#define T_ERAB_US   10000   // TERAB 5 ms max
#define T_EXIT_US   10

static const uint32_t LVP_KEY = 0x4D434850u;   // "MCHP"

static void clock_out(const pic_hal_t *h, uint32_t bits, int n)
{
    for (int i = 0; i < n; i++) {
        h->clk(h->ctx, 1);
        h->dat_out(h->ctx, (int)((bits >> i) & 1));
        h->delay_us(h->ctx, T_HALF_US);
        h->clk(h->ctx, 0);
        h->delay_us(h->ctx, T_HALF_US);
    }
}

static void command(const pic_hal_t *h, uint8_t cmd)
{
    clock_out(h, cmd, 6);
    h->delay_us(h->ctx, T_DLY_US);
}

static void load_word(const pic_hal_t *h, uint8_t cmd, uint16_t w)
{
    command(h, cmd);
    clock_out(h, (uint32_t)(w & 0x3FFF) << 1, 16);   // start bit 0, stop bit 0
    h->delay_us(h->ctx, T_DLY_US);
}

static uint16_t read_word(const pic_hal_t *h)
{
    command(h, CMD_READ_PROG);
    h->dat_release(h->ctx);
    uint16_t w = 0;
    // The PIC drives its 14 data bits after rising edges 2..15 (Fig. 4-4).
    for (int i = 0; i < 16; i++) {
        h->clk(h->ctx, 1);
        h->delay_us(h->ctx, T_HALF_US);
        if (i >= 1 && i <= 14 && h->dat_in(h->ctx)) w |= (uint16_t)(1u << (i - 1));
        h->clk(h->ctx, 0);
        h->delay_us(h->ctx, T_HALF_US);
    }
    h->dat_out(h->ctx, 0);
    h->delay_us(h->ctx, T_DLY_US);
    return w;
}

// MCLR goes low before CLK or DAT is driven: while the part runs, RA0 is
// TashTalk's UART TX, and driving it from here would be a short through R16.
// No rising edge first -- key matching starts without one (Fig. 8-9, note 1).
static void enter_lvp(const pic_hal_t *h)
{
    h->mclr(h->ctx, 0);
    h->clk(h->ctx, 0);
    h->dat_out(h->ctx, 0);
    h->delay_us(h->ctx, T_ENTH_US);
    clock_out(h, LVP_KEY, 32);
    clock_out(h, 0, 1);                 // the 33rd clock (Fig. 8-8)
    h->delay_us(h->ctx, T_DLY_US);
}

// Leaves Program/Verify mode by letting MCLR rise. DAT is released first for
// the same reason as above: the part starts running TashTalk straight away.
static void exit_lvp(const pic_hal_t *h)
{
    h->clk(h->ctx, 0);
    h->dat_release(h->ctx);
    h->mclr(h->ctx, 1);
    h->delay_us(h->ctx, T_EXIT_US);
}

static void inc(const pic_hal_t *h, unsigned n)
{
    while (n--) command(h, CMD_INC_ADDR);
}

// PC must be at 0x8000 (just after Load Configuration).
static uint16_t read_devid_from_8000(const pic_hal_t *h)
{
    inc(h, PIC_ADDR_DEVID - 0x8000u);
    return read_word(h);
}

static pic_err_t classify_id(uint16_t id)
{
    if (id == 0x0000 || id == 0x3FFF) return PIC_ERR_NO_DEVICE;
    if ((id & PIC_DEVID_MASK) != PIC_DEVID_12F1840) return PIC_ERR_WRONG_DEVICE;
    return PIC_OK;
}

pic_err_t pic_read_id(const pic_hal_t *h, uint16_t *devid)
{
    enter_lvp(h);
    load_word(h, CMD_LOAD_CONFIG, PIC_BLANK);
    uint16_t id = read_devid_from_8000(h);
    exit_lvp(h);
    if (devid) *devid = id;
    return classify_id(id);
}

static int row_blank(const uint16_t *w)
{
    for (unsigned i = 0; i < PIC_ROW_WORDS; i++)
        if (w[i] != PIC_BLANK) return 0;
    return 1;
}

static pic_err_t mismatch(pic_result_t *r, uint32_t addr, uint16_t want, uint16_t got)
{
    r->err = PIC_ERR_VERIFY;
    r->bad_addr = addr;
    r->want = want;
    r->got = got;
    return PIC_ERR_VERIFY;
}

static pic_err_t flash_body(const pic_hal_t *h, const pic_image_t *img, pic_result_t *r)
{
    // Identify. Load Configuration leaves the PC at 0x8000, and from there
    // Bulk Erase also takes the configuration words and user IDs (4.3.11).
    load_word(h, CMD_LOAD_CONFIG, PIC_BLANK);
    r->devid = read_devid_from_8000(h);
    r->err = classify_id(r->devid);
    if (r->err != PIC_OK) return r->err;

    command(h, CMD_BULK_ERASE);
    h->delay_us(h->ctx, T_ERAB_US);

    // Program memory, a row of latches at a time. The PC when Begin
    // Programming arrives picks the row, so blank rows are stepped over.
    command(h, CMD_RESET_ADDR);
    for (uint32_t row = 0; row < PIC_PROG_WORDS; row += PIC_ROW_WORDS) {
        const uint16_t *w = &img->prog[row];
        if (row_blank(w)) { inc(h, PIC_ROW_WORDS); continue; }
        for (unsigned i = 0; i < PIC_ROW_WORDS; i++) {
            load_word(h, CMD_LOAD_PROG, w[i]);
            if (i + 1 < PIC_ROW_WORDS) command(h, CMD_INC_ADDR);
        }
        command(h, CMD_BEGIN_INT);
        h->delay_us(h->ctx, T_PINT_PROG);
        command(h, CMD_INC_ADDR);
    }

    // Verify every word, blank or not: that checks the erase too.
    command(h, CMD_RESET_ADDR);
    for (uint32_t a = 0; a < PIC_PROG_WORDS; a++) {
        uint16_t got = read_word(h);
        if (got != img->prog[a]) return mismatch(r, a, img->prog[a], got);
        command(h, CMD_INC_ADDR);
    }

    // Configuration words last, as Figure 5-1 orders it, one word per cycle.
    load_word(h, CMD_LOAD_CONFIG, PIC_BLANK);
    inc(h, PIC_ADDR_CONFIG1 - 0x8000u);
    load_word(h, CMD_LOAD_PROG, img->config1);
    command(h, CMD_BEGIN_INT);
    h->delay_us(h->ctx, T_PINT_CFG);
    uint16_t got = read_word(h);
    if ((got ^ img->config1) & PIC_CONFIG1_MASK)
        return mismatch(r, PIC_ADDR_CONFIG1, img->config1, got);

    command(h, CMD_INC_ADDR);
    load_word(h, CMD_LOAD_PROG, img->config2);
    command(h, CMD_BEGIN_INT);
    h->delay_us(h->ctx, T_PINT_CFG);
    got = read_word(h);
    if ((got ^ img->config2) & PIC_CONFIG2_MASK)
        return mismatch(r, PIC_ADDR_CONFIG2, img->config2, got);

    // Read the whole configuration block back for the log. The two
    // single-word writes above should leave the user IDs blank and can't
    // touch the calibration words; this is where a surprise would show.
    load_word(h, CMD_LOAD_CONFIG, PIC_BLANK);
    for (unsigned i = 0; i < PIC_CFG_WORDS; i++) {
        r->cfg[i] = read_word(h);
        command(h, CMD_INC_ADDR);
    }
    return PIC_OK;
}

pic_err_t pic_flash(const pic_hal_t *h, const pic_image_t *img, pic_result_t *r)
{
    memset(r, 0, sizeof *r);
    if (!img->has_config) return r->err = PIC_ERR_IMAGE;

    enter_lvp(h);
    pic_err_t e = flash_body(h, img, r);
    exit_lvp(h);
    r->err = e;
    return e;
}

// Whole ticks, rounded up, plus one: the first tick of a sleep is partial and
// can be any fraction of a period. Rounding to milliseconds first is wrong at
// 100 Hz, where pdMS_TO_TICKS() makes 3 and 6 ms zero ticks and a TPINT wait
// became "until the next tick", 0-10 ms (W-1035).
uint32_t pic_sleep_ticks(uint32_t us, uint32_t tick_us)
{
    if (us < 1000) return 0;
    return (us + tick_us - 1) / tick_us + 1;
}

const char *pic_err_str(pic_err_t e)
{
    switch (e) {
    case PIC_OK:               return "ok";
    case PIC_ERR_NO_DEVICE:    return "no PIC answered, or its LVP bit is off";
    case PIC_ERR_WRONG_DEVICE: return "not a PIC12F1840";
    case PIC_ERR_VERIFY:       return "verify failed";
    case PIC_ERR_IMAGE:        return "image has no configuration words";
    case PIC_ERR_UNAVAILABLE:  return "no ICSP pins in this build";
    case PIC_ERR_BUSY:         return "already flashing";
    case PIC_ERR_NOMEM:        return "out of memory";
    }
    return "?";
}

void pic_result_json(const pic_result_t *r, char *buf, size_t cap)
{
    int n = snprintf(buf, cap, "{\"ok\":%s,\"err\":\"%s\",\"devid\":\"0x%04X\",\"rev\":%u",
                     r->err == PIC_OK ? "true" : "false", pic_err_str(r->err),
                     r->devid, r->devid & 0x1Fu);
    if (n < 0 || (size_t)n >= cap) return;
    if (r->err == PIC_ERR_VERIFY)
        n += snprintf(buf + n, cap - (size_t)n,
                      ",\"addr\":\"0x%04X\",\"want\":\"0x%04X\",\"got\":\"0x%04X\"",
                      (unsigned)r->bad_addr, r->want, r->got);
    if (n >= 0 && (size_t)n < cap) snprintf(buf + n, cap - (size_t)n, "}");
}
