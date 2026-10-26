// SPDX-License-Identifier: GPL-3.0-or-later
#include "ipp_server.h"
#include "ipp_req.h"
#include "ipp_attrs.h"
#include "ipp_http.h"
#include "sdkconfig.h"
#include "ipp_job.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "mdns.h"

static const char *TAG = "ipp";

// One job at a time, in ipp_job's slot: enough to answer Get-Job-Attributes
// while a job is in flight.
#define RESP_MAX      5120   // printer attributes measure ~3.2KB with media-col, ~3.8KB with a StyleWriter's three sheets
#define HDRBUF_MAX    2048   // the IPP attribute section, before the document
#define CHUNK         1024

// iOS polls Get-Job-Attributes on a second connection while the document is
// still streaming on the first. One worker would make those polls queue behind
// the transfer, and the phone would call the printer unresponsive. A paused
// job bigger than the job buffer holds its worker for minutes, so there are
// three beyond that.
#define WORKERS       4
#define WORKER_STACK  5120

// Connections lwIP completes and holds for accept() while every worker is
// busy. Past this a SYN is dropped and the client retries it a second later.
// CONFIG_LWIP_TCP_ACCEPTMBOX_SIZE must be at least this, or the overflow is
// reset instead.
#define LISTEN_BACKLOG 8

// How long a kept-alive connection may sit between requests. A worker serves
// one connection at a time, so an idle one blocks every connection queued
// behind it: iOS and macOS open bursts of a dozen or more after a new
// printer-uuid, and with each idle connection held for 30 s they timed out
// before ever sending a job (W-1040 bench). While another worker is free the
// connection keeps the whole window; once all are busy it gets a short grace
// and is closed, and the client reconnects -- HTTP/1.1 lets either side close
// an idle keep-alive connection.
#define KEEPALIVE_IDLE_MS  30000
#define BUSY_GRACE_MS        300
#define FIRST_REQ_GRACE_MS  3000   // a fresh connection's request is still in flight

// Per-connection scratch, too big for a task stack and allocated once per
// worker rather than once per request.
typedef struct {
    hrb_t   rb;
    uint8_t resp[RESP_MAX];
    uint8_t hdr[HDRBUF_MAX];
    uint8_t chunk[CHUNK];
} work_t;

static int s_listen_fd = -1;
static int s_running;
static int s_idle_workers;   // workers in accept(), free for a new connection
static ipp_server_cfg_t s_cfg;
static char s_adminurl[96];
static char s_hostname[40];
static volatile int s_mdns_up;

// W-1082: each printer the server answers for. Printer 0 is the ImageWriter
// (ipp_server_cfg_t), set up at start and there for good; printer 1 a shared
// LaserWriter (ipp_server_set_printer), there while it is shared. Strings
// are copies. The job slot, ids and stop reason are each printer's; the
// print lock, below, is one: one job at a time across both, since the job
// buffer and the rip are single.
typedef struct {
    int             on;                  // a printer is in this slot (W-1088: shared)
    char            resource[24];        // "ipp/print": its path, TXT rp
    char            legacy[24];          // another path it answers at ("ipp/laser"), or 
    char            uri[128];
    char            uuid[64];
    char            name[64];            // printer-name; printer 1's Bonjour instance too
    char            info[64];            // printer-info
    char            icon_uri[3][96];
    const printer_caps_t *caps;          // NULL: printer_caps_imagewriter
    printer_caps_t  caps_copy;           // printer 1's, its strings in the two below
    char            make_model[72], device_id[112];
    ipp_doc_sink_t  sink;
    ipp_job_rec_t   job;
    int32_t         next_job_id;
    const char     *stop_reason;         // NULL when the printer is fine
    const char     *note_reason;         // a reason that does not stop it (toner low), or NULL
    int             busy;                // working on something not ours: processing
    char            stop_message[256];   // printer-state-message with either reason or busy
    char            job_message[128];    // job-state-message for the job in the slot
    volatile int    colour;              // W-1064: the colour ribbon is in (printer 0)
    int             txt_colour;          // what the TXT record says
    volatile int    airprint;            // advertised and taking jobs: on, and AirPrint on (W-1079)
    int             print_svcs;          // its Bonjour services are registered
    char            svc_name[64];        // the instance they were registered as
} printer_t;

// In PSRAM, allocated at start (internal RAM is Wi-Fi's: W-1082 bench). What
// main.c sets for the ImageWriter before then waits in the s_pre_ ones.
static printer_t *s_p;
static volatile int s_pre_airprint = 1;     // W-1079: on until set
// AirPrint for the whole box (W-1079's switch; W-1088: every printer).
static volatile int s_box_airprint = 1;
static volatile int s_pre_colour;
static const char  *s_pre_reason;
static char         s_pre_message[128];
static SemaphoreHandle_t s_job_lock;     // guards s_p's jobs, ids, stops and strings
// Held by the printing job, from Print-Job until its document has printed --
// well after the reply, which goes back when the upload ends (W-1042).
// Binary, not a mutex: the Print-Job worker takes it and the sink's pipeline
// task gives it back, and a mutex given by a task that does not hold it trips
// FreeRTOS's xTaskPriorityDisinherit assert -- a panic after every job on the
// W-1042 bench, the same trap as main.c's job lock in W-1033.
static SemaphoreHandle_t s_print_lock;

static uint32_t uptime_s(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000000);
}

