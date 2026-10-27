// SPDX-License-Identifier: GPL-3.0-or-later
//
// How a StyleWriter's state shows (W-1103, design "Status, panel, test page, settings"):
// what the phone is told for each sw_state_t (sw_session.h), and which states mean the
// printer could print or needs someone. Pure C; tools/rip builds it on the host, the board's
// main.c uses it for IPP and for the panel and the page.
#pragma once
#include "sw_session.h"

// What IPP reports: a printer-state-reasons keyword ("-error" says the printer is stopped)
// and the printer-state-message. NULL and NULL for a printer that can print. Both are
// literals that live for the program (ipp_server compares reasons by address).
typedef struct {
    const char *reason;
    const char *message;
} sw_ipp_t;

sw_ipp_t sw_state_ipp(sw_state_t st);

// The printer is there and could take a page now: SWS_READY, or SWS_PRINTING (it is busy).
int sw_state_ready(sw_state_t st);

// A fault someone has to see to (no paper, a jam, the cartridge, the ink, the cover, the waste
// ink, or one the design does not name). Not SWS_OFFLINE, which is a printer that does not
// answer, and not progress.
int sw_state_fault(sw_state_t st);

// What a StyleWriter record shows when its printer, idle, did not answer (the plain question
// or `1` and `2` got nothing): SWS_OFFLINE. But the original StyleWriter ignores the plain
// question until it is reset, so its silence while idle says nothing: it reads as ready (shared
// and taking jobs, and asked no `1` and `2` while it is not answering, so an ImageWriter
// swapped in hears none), unless a job's full sequence got nothing, or a job lost it, since it
// last answered (`job_silent`): then offline, until it answers or the next job begins.
sw_state_t sw_idle_silent(sw_model_t m, int job_silent);
