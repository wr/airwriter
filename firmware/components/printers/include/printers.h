// SPDX-License-Identifier: GPL-3.0-or-later
//
// The board's printer list (W-1088): printers_core's records kept in NVS
// (namespace "printers", key "list"), fed what the port says, and the port's
// printer put on AirPrint (ipp_server printer 0). A board set up before W-1088
// gets its records from what it had, once; the old keys stay where they were.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "printers_core.h"
#include "ipp_server.h"
#include "lt_node.h"

typedef enum {
    PORT_NONE,              // nothing found on the port yet
    PORT_IMAGEWRITER,       // ImageWriter commands may go to J2
    PORT_UNKNOWN_FOUND,     // something that never answered ESC ?: asked again at line-up
    PORT_UNKNOWN_SAID,      // the person said "Something else": nothing is sent to it
} port_mode_t;

typedef struct {
    uint8_t  mac[6];
    uint16_t epoch;                  // CONFIG_AIRWRITER_LASER_UUID_EPOCH: W-1082's uuids hold
    // Set up before W-1088 (Wi-Fi saved, no list stored): its records are
    // made from `legacy`, whose ImageWriter fields the caller fills; the
    // LaserWriter's and the port's old uuid are read here.
    int         updated_board;
    pr_legacy_t legacy;
    ipp_doc_sink_t port_sink;        // the port printer's jobs
    // An AppleTalk LaserWriter's jobs: its ctx is set per printer to
    // (slot << 8 | record id), for laser_job_begin and ipp_server_job_done_for.
    ipp_doc_sink_t at_sink;
    void (*port_live)(pr_live_t *out);                  // the port printer's state now
    void (*port_screen)(const char *dither, int gain);  // its halftone was changed
    void (*look)(void);              // "Look for printers": ask the port again
    void (*changed)(void);           // names, sharing or the port's kind changed
} printers_cfg_t;

// Whether a list is stored and reads back: before printers_start, to tell a
// board set up before W-1088 from one already on it.
int         printers_stored(void);
// Before ipp_server_start: loads (or makes) the list.
esp_err_t   printers_start(const printers_cfg_t *cfg);

// What the port said when asked: the self-ID's model (iw_model_t) with the
// line ready, or not ready.
void        printers_port_seen(int iw_model, int ready);
// What laser.c heard (once per printer, its link as at_link_t) and its query:
// answered on at_link at net.node with its page count (-1 none), or not
// (at_link -1). W-1098: a printer the list has under an old name keeps its
// record (pr_at_answered).
void        printers_at_seen(int at_link, const uint8_t *name, const uint8_t *type);
void        printers_at_queried(int id, int ok, const lw_info_t *info, int at_link, uint16_t net,
                                uint8_t node, int32_t pages);

// What old Macs are offered on a link (at_link_t): on a way in that is on,
// every printer AirWriter can carry there (pr_offers): the port's ImageWriter
// as the node's own name, AppleTalk printers from other links under their own;
// on one that is off, nothing. The AirWriter marker (host) either way.
void        printers_offers(int at_link, int on, const char *host, lt_offers_t *o);

// Who gets the test page (the button, DIP4, the page): the port's ImageWriter,
// else the first shared printer. Its id, *port 1 for the port's; -1 for none.
int         printers_test_target(int *port);
// What printer id's test page is: 1 the port's ImageWriter's, 2 a
// LaserWriter's, 0 none (no such printer, or one with no page).
int         printers_test_kind(int id);
// A printer's name, and a LaserWriter's product and PostScript version ("" for
// none): -1 with no such printer.
int         printers_describe(int id, char *name, size_t ncap, char *product, size_t pcap,
                              char *version, size_t vcap);

port_mode_t printers_port_mode(void);
int         printers_port_is_imagewriter(void);
// An ImageWriter on the port, shared over AirPrint: the panel follows its line.
int         printers_port_shared(void);
pr_model_t  printers_port_model(void);
// The port printer's name, "" with none: the Chooser's name and the test page's.
void        printers_port_name(char *out, size_t cap);
// The port printer's halftone and gain, from its record: -1 with no port printer.
int         printers_port_screen(char *dither, size_t cap, int *gain);
// The page's old Printer pane changed the screen: the port record follows.
void        printers_port_set_screen(const char *dither, int gain);

// The port printer's AirPrint slot as its record says, for ipp_server_start;
// NULL when it is not shared or not an ImageWriter. Counts as applied.
const ipp_printer_cfg_t *printers_port_ipp(void);
// Puts each shared printer on AirPrint as its record now says, where that
// changed since it was last put there, and gives laser.c the AppleTalk
// printers. A change that met a job in its slot waits: the job's end calls
// this again. After laser_start.
void        printers_apply(void);

// GET /api/printers and POST /api/printers (pr_post's actions): 0, or an
// HTTP status with a sentence for the page in err.
void        printers_json(char *buf, size_t cap);
int         printers_post(const char *body, char *err, size_t cap);
// The "printers" member of /api/status: {"count":N,"shared":M,"port":"..."}.
void        printers_status_json(char *buf, size_t cap);