// The port printer's printer-uuid before W-1088, derived from the eFuse MAC
// so it survived a reflash, the third group the UUID epoch (bumped by hand
// when the advertised capabilities changed). SPEC.md 3.5. A board updated to
// W-1088 keeps it in its port record; records made since have their own.
void ipp_server_legacy_uuid(char out[46])
{
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(out, 46, "urn:uuid:41575254-4552-%04x-%02x%02x-%02x%02x%02x%02x",
             (unsigned)CONFIG_AIRWRITER_UUID_EPOCH, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

// printer-icons, built in from icons/ by tools/icon/mkicons.py and served
// on the IPP port itself.
extern const uint8_t icon_48_start[]  asm("_binary_icon_48_png_start");
extern const uint8_t icon_48_end[]    asm("_binary_icon_48_png_end");
extern const uint8_t icon_128_start[] asm("_binary_icon_128_png_start");
extern const uint8_t icon_128_end[]   asm("_binary_icon_128_png_end");
extern const uint8_t icon_512_start[] asm("_binary_icon_512_png_start");
extern const uint8_t icon_512_end[]   asm("_binary_icon_512_png_end");
// W-1082: a LaserWriter, for a shared one.
extern const uint8_t laser_48_start[]  asm("_binary_laser_48_png_start");
extern const uint8_t laser_48_end[]    asm("_binary_laser_48_png_end");
extern const uint8_t laser_128_start[] asm("_binary_laser_128_png_start");
extern const uint8_t laser_128_end[]   asm("_binary_laser_128_png_end");
extern const uint8_t laser_512_start[] asm("_binary_laser_512_png_start");
extern const uint8_t laser_512_end[]   asm("_binary_laser_512_png_end");

static const struct {
    const char    *prefix;
    unsigned       size;
    const uint8_t *start, *end;
} k_icons[6] = {
    { "icon",   48, icon_48_start,   icon_48_end   },
    { "icon",  128, icon_128_start,  icon_128_end  },
    { "icon",  512, icon_512_start,  icon_512_end  },
    { "laser",  48, laser_48_start,  laser_48_end  },
    { "laser", 128, laser_128_start, laser_128_end },
    { "laser", 512, laser_512_start, laser_512_end },
};

static void icon_uris(printer_t *p, const char *prefix)
{
    for (int i = 0; i < 3; i++)
        snprintf(p->icon_uri[i], sizeof p->icon_uri[i], "http://%s.local:%u/%s-%u.png",
                 s_hostname, (unsigned)s_cfg.port, prefix, k_icons[i].size);
}

// Callers hold s_job_lock.
static void fill_printer(const printer_t *p, ipp_printer_t *out)
{
    out->uri            = p->uri;
    out->name           = p->name;
    out->info           = p->info;
    out->uuid           = p->uuid;
    out->adminurl       = s_adminurl;
    for (int i = 0; i < 3; i++) out->icons[i] = p->icon_uri[i];
    out->location       = "";
    out->state          = p->stop_reason               ? IPP_PSTATE_STOPPED
                        : (p->job.state == IPP_JSTATE_PROCESSING || p->busy) ? IPP_PSTATE_PROCESSING
                                                                             : IPP_PSTATE_IDLE;
    out->state_reason   = p->stop_reason ? p->stop_reason : p->note_reason ? p->note_reason : "none";
    out->state_message  = p->stop_reason || p->note_reason || p->busy ? p->stop_message : NULL;
    // Still accepting: the job queues and prints when paper goes back in,
    // which is what a user expects after clearing the alert.
    out->accepting_jobs = p->airprint;
    out->queued_jobs    = ipp_job_active(&p->job) ? 1 : 0;
    out->uptime_s       = uptime_s();
    out->colour         = p->colour;
    out->caps           = p->caps;
}

// `src` and `printer_stopped` are a snapshot taken under s_job_lock; the
// strings point into `src` and `msg`, so they must outlive the encode.
static void fill_job(const printer_t *p, ipp_job_t *j, const ipp_job_rec_t *src,
                     int printer_stopped, const char *msg)
{
    j->id               = src->id;
    j->uri              = src->uri;
    j->printer_uri      = p->uri;
    j->name             = src->name[0] ? src->name : "Untitled";
    j->user             = src->user[0] ? src->user : "anonymous";
    ipp_job_view(src, printer_stopped, &j->state, &j->state_reason);
    j->state_message    = msg;
    j->impressions_done = 0;
    j->time_at_creation = src->created_s;
}

// A copy of a printer's job, whether it is stopped and its job message,
// consistent with each other.
typedef struct {
    ipp_job_rec_t job;
    int           stopped;
    char          msg[128];
} job_snap_t;

static void job_snapshot(const printer_t *p, job_snap_t *out)
{
    xSemaphoreTake(s_job_lock, portMAX_DELAY);
    out->job = p->job;
    out->stopped = p->stop_reason != NULL;
    memcpy(out->msg, p->job_message, sizeof out->msg);
    xSemaphoreGive(s_job_lock);
}

// The response must carry the version the client asked for. A 2.0 client that
// gets a 1.1 response treats it as a protocol error, not as a downgrade.
static uint16_t resp_version(const ipp_req_t *r)
{
    return ((r->version >> 8) == 2) ? 0x0200 : IPP_VERSION_1_1;
}

static int send_ipp(int fd, const ipp_enc_t *e, int keep_alive)
{
    if (!ipp_enc_ok(e)) {
        ESP_LOGE(TAG, "response overflowed %d bytes", RESP_MAX);
        return hresp_send(fd, 500, "Internal Server Error", "text/plain",
                          "ipp encode\n", 11, 0);
    }
    return hresp_send(fd, 200, "OK", "application/ipp",
                      e->buf, ipp_enc_len(e), keep_alive);
}

// A response carrying nothing but a status. Every error path uses this rather
// than closing the connection, which SPEC.md 3.4 calls for explicitly.
static int send_status(work_t *w, int fd, const ipp_req_t *r, uint16_t status,
                       int keep_alive)
{
    ipp_enc_t e;
    ipp_enc_init(&e, w->resp, RESP_MAX);
    ipp_resp_begin(&e, resp_version(r), status, r->request_id);
    ipp_write_op_attrs(&e);
    ipp_end(&e);
    return send_ipp(fd, &e, keep_alive);
}

// A response carrying one job's attributes: Print-Job and Get-Job-Attributes
// both answer with this.
static int send_job(work_t *w, int fd, const ipp_req_t *r, const printer_t *p,
                    const job_snap_t *snap, int keep_alive)
{
    ipp_job_t j;
    fill_job(p, &j, &snap->job, snap->stopped, snap->msg);
    ipp_enc_t e;
    ipp_enc_init(&e, w->resp, RESP_MAX);
    ipp_resp_begin(&e, resp_version(r), IPP_STATUS_OK, r->request_id);
    ipp_write_op_attrs(&e);
    ipp_write_job_attrs(&e, &j);
    ipp_end(&e);
    return send_ipp(fd, &e, keep_alive);
}

// The printer is one job's at a time. A second job is told server-error-busy
// at once and asks again, never kept waiting: it waited here up to 30 s with
// its document unread, and frames left unread hold internal RAM (W-1076
// bench, the sink's claim).
static int acquire_printer(void)
{
    return xSemaphoreTake(s_print_lock, 0) == pdTRUE;
}

// Puts a new job in the slot. The caller holds s_print_lock.
static int32_t start_job(printer_t *p, const ipp_req_t *r)
{
    xSemaphoreTake(s_job_lock, portMAX_DELAY);
    int32_t id = p->next_job_id++;
    ipp_job_start(&p->job, id, uptime_s(), r->job_name, r->requesting_user, p->uri);
    p->job_message[0] = '\0';
    xSemaphoreGive(s_job_lock);
    return id;
}

static int format_supported(const char *fmt)
{
    // An absent or unspecified format means "you decide", which for this
    // printer is URF. Only an explicit foreign format is a refusal.
    return fmt[0] == '\0'
        || strcmp(fmt, "image/urf") == 0
        || strcmp(fmt, "application/octet-stream") == 0;
}

// Streams the document into the sink and answers as soon as the whole body is
// in, with the job processing: the sink prints it from its buffer in the
// background, and ipp_server_job_done finishes the job and frees the printer
// when the paper stops. The caller holds s_print_lock and the job is
// processing.
static int run_document(work_t *w, int fd, const ipp_req_t *r, hbody_t *body,
                        const uint8_t *pre, size_t pre_len, int32_t job_id,
                        int keep_alive, printer_t *p)
{
    const ipp_doc_sink_t *sink = &p->sink;
    xSemaphoreTake(s_job_lock, portMAX_DELAY);
    ipp_job_rec_t kept = p->job;
    xSemaphoreGive(s_job_lock);

    int ok = 1;
    if (sink->begin && sink->begin(sink->ctx, r->document_format, kept.name, &r->opts) != ESP_OK)
        ok = 0;

    // A sink without claim waits in begin(), and a Cancel-Job in that time
    // reached a sink with nothing started, which begin() then cleared. Pass it
    // on again now that there is a job to stop.
    if (ok) {
        xSemaphoreTake(s_job_lock, portMAX_DELAY);
        if (p->job.canceled && sink->cancel) sink->cancel(sink->ctx);
        xSemaphoreGive(s_job_lock);
    }

    // Where the upload's time goes: waiting on the network, or on the sink.
    // With the job buffered the sink should never hold the socket up, so a
    // slow upload with short pushes is the Wi-Fi link (W-1042 bench).
    int64_t t0 = esp_timer_get_time();
    int64_t wait_max = 0, push_max = 0;
    size_t wait_at = 0;

    size_t total = 0;
    if (ok) {
        // The attribute section and the first document bytes usually arrive in
        // one read, so hand that slice over before touching the socket again.
        if (pre_len && sink->data && sink->data(sink->ctx, pre, pre_len) != ESP_OK)
            ok = 0;
        total += pre_len;

        while (ok) {
            int64_t a = esp_timer_get_time();
            int n = hbody_read(body, w->chunk, CHUNK);
            int64_t b = esp_timer_get_time();
            if (b - a > wait_max) { wait_max = b - a; wait_at = total; }
            if (b - a > 2000000)
                ESP_LOGW(TAG, "job %d: no data for %u ms at byte %u", (int)job_id,
                         (unsigned)((b - a) / 1000), (unsigned)total);
            if (n == 0) break;
            if (n < 0) {
                // The client hung up mid-document. With the job buffered the
                // socket is read to the end, so this is seen at once rather
                // than when the printer next takes a byte.
                ESP_LOGW(TAG, "job %d: body truncated", (int)job_id);
                ok = 0;
                break;
            }
            if (sink->data && sink->data(sink->ctx, w->chunk, (size_t)n) != ESP_OK)
                ok = 0;
            int64_t c = esp_timer_get_time();
            if (c - b > push_max) push_max = c - b;
            total += (size_t)n;
        }
    }

    ESP_LOGI(TAG, "job %d: %u document bytes in %u ms, %s; longest wait for data "
             "%u ms at byte %u, longest push %u ms",
             (int)job_id, (unsigned)total, (unsigned)((esp_timer_get_time() - t0) / 1000),
             ok ? "received" : "failed", (unsigned)(wait_max / 1000), (unsigned)wait_at,
             (unsigned)(push_max / 1000));
    if (sink->end) sink->end(sink->ctx, ok);
    else ipp_server_job_done_for((int)(p - s_p), ok);

    // Respond only now, with the whole body in. A client that sees the
    // response while it is still sending treats the connection as finished,
    // and the rest is lost. The job may already be over -- a sink that could
    // not take it -- and its printer handed to the next job, in which case
    // the slot holds that one and the copy kept here is finished instead.
    job_snap_t snap;
    xSemaphoreTake(s_job_lock, portMAX_DELAY);
    if (p->job.id == job_id) kept = p->job;
    else ipp_job_finish(&kept, 0);
    snap.job = kept;
    snap.stopped = p->stop_reason != NULL;
    snprintf(snap.msg, sizeof snap.msg, "%s", p->job.id == job_id ? p->job_message : "");
    xSemaphoreGive(s_job_lock);
    return send_job(w, fd, r, p, &snap, keep_alive);
}

static int op_print_job(work_t *w, int fd, const ipp_req_t *r, hbody_t *body,
                        const uint8_t *pre, size_t pre_len, int keep_alive, printer_t *p)
{
    if (!format_supported(r->document_format)) {
        ESP_LOGW(TAG, "refusing document-format %s", r->document_format);
        hbody_drain(body);
        return send_status(w, fd, r, IPP_STATUS_ERR_FORMAT_NOT_SUPPORTED, keep_alive);
    }

    // Busy, not not-accepting: printer-is-accepting-jobs stays true, and
    // RFC 8011 has busy mean "try the same request again later".
    if (!acquire_printer()) {
        hbody_drain(body);
        return send_status(w, fd, r, IPP_STATUS_ERR_BUSY, keep_alive);
    }
    // A LaserWriter unshared while this request waited for the printer.
    if (!p->on) {
        xSemaphoreGive(s_print_lock);
        hbody_drain(body);
        return send_status(w, fd, r, IPP_STATUS_ERR_NOT_ACCEPTING_JOBS, keep_alive);
    }
    // Another link's job, the test page or an update has the printer: busy
    // too, read to the end so the frames go at once.
    if (p->sink.claim && !p->sink.claim(p->sink.ctx)) {
        xSemaphoreGive(s_print_lock);
        hbody_drain(body);
        return send_status(w, fd, r, IPP_STATUS_ERR_BUSY, keep_alive);
    }
    int32_t job_id = start_job(p, r);
    return run_document(w, fd, r, body, pre, pre_len, job_id, keep_alive, p);
}

static int dispatch(work_t *w, int fd, const ipp_req_t *r, hbody_t *body,
                    const uint8_t *pre, size_t pre_len, int keep_alive, printer_t *p)
{
    ipp_enc_t e;
    ipp_enc_init(&e, w->resp, RESP_MAX);

    // Print-Job owns the body. Every other operation must consume it before
    // replying, or the next request on this connection starts mid-document.
    if (r->op != IPP_OP_PRINT_JOB && hbody_drain(body) < 0) return -1;

    switch (r->op) {
    case IPP_OP_GET_PRINTER_ATTRIBUTES: {
        ipp_printer_t pa;
        xSemaphoreTake(s_job_lock, portMAX_DELAY);
        fill_printer(p, &pa);
        ipp_resp_begin(&e, resp_version(r), IPP_STATUS_OK, r->request_id);
        ipp_write_op_attrs(&e);
        ipp_write_printer_attrs(&e, &pa, &r->requested);
        xSemaphoreGive(s_job_lock);
        ipp_end(&e);
        ESP_LOGI(TAG, "  attributes of %s: %u bytes, state %d", p->resource,
                 (unsigned)ipp_enc_len(&e), (int)pa.state);
        return send_ipp(fd, &e, keep_alive);
    }

    case IPP_OP_VALIDATE_JOB:
        if (s_cfg.on_job_op) s_cfg.on_job_op();
        if (!p->airprint) return send_status(w, fd, r, IPP_STATUS_ERR_NOT_ACCEPTING_JOBS, keep_alive);
        if (!format_supported(r->document_format))
            return send_status(w, fd, r, IPP_STATUS_ERR_FORMAT_NOT_SUPPORTED, keep_alive);
        return send_status(w, fd, r, IPP_STATUS_OK, keep_alive);

    case IPP_OP_PRINT_JOB:
        if (s_cfg.on_job_op) s_cfg.on_job_op();
        if (!p->airprint) {                      // W-1079: AirPrint is switched off
            hbody_drain(body);
            return send_status(w, fd, r, IPP_STATUS_ERR_NOT_ACCEPTING_JOBS, keep_alive);
        }
        return op_print_job(w, fd, r, body, pre, pre_len, keep_alive, p);

    case IPP_OP_GET_JOB_ATTRIBUTES: {
        job_snap_t snap;
        job_snapshot(p, &snap);
        if (!ipp_job_matches(&snap.job, r->job_id))
            return send_status(w, fd, r, IPP_STATUS_ERR_NOT_FOUND, keep_alive);
        return send_job(w, fd, r, p, &snap, keep_alive);
    }

    case IPP_OP_GET_JOBS: {
        job_snap_t snap;
        job_snapshot(p, &snap);
        ipp_resp_begin(&e, resp_version(r), IPP_STATUS_OK, r->request_id);
        ipp_write_op_attrs(&e);
        // An empty list is a complete answer, and the only honest one while no
        // job is in flight.
        if (ipp_job_active(&snap.job)) {
            ipp_job_t j;
            fill_job(p, &j, &snap.job, snap.stopped, snap.msg);
            ipp_write_job_attrs(&e, &j);
        }
        ipp_end(&e);
        return send_ipp(fd, &e, keep_alive);
    }

    case IPP_OP_CANCEL_JOB: {
        xSemaphoreTake(s_job_lock, portMAX_DELAY);
        ipp_cancel_t c = ipp_job_cancel(&p->job, r->job_id);
        int32_t id = p->job.id;
        // Stop the paper, not just the bookkeeping. Under the lock, so the job
        // cannot finish in between and hand the printer to the next one, which
        // the cancel would then land on.
        if (c == IPP_CANCEL_STOP_PRINT && p->sink.cancel) p->sink.cancel(p->sink.ctx);
        xSemaphoreGive(s_job_lock);

        if (c == IPP_CANCEL_NOT_FOUND)
            return send_status(w, fd, r, IPP_STATUS_ERR_NOT_FOUND, keep_alive);
        if (c == IPP_CANCEL_NOT_POSSIBLE)
            return send_status(w, fd, r, IPP_STATUS_ERR_NOT_POSSIBLE, keep_alive);
        ESP_LOGI(TAG, "job %d canceled", (int)id);
        return send_status(w, fd, r, IPP_STATUS_OK, keep_alive);
    }

    default:
        ESP_LOGW(TAG, "unsupported operation 0x%04x", r->op);
        return send_status(w, fd, r, IPP_STATUS_ERR_OP_NOT_SUPPORTED, keep_alive);
    }
}

// requested-attributes as "a,b,c" for the log, cut to fit.
static void names_str(const ipp_names_t *nm, char *out, size_t cap)
{
    size_t k = 0;
    const uint8_t *q = nm->p, *end = nm->p ? nm->p + nm->len : NULL;
    out[0] = '\0';
    if (!nm->n) {
        snprintf(out, cap, "-");
        return;
    }
    for (uint16_t i = 0; i < nm->n && q && q + 3 <= end; i++) {
        size_t nl = (size_t)(q[1] << 8 | q[2]);
        q += 3 + nl;
        if (q + 2 > end) break;
        size_t vl = (size_t)(q[0] << 8 | q[1]);
        q += 2;
        if (q + vl > end) break;
        int w = snprintf(out + k, cap - k, "%s%.*s", i ? "," : "", (int)vl, (const char *)q);
        if (w < 0 || (size_t)w >= cap - k) break;
        k += (size_t)w;
        q += vl;
    }
}

static int handle_post(work_t *w, int fd, const hreq_t *hr)
{
    hbody_t body;
    hbody_init(&body, &w->rb, hr->content_len, hr->chunked);

    // CUPS sends Expect: 100-continue and waits before the body. Without this
    // the job stalls for the client's whole timeout and then retries.
    if (hr->expect_continue && hresp_continue(fd) < 0) return -1;

    // Read until the attribute section parses. It arrives split across chunks,
    // so a short read is the normal case and not an error.
    size_t have = 0;
    ipp_req_t r;
    int rc;
    for (;;) {
        rc = ipp_req_parse(&r, w->hdr, have);
        if (rc != IPP_REQ_SHORT) break;
        if (have == HDRBUF_MAX) {
            ESP_LOGE(TAG, "attribute section over %d bytes", HDRBUF_MAX);
            return hresp_send(fd, 400, "Bad Request", "text/plain",
                              "ipp header too large\n", 21, 0);
        }
        int n = hbody_read(&body, w->hdr + have, HDRBUF_MAX - have);
        if (n <= 0) {
            ESP_LOGW(TAG, "body ended inside the attribute section");
            return -1;
        }
        have += (size_t)n;
    }

    if (rc != IPP_REQ_OK)
        return hresp_send(fd, 400, "Bad Request", "text/plain", "malformed ipp\n", 14, 0);


    // Major version 1 and 2 share this encoding; anything else we cannot
    // speak, but the client still needs a well-formed answer.
    if ((r.version >> 8) != 1 && (r.version >> 8) != 2) {
        hbody_drain(&body);
        return send_status(w, fd, &r, IPP_STATUS_ERR_BAD_REQUEST, hr->keep_alive);
    }

    // W-1082: which printer, by the request's path.
    const char *res[IPP_PRINTERS], *legacy[IPP_PRINTERS];
    xSemaphoreTake(s_job_lock, portMAX_DELAY);
    for (int i = 0; i < IPP_PRINTERS; i++) {
        res[i] = s_p[i].on ? s_p[i].resource : NULL;
        legacy[i] = s_p[i].on && s_p[i].legacy[0] ? s_p[i].legacy : NULL;
    }
    int idx = ipp_route(hr->uri, res, legacy);
    xSemaphoreGive(s_job_lock);
    // Which printer and what was asked: an iPhone that asks and never sends
    // a job says why only here (W-1044, W-1082 bench).
    char asked[160];
    names_str(&r.requested, asked, sizeof asked);
    ESP_LOGI(TAG, "op 0x%04x id %u v%u.%u from %s to %s (%d) asks %s", r.op,
             (unsigned)r.request_id, (unsigned)(r.version >> 8), (unsigned)(r.version & 0xFF),
             r.requesting_user[0] ? r.requesting_user : "-", hr->uri, idx, asked);
    if (idx < 0) {
        hbody_drain(&body);
        return send_status(w, fd, &r, IPP_STATUS_ERR_NOT_FOUND, hr->keep_alive);
    }
    return dispatch(w, fd, &r, &body, w->hdr + r.header_len, have - r.header_len,
                    hr->keep_alive, &s_p[idx]);
}

// Waits for the next request on a connection to start arriving. Returns 1 when
// there is something to read (or the peer closed, which the read will see),
// 0 when the connection should be closed instead.
static int wait_for_request(work_t *w, int fd, int first)
{
    if (w->rb.pos < w->rb.have) return 1;   // pipelined: already buffered

    int grace_ms = first ? FIRST_REQ_GRACE_MS : BUSY_GRACE_MS;
    for (int waited_ms = 0; waited_ms < KEEPALIVE_IDLE_MS; waited_ms += 100) {
        fd_set rf;
        FD_ZERO(&rf);
        FD_SET(fd, &rf);
        struct timeval tv = { .tv_sec = 0, .tv_usec = 100 * 1000 };
        int n = select(fd + 1, &rf, NULL, NULL, &tv);
        if (n != 0) return n > 0;
        if (waited_ms >= grace_ms && __atomic_load_n(&s_idle_workers, __ATOMIC_RELAXED) == 0)
            return 0;
    }
    return 0;
}

static int handle_get(int fd, const hreq_t *hr)
{
    const char *path = hr->uri;
    if (strncmp(path, "http", 4) == 0) {           // absolute form
        const char *a = strstr(path, "://");
        path = a ? strchr(a + 3, '/') : NULL;
        if (!path) path = "/";
    }
    size_t plen = strcspn(path, "?");
    for (size_t i = 0; i < sizeof k_icons / sizeof k_icons[0]; i++) {
        char want[24];
        int n = snprintf(want, sizeof want, "/%s-%u.png", k_icons[i].prefix, k_icons[i].size);
        if ((size_t)n == plen && strncmp(path, want, plen) == 0)
            return hresp_send(fd, 200, "OK", "image/png", k_icons[i].start,
                              (size_t)(k_icons[i].end - k_icons[i].start),
                              hr->keep_alive);
    }
    // Some clients probe the queue with a GET before they trust it.
    static const char page[] = "AirWriter\n";
    return hresp_send(fd, 200, "OK", "text/plain", page, sizeof page - 1,
                      hr->keep_alive);
}

static void handle_conn(work_t *w, int fd)
{
    struct timeval tv = { .tv_sec = 30, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);

    hrb_init(&w->rb, fd);

    int first = 1;
    for (;;) {
        hreq_t hr;
        if (!wait_for_request(w, fd, first)) break;
        first = 0;
        if (hreq_read(&w->rb, &hr) < 0) break;

        int rc;
        if (strcmp(hr.method, "POST") == 0) {
            rc = handle_post(w, fd, &hr);
        } else if (strcmp(hr.method, "GET") == 0) {
            rc = handle_get(fd, &hr);
        } else {
            rc = hresp_send(fd, 405, "Method Not Allowed", "text/plain", "", 0, 0);
        }

        if (rc < 0 || !hr.keep_alive) break;
    }
    close(fd);
}

// Every worker blocks in accept() on the same listening socket, which lwIP
// serialises. No dispatcher task and no connection queue of our own.
static void worker_task(void *arg)
{
    work_t *w = calloc(1, sizeof *w);
    if (!w) {
        ESP_LOGE(TAG, "worker %d: out of memory", (int)(intptr_t)arg);
        vTaskDelete(NULL);
        return;
    }

    while (s_running) {
        struct sockaddr_in peer;
        socklen_t plen = sizeof peer;
        __atomic_add_fetch(&s_idle_workers, 1, __ATOMIC_RELAXED);
        int fd = accept(s_listen_fd, (struct sockaddr *)&peer, &plen);
        __atomic_sub_fetch(&s_idle_workers, 1, __ATOMIC_RELAXED);
        if (fd < 0) {
            if (!s_running) break;
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        handle_conn(w, fd);
    }

    free(w);
    vTaskDelete(NULL);
}

// The printer's own services, _ipp (with its _universal subtype) and _printer:
// what AirPrint finds, and what switching it off takes away (W-1079).
// A printer's Bonjour records: _ipp with the _universal subtype (without it
// iOS never lists the printer, SPEC.md 3.2), and for the port's printer the
// _printer stub older clients probe first. Every call names the instance:
// with none, mDNS takes the newest _ipp, another printer's (W-1082).
// Callers hold s_job_lock or are starting up.
static esp_err_t add_services(printer_t *p)
{
    int slot0 = p == &s_p[0];
    const printer_caps_t *caps = p->caps ? p->caps : &printer_caps_imagewriter;
    char urf[64], bare[sizeof p->uuid];
    p->txt_colour = p->colour;
    printer_caps_urf(caps, p->txt_colour, urf, sizeof urf);
    // The TXT UUID has no urn:uuid: prefix, unlike the IPP attribute.
    const char *u = strstr(p->uuid, "urn:uuid:");
    snprintf(bare, sizeof bare, "%s", u ? u + strlen("urn:uuid:") : p->uuid);
    mdns_txt_item_t txt[] = {
        { "txtvers",  "1" },
        { "rp",       p->resource },
        { "ty",       caps->make_model },
        { "product",  "(AirWriter)" },
        { "pdl",      "image/urf" },
        { "adminurl", s_adminurl },
        { "priority", slot0 ? "10" : "20" },
        { "UUID",     bare },
        { "Color",    p->txt_colour ? "T" : "F" },
        { "Duplex",   "F" },
        { "URF",      urf },
        { "TLS",      "" },
        { "note",     "" },
    };
    esp_err_t err = mdns_service_add_for_host(p->name, "_ipp", "_tcp", NULL, s_cfg.port,
                                              txt, sizeof txt / sizeof txt[0]);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "\"%s\": not advertised (%s)", p->name, esp_err_to_name(err));
        return err;
    }
    snprintf(p->svc_name, sizeof p->svc_name, "%s", p->name);
    p->print_svcs = 1;
    err = mdns_service_subtype_add_for_host(p->name, "_ipp", "_tcp", NULL, "_universal");
    if (err != ESP_OK) ESP_LOGE(TAG, "\"%s\": no _universal subtype, iOS will not list it (%s)",
                                p->name, esp_err_to_name(err));
    if (slot0 && mdns_service_add_for_host(p->name, "_printer", "_tcp", NULL, 515, NULL, 0) != ESP_OK)
        ESP_LOGW(TAG, "no _printer._tcp advertisement");
    return ESP_OK;
}

static void remove_services(printer_t *p)
{
    if (!p->print_svcs) return;
    mdns_service_remove_for_host(p->svc_name, "_ipp", "_tcp", NULL);
    if (p == &s_p[0]) mdns_service_remove_for_host(p->svc_name, "_printer", "_tcp", NULL);
    p->print_svcs = 0;
}

// What a printer offers follows its slot and the box's switch. Callers hold
// s_job_lock or are starting up.
static void refresh(printer_t *p)
{
    p->airprint = p->on && s_box_airprint;
    if (!s_mdns_up) return;
    if (p->airprint && !p->print_svcs) add_services(p);
    else if (!p->airprint && p->print_svcs) remove_services(p);
}

static esp_err_t start_mdns(void)
{
    esp_err_t err = mdns_init();
    if (err != ESP_OK) return err;

    ESP_ERROR_CHECK(mdns_hostname_set(s_hostname));
    ESP_ERROR_CHECK(mdns_instance_name_set(s_cfg.queue_name));

    for (int i = 0; i < IPP_PRINTERS; i++)
        if (s_p[i].airprint && add_services(&s_p[i]) != ESP_OK && i == 0) return ESP_FAIL;
    // The settings page, whether or not AirPrint is on.
    if (mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0) != ESP_OK)
        ESP_LOGW(TAG, "no _http._tcp advertisement");
    return ESP_OK;
}

