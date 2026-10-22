// SPDX-License-Identifier: GPL-3.0-or-later
//
// A LaserWriter's status as the phone sees it (W-1082, laser printers design,
// "Status and errors"). PAP's SendStatus answers with a string such as
// "status: idle" or "job: Letter; status: busy; source: AppleTalk", and a
// fault reads "status: PrinterError: out of paper". Matched loosely and
// without case: the exact texts are guesses until a LaserWriter is benched.
#pragma once
#include <stddef.h>
#include <stdint.h>

typedef struct {
    const char *reason;   // IPP printer-state-reasons keyword: "none", "media-empty-error", ...
    uint8_t     stopped;  // an -error: the printer is stopped, a job processing-stopped
    uint8_t     busy;     // working on something (a job, warming up): processing
} lw_state_t;

// Toner low is a warning: the printer prints on, and so does the job.
void lw_status_map(const char *status, lw_state_t *out);
// A PostScript error in the printer's output, "%%[ Error: undefined;
// OffendingCommand: foo ]%%": 1 and its text ("undefined; OffendingCommand:
// foo") for job-state-message. 0 for anything else.
int  lw_ps_error(const char *line, char *msg, size_t cap);
