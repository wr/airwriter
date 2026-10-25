// SPDX-License-Identifier: GPL-3.0-or-later
//
// The AppleTalk printers (W-1082, W-1088 printers design): the board's side of
// lw_core's policy. It looks for LaserWriters and ImageWriters on every
// running link and tells the printer list what it hears; for the printers the
// list gives it (targets) it keeps their state with SendStatus, runs the
// LaserWriter query job, pushes each shared one's state to its AirPrint slot,
// and prints a LaserWriter job through one PAP client and a spool between the
// rip and it (ps_page's PostScript). The printer list owns the records, the
// slots and what is stored; nothing here is in NVS but what W-1082 left
// (laser_saved, read once to carry a board over).
//
// LaserWriters are untested on a real printer: the laser printers design's
// "What only a printer settles" lists what a bench must check.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#include "ipp_server.h"
#include "job_pipe.h"
#include "lw_query.h"
#include "ps_testpage.h"

// W-1098: where a printer answered its query (at_link_t, its net and node)
// and the page count it gave (-1 none): how a renamed one is told.
typedef struct laser_at {
    int      link;
    uint16_t net;
    uint8_t  node;
    int32_t  pages;
} laser_at_t;

typedef struct {
    const char *version;          // %%Creator: AirWriter <version>
    // The first look this long after start (10 s with Wi-Fi up, for its TLS
    // handshakes; 0 in setup mode, where the page waits on it).
    uint32_t    first_look_ms;
    // Which links the printers need running (one is on each): main.c starts
    // the cable's node and sets EtherTalk roles from it.
    void (*links)(int cable, int tunnel, int radio);
    // A printer heard (once): its link (at_link_t), NBP name and type, Pascal.
    void (*found)(int link, const uint8_t *name, const uint8_t *type);
    // The query job for target `id` ended: answered (info, and where: at)
    // or not (both NULL).
    void (*queried)(int id, int ok, const lw_info_t *info, const laser_at_t *at);
    // A shared printer's readiness changed (laser_ready): the panel may follow.
    void (*changed)(void);
    // W-1088: a Mac's job relayed to a printer on another link takes the job
    // lock as any job does (1, without waiting: 0, or nonzero when the box is
    // busy) and gives it back (0) when both ends are done.
    int  (*relay)(int take);
} laser_cfg_t;

// After ipp_server_start, with the network up.
esp_err_t laser_start(const laser_cfg_t *cfg);

// A printer of the list's on AppleTalk.
enum { LASER_KIND_LW = 1, LASER_KIND_IW = 2 };
typedef struct {
    uint8_t   id, kind;
    int8_t    where;              // at_link_t it is reached on, the list's
    int8_t    slot;               // its AirPrint printer (1-3), or -1 not shared
    uint8_t   name[33];           // NBP, Pascal, MacRoman
    char      dither[12];
    uint8_t   gain;
    uint8_t   have_info;          // a LaserWriter's query answers (or the defaults)
    lw_info_t info;
} laser_target_t;
void      laser_set_targets(const laser_target_t *t, int n);
// "Look for printers": lookups every 30 s again for five minutes.
void      laser_look_now(void);

// Any target shared; the first shared one's line for the panel: -1 none
// shared, 0 stopped or offline, 1 ready.
int       laser_shared(void);
int       laser_ready(void);
// A target's state now: 0, or -1 for no such target.
typedef struct { uint8_t online, ready, printing, stopped, busy; } laser_state_t;
int       laser_state(int id, laser_state_t *out);

// The print path, from main.c's sink for a LaserWriter's slot, under the job
// lock. begin opens PAP to target id (waiting out a query job), writes the
// PostScript header and fills t for job_pipe_begin_to: 0, or -1 when it is
// not shared or not heard. end (on the pipeline task, after the rip) ends the
// PostScript, gives EOF and waits for the printer (to the stall limit): 1 if
// the page printed. cancel gives EOF at once, from any task, without blocking.
int       laser_job_begin(int id, const char *name, job_pipe_target_t *t);
int       laser_job_end(int ok);
void      laser_job_cancel(void);
int       laser_job_active(void);
// W-1088: the PostScript test page to LaserWriter id, sent as it is, under the
// job lock: 1 if it printed. laser_job_cancel stops it.
int       laser_test_page(int id, const ps_tp_info_t *info);

// W-1099: turns LaserWriter id's start page on (1) or off (0) with a job
// that prints nothing (lw_start_page_job), on the laser task as a query is,
// and waits for the printer's answer, up to about 25 s: one waits out a busy
// printer for 8 s. The target's info, and its record (laser_cfg_t's
// queried), follows what the printer then says it does. Writes the printer's
// non-volatile memory, which wears: only on a change.
enum { LASER_SP_OK = 0, LASER_SP_BUSY, LASER_SP_REFUSED, LASER_SP_NO_ANSWER, LASER_SP_NOT_HEARD };
int       laser_set_start_page(int id, int on);

// GET /api/laser: what is heard, the targets and the last job's figures.
void      laser_json(char *buf, size_t cap);
// The "laser" member of /api/status: {"targets":N,"online":M,"job":bool}.
void      laser_status_json(char *buf, size_t cap);

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
