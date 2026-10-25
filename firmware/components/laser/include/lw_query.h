// SPDX-License-Identifier: GPL-3.0-or-later
//
// The query job (W-1082, laser printers design, "The query job"): an ordinary
// PAP job that prints nothing and asks the LaserWriter what it is. Level 1
// PostScript, every question inside `stopped` so a failure answers "?" and
// the default stands. Its output comes back over PAP (the client's own
// SendData); lines starting %%[ are the printer's status and are skipped.
//
// Each answer is one line, aw:<key>=<value>: product, version, revision,
// level, res (the device's dots per inch across and down), area (the
// imageable area, llx lly urx ury in points), size (Level 2's PageSize),
// startpage (W-1099: whether it prints its start page at power-on, true or
// false), probe, end. The probe is 256 bytes 0x00-0xFF read with readstring: its
// count and whether each byte arrived as itself. Binary goes to this printer
// only if they did; otherwise every page goes as hex. That is how a Level 1
// LaserWriter answers the binary question no document does. TailTalk's
// pap-print query and dump-statusdict are the model.
#pragma once
#include <stdint.h>
#include <stddef.h>

typedef enum { LW_LETTER = 0, LW_A4, LW_LEGAL } lw_page_t;
// The start page at power-on: 0 when the printer never said.
enum { LW_START_UNKNOWN = 0, LW_START_OFF, LW_START_ON };

typedef struct {
    char      product[64];      // statusdict /product: "LaserWriter II NT"
    char      version[16];      // the PostScript version: "47.0"
    int32_t   revision;         // statusdict /revision
    uint8_t   level;            // PostScript language level: 1, 2 or 3
    uint8_t   binary;           // the probe came back, 256 bytes in order
    uint16_t  dpi;              // the device's resolution
    lw_page_t page;             // the paper: from size on Level 2, else the area
    int32_t   area[4];          // imageable area llx lly urx ury, 1/100 pt
    uint8_t   complete;         // the probe (the last question) or aw:end was read
    uint8_t   start_page;       // LW_START_*, W-1099: in what was padding, below
} lw_info_t;
// Stored whole in NVS (each printer record, and W-1082's "laser" info), so
// its size is its layout: start_page took a byte of the tail padding, which
// lw_parse_init and lw_info_default clear. Anything but OFF or ON there reads
// as unknown, and the board asks again at every start.
_Static_assert(sizeof(lw_info_t) == 112, "lw_info_t is stored in NVS: keep its size");

// A LaserWriter that never answered: Level 1, hex, 300 dpi, Letter with a
// quarter inch all round.
void   lw_info_default(lw_info_t *i);
const char *lw_page_name(lw_page_t p);      // "letter", "a4", "legal"

// The query job, probe included, and a line of spaces after the probe as long
// as it. Its length, or 0 when cap is too small.
size_t lw_query_job(uint8_t *out, size_t cap);

// W-1099: the job that sets the start page (on 1, off 0) and answers
// aw:startpage and aw:end, LaserWriter Utility's recipe: exitserver with the
// default password, 0, then setdostartpage, which the printer keeps in its
// non-volatile memory. A printer with another password answers invalidaccess
// (lw_parse_t's refused) and nothing else. Its length, or 0 when cap is too small.
size_t lw_start_page_job(uint8_t *out, size_t cap, int on);

// The printer's output as it comes, split anywhere, CR or LF line ends.
typedef struct {
    lw_info_t info;
    char      line[160];
    size_t    n;
    uint8_t   sized;            // aw:size gave the page
    uint8_t   refused;          // an invalidaccess error came back: the password
} lw_parse_t;

void lw_parse_init(lw_parse_t *p);
void lw_parse_feed(lw_parse_t *p, const uint8_t *d, size_t n);
