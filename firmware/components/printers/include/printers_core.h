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

enum { PR_MAX = 8, PR_SHARE_MAX = 4, PR_AT_SHARE_MAX = 3, PR_NAME_MAX = 33, PR_NOTES_MAX = 8 };
// The stored form's version. 2 (W-1103) adds the StyleWriter kind and its
// seven models to the numbers a record may hold; the record's layout is the
// same, so pr_load reads a 1 with the old bounds. A firmware that stops at 1
// takes a 2 for a newer firmware's and keeps it (pr_blob_newer). A new kind or
// model needs a new version; a new fact beside the list does not (pr_seen_save).
#define PR_BLOB_VER 2

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
// now (a new printer just appears in the list); one stored before is dropped
// on load. Its number stays so stored kinds keep theirs.
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
} pr_list_t;

// A printer's live state, by its index in r[]: the board fills it in. cart
// (W-1103) is a StyleWriter's cartridge as last read: 0 not known, 1 black,
// 2 colour. It lives here and not in the record, where an ImageWriter's ribbon
// is the board's too.
typedef struct { uint8_t present, ready, attention, printing, failed, cart; } pr_live_t;
typedef enum { PR_BOX_OFFLINE, PR_BOX_READY, PR_BOX_PRINTING, PR_BOX_ATTENTION,
               PR_BOX_FAILED } pr_box_t;

void pr_init(pr_list_t *l, const uint8_t mac[6], uint16_t epoch);

// What was found. Each returns the record's id, or -1 when there is none.
int  pr_port_seen(pr_list_t *l, pr_port_seen_t what);
// W-1103: a StyleWriter answered on the port as `m` (a PR_MODEL_SW*). On an empty
// port, or an unknown one AirWriter found itself, it makes the port's record,
// shared; on an ImageWriter's, or another StyleWriter's, it raises the "changed"
// notice. Anything else is ignored.
void pr_port_seen_sw(pr_list_t *l, pr_model_t m);
// W-1103: the printer, asked with the full sequence, says the port's StyleWriter is `m`,
// not the model the person picked: the record takes it, quietly. Its name follows if it
// was the printer's own; sharing and who found it stay. 1 if it changed, else 0 (not a
// StyleWriter record, or the model it has).
int  pr_port_sw_found(pr_list_t *l, pr_model_t m);
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
int  pr_set_model(pr_list_t *l, int id, pr_model_t m);      // the port printer only
// "Add a Printer…" for the port: its model (an original ImageWriter, or one
// that is off, never answers; a StyleWriter, W-1103, the original's included).
// Makes the port's record, the person's, shared, or sets the model of the one
// there. Its id, or -1.
int  pr_add_port(pr_list_t *l, pr_model_t m);
int  pr_remove(pr_list_t *l, int id);
int  pr_use_anyway(pr_list_t *l, int id);                   // a LaserWriter that won't answer
int  pr_accept_change(pr_list_t *l, int note);
void pr_dismiss(pr_list_t *l, int note);

pr_rec_t       *pr_get(pr_list_t *l, int id);
const pr_rec_t *pr_port(const pr_list_t *l);
int  pr_index(const pr_list_t *l, int id);                  // its place in r[], or -1
int  pr_port_is_imagewriter(const pr_list_t *l);            // ImageWriter commands may go to J2
int  pr_port_is_stylewriter(const pr_list_t *l);            // the port's printer is a StyleWriter

// The records old Macs are offered on `link` (ids, in list order): every
// identified printer AirWriter can carry, shared over AirPrint or not; an
// AppleTalk printer never on its own link. A port StyleWriter is carried to
// none until StyleWriters phase 2.
int  pr_offers(const pr_list_t *l, pr_where_t link, int ids[], int max);

pr_box_t pr_box_state(const pr_list_t *l, const pr_live_t live[PR_MAX]);
int  pr_test_target(const pr_list_t *l, const pr_live_t live[PR_MAX]);   // the port's ImageWriter or StyleWriter, else the first shared

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

size_t pr_json(const pr_list_t *l, const pr_live_t live[PR_MAX], char *buf, size_t cap);

// What the Printers section sends: flat JSON, {"look":true}, {"add_port":
// "ImageWriter II"|"ImageWriter LQ"|"ImageWriter"|a StyleWriter's name
// (pr_model_name)}, {"note":N, "accept"|"dismiss":true}, or {"id":N, then any
// of "name":"...", "shared":bool, "model":(the same names)|"none",
// "remove":true, "use_anyway":true, "dither":"...", "gain":0-45,
// "start_page":bool, "clean":true}. 0 done; -1 with a sentence for the page in err.
// start_page (W-1099) changes nothing here: it is the printer's own, set by a
// job to it (start_page_id's, 0 off or 1 on; -1 not asked), and the record
// follows the printer's answer. So is clean (W-1103): clean_id is the
// StyleWriter to clean once (-1 not asked), none that has no cleaning.
// confirm_sw_id is the port's record when the post made it a StyleWriter by a
// model the person picked (add_port, model): the board then asks the printer
// with the full sequence (-1 otherwise).
typedef struct {
    int changed, look, screen_id, start_page_id, start_page, clean_id, confirm_sw_id;
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
