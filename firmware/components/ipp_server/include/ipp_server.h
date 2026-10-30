// SPDX-License-Identifier: GPL-3.0-or-later
//
// IPP/1.1 over HTTP on port 631, plus the mDNS advertisement that makes iOS
// list the printer at all. SPEC.md 3.2 and 3.4.
//
// The document body is handed to a sink as it streams. The sink buffers it
// (job_pipe's PSRAM ring) and prints in the background, so the server answers
// Print-Job as soon as the upload is complete, with the job still processing.
// The job finishes, and the printer is free for the next one, when the sink
// says so with ipp_server_job_done.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#include "ipp_req.h"
#include "printer_caps.h"

typedef struct {
    // Takes the printer for a job about to start, without waiting: 0 when
    // something else has it (a Mac's job on AppleTalk, the test page, an
    // update). The client is then told server-error-busy and asks again. A
    // document left unread while its job waited held ~20 KB of internal RAM in
    // received frames, and the job then could not start (W-1076 bench). Every
    // claim that returned 1 is followed by begin. May be NULL.
    int       (*claim)(void *ctx);
    // Called once per job when the document starts. Return ESP_OK to accept.
    // `format` is the client's document-format, already checked against the
    // formats we advertise. `opts` is what the job asked for in its Job group
    // (W-1103), valid for the call; a printer with no use for an option ignores
    // it.
    esp_err_t (*begin)(void *ctx, const char *format, const char *job_name,
                       const ipp_job_opts_t *opts);
    // Called repeatedly with document bytes in arrival order.
    esp_err_t (*data)(void *ctx, const uint8_t *buf, size_t n);
    // Called once when the body ends, after begin -- even a begin that failed.
    // `ok` is 0 if the transfer was cut short or the sink refused it. Must not
    // wait for the printer: the client is answered only after this returns.
    // The sink calls ipp_server_job_done exactly once afterwards, or before
    // returning if there is nothing to print.
    void      (*end)(void *ctx, int ok);
    // Called when the client cancels a job that is still printing. Runs on the
    // canceling request's task, not the one streaming the document, with the
    // server's job lock held: it must not block or call back into the server.
    void      (*cancel)(void *ctx);
    void       *ctx;
} ipp_doc_sink_t;

typedef struct ipp_printer_cfg ipp_printer_cfg_t;

typedef struct {
    const char     *queue_name;   // mDNS's default instance name
    const char     *hostname;     // without .local
    uint16_t        port;         // 631
    // W-1088: the port's printer at start (printer 0), or NULL for none on
    // the port; ipp_server_set_printer(0, ...) changes it later.
    const ipp_printer_cfg_t *port_printer;
    // A job operation arrived (Validate-Job, Print-Job): a person is printing,
    // so the panel should wake. Deliberately not called for
    // Get-Printer-Attributes, which Home Assistant polls every 30 s. May be NULL.
    void          (*on_job_op)(void);
    // The colour ribbon is in at start: advertise colour (W-1064).
    int             colour;
} ipp_server_cfg_t;

// Starts the HTTP server and publishes the mDNS records. Call after the
// network is up -- mDNS needs a netif with an address.
esp_err_t ipp_server_start(const ipp_server_cfg_t *cfg);
void      ipp_server_stop(void);

// The printer URI the server answers on, for logging. Valid after start.
const char *ipp_server_uri(void);

// The sink is finished with the document handed over by its last end():
// printed (`ok` 1), or failed, abandoned or canceled (`ok` 0). The job takes
// its final state -- completed, aborted, or canceled if Cancel-Job stopped
// it -- and the printer is free for the next job. Any task.
void ipp_server_job_done(int ok);

// Reports the printer stopped, with an IPP printer-state-reasons keyword such
// as "media-empty-error" and a printer-state-message to go with it. Pass NULL
// to clear both. A job printing meanwhile reads processing-stopped. This is
// what turns a stalled HSKo line into an alert on the phone -- SPEC.md 3.9.
// The reason must outlive the call (pass a literal); the message is copied.
void ipp_server_set_stopped(const char *reason_or_null, const char *message);

// The colour ribbon came or went (W-1064). The printer attributes change at
// once and the TXT record's Color and URF keys are re-announced, all under
// the same printer-uuid. Any task. W-1116: by slot, for the printer on the
// port, whichever slot its record has; ipp_server_set_colour is slot 0's.
// Before ipp_server_start only slot 0's is kept (ipp_server_cfg_t.colour).
void ipp_server_set_colour_for(int idx, int on);
void ipp_server_set_colour(int on);

// AirPrint on or off (W-1079). Off, the printer leaves Bonjour (_ipp and
// _printer go; _http, the settings page, stays) and refuses Print-Job and
// Validate-Job as not accepting jobs; on, it comes back under the same
// printer-uuid. Callable before ipp_server_start, which then starts that way.
// Any task.
void ipp_server_set_airprint(int on);

// ------------------------------------------------------------- W-1082
// More printers beside slot 0's: shared LaserWriters (W-1082, W-1088) and,
// since W-1116, the printer port's other records. The functions above act on
// slot 0; these take the slot. Each answers at its own resource, under its own
// Bonjour instance and printer-uuid; one job at a time across all of them
// (the job buffer is single), so a job for any waits behind the one printing.
struct ipp_printer_cfg {
    const char *resource;        // "ipp/print" for the oldest port record's, "ipp/p<id>": its path, TXT rp
    const char *legacy_resource; // another path it answers at ("ipp/laser"; slot 0's own "ipp/p<id>"), or NULL
    const char *name;            // Bonjour instance and printer-name
    const char *info;            // printer-info: the printer's NBP name
    const char *uuid;            // urn:uuid:...
    const printer_caps_t *caps;  // make-and-model, device id, URF, media, margins, resolution
    const char *icon;            // "laser": served as /laser-48.png and so on
    ipp_doc_sink_t sink;
};

// The port printer's printer-uuid before W-1088 (MAC and UUID epoch), for a
// board's first boot on the printer list. Any time.
void      ipp_server_legacy_uuid(char out[46]);

// Puts printer idx (0 the oldest port record's, 1 to IPP_PRINTERS-1 any other
// shared record's; W-1088, W-1116) on the network, or replaces it, or with NULL
// takes it off. Strings and caps are copied. The same printer put again (its
// uuid: a rename, a model change) keeps its state and colour; another one
// starts with none, until its owner pushes them. ESP_ERR_INVALID_STATE while
// its job holds the printer. Any task, after ipp_server_start.
esp_err_t ipp_server_set_printer(int idx, const ipp_printer_cfg_t *cfg);
void      ipp_server_job_done_for(int idx, int ok);
void      ipp_server_set_stopped_for(int idx, const char *reason_or_null, const char *message);
// The whole of a printer's state as its own printer reports it (a LaserWriter,
// review I3): a reason (literal; "none" for none) that stops it when
// `stopped`, or is reported with it working on (toner-low-warning); busy for
// processing with no job of ours (another Mac's job, warming up); its words
// as printer-state-message (copied).
void      ipp_server_set_state_for(int idx, const char *reason, const char *message, int stopped,
                                   int busy);
// job-state-message for the printer's job in the slot (copied): a
// LaserWriter's PostScript error. Cleared when the next job starts.
void      ipp_server_job_message_for(int idx, const char *msg);
