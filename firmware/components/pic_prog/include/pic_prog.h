// SPDX-License-Identifier: GPL-3.0-or-later
//
// Flashes TashTalk into a LocalTalk PIC (PIC12F1840) over low-voltage ICSP, with
// the ESP32 bit-banging MCLR, ICSPCLK and ICSPDAT. U7's are IO2 (LT_RESET), IO4
// (LT_UART_TX via R15) and IO5 (LT_UART_RX via R16); rev 2026.11's U10, J2's, is on
// IO42, IO11 and IO12 (W-1179). No 9 V is involved anywhere.
//
// The image is lampmerchant's TashTalk 2.1.3 release hex, vendored unchanged
// (tashtalk-2.1.3.hex, sha256 903bd2ad...cdfb1853c) and patched at runtime in
// exactly one place: CONFIG2.LVP is set, so the part stays reprogrammable from
// here. Upstream clears it, and a part in LVP mode refuses to clear it anyway.
//
// The caller owns the three pins for the duration: nothing else may be using
// the LocalTalk UART. Afterwards all three go back to their reset state, which
// releases MCLR and lets TashTalk run.
#pragma once
#include "pic_prog_core.h"

// W-1179: which PIC. Rev 2026.11 has one per socket, U10 on J2 and U7 on J4, each flashed
// the same way. keep_reset leaves MCLR low afterwards, with CLK and DAT pulled down, for a
// socket doing the printer job (its PIC must stay off the line); otherwise all three go back
// to their reset state and TashTalk runs.
typedef struct { int mclr, clk, dat, keep_reset; } pic_pins_t;

// U7's pins as this build has them (CONFIG_AIRWRITER_PIC_*), -1 where absent.
pic_pins_t pic_prog_u7(void);

// 1 when all three of U7's pins are configured for this build.
int pic_prog_available(void);

// Reads a PIC's device ID and lets it run again (or holds it in reset: keep_reset): a few
// milliseconds, and the PIC restarts. PIC_ERR_NO_DEVICE (ID 0x0000) for an empty socket; the
// cable test uses it to tell one from a bad cable (W-1063). Same pin rules as a flash.
pic_err_t pic_prog_read_id_on(const pic_pins_t *pins, uint16_t *devid);

// Blocks for about two seconds. Returns PIC_OK or the reason it stopped (a
// second caller gets PIC_ERR_BUSY); *res carries the device ID and, on a
// verify failure, the first bad address.
pic_err_t pic_prog_flash_tashtalk_on(const pic_pins_t *pins, pic_result_t *res);

// U7's, as before W-1179.
pic_err_t pic_prog_read_id(uint16_t *devid);
pic_err_t pic_prog_flash_tashtalk(pic_result_t *res);
