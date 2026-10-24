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
    uint32_t  held_lf;      // LFs owed by the strips so far, sent ahead of the
                             // next strip that prints: never after the page's
                             // last, and a blank strip costs only its LF
} iw_t;

void iw_init(iw_t *w, uint32_t width_dots, iw_out_fn out, void *ctx);
void iw_begin_page(iw_t *w);
void iw_row(iw_t *w, const uint8_t *bits);
void iw_end_page(iw_t *w);
void iw_transpose(const uint8_t *rows[8], uint8_t *cols, uint32_t width_dots);

// Colour (W-1064). The colour ribbon has yellow, magenta, cyan and black
// bands, chosen by ESC K 1, 2, 3 and 0 (Table 8-6, p. 123). A strip prints
// one pass per ink it uses, the same columns again after a CR with no LF --
// the Technical Reference's fastest way to print colour graphics -- yellow
// first so darker ink on the paper does not dirty the yellow band. Inks are
// numbered in that print order.
typedef enum { IW_YELLOW, IW_MAGENTA, IW_CYAN, IW_BLACK, IW_INKS } iw_ink_t;

typedef struct {
    uint32_t  width_dots;
    iw_out_fn out;
    void     *ctx;
    uint8_t   rows[IW_INKS][8][(IW_MAX_DOTS + 7) / 8];
    uint32_t  nrows;
    uint8_t   cols[IW_MAX_DOTS];
    uint32_t  held_lf;  // LFs owed before the next strip that prints anything
    int       ink;      // the ink selected now; -1 before the page's first
} iwc_t;

// As iw_*, with one 1bpp row per ink. A pass stops at its ink's last dot
// (a shorter ESC G count): at 9600 baud a full-width pass costs 1.2 s, and
// colour passes are mostly sparse. An ink with no dots in a strip is not
// printed, and a strip with none at all only moves the paper. The page ends
// with black selected, so the stream leaves the printer as ESC c found it.
void iwc_init(iwc_t *w, uint32_t width_dots, iw_out_fn out, void *ctx);
void iwc_begin_page(iwc_t *w);
void iwc_row(iwc_t *w, const uint8_t *const bits[IW_INKS]);
void iwc_end_page(iwc_t *w);

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
    int off_tof;   // the paper may not be at top of form: the last stream to
                   // move it was cut, or (not ours) stopped mid-page
    int ff_asked;  // iw_link_need_ff has answered for the current job
} iw_link_t;

// Which ribbon is in, from the printer's self-ID (W-1064). ESC ? makes the
// ImageWriter II send "IW10CF" (Table 6-7, pp. 88-89): IW, the carriage width
// in inches, then C only with a colour ribbon in and F only with a SheetFeeder
// fitted. The colour option turns on by itself when the colour ribbon goes in
// (p. 123). The ImageWriter LQ sends "LQ1CF1" and a CR: LQ, one digit of
// carriage width (1 is 15 in), C, F, then the number of feeder bins. That shape
// is from GSport's emulator (src/scc_imagewriter.c), not Apple's manual, and is
// untested on a real LQ (W-1087). The original ImageWriter sends nothing.
typedef enum {
    IW_RIBBON_UNKNOWN = 0,   // no reply, or not one we can read
    IW_RIBBON_BLACK   = 1,
    IW_RIBBON_COLOUR  = 2,
} iw_ribbon_t;

// Which ImageWriter answered (W-1088): NONE for no reply, or one not read.
typedef enum { IW_MODEL_NONE = 0, IW_MODEL_II, IW_MODEL_LQ } iw_model_t;
typedef struct { iw_model_t model; iw_ribbon_t ribbon; } iw_self_id_t;

// Reads a reply to ESC ?. Bit 7 is ignored (the printer sends space parity)
// and anything ahead of "IW" or "LQ" is skipped, so line noise does not hide it.
iw_self_id_t iw_self_id(const uint8_t *reply, size_t n);
iw_ribbon_t  iw_self_id_ribbon(const uint8_t *reply, size_t n);   // iw_self_id's ribbon

void iw_link_job_start(iw_link_t *l);
// Before each write of a job's bytes. 1 at most once per job, before its first
// byte, when the last stream was cut: send iw_resync first.
int  iw_link_need_resync(iw_link_t *l);
// After iw_link_need_resync, before each write. 1 at most once per job, when
// the paper was left away from top of form: send a form feed first, so the
// job starts on a fresh page. ESC c leaves top of form where it is (Technical
// Reference Table A-2), so without it a job after a cut one starts mid-sheet.
// At power-up the printer sets top of form where the paper is: 0.
int  iw_link_need_ff(iw_link_t *l);
// clean: every byte of the job was queued and its last page ended normally,
// with its form feed (our own streams).
void iw_link_job_end(iw_link_t *l, int clean);
// For a stream not ours (a Mac's): where it left the paper, from its scanner
// (iw_scan_at_tof).
void iw_link_job_end_at(iw_link_t *l, int clean, int at_tof);
// An iw_resync went out between jobs, all of it (W-1064: the ribbon check
// sends one before ESC ?, which a printer mid-graphics would take as dots).
void iw_link_resynced(iw_link_t *l);

// Where a stream someone else wrote stands between commands (W-1053). A
// LocalTalk job is the Mac driver's own ImageWriter stream, passed through,
// so whether it was cut mid-command has to be read off the bytes: a Mac that
// cancels just closes the connection, wherever the stream is. Argument
// lengths from Appendix A of the Technical Reference: ESC G/S nnnn and
// ESC g nnn (x8) carry graphics, ESC V nnnn c and ESC R nnn c one character,
// ESC H/F nnnn, ESC L/u nnn, ESC T nn, ESC Z/D two bytes, ESC a/s/l/K one,
// ESC ( and ESC ) a tab list ending '.', ESC I custom characters (KEY,
// WIDTH A-P or a-p for 1-16 data bytes, the DATA, repeated, then CTRL-D where
// a KEY would be: Table 7-1, p. 95), and CTRL-_ one count byte.
typedef struct {
    uint8_t  state, cmd, digits, after;   // after: where a skip returns to
    uint32_t num, left;
    // The paper (1/144 in): moved since top of form, the line feed (ESC T,
    // A, B), its direction (ESC r, f) and the page length (ESC H; 11 in by
    // DIP switch 1-4 open). A Mac's stream never form-feeds: it sets an 11 in
    // page and feeds by line, forward and back, to exactly the next page.
    int32_t  moved;
    uint16_t spacing, page;
    uint8_t  reverse;
} iw_scan_t;

void iw_scan_init(iw_scan_t *s);
void iw_scan(iw_scan_t *s, const uint8_t *buf, size_t n);
// 1 when the bytes so far end between commands (the printer is in text).
int  iw_scan_between(const iw_scan_t *s);
// 1 when the bytes so far leave the paper at a top of form (after a form
// feed, ESC v, or line feeds that come to whole pages), taking the stream to
// start at one.
int  iw_scan_at_tof(const iw_scan_t *s);
