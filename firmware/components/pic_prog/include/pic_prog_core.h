// SPDX-License-Identifier: GPL-3.0-or-later
// PIC12F1840 low-voltage (LVP) programmer, pure C. The ESP32 bit-bangs the
// three ICSP lines through pic_hal_t; tools/rip drives the same code against
// a simulated PIC. Protocol and timings are Microchip DS41439A (PIC16F/LF1847,
// PIC12F/LF1840 Memory Programming Specification).
#pragma once
#include <stddef.h>
#include <stdint.h>

#define PIC_PROG_WORDS   0x1000u   // 4 K words of program memory
#define PIC_ROW_WORDS    32u       // write latches per row (Table 4-2)
#define PIC_BLANK        0x3FFFu   // an erased 14-bit word
#define PIC_ADDR_CONFIG1 0x8007u
#define PIC_ADDR_CONFIG2 0x8008u
#define PIC_ADDR_DEVID   0x8006u
#define PIC_CFG_WORDS    11u       // 0x8000..0x800A: user IDs to calibration

#define PIC_DEVID_MASK   0x3FE0u   // DEV<8:0>; REV<4:0> is the low five bits
#define PIC_DEVID_12F1840 0x1B80u

// Bits that read back as written; the rest are unimplemented (Table 7-1).
#define PIC_CONFIG1_MASK 0x3FFFu
#define PIC_CONFIG2_MASK 0x3713u
#define PIC_CONFIG2_LVP  (1u << 13)

typedef struct {
    uint16_t prog[PIC_PROG_WORDS];  // PIC_BLANK where the hex says nothing
    uint16_t config1, config2;
    uint8_t  has_config;            // the hex carried both configuration words
} pic_image_t;

typedef enum {
    PIC_HEX_OK = 0,
    PIC_HEX_SYNTAX,     // not ':', odd length, bad hex digit
    PIC_HEX_CHECKSUM,   // a record's checksum byte is wrong
    PIC_HEX_RANGE,      // data outside program memory and the config words
    PIC_HEX_NO_EOF,     // ran out of input before the end-of-file record
} pic_hex_err_t;

// Parses an Intel HEX (INHX32) image. Two bytes per word, little-endian, at
// twice the word address. User IDs (0x8000-0x8003) and the device ID word are
// accepted and ignored; data EEPROM is rejected because nothing here writes it.
// *line gets the 1-based line of the first error.
pic_hex_err_t pic_hex_parse(const char *hex, size_t len, pic_image_t *img, int *line);

// Sets CONFIG2.LVP so the part can be programmed again without high voltage.
void pic_image_enable_lvp(pic_image_t *img);

// The three ICSP lines. MCLR and CLK are always outputs from the programmer;
// DAT turns around for reads, so the programmer releases it before the PIC
// drives it.
typedef struct {
    void (*mclr)(void *ctx, int level);
    void (*clk)(void *ctx, int level);
    void (*dat_out)(void *ctx, int level);   // drive DAT
    void (*dat_release)(void *ctx);          // stop driving DAT
    int  (*dat_in)(void *ctx);               // sample DAT
    void (*delay_us)(void *ctx, uint32_t us);
    void *ctx;
} pic_hal_t;

typedef enum {
    PIC_OK = 0,
    PIC_ERR_NO_DEVICE,     // ID read as all zeros or all ones: nothing answered,
                           // or a part with LVP off (only 9 V reaches it then)
    PIC_ERR_WRONG_DEVICE,  // something answered, but it is not a PIC12F1840
    PIC_ERR_VERIFY,        // read-back differs from the image
    PIC_ERR_IMAGE,         // the image has no configuration words
    // Raised by the ESP32 glue, never by the core:
    PIC_ERR_UNAVAILABLE,   // this build has no ICSP pins configured
    PIC_ERR_BUSY,          // another flash is already running
    PIC_ERR_NOMEM,
} pic_err_t;

typedef struct {
    pic_err_t err;
    uint16_t  devid;       // raw word at 0x8006 (REV in the low five bits)
    uint32_t  bad_addr;    // first mismatch: word address, or 0x8007/0x8008
    uint16_t  want, got;
    uint16_t  cfg[PIC_CFG_WORDS];  // 0x8000..0x800A as read back after success
} pic_result_t;

// Enters LVP, reads the ID, leaves programming mode. MCLR is left high and DAT
// released, so the part runs whatever it holds.
pic_err_t pic_read_id(const pic_hal_t *hal, uint16_t *devid);

// Identify, bulk erase, program, verify, write and verify the configuration
// words. Refuses to erase anything that is not a PIC12F1840. Exits as above.
pic_err_t pic_flash(const pic_hal_t *hal, const pic_image_t *img, pic_result_t *res);

// For a delay_us that sleeps on an RTOS tick of tick_us: the tick count that
// sleeps at least us, or 0 when the wait is short enough to spin instead.
uint32_t pic_sleep_ticks(uint32_t us, uint32_t tick_us);

const char *pic_err_str(pic_err_t e);

// {"ok":true|false,"err":"..","devid":"0x1B85","rev":5} plus, after a verify
// failure, "addr","want","got". For the web UI.
void pic_result_json(const pic_result_t *r, char *buf, size_t cap);
