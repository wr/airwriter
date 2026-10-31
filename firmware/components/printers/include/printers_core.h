// SPDX-License-Identifier: GPL-3.0-or-later
//
// The printers AirWriter has (W-1088, docs/superpowers/specs/2026-10-23-printers-design.md):
// one record per printer, found or told, and the rules for naming, identity,
// sharing over AirPrint (at most PR_SHARE_MAX), the notices the Printers
// section shows, what old Macs are offered on each AppleTalk link, a board's
// records from before W-1088, and the stored form. Pure C: the host tests
// hold it, and printers.c is its board side.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "lw_query.h"
#include "sw_model.h"

// PR_SHARE_MAX (W-1116): six shared over AirPrint, any mix of the port's records and
// AppleTalk printers (before, the port's and three on AppleTalk); ipp_server has a slot
// for each (IPP_PRINTERS), and Bonjour room for their services.
enum { PR_MAX = 8, PR_SHARE_MAX = 6, PR_NAME_MAX = 33, PR_NOTES_MAX = 8 };
// The stored form's version. 2 (W-1103) adds the StyleWriter kind and its
// seven models to the numbers a record may hold; 3 (W-1116) is a second port
// record in the list. The record's layout is the same throughout, so pr_load
// reads a 1 with the old bounds and a 2 or 3 with the same. pr_save writes the
// lowest version that holds the list: 1 until a record or a notice names a
// StyleWriter, 2 until a second port record, so a one-printer list is still one
// an older firmware reads (an OTA rollback). A firmware that stops at 1 or 2
// takes a later stamp for a newer firmware's and keeps it (pr_blob_newer). A new
// kind or model, or a shape the old reader's own rules can't hold, needs a new
// version; a new fact beside the list does not (pr_seen_save).
#define PR_BLOB_VER 3

// Where a printer is: the printer port, or an AppleTalk link. The links'
// order is the preference for a printer heard on more than one.
typedef enum { PR_WHERE_PORT, PR_WHERE_CABLE, PR_WHERE_NETWORK, PR_WHERE_TUNNEL,
               PR_WHERE_ZONE } pr_where_t;
typedef enum { PR_KIND_UNKNOWN, PR_KIND_IMAGEWRITER, PR_KIND_LASERWRITER,
               PR_KIND_AT_IMAGEWRITER, PR_KIND_STYLEWRITER } pr_kind_t;
// The port printer's model; NONE is "Something else". The ImageWriters, then
// (W-1103) the StyleWriters in sw_model_t's order: the original, II, 1200, then
// the Color StyleWriters 1500, 2200, 2400, 2500. Stored numbers: append only.
typedef enum { PR_MODEL_NONE, PR_MODEL_IW2, PR_MODEL_IWLQ, PR_MODEL_IW1,
               PR_MODEL_SW1, PR_MODEL_SW2, PR_MODEL_SW1200, PR_MODEL_SWC1500,
               PR_MODEL_SWC2200, PR_MODEL_SWC2400, PR_MODEL_SWC2500 } pr_model_t;
// What the port says when asked: nothing ready, ready but no answer to ESC ?,
// or an ImageWriter that named itself. (A StyleWriter that answers is
// pr_port_seen_sw's: it names its own model.)
typedef enum { PR_PORT_NOTHING, PR_PORT_SILENT, PR_PORT_IW2, PR_PORT_IWLQ } pr_port_seen_t;

// W-1103: the family of the printer that last answered on the port: an ImageWriter (ESC ?
// named one; for an original ImageWriter record, its line was up and ESC ? got nothing) or a
// StyleWriter (the plain question, the full sequence, `1` and `2`). Not stored: a start
// knows nothing until the port answers again.
typedef enum { PR_HEARD_NONE, PR_HEARD_IMAGEWRITER, PR_HEARD_STYLEWRITER } pr_heard_t;

