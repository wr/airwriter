// SPDX-License-Identifier: GPL-3.0-or-later
//
// The board's printer list (W-1088): printers_core's records kept in NVS
// (namespace "printers", key "list"), fed what the port says, and each shared
// record put on AirPrint (the oldest port record at ipp_server printer 0, the
// rest at 1..; W-1116). A board set up before W-1088 gets its records from what
// it had, once; the old keys stay where they were.
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
    // A port record's jobs. W-1116: its ctx is set per record to (slot << 8 | record id),
    // as at_sink's, so the job's end goes to the record's slot (ipp_server_job_done_for).
    ipp_doc_sink_t port_sink;
    // An AppleTalk LaserWriter's jobs: its ctx is set per printer to
    // (slot << 8 | record id), for laser_job_begin and ipp_server_job_done_for.
    ipp_doc_sink_t at_sink;
    // The port's own state now: the printer on the port's (pr_port_live_id; every other
    // port record is offline, W-1116).
    void (*port_live)(pr_live_t *out);
    void (*port_screen)(const char *dither, int gain);  // its halftone was changed
    void (*look)(void);              // "Look for printers": ask the port again
    void (*changed)(void);           // names, sharing or the port's kind changed
    // W-1116: a record was put on AirPrint, moved or taken off (printers_apply), after the
    // job that held its slot if there was one: a slot starts with no state, so what each
    // port slot says is pushed again (main's port_push). Optional. No list lock held.
    void (*placed)(void);
    // W-1103, both optional, both after the post is stored and applied and with
    // no list lock held. clean: one cleaning of printer `id`'s print head
    // (0, or an HTTP status: 409 when a job holds the printer); the status is
    // printers_post's. confirm_sw: the person named the port's printer a
    // StyleWriter; ask it with the full sequence and record the model it gives
    // (printers_port_set_sw_model), or keep the pick, offline, if it is silent.
    int  (*clean)(int id);
    void (*confirm_sw)(int id);
    // W-1116, optional, after the post is stored and applied, no list lock held: the person
    // set port record `id`'s model (pr_post_t.model_id, any family) or removed it (R19; `id`
    // is gone by then); the port's last answer is forgotten for the record on the port, so
    // the board asks the port again at once.
    void (*port_model)(int id);
} printers_cfg_t;

// Whether a list is stored and reads back: before printers_start, to tell a
// board set up before W-1088 from one already on it.
int         printers_stored(void);
// Before ipp_server_start: loads (or makes) the list.
esp_err_t   printers_start(const printers_cfg_t *cfg);