void ipp_server_set_airprint(int on)
{
    on = on ? 1 : 0;
    if (!s_p) {                                 // before start: start takes it from here
        s_pre_airprint = on;
        return;
    }
    if (on == s_box_airprint) return;
    ESP_LOGI(TAG, "AirPrint %s", on ? "on" : "off: not advertised, not taking jobs");
    if (s_job_lock) xSemaphoreTake(s_job_lock, portMAX_DELAY);
    s_box_airprint = on;
    for (int i = 0; i < IPP_PRINTERS; i++) refresh(&s_p[i]);
    if (s_job_lock) xSemaphoreGive(s_job_lock);
}

static esp_err_t start_listener(void)
{
    s_listen_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s_listen_fd < 0) return ESP_FAIL;

    int one = 1;
    setsockopt(s_listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);

    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_port        = htons(s_cfg.port),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(s_listen_fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
        ESP_LOGE(TAG, "bind %u: errno %d", (unsigned)s_cfg.port, errno);
        close(s_listen_fd);
        s_listen_fd = -1;
        return ESP_FAIL;
    }
    if (listen(s_listen_fd, LISTEN_BACKLOG) < 0) {
        close(s_listen_fd);
        s_listen_fd = -1;
        return ESP_FAIL;
    }

    s_running = 1;
    for (int i = 0; i < WORKERS; i++) {
        char name[12];
        snprintf(name, sizeof name, "ipp%d", i);
        xTaskCreate(worker_task, name, WORKER_STACK, (void *)(intptr_t)i, 5, NULL);
    }
    return ESP_OK;
}

