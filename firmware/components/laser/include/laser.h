// SPDX-License-Identifier: GPL-3.0-or-later
//
// A LaserWriter on AppleTalk shared as a second AirPrint printer (W-1082,
// docs/superpowers/specs/2026-10-21-laser-printers-design.md). The board's
// side of it: the LaserWriters heard, the one shared and what its query job
// said (NVS namespace "laser"), the PAP client and the spool between the rip
// and it, the AirPrint printer it becomes (ipp_server printer 1), and the
// JSON for /api/laser. The policy is lw_core's; the PostScript is ps_page's.
//
// Sharing is off until a LaserWriter is chosen on the settings page. A shared
// one is advertised once its query job has answered (its paper, margins and
// resolution come from it), and from then on at every boot, offline until it
// answers. LaserWriters are untested on a real printer: the design's "What
// only a printer settles" lists what a bench must check.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#include "ipp_server.h"
#include "job_pipe.h"
#include "lw_query.h"

typedef struct {
    uint8_t     mac[6];           // the station's: the LaserWriter's printer-uuid
    const char *version;          // %%Creator: AirWriter <version>
    // The IPP side of a LaserWriter job, main.c's (it holds the job lock).
    ipp_doc_sink_t sink;
    // Which links the LaserWriter needs running: the cable while the card is
    // open or one is shared there, the tunnel or the radio while one is
    // shared there. main.c starts the cable's node and sets roles from it.
    void (*links)(int cable, int tunnel, int radio);
    // The shared LaserWriter's readiness changed (laser_ready), or whether
    // one is shared: main.c's panel follows it when it has no ImageWriter.
    void (*changed)(void);
} laser_cfg_t;

// After ipp_server_start, with the network up.
esp_err_t laser_start(const laser_cfg_t *cfg);
// Whether a LaserWriter was shared at the last save, from NVS: before start,
// for the panel's line source at boot.
int       laser_shared_saved(void);
// W-1088: what W-1082 kept in NVS ("laser"), for a board's first boot on the
// printer list: the shared LaserWriter, where it was heard (at_link_t, -1
// unknown), its query's answers and its screen. 0, or -1 with none shared.
typedef struct {
    uint8_t   name[33];          // Pascal, MacRoman
    int       link;
    int       have_info;
    lw_info_t info;
    char      dither[12];
    uint8_t   gain;
} laser_saved_t;
int       laser_saved(laser_saved_t *out);
int       laser_shared(void);
// -1 none shared, 0 stopped or offline, 1 ready: the panel's printer line.
int       laser_ready(void);

// The print path, from main.c's sink for printer 1, under the job lock.
// begin opens PAP to the shared LaserWriter (waiting out a query job), writes
// the PostScript header and fills t for job_pipe_begin_to: 0, or -1 when
// none is shared and advertised. end (on the pipeline task, after the rip)
// ends the PostScript, gives EOF and waits for the printer (to the stall
// limit): 1 if the page printed. cancel gives EOF at once, from any task,
// without blocking.
int       laser_job_begin(const char *name, job_pipe_target_t *t);
int       laser_job_end(int ok);
void      laser_job_cancel(void);
int       laser_job_active(void);

// GET /api/laser (and the card counts as open): the LaserWriters heard, the
// shared one and its query answers, its job.
void      laser_json(char *buf, size_t cap);
// POST /api/laser: share (an NBP name, UTF-8), or none (unshare 1), and the
// shared one's screen (dither NULL and gain < 0 keep each). 0, 400 for a name
// not heard or a screen not known, 409 while a job prints.
int       laser_set(int unshare, const char *share, const char *dither, int gain);
// The "laser" member of /api/status: {"shared":...,"state":...}.
void      laser_status_json(char *buf, size_t cap);