// A StyleWriter model as stylewriter's; SW_NONE for anything else.
sw_model_t pr_model_to_sw(pr_model_t m);
// And back; PR_MODEL_NONE for SW_NONE.
pr_model_t pr_model_from_sw(sw_model_t m);

typedef struct {
    uint8_t   id;             // stable for the record's life; 0 is never used
    uint8_t   where;          // pr_where_t
    uint8_t   kind;           // pr_kind_t
    uint8_t   model;          // pr_model_t, port printers
    uint8_t   shared;         // offered over AirPrint
    uint8_t   found_auto;     // found and identified by AirWriter, not told by the person
    uint8_t   identified;     // AirWriter knows what it is (a LaserWriter: its query answered)
    uint8_t   query_fails;    // a LaserWriter's unanswered queries in a row
    uint8_t   announced;      // shared on being found (once in its life)
    uint8_t   ipp_laser;      // the LaserWriter W-1082 shared: also answers on ipp/laser
    char      name[PR_NAME_MAX];      // UTF-8: Bonjour, IPP, the page
    uint8_t   nbp[33];        // an AppleTalk printer's NBP name, Pascal, MacRoman
    uint8_t   nbp_type[33];   // and its type
    char      uuid[46];       // urn:uuid:..., never changes
    char      dither[12];     // its halftone
    uint8_t   gain;
    int16_t   top_margin;     // ImageWriters, 1/144 in
    uint8_t   ribbon;         // ImageWriters: iw_ribbon_t
    lw_info_t lw;             // LaserWriters: the query's answers
} pr_rec_t;
// The stored form is these as they are in RAM. 284 on the host and the
// board; a change is a new PR_BLOB_VER and a reader for the old one.
_Static_assert(sizeof(pr_rec_t) == 284, "pr_rec_t is the stored record");

// Notices are for what asks the person something. PR_NOTE_FOUND is never made
// now (a new printer just appears in the list), nor PR_NOTE_CHANGED since
// W-1116 (a different printer answering on the port is a record of its own);
// one of either stored before is dropped on load. Their numbers stay so stored
// kinds keep theirs.
typedef enum { PR_NOTE_FOUND, PR_NOTE_UNKNOWN_PORT, PR_NOTE_CHANGED, PR_NOTE_NO_ANSWER,
               PR_NOTE_TOO_MANY } pr_note_kind_t;
typedef struct { uint8_t kind, id, model; } pr_note_t;

// W-1098: where an AppleTalk printer last answered its query (its link and
// its address there) and the page count it gave then (-1 none). Kept beside
// the records, not in them (pr_seen_save), so that a firmware without it
// reads the list as it is. id 0: unused.
typedef struct {
    uint8_t  id, where, node;
    uint16_t net;
    int32_t  pages;
} pr_seen_t;

typedef struct {
    pr_rec_t  r[PR_MAX];
    uint8_t   n, next_id;
    pr_note_t notes[PR_NOTES_MAX];
    uint8_t   n_notes;
    uint8_t   mac[6];
    uint16_t  epoch;
    pr_seen_t seen[PR_MAX];
    uint8_t   heard;          // W-1103: pr_heard_t, the port's last answer; not stored
    uint8_t   heard_model;    // ...the pr_model_t it named (0: none), not stored
    uint8_t   pending_model;  // W-1116: a model answered once with no record yet; not stored
} pr_list_t;

// A StyleWriter's fault as the board last read it, said to the settings page so it can
// name it ("check paper" for a missing cartridge would send its owner to the wrong part of
// the printer). PR_FAULT_OTHER is one the design does not name.
typedef enum { PR_FAULT_NONE, PR_FAULT_PAPER, PR_FAULT_JAM, PR_FAULT_CARTRIDGE,
               PR_FAULT_CART_JAM, PR_FAULT_INK, PR_FAULT_COVER, PR_FAULT_WASTE,
               PR_FAULT_OTHER } pr_fault_t;
// The fault for a sw_state_t (sw_session.h): PR_FAULT_NONE for ready, printing and offline.
int pr_fault_from_sw(int sw_state);