static void fill(printer_t *p, const ipp_printer_cfg_t *cfg);

esp_err_t ipp_server_start(const ipp_server_cfg_t *cfg)
{
    s_p = heap_caps_calloc(IPP_PRINTERS, sizeof *s_p, MALLOC_CAP_SPIRAM);
    if (!s_p) s_p = calloc(IPP_PRINTERS, sizeof *s_p);
    if (!s_p) return ESP_ERR_NO_MEM;
    printer_t *p = &s_p[0];
    s_box_airprint = s_pre_airprint;
    p->stop_reason = s_pre_reason;
    snprintf(p->stop_message, sizeof p->stop_message, "%s", s_pre_message);
    s_cfg  = *cfg;
    if (!s_cfg.port) s_cfg.port = 631;

    strncpy(s_hostname, cfg->hostname, sizeof s_hostname - 1);
    snprintf(s_adminurl, sizeof s_adminurl, "http://%s.local/", s_hostname);
    // W-1088: the port's printer as its record says, or none on the port.
    if (cfg->port_printer) fill(p, cfg->port_printer);
    p->colour = cfg->colour || s_pre_colour ? 1 : 0;
    p->airprint = s_box_airprint;
    for (int i = 0; i < IPP_PRINTERS; i++) {
        s_p[i].next_job_id = 1;
        s_p[i].job.state = IPP_JSTATE_COMPLETED;   // no job yet; nothing is pending
    }

    s_job_lock   = xSemaphoreCreateMutex();
    s_print_lock = xSemaphoreCreateBinary();
    if (!s_job_lock || !s_print_lock) return ESP_ERR_NO_MEM;
    xSemaphoreGive(s_print_lock);         // a binary semaphore starts taken

    esp_err_t err = start_mdns();
    if (err != ESP_OK) return err;
    s_mdns_up = 1;

    err = start_listener();
    if (err != ESP_OK) return err;

    ESP_LOGI(TAG, "listening on port %u", (unsigned)s_cfg.port);
    if (p->on) ESP_LOGI(TAG, "printer 0 \"%s\" at %s, uuid %s", p->name, p->uri, p->uuid);
    return ESP_OK;
}

