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
    // Called once per job when the document starts. Return ESP_OK to accept.
    // `format` is the client's document-format, already checked against the
    // formats we advertise.
    esp_err_t (*begin)(void *ctx, const char *format, const char *job_name);
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

typedef struct {
    const char     *queue_name;   // shown in the print sheet
    const char     *hostname;     // without .local
    uint16_t        port;         // 631
    ipp_doc_sink_t  sink;         // all-NULL callbacks discard the document
    int             dev_mode;     // regenerate the UUID every boot
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
// the same printer-uuid. Any task.
void ipp_server_set_colour(int on);

// AirPrint on or off (W-1079). Off, the printer leaves Bonjour (_ipp and
// _printer go; _http, the settings page, stays) and refuses Print-Job and
// Validate-Job as not accepting jobs; on, it comes back under the same
// printer-uuid. Callable before ipp_server_start, which then starts that way.
// Any task.
void ipp_server_set_airprint(int on);

// ------------------------------------------------------------- W-1082
// A second printer: a shared LaserWriter, beside the ImageWriter. The
// functions above act on the ImageWriter (printer 0); these take the printer.
// Printer 1 answers at its own resource, under its own Bonjour instance and
// printer-uuid; one job at a time across both (the job buffer is single), so
// a job for either waits behind the other as a second ImageWriter job does.
typedef struct {
    const char *resource;        // "ipp/laser" (IPP_RESOURCE_LASER): its path, TXT rp
    const char *name;            // Bonjour instance and printer-name
    const char *info;            // printer-info: the printer's NBP name
    const char *uuid;            // urn:uuid:...
    const printer_caps_t *caps;  // make-and-model, device id, URF, media, margins, resolution
    const char *icon;            // "laser": served as /laser-48.png and so on
    ipp_doc_sink_t sink;
} ipp_printer_cfg_t;

// Puts printer idx (1) on the network, or replaces it, or with NULL takes it
// off. Strings and caps are copied. ESP_ERR_INVALID_STATE while its job
// holds the printer. Any task, after ipp_server_start.
esp_err_t ipp_server_set_printer(int idx, const ipp_printer_cfg_t *cfg);
void      ipp_server_job_done_for(int idx, int ok);
void      ipp_server_set_stopped_for(int idx, const char *reason_or_null, const char *message);
// job-state-message for the printer's job in the slot (copied): a
// LaserWriter's PostScript error. Cleared when the next job starts.
void      ipp_server_job_message_for(int idx, const char *msg);