// A printer's live state, by its index in r[]: the board fills it in. cart
// (W-1103) is a StyleWriter's cartridge as last read: 0 not known, 1 black,
// 2 colour. fault is its pr_fault_t while it holds one. Both live here and not in
// the record, where an ImageWriter's ribbon is the board's too.
typedef struct { uint8_t present, ready, attention, printing, failed, cart, fault; } pr_live_t;
typedef enum { PR_BOX_OFFLINE, PR_BOX_READY, PR_BOX_PRINTING, PR_BOX_ATTENTION,
               PR_BOX_FAILED } pr_box_t;

void pr_init(pr_list_t *l, const uint8_t mac[6], uint16_t epoch);

// What the port said. pr_port_seen: an ImageWriter's answer to ESC ? (the II or the LQ),
// nothing said with the line ready (SILENT), or the line not ready (NOTHING).
// pr_port_seen_sw (W-1103): a StyleWriter answered the plain question or the full sequence
// as `m` (a PR_MODEL_SW*; anything else is ignored, -1).
//
// W-1116: the printer port keeps a record for every model that answers on it, one per
// model (pr_port_of), and the one that answered last is the one on the port
// (pr_port_current). An answer naming a recorded model, where nothing else would change, is
// that record's, at once. One that would change the list (a model with no record, or one
// that would replace a "Something else" record or drop a found "Unknown printer" record,
// whether or not the model has a record: ruling R16) changes it only when it comes twice in
// a row (PR_SEEN_AGAIN for the first; any other answer in between, SILENT included, starts
// over; the line down does not), so a garbled reply never changes the list: a found "Unknown
// printer" record becomes it (its uuid kept) or, when the model has a record already, goes
// with its notice; a "Something else" record is replaced; else a record of its own is made,
// named by its model, shared if there is room. SILENT with an original ImageWriter record is
// that record's answer; with other port records it changes nothing (a lost answer); with
// none it makes the "Unknown printer" record and its notice. NOTHING changes nothing.
// Returns the record the answer belongs to (SILENT and NOTHING: the one on the port),
// PR_SEEN_AGAIN, or -1 when there is none.
#define PR_SEEN_AGAIN (-2)
int  pr_port_seen(pr_list_t *l, pr_port_seen_t what);
int  pr_port_seen_sw(pr_list_t *l, pr_model_t m);
// W-1103/W-1116: the printer, asked with the full sequence, says the port's StyleWriter is
// `m`. The person named port record `id` a StyleWriter (Add a Printer…, the Model detail); its
// record takes `m`, quietly, when `m` has no record of its own, and either way `m` is the
// printer on the port now (heard). Its name follows if it was the printer's own; sharing and
// who found it stay. id <= 0 keeps the old reach (the one on the port when that is a
// StyleWriter, else the newest StyleWriter record). 1 if record `id` took `m`, else 0 (not a
// StyleWriter record, or `m` has a record of its own already, which is then the one on the
// port).
int  pr_port_sw_found(pr_list_t *l, int id, pr_model_t m);
int  pr_at_seen(pr_list_t *l, pr_where_t link, const uint8_t *nbp_name, const uint8_t *nbp_type,
                int from_airwriter);
void pr_at_queried(pr_list_t *l, int id, int ok, const lw_info_t *info);
// W-1098: a LaserWriter's query answered on `where` at net.node, with its
// page count (-1 none), before pr_at_queried. A printer new to the list may
// be one it has under an old name: renamed, a LaserWriter comes back at the
// address it had, gives the same product and a page count at or above its
// last, and its old name is silent (live[].present 0). That record then
// takes the new name (its own name follows, unless the person gave it one)
// and the new one goes. The id the answers belong to, for pr_at_queried.
int  pr_at_answered(pr_list_t *l, int id, pr_where_t where, uint16_t net, uint8_t node,
                    int32_t pages, const lw_info_t *info, const pr_live_t live[PR_MAX]);
const pr_seen_t *pr_seen_of(const pr_list_t *l, int id);   // NULL: never answered

