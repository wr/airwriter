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
    PORT_STYLEWRITER,       // W-1103: a StyleWriter's own bytes (57600, no flow control) go to J2
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
    // W-1103, both optional, both after the post is stored and applied and with
    // no list lock held. clean: one cleaning of printer `id`'s print head
    // (0, or an HTTP status: 409 when a job holds the printer); the status is
    // printers_post's. confirm_sw: the person named the port's printer a
    // StyleWriter; ask it with the full sequence and record the model it gives
    // (printers_port_set_sw_model), or keep the pick, offline, if it is silent.
    int  (*clean)(int id);
    void (*confirm_sw)(int id);
} printers_cfg_t;

// Whether a list is stored and reads back: before printers_start, to tell a
// board set up before W-1088 from one already on it.
int         printers_stored(void);
// Before ipp_server_start: loads (or makes) the list.
esp_err_t   printers_start(const printers_cfg_t *cfg);

// What the port said when asked: the self-ID's model (iw_model_t) with the
// line ready, or not ready.
void        printers_port_seen(int iw_model, int ready);
// W-1103: a StyleWriter answered on the port as `m` (SW_NONE: nothing did, no
// change). Makes the port's record, or raises the "changed" notice
// (pr_port_seen_sw).
void        printers_port_seen_sw(sw_model_t m);
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

// Who gets the test page (the button, DIP4, the page): the port's ImageWriter
// or StyleWriter, else the first shared printer. Its id, *port 1 for the port's; -1 for none.
int         printers_test_target(int *port);
// What printer id's test page is: 1 the port's ImageWriter's, 2 a
// LaserWriter's, 3 the port's StyleWriter's (W-1103), 0 none (no such
// printer, or one with no page).
int         printers_test_kind(int id);
// A printer's name, and a LaserWriter's product and PostScript version ("" for
// none): -1 with no such printer.
int         printers_describe(int id, char *name, size_t ncap, char *product, size_t pcap,
                              char *version, size_t vcap);

port_mode_t printers_port_mode(void);
int         printers_port_is_imagewriter(void);
int         printers_port_is_stylewriter(void);
// An ImageWriter or a StyleWriter on the port, shared over AirPrint: the panel
// follows its state.
int         printers_port_shared(void);
// The port ImageWriter's model; PR_MODEL_NONE for anything else.
pr_model_t  printers_port_model(void);
// The port StyleWriter's model; SW_NONE for anything else.
sw_model_t  printers_port_sw_model(void);
// W-1103: who answered on the port last (pr_heard_t), an answer of the record's own printer
// that names no model (a StyleWriter's `1` and `2`, a job that printed, the full sequence once
// the record has its model; PR_HEARD_NONE changes nothing),
// and whether that was the other family than the record's (pr_port_conflict): then the
// record's AirPrint jobs, test page and cleaning are refused. Not stored. Any task, the
// printer task included: nothing here writes flash.
int         printers_port_heard(void);
void        printers_port_answered(int heard);
int         printers_port_conflict(void);
// What the phone and the page show as a swap (pr_port_swapped): the conflict, or another
// StyleWriter model on a StyleWriter record's port.
int         printers_port_swapped(void);
// The full sequence found the port's StyleWriter to be `m`: its record takes it
// (no notice: the person had named it a StyleWriter already; sharing stays as it
// was). No-op for a port that is not a StyleWriter, or for the model it has.
void        printers_port_set_sw_model(sw_model_t m);
// The port printer's name, "" with none: the Chooser's name and the test page's.
void        printers_port_name(char *out, size_t cap);
// The port printer's halftone and gain, from its record: -1 with no port printer.
int         printers_port_screen(char *dither, size_t cap, int *gain);
// The page's old Printer pane changed the screen: the port record follows.
void        printers_port_set_screen(const char *dither, int gain);

// The port printer's AirPrint slot as its record says, for ipp_server_start;
// NULL when it is not shared or not an ImageWriter or a StyleWriter. Counts as applied.
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