void ipp_server_stop(void)
{
    s_running = 0;
    if (s_listen_fd >= 0) { close(s_listen_fd); s_listen_fd = -1; }
    s_mdns_up = 0;
    mdns_free();
}

void ipp_server_set_colour(int on)
{
    on = on ? 1 : 0;
    if (!s_p) {
        s_pre_colour = on;
        return;
    }
    printer_t *p = &s_p[0];
    if (on != p->colour) ESP_LOGI(TAG, "colour %s", on ? "on: the colour ribbon is in" : "off");
    p->colour = on;
    // The TXT record is tracked on its own: a change during start_mdns lands
    // in the attributes but not the record, and the call after start fixes it.
    if (!s_mdns_up || !p->print_svcs || on == p->txt_colour) return;
    p->txt_colour = on;
    // By name, as in ipp_server_set_airprint: not a shared LaserWriter's record.
    char urf[64];
    printer_caps_urf(p->caps ? p->caps : &printer_caps_imagewriter, on, urf, sizeof urf);
    esp_err_t a = mdns_service_txt_item_set_for_host(p->svc_name, "_ipp", "_tcp", NULL, "Color",
                                                     on ? "T" : "F");
    esp_err_t b = mdns_service_txt_item_set_for_host(p->svc_name, "_ipp", "_tcp", NULL, "URF", urf);
    if (a != ESP_OK || b != ESP_OK)
        ESP_LOGW(TAG, "TXT update failed: %s / %s", esp_err_to_name(a), esp_err_to_name(b));
}

