// SPDX-License-Identifier: GPL-3.0-or-later
//
// ImageWriter II output stage. 144 dpi horizontal, 72 dpi vertical, one pass.
//
// Escape codes from the Apple ImageWriter II Technical Reference:
//   ESC c        reset                              (Table A-2,  p. 135)
//   ESC p        144 dpi, 1152 dots max             (Table 8-2,  p. 106)
//   ESC T nn     line feed of nn/144 inch, 2 digits (Table A-15, p. 144)
//   ESC G nnnn   nnnn bytes of graphics, 4 digits   (Table 8-1,  p. 105)
// Bit 0 of a graphics byte is the TOP pin (p. 107).
// ESC T16 is 16/144 = 8/72 inch, which exactly abuts successive strips.
#pragma once
#include <stdint.h>
#include <stddef.h>

#define IW_MAX_DOTS 1152

typedef void (*iw_out_fn)(void *ctx, const uint8_t *buf, size_t n);

typedef struct {
    uint32_t  width_dots;
    iw_out_fn out;
    void     *ctx;
    uint8_t   rows[8][(IW_MAX_DOTS + 7) / 8];
    uint32_t  nrows;
    uint8_t   cols[IW_MAX_DOTS];
    int       pending_lf;   // LF from the previous strip, held back in case
                             // it turns out to be the page's last strip
} iw_t;

void iw_init(iw_t *w, uint32_t width_dots, iw_out_fn out, void *ctx);
void iw_begin_page(iw_t *w);
void iw_row(iw_t *w, const uint8_t *bits);
void iw_end_page(iw_t *w);
void iw_transpose(const uint8_t *rows[8], uint8_t *cols, uint32_t width_dots);

// Recovery from a stream cut mid-command (W-1041). A job aborted mid-page --
// cancelled, its transfer cut, or a stall -- stops wherever it was, almost
// always inside a strip's 1152 column bytes. The printer is then still
// counting graphics data and prints the start of the next job (ESC c, ESC p,
// ESC T16, ESC G ...) as dots. The fix is the Technical Reference's own
// (Cancel Current Line, p. 85): 1280 NULs, which run out any graphics count
// and are ignored as text, then CTRL-X, which erases the half-received line.
#define IW_RESYNC_NULS  1280
#define IW_RESYNC_BYTES (IW_RESYNC_NULS + 1)
void iw_resync(iw_out_fn out, void *ctx);

// Whether the printer may be left mid-command. One per printer, zeroed at
// start. A job's stream that sent its first byte and did not end cleanly
// leaves the link dirty; the next job resyncs before its own first byte. A
// job that sends nothing changes nothing, so a clean job's stream is exactly
// what it always was.
typedef struct {
    int dirty;     // the last stream to start was cut short
    int started;   // the current job has sent its first byte
} iw_link_t;

void iw_link_job_start(iw_link_t *l);
// Before each write of a job's bytes. 1 at most once per job, before its first
// byte, when the last stream was cut: send iw_resync first.
int  iw_link_need_resync(iw_link_t *l);
// clean: every byte of the job was queued and its last page ended normally.
void iw_link_job_end(iw_link_t *l, int clean);

// Where a stream someone else wrote stands between commands (W-1053). A
// LocalTalk job is the Mac driver's own ImageWriter stream, passed through,
// so whether it was cut mid-command has to be read off the bytes: a Mac that
// cancels just closes the connection, wherever the stream is. Argument
// lengths from Appendix A of the Technical Reference: ESC G/S nnnn and
// ESC g nnn (x8) carry graphics, ESC V nnnn c and ESC R nnn c one character,
// ESC H/F nnnn, ESC L/u nnn, ESC T nn, ESC Z/D two bytes, ESC a/s/l/K one,
// ESC ( and ESC ) a tab list ending '.', ESC I custom characters ending
// CTRL-D, and CTRL-_ one count byte.
typedef struct {
    uint8_t  state, cmd, digits;
    uint32_t num, left;
} iw_scan_t;

void iw_scan_init(iw_scan_t *s);
void iw_scan(iw_scan_t *s, const uint8_t *buf, size_t n);
// 1 when the bytes so far end between commands (the printer is in text).
int  iw_scan_between(const iw_scan_t *s);