// What the port said when asked: the self-ID's model (iw_model_t) with the
// line ready, or not ready. Returns the core's result (pr_port_seen): the record id the
// answer belongs to, PR_SEEN_AGAIN for the first of the two answers that change the list
// (R16: a new model, or one that replaces Something else or drops an Unknown record), or -1.
int         printers_port_seen(int iw_model, int ready);
// W-1103: a StyleWriter answered on the port as `m` (SW_NONE: nothing did, no
// change). Its record's at once where nothing else would change; else (a record
// of its own to make, a Something else record to replace, an Unknown record to
// drop: R16) once it has answered twice (pr_port_seen_sw, W-1116). Returns the
// core's result (the record id, PR_SEEN_AGAIN for the first of those two, or -1).
int         printers_port_seen_sw(sw_model_t m);
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
// W-1116: what the port's records hold, for port_policy's order of questions, read in one
// pass: any StyleWriter record (sw), an original ImageWriter record (iw1), an original
// StyleWriter record (sw1). Each out may be NULL.
void        printers_port_listed(int *sw, int *iw1, int *sw1);
// W-1103: who answered on the port last (pr_heard_t), an answer of the record's own printer
// that names no model (a StyleWriter's `1` and `2`, a job that printed, the full sequence once
// the record has its model; PR_HEARD_NONE changes nothing), and whether the port last answered
// as a family other than `family` (pr_port_other_family: that family's bytes must not go
// out; the StyleWriter glue's swapped, whichever record). Not stored. Any task, the printer
// task included: nothing here writes flash.
int         printers_port_heard(void);
void        printers_port_answered(int heard);
int         printers_port_other_family(int family);
// W-1116: a job, test page or cleaning is for port record `id`, but another port record's
// printer is the one on the port (pr_port_absent): the job is answered busy and the test page
// and cleaning 409, so one record's work never goes to another's printer. 0 for the one on the
// port, a quiet port, or an AppleTalk printer. Reads the live state as printers_json does.
int         printers_port_absent(int id);
// W-1116: whether port record `id` is a StyleWriter (doc_begin reads the job's own family from
// its record, not the port's). 0 for an ImageWriter, an AppleTalk printer, or no such record.
int         printers_record_is_sw(int id);
// W-1116: what an ImageWriter job on port record `id` does after its own ESC ? and the plain
// question (pr_job_gate), given the port's last answer and whether the line is up now:
// PR_GATE_GO to send its page, PR_GATE_OTHER to abort (another model or a StyleWriter answered),
// PR_GATE_WAIT to keep waiting with nothing sent.
pr_gate_t   printers_job_gate(int id, int line_up);
// The full sequence found the port's StyleWriter to be `m`: the record the person picked
// (W-1116, `id`) takes it (no notice: the person had named it a StyleWriter already; sharing
// stays as it was), and `m` is the printer on the port now. No-op for an `id` that is not a
// StyleWriter port record.
void        printers_port_set_sw_model(int id, sw_model_t m);
// The printer on the port's name (pr_port_current), "" with none: the test page's.
void        printers_port_name(char *out, size_t cap);
// W-1116: the name vintage Macs see for the printer port and the queue name at boot
// (pr_chooser_name): the oldest ImageWriter record's, fixed across swaps, else the oldest
// port record's, else "AirWriter".
void        printers_chooser_name(char *out, size_t cap);
// W-1116: a record's AirPrint slot as last put there (0 the oldest port record's), -1 none.
int         printers_slot_of(int id);
// W-1116: the port records on AirPrint, in list order, for what each one's printer is told
// (main's port_push): its slot, kind and model and what it says of itself (pr_port_say_t);
// on_name is the name of the printer on the port ("" none), for pr_port_say_words. The live
// state is read as printers_json reads it. How many; a record without a slot is left out.
typedef struct { uint8_t slot, id, kind, model, say; } printers_port_slot_t;
int         printers_port_slots(printers_port_slot_t out[], int max, char on_name[PR_NAME_MAX]);
// The port printer's halftone and gain, from its record: -1 with no port printer.
int         printers_port_screen(char *dither, size_t cap, int *gain);
// The page's old Printer pane changed the screen: the port record follows.
void        printers_port_set_screen(const char *dither, int gain);

// Printer 0 as its record says (the oldest port record, at ipp/print and its own ipp/p<id>),
// for ipp_server_start; NULL when it is not shared or not an ImageWriter or a StyleWriter.
// Counts as applied.
const ipp_printer_cfg_t *printers_port_ipp(void);
// Puts each shared printer on AirPrint as its record now says, where that
// changed since it was last put there (every other shared record, port or
// AppleTalk, in the lowest free slot of 1.., kept for its life there), and
// gives laser.c the AppleTalk printers. A change that met a job in its slot
// waits: the job's end calls this again. After laser_start.
void        printers_apply(void);

// GET /api/printers and POST /api/printers (pr_post's actions): 0, or an
// HTTP status with a sentence for the page in err.
void        printers_json(char *buf, size_t cap);
int         printers_post(const char *body, char *err, size_t cap);
// The "printers" member of /api/status: {"count":N,"shared":M,"port":"..."}.
void        printers_status_json(char *buf, size_t cap);
