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

enum { PR_MAX = 8, PR_SHARE_MAX = 4, PR_AT_SHARE_MAX = 3, PR_NAME_MAX = 33, PR_NOTES_MAX = 8 };
#define PR_BLOB_VER 1

// Where a printer is: the printer port, or an AppleTalk link. The links'
// order is the preference for a printer heard on more than one.
typedef enum { PR_WHERE_PORT, PR_WHERE_CABLE, PR_WHERE_NETWORK, PR_WHERE_TUNNEL,
               PR_WHERE_ZONE } pr_where_t;
typedef enum { PR_KIND_UNKNOWN, PR_KIND_IMAGEWRITER, PR_KIND_LASERWRITER,
               PR_KIND_AT_IMAGEWRITER } pr_kind_t;
// The port printer's model; NONE is "Something else".
typedef enum { PR_MODEL_NONE, PR_MODEL_IW2, PR_MODEL_IWLQ, PR_MODEL_IW1 } pr_model_t;
// What the port says when asked: nothing ready, ready but no answer to ESC ?,
// or an ImageWriter that named itself.
typedef enum { PR_PORT_NOTHING, PR_PORT_SILENT, PR_PORT_IW2, PR_PORT_IWLQ } pr_port_seen_t;

typedef struct {
    uint8_t   id;             // stable for the record's life; 0 is never used
    uint8_t   where;          // pr_where_t
    uint8_t   kind;           // pr_kind_t
    uint8_t   model;          // pr_model_t, port printers
    uint8_t   shared;         // offered over AirPrint
    uint8_t   found_auto;     // found and identified by AirWriter, not told by the person
    uint8_t   identified;     // AirWriter knows what it is (a LaserWriter: its query answered)
    uint8_t   query_fails;    // a LaserWriter's unanswered queries in a row
    uint8_t   announced;      // its "found" notice was given (once in its life)
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

typedef enum { PR_NOTE_FOUND, PR_NOTE_UNKNOWN_PORT, PR_NOTE_CHANGED, PR_NOTE_NO_ANSWER,
               PR_NOTE_TOO_MANY } pr_note_kind_t;
typedef struct { uint8_t kind, id, model; } pr_note_t;

typedef struct {
    pr_rec_t  r[PR_MAX];
    uint8_t   n, next_id;
    pr_note_t notes[PR_NOTES_MAX];
    uint8_t   n_notes;
    uint8_t   mac[6];
    uint16_t  epoch;
} pr_list_t;

// A printer's live state, by its index in r[]: the board fills it in.
typedef struct { uint8_t present, ready, attention, printing, failed; } pr_live_t;
typedef enum { PR_BOX_OFFLINE, PR_BOX_READY, PR_BOX_PRINTING, PR_BOX_ATTENTION,
               PR_BOX_FAILED } pr_box_t;

void pr_init(pr_list_t *l, const uint8_t mac[6], uint16_t epoch);

// What was found. Each returns the record's id, or -1 when there is none.
int  pr_port_seen(pr_list_t *l, pr_port_seen_t what);
int  pr_at_seen(pr_list_t *l, pr_where_t link, const uint8_t *nbp_name, const uint8_t *nbp_type,
                int from_airwriter);
void pr_at_queried(pr_list_t *l, int id, int ok, const lw_info_t *info);

// What the person said. 0 done, -1 refused.
int  pr_rename(pr_list_t *l, int id, const char *name);     // "" or NULL: the printer's own
int  pr_set_shared(pr_list_t *l, int id, int on);           // -1 at the cap, or unidentified
int  pr_set_model(pr_list_t *l, int id, pr_model_t m);      // the port printer only
int  pr_remove(pr_list_t *l, int id);
int  pr_use_anyway(pr_list_t *l, int id);                   // a LaserWriter that won't answer
int  pr_accept_change(pr_list_t *l, int note);
void pr_dismiss(pr_list_t *l, int note);

pr_rec_t       *pr_get(pr_list_t *l, int id);
const pr_rec_t *pr_port(const pr_list_t *l);
int  pr_index(const pr_list_t *l, int id);                  // its place in r[], or -1
int  pr_port_is_imagewriter(const pr_list_t *l);            // ImageWriter commands may go to J2

// The records old Macs are offered on `link` (ids, in list order): every
// identified printer AirWriter can carry, shared over AirPrint or not; an
// AppleTalk printer never on its own link.
int  pr_offers(const pr_list_t *l, pr_where_t link, int ids[], int max);

pr_box_t pr_box_state(const pr_list_t *l, const pr_live_t live[PR_MAX]);
int  pr_test_target(const pr_list_t *l, const pr_live_t live[PR_MAX]);

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

size_t pr_json(const pr_list_t *l, const pr_live_t live[PR_MAX], char *buf, size_t cap);

// Names for the page and the log.
const char *pr_where_name(int where);      // "port", "cable", "network", "tunnel", "zone"
const char *pr_kind_name(int kind);        // "unknown", "imagewriter", "laserwriter", ...
const char *pr_model_name(int model);      // "ImageWriter II" ...