// What the person said. 0 done, -1 refused.
int  pr_rename(pr_list_t *l, int id, const char *name);     // "" or NULL: the printer's own
int  pr_set_shared(pr_list_t *l, int id, int on);           // -1 at the cap, or unidentified
// A port record only; -2 when another port record has the model, or is "Something else"
// already for PR_MODEL_NONE (W-1116: one record per model, one Something else per port).
// The record is shared if there is room, else the "too many" notice. A model named on the
// record that is on the port (pr_port_current), Something else included, forgets the port's
// last answer (heard and heard_model) and any sighting pending from before it (so it is not
// the first of R16's two answers): the person's word is the newest, and the board asks the
// port again at once. Named on another record, it leaves the one on the port as it is.
int  pr_set_model(pr_list_t *l, int id, pr_model_t m);
// "Add a Printer…" for the port: its model (an original ImageWriter, or one
// that is off, never answers; a StyleWriter, W-1103, the original's included).
// Makes a port record, the person's, shared, beside the records there (W-1116),
// or sets the model of an "Unknown printer" or "Something else" record. Its
// id; -1 for no such model or no room; -2 when the model has a record already.
int  pr_add_port(pr_list_t *l, pr_model_t m);
// Removing the port record that is on the port (pr_port_current) forgets the port's last
// answer (heard and heard_model, and a sighting pending from before, as a model change does,
// ruling R19): the oldest record is the port's until something answers, and the board asks
// the port again at once.
int  pr_remove(pr_list_t *l, int id);
int  pr_use_anyway(pr_list_t *l, int id);                   // a LaserWriter that won't answer
void pr_dismiss(pr_list_t *l, int note);

pr_rec_t       *pr_get(pr_list_t *l, int id);
// The port's records (W-1116). pr_port: the oldest (first in r[]), the one a board had
// before, fixed for the record's life. pr_port_current: the one on the port, whose model
// answered last (heard_model), else the oldest. pr_port_of: the record with that model
// (NULL for PR_MODEL_NONE). pr_port_said_other: the "Something else" record. pr_port_count:
// how many, "Unknown printer" and "Something else" included.
const pr_rec_t *pr_port(const pr_list_t *l);
const pr_rec_t *pr_port_current(const pr_list_t *l);
const pr_rec_t *pr_port_of(const pr_list_t *l, pr_model_t m);
const pr_rec_t *pr_port_said_other(const pr_list_t *l);
int  pr_port_count(const pr_list_t *l);
int  pr_index(const pr_list_t *l, int id);                  // its place in r[], or -1
// The printer on the port (pr_port_current) is an ImageWriter: its commands may go to J2.
int  pr_port_is_imagewriter(const pr_list_t *l);
int  pr_port_is_stylewriter(const pr_list_t *l);            // ... is a StyleWriter

// W-1103: who answered on the port last (pr_heard_t), and the model it named. pr_port_seen and
// pr_port_seen_sw keep them; pr_port_answered is for an answer of the record's own printer that
// names no model (a StyleWriter's `1` and `2`, a job that printed, the full sequence once the
// record has the model it found): the one on the port keeps its model when it is of that
// family, else no model is named. PR_HEARD_NONE changes nothing: silence is not an answer.
int  pr_port_heard(const pr_list_t *l);
void pr_port_answered(pr_list_t *l, int heard);
// W-1116: 1 while the port last answered as a family other than `family` (an ImageWriter's
// record must not hear a StyleWriter's language, nor the reverse); 0 with nothing answered.
int  pr_port_other_family(const pr_list_t *l, int family);
// W-1116: what an ImageWriter job on record `id` does after its own ESC ? (and the plain
// question), given what they got: GO when the answer named the record's model (or the record
// is an original ImageWriter, nothing answered, and the line is up: that is its answer),
// OTHER when another model or a StyleWriter answered (or `id` is not an ImageWriter record),
// WAIT when nothing answered.
typedef enum { PR_GATE_GO, PR_GATE_WAIT, PR_GATE_OTHER } pr_gate_t;
pr_gate_t pr_job_gate(const pr_list_t *l, int id, int heard, pr_model_t heard_model, int line_up);

