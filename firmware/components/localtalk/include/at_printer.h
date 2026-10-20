// SPDX-License-Identifier: GPL-3.0-or-later
//
// The printer as every AppleTalk link shares it (W-1076): the cable's
// LocalTalk node and both EtherTalk links print through one spool and one
// print task, one job at a time. A PAP job is the Mac driver's own
// ImageWriter stream (W-1053): spooled in 16 KB and written to the printer
// through main.c's hooks by the print task, so a paused printer holds the Mac
// (no next SendData until the spool has room), never a link's task. The last
// job's bytes are kept in PSRAM for GET /api/lt/job.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

typedef enum { AT_LINK_CABLE = 0, AT_LINK_TUNNEL, AT_LINK_ZONE, AT_LINK_RADIO } at_link_t;

// The printer, as main.c owns it. All run on the print task except begin.
typedef struct {
    void *ctx;
    // Take the printer for a job from `link` without waiting (the link's
    // task): 0, or nonzero when busy (an IPP job, a test page), which the Mac
    // is told.
    int  (*begin)(void *ctx, at_link_t link);
    // Write job bytes, blocking while the printer is paused. 0, or nonzero
    // when the printer gave up (stalled past the limit): the job is aborted.
    int  (*write)(void *ctx, const uint8_t *d, size_t n);
    // The job is over and its bytes are written or dropped: normal is 1 when
    // the Mac ended it (CloseConn, EOF), 0 when it was abandoned or aborted.
    // Returns 1 when the whole job reached the printer.
    int  (*end)(void *ctx, int normal);
} localtalk_printer_t;

// Who holds the printer: the link, how to tell its node the printer gave up
// (lt_node_pap_abort under that link's lock), and where its log lines go.
typedef struct {
    at_link_t link;
    void    (*abort)(void *ctx);
    void    (*note)(const char *line);
    void     *ctx;
} at_owner_t;

// Once; later calls keep the first printer.
esp_err_t at_printer_init(const localtalk_printer_t *p);
// The node ops' job_* for a link's node, from that link's task.
int       at_job_begin(const at_owner_t *o);      // 0, or 1 busy (one job at a time)
void      at_job_data(const at_owner_t *o, const uint8_t *d, size_t n);
size_t    at_job_room(const at_owner_t *o);
void      at_job_end(const at_owner_t *o, int normal);
// The link holding the printer, from begin until its end has run; -1 for none.
int       at_job_link(void);
// A line for the holder's log (lt_print_end's summary); dropped with no holder.
void      at_printer_note(const char *line);
// The last job's bytes as captured (up to 256 KB), or NULL.
const uint8_t *at_job_bytes(size_t *len);
// The "job":{..},"print":{..} members of /api/lt, without braces around them.
void      at_printer_json(char *buf, size_t cap);