const char *ipp_server_uri(void) { return s_p ? s_p[0].uri : ""; }

void ipp_server_job_done_for(int idx, int ok)
{
    if (idx < 0 || idx >= IPP_PRINTERS || !s_p) return;
    printer_t *p = &s_p[idx];
    uint8_t state;
    const char *reason;
    xSemaphoreTake(s_job_lock, portMAX_DELAY);
    int ended = ipp_job_done(&p->job, ok);
    int32_t id = p->job.id;
    ipp_job_view(&p->job, 0, &state, &reason);
    xSemaphoreGive(s_job_lock);

    if (!ended) {
        ESP_LOGE(TAG, "job done reported with no job printing");
        return;
    }
    ESP_LOGI(TAG, "%sjob %d: %s", idx ? "LaserWriter " : "", (int)id, reason);
    xSemaphoreGive(s_print_lock);
}

void ipp_server_job_done(int ok) { ipp_server_job_done_for(0, ok); }

void ipp_server_set_stopped_for(int idx, const char *reason_or_null, const char *message)
{
    if (idx < 0 || idx >= IPP_PRINTERS) return;
    if (!s_p) {                                 // before start: the ImageWriter's, kept
        if (idx == 0) {
            s_pre_reason = reason_or_null;
            snprintf(s_pre_message, sizeof s_pre_message, "%s", message ? message : "");
        }
        return;
    }
    printer_t *p = &s_p[idx];
    if (s_job_lock) xSemaphoreTake(s_job_lock, portMAX_DELAY);
    int changed = p->stop_reason != reason_or_null;
    p->stop_reason = reason_or_null;
    snprintf(p->stop_message, sizeof p->stop_message, "%s", message ? message : "");
    if (s_job_lock) xSemaphoreGive(s_job_lock);
    if (changed)
        ESP_LOGW(TAG, "%sprinter-state-reasons now %s", idx ? "LaserWriter " : "",
                 reason_or_null ? reason_or_null : "none");
}