// The records old Macs are offered on `link` (ids, in list order): one ImageWriter for the
// printer port, the oldest ImageWriter record's (W-1116: fixed across swaps, so a Chooser
// selection keeps working; a Mac's job goes to whichever ImageWriter answers), and every
// identified AppleTalk printer, shared over AirPrint or not, never on its own link. A port
// StyleWriter is carried to none until StyleWriters phase 2.
int  pr_offers(const pr_list_t *l, pr_where_t link, int ids[], int max);

pr_box_t pr_box_state(const pr_list_t *l, const pr_live_t live[PR_MAX]);
// Where the test page goes (W-1116): the printer on the port (pr_port_current, an ImageWriter
// or StyleWriter record) while it is present; else the first shared printer that is ready;
// else that port record (the board then runs the cable test); else the first shared. -1: none.
int  pr_test_target(const pr_list_t *l, const pr_live_t live[PR_MAX]);
// W-1116: 1 when `id` is a port record and another port record's printer is the one on the
// port (pr_port_current, present): a job to `id` is answered busy, nothing held. 0 for the
// one on the port, with nothing present, or for an AppleTalk printer.
int  pr_port_absent(const pr_list_t *l, const pr_live_t live[PR_MAX], int id);
// W-1116: the port record the board's own port state belongs to, for live[]: the one on the
// port (pr_port_current), or the "Something else" record while one is listed (the port is
// its, and asked nothing). Every other port record's live state is all zero: offline. -1
// with no port record.
int  pr_port_live_id(const pr_list_t *l);
// W-1116: the port record whose printer is on the port right now: pr_port_current while it
// is present (pr_json's port_on). Its id; 0 for none.
int  pr_port_on(const pr_list_t *l, const pr_live_t live[PR_MAX]);
// W-1116: what port record `id`'s AirPrint printer says of itself, beside its own state:
//   OWN: it is the printer on the port (pr_port_current): its own state, in the board's
//        words for its family. Also for a record that is not the port's, or none.
//   TAKEN: the port is another's: another port record's printer is on it (pr_port_absent),
//        or a Something else record is in the list (and `id` is not it): stopped,
//        `offline-report`, in its family's offline words.
//   OFFLINE: nothing answering: stopped, in its family's offline reason and words.
// One kind of Offline (Wells, 2026-10-31): the words never say why, on the page or the
// phone; only the reason keyword tells a taken port from a quiet one. Why a printer is
// offline is the wiki's Offline troubleshooting row's.
typedef enum { PR_PORT_SAY_OWN, PR_PORT_SAY_TAKEN, PR_PORT_SAY_OFFLINE } pr_port_say_t;
pr_port_say_t pr_port_say(const pr_list_t *l, const pr_live_t live[PR_MAX], int id);
// W-1116: the name vintage Macs see for the printer port (the node's own NBP name) and the
// queue name at boot: the oldest ImageWriter record's (pr_offers' one port offer, fixed across
// swaps so a Chooser selection keeps working), else the oldest port record's, else
// "AirWriter".
void pr_chooser_name(const pr_list_t *l, char *out, size_t cap);

// A board from before W-1088: its one ImageWriter and its one shared
// LaserWriter, as NVS kept them. Nothing changes on its phones.
typedef struct {
    const char *model, *name;        // "ImageWriter II" ...; "" for the model's
    int         airprint;
    const char *dither;
    uint8_t     gain;
    int16_t     top_margin;
    uint8_t     ribbon;
    const char *uuid;                // today's printer-uuid
    int         laser_shared;
    const uint8_t *laser_nbp;        // Pascal
    int         laser_link;          // at_link_t
    const lw_info_t *laser_info;     // NULL: never answered
    const char *laser_dither;
    uint8_t     laser_gain;
} pr_legacy_t;
void pr_migrate(pr_list_t *l, const pr_legacy_t *old);

size_t pr_save(const pr_list_t *l, uint8_t *buf, size_t cap);   // 0: too small
int    pr_load(pr_list_t *l, const uint8_t *buf, size_t n);     // -1: refused, l untouched
// 1 when buf is a list stored by a newer firmware (a later PR_BLOB_VER): one
// pr_load refuses but nothing here may write over, so that an OTA rollback or
// the known-good reset leaves the newer firmware its printers.
int    pr_blob_newer(const uint8_t *buf, size_t n);
// W-1098: the seen[] entries, in a blob of their own. pr_seen_load after
// pr_load keeps only the list's printers'; -1: refused, l untouched.
size_t pr_seen_save(const pr_list_t *l, uint8_t *buf, size_t cap);    // 0: too small
int    pr_seen_load(pr_list_t *l, const uint8_t *buf, size_t n);

// The Printers section's JSON: the records with their live state and, while shared, each one's
// AirPrint path `rp` (W-1116: the oldest port record's ipp/print, every other record's
// ipp/p<id>, W-1082's LaserWriter ipp/laser), the notices, then `port_on` (the id of the port
// record whose printer is on the port, pr_port_current while present; 0 for none),
// `said_other` (a Something else record is in the list), `chooser` (the names vintage Macs
// see: the port ImageWriter's as on the page, an AppleTalk printer's own NBP name) and
// `max_shared` (PR_SHARE_MAX).
size_t pr_json(const pr_list_t *l, const pr_live_t live[PR_MAX], char *buf, size_t cap);

// What the Printers section sends: flat JSON, {"look":true}, {"add_port":
// "ImageWriter II"|"ImageWriter LQ"|"ImageWriter"|a StyleWriter's name
// (pr_model_name)}, {"note":N, "dismiss":true}, or {"id":N, then any
// of "name":"...", "shared":bool, "model":(the same names)|"none",
// "remove":true, "use_anyway":true, "dither":"...", "gain":0-45,
// "start_page":bool, "clean":true}. 0 done; -1 with a sentence for the page in err
// ("<model> is already in Printers." for add_port or model naming a model another port
// record has, "Something else is already in Printers." for model "none" while another port
// record is one, W-1116).
// start_page (W-1099) changes nothing here: it is the printer's own, set by a
// job to it (start_page_id's, 0 off or 1 on; -1 not asked), and the record
// follows the printer's answer. So is clean (W-1103): clean_id is the
// StyleWriter to clean once (-1 not asked), none that has no cleaning.
// confirm_sw_id is the port's record when the post made it a StyleWriter by a
// model the person picked (add_port, model): the board then asks the printer
// with the full sequence (-1 otherwise). model_id (W-1116) is the port record
// the person set the model of this post, any family (add_port makes one, model
// changes one), or removed (R19; the id is gone from the list by then); -1
// otherwise. The port's last answer is forgotten for the record on the port
// (pr_set_model, pr_remove), so the board asks the port again at once.
typedef struct {
    int changed, look, screen_id, start_page_id, start_page, clean_id, confirm_sw_id, model_id;
    char err[64];
} pr_post_t;
int pr_post(pr_list_t *l, const char *body, pr_post_t *out);

// A board's first boot on W-1088 (no host name stored yet): one set up before
// (Wi-Fi saved) keeps "imagewriter-%s" and its DIP rule for the AppleTalk
// port (-1); a new board is "airwriter-%s" with the port on (1). Stored then.
const char *pr_default_host_fmt(int has_wifi);
int         pr_default_lt_port(int has_wifi);

// Names for the page and the log.
const char *pr_where_name(int where);      // "port", "cable", "network", "tunnel", "zone"
const char *pr_kind_name(int kind);        // "unknown", "imagewriter", "laserwriter", ...
const char *pr_model_name(int model);      // "ImageWriter II", "Color StyleWriter 2500" ...