void ipp_server_set_stopped(const char *reason_or_null, const char *message)
{
    ipp_server_set_stopped_for(0, reason_or_null, message);
}

void ipp_server_set_state_for(int idx, const char *reason, const char *message, int stopped,
                              int busy)
{
    if (idx < 0 || idx >= IPP_PRINTERS || !s_p || !s_job_lock) return;
    printer_t *p = &s_p[idx];
    int real = reason && strcmp(reason, "none");
    xSemaphoreTake(s_job_lock, portMAX_DELAY);
    p->stop_reason = stopped && real ? reason : NULL;
    p->note_reason = !stopped && real ? reason : NULL;
    p->busy = busy ? 1 : 0;
    snprintf(p->stop_message, sizeof p->stop_message, "%s", message ? message : "");
    xSemaphoreGive(s_job_lock);
}

void ipp_server_job_message_for(int idx, const char *msg)
{
    if (idx < 0 || idx >= IPP_PRINTERS || !s_p || !s_job_lock) return;
    xSemaphoreTake(s_job_lock, portMAX_DELAY);
    snprintf(s_p[idx].job_message, sizeof s_p[idx].job_message, "%s", msg ? msg : "");
    xSemaphoreGive(s_job_lock);
}

// A slot's printer from its cfg; strings and caps are copied.
static void fill(printer_t *p, const ipp_printer_cfg_t *cfg)
{
    snprintf(p->resource, sizeof p->resource, "%s", cfg->resource);
    snprintf(p->legacy, sizeof p->legacy, "%s", cfg->legacy_resource ? cfg->legacy_resource : "");
    snprintf(p->uri, sizeof p->uri, "ipp://%s.local:%u/%s", s_hostname,
             (unsigned)s_cfg.port, cfg->resource);
    snprintf(p->name, sizeof p->name, "%s", cfg->name);
    snprintf(p->info, sizeof p->info, "%s", cfg->info);
    snprintf(p->uuid, sizeof p->uuid, "%s", cfg->uuid);
    p->caps_copy = *cfg->caps;
    snprintf(p->make_model, sizeof p->make_model, "%s", cfg->caps->make_model);
    snprintf(p->device_id, sizeof p->device_id, "%s", cfg->caps->device_id);
    p->caps_copy.make_model = p->make_model;
    p->caps_copy.device_id = p->device_id;
    p->caps = &p->caps_copy;
    p->sink = cfg->sink;
    icon_uris(p, cfg->icon ? cfg->icon : "icon");
    p->on = 1;
}

esp_err_t ipp_server_set_printer(int idx, const ipp_printer_cfg_t *cfg)
{
    if (idx < 0 || idx >= IPP_PRINTERS || !s_p || !s_job_lock) return ESP_ERR_INVALID_ARG;
    printer_t *p = &s_p[idx];
    xSemaphoreTake(s_job_lock, portMAX_DELAY);
    if (ipp_job_active(&p->job)) {
        xSemaphoreGive(s_job_lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (s_mdns_up) remove_services(p);
    p->on = 0;
    // The port's printer keeps its line's state (W-1088: a rename re-puts the
    // slot, and the line has not changed); an AppleTalk printer's is pushed
    // again by laser.c.
    if (idx) p->stop_reason = p->note_reason = NULL;
    p->busy = 0;
    if (cfg) {
        fill(p, cfg);
        if (idx) p->colour = 0;               // the port's printer keeps its ribbon's
    }
    refresh(p);
    xSemaphoreGive(s_job_lock);
    if (cfg) ESP_LOGI(TAG, "printer %d \"%s\" at %s, uuid %s", idx, p->name, p->uri, p->uuid);
    else ESP_LOGI(TAG, "printer %d no longer shared", idx);
    return ESP_OK;
}
