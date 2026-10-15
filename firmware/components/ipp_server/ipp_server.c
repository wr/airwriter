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
#include "esp_random.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "mdns.h"

static const char *TAG = "ipp";

// One job at a time, in ipp_job's slot: enough to answer Get-Job-Attributes
// while a job is in flight.
#define RESP_MAX      5120   // printer attributes measure ~3.2KB with media-col
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
static ipp_doc_sink_t   s_sink;
static char s_uri[128];
static char s_adminurl[96];
static char s_uuid[64];
static char s_hostname[40];

static ipp_job_rec_t s_job;
static int32_t s_next_job_id = 1;
static int s_printer_waiters;       // requests blocked in acquire_printer
static const char *s_stop_reason;   // NULL when the printer is fine
static const char *s_stop_message;  // printer-state-message while stopped
static SemaphoreHandle_t s_job_lock;     // guards s_job, s_next_job_id, s_stop_reason
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

// printer-uuid must survive a reflash, so it is derived from the eFuse MAC.
// In dev mode it is random every boot instead: iOS caches capabilities against
// the UUID hard enough that a stale media list otherwise outlives any number
// of reflashes. SPEC.md 3.5. Outside dev mode the third group is the UUID
// epoch, bumped by hand when the advertised capabilities change.
static void make_uuid(int dev_mode)
{
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);

    uint16_t rnd = dev_mode ? (uint16_t)(esp_random() & 0xFFFF)
                            : (uint16_t)CONFIG_AIRWRITER_UUID_EPOCH;
    snprintf(s_uuid, sizeof s_uuid,
             "urn:uuid:41575254-4552-%04x-%02x%02x-%02x%02x%02x%02x",
             rnd, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

// Callers hold s_job_lock.
static void fill_printer(ipp_printer_t *p)
{
    p->uri            = s_uri;
    p->name           = s_cfg.queue_name;
    p->info           = "Apple ImageWriter II";
    p->uuid           = s_uuid;
    p->adminurl       = s_adminurl;
    p->location       = "";
    p->state          = s_stop_reason               ? IPP_PSTATE_STOPPED
                      : (s_job.state == IPP_JSTATE_PROCESSING) ? IPP_PSTATE_PROCESSING
                                                               : IPP_PSTATE_IDLE;
    p->state_reason   = s_stop_reason ? s_stop_reason : "none";
    p->state_message  = s_stop_reason ? s_stop_message : NULL;
    // Still accepting: the job queues and prints when paper goes back in,
    // which is what a user expects after clearing the alert.
    p->accepting_jobs = 1;
    p->queued_jobs    = ipp_job_active(&s_job) ? 1 : 0;
    p->uptime_s       = uptime_s();
}

// `src` and `printer_stopped` are a snapshot taken under s_job_lock; the
// strings point into `src`, so it must outlive the encode.
static void fill_job(ipp_job_t *j, const ipp_job_rec_t *src, int printer_stopped)
{
    j->id               = src->id;
    j->uri              = src->uri;
    j->printer_uri      = s_uri;
    j->name             = src->name[0] ? src->name : "Untitled";
    j->user             = src->user[0] ? src->user : "anonymous";
    ipp_job_view(src, printer_stopped, &j->state, &j->state_reason);
    j->impressions_done = 0;
    j->time_at_creation = src->created_s;
}

// A copy of the job and whether the printer is stopped, consistent with each
// other.
static ipp_job_rec_t job_snapshot(int *printer_stopped)
{
    xSemaphoreTake(s_job_lock, portMAX_DELAY);
    ipp_job_rec_t snap = s_job;
    *printer_stopped = s_stop_reason != NULL;
    xSemaphoreGive(s_job_lock);
    return snap;
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
static int send_job(work_t *w, int fd, const ipp_req_t *r, const ipp_job_rec_t *job,
                    int printer_stopped, int keep_alive)
{
    ipp_job_t j;
    fill_job(&j, job, printer_stopped);
    ipp_enc_t e;
    ipp_enc_init(&e, w->resp, RESP_MAX);
    ipp_resp_begin(&e, resp_version(r), IPP_STATUS_OK, r->request_id);
    ipp_write_op_attrs(&e);
    ipp_write_job_attrs(&e, &j);
    ipp_end(&e);
    return send_ipp(fd, &e, keep_alive);
}

// Waits for the printer, as long as a client will reasonably wait for an
// answer. A second job waits here rather than interleaving its raster with the
// first.
//
// Only one request waits at a time; the rest are told server-error-busy at
// once. Each waiter pins a worker, and a paused job bigger than the job
// buffer holds another for minutes; the phone's Get-Job-Attributes polls and
// Cancel-Job need the rest.
static int acquire_printer(void)
{
    if (xSemaphoreTake(s_print_lock, 0) == pdTRUE) return 1;

    xSemaphoreTake(s_job_lock, portMAX_DELAY);
    int first = (s_printer_waiters == 0);
    if (first) s_printer_waiters++;
    xSemaphoreGive(s_job_lock);
    if (!first) return 0;

    int got = xSemaphoreTake(s_print_lock, pdMS_TO_TICKS(30000)) == pdTRUE;

    xSemaphoreTake(s_job_lock, portMAX_DELAY);
    s_printer_waiters--;
    xSemaphoreGive(s_job_lock);
    return got;
}

// Puts a new job in the slot. The caller holds s_print_lock.
static int32_t start_job(const ipp_req_t *r)
{
    xSemaphoreTake(s_job_lock, portMAX_DELAY);
    int32_t id = s_next_job_id++;
    ipp_job_start(&s_job, id, uptime_s(), r->job_name, r->requesting_user, s_uri);
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
                        int keep_alive)
{
    xSemaphoreTake(s_job_lock, portMAX_DELAY);
    ipp_job_rec_t kept = s_job;
    xSemaphoreGive(s_job_lock);

    int ok = 1;
    if (s_sink.begin && s_sink.begin(s_sink.ctx, r->document_format, kept.name) != ESP_OK)
        ok = 0;

    // begin() can wait out a test page, and a Cancel-Job in that time reached
    // a sink with nothing started, which begin() then cleared. Pass it on
    // again now that there is a job to stop.
    if (ok) {
        xSemaphoreTake(s_job_lock, portMAX_DELAY);
        if (s_job.canceled && s_sink.cancel) s_sink.cancel(s_sink.ctx);
        xSemaphoreGive(s_job_lock);
    }

    size_t total = 0;
    if (ok) {
        // The attribute section and the first document bytes usually arrive in
        // one read, so hand that slice over before touching the socket again.
        if (pre_len && s_sink.data && s_sink.data(s_sink.ctx, pre, pre_len) != ESP_OK)
            ok = 0;
        total += pre_len;

        while (ok) {
            int n = hbody_read(body, w->chunk, CHUNK);
            if (n == 0) break;
            if (n < 0) {
                // The client hung up mid-document. With the job buffered the
                // socket is read to the end, so this is seen at once rather
                // than when the printer next takes a byte.
                ESP_LOGW(TAG, "job %d: body truncated", (int)job_id);
                ok = 0;
                break;
            }
            if (s_sink.data && s_sink.data(s_sink.ctx, w->chunk, (size_t)n) != ESP_OK)
                ok = 0;
            total += (size_t)n;
        }
    }

    ESP_LOGI(TAG, "job %d: %u document bytes, %s",
             (int)job_id, (unsigned)total, ok ? "received" : "failed");
    if (s_sink.end) s_sink.end(s_sink.ctx, ok);
    else ipp_server_job_done(ok);

    // Respond only now, with the whole body in. A client that sees the
    // response while it is still sending treats the connection as finished,
    // and the rest is lost. The job may already be over -- a sink that could
    // not take it -- and its printer handed to the next job, in which case
    // the slot holds that one and the copy kept here is finished instead.
    xSemaphoreTake(s_job_lock, portMAX_DELAY);
    if (s_job.id == job_id) kept = s_job;
    else ipp_job_finish(&kept, 0);
    int stopped = s_stop_reason != NULL;
    xSemaphoreGive(s_job_lock);
    return send_job(w, fd, r, &kept, stopped, keep_alive);
}

static int op_print_job(work_t *w, int fd, const ipp_req_t *r, hbody_t *body,
                        const uint8_t *pre, size_t pre_len, int keep_alive)
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
    int32_t job_id = start_job(r);
    return run_document(w, fd, r, body, pre, pre_len, job_id, keep_alive);
}

static int dispatch(work_t *w, int fd, const ipp_req_t *r, hbody_t *body,
                    const uint8_t *pre, size_t pre_len, int keep_alive)
{
    ipp_enc_t e;
    ipp_enc_init(&e, w->resp, RESP_MAX);

    // Print-Job owns the body. Every other operation must consume it before
    // replying, or the next request on this connection starts mid-document.
    if (r->op != IPP_OP_PRINT_JOB && hbody_drain(body) < 0) return -1;

    switch (r->op) {
    case IPP_OP_GET_PRINTER_ATTRIBUTES: {
        ipp_printer_t p;
        xSemaphoreTake(s_job_lock, portMAX_DELAY);
        fill_printer(&p);
        xSemaphoreGive(s_job_lock);
        ipp_resp_begin(&e, resp_version(r), IPP_STATUS_OK, r->request_id);
        ipp_write_op_attrs(&e);
        ipp_write_printer_attrs(&e, &p, &r->requested);
        ipp_end(&e);
        return send_ipp(fd, &e, keep_alive);
    }

    case IPP_OP_VALIDATE_JOB:
        if (s_cfg.on_job_op) s_cfg.on_job_op();
        if (!format_supported(r->document_format))
            return send_status(w, fd, r, IPP_STATUS_ERR_FORMAT_NOT_SUPPORTED, keep_alive);
        return send_status(w, fd, r, IPP_STATUS_OK, keep_alive);

    case IPP_OP_PRINT_JOB:
        if (s_cfg.on_job_op) s_cfg.on_job_op();
        return op_print_job(w, fd, r, body, pre, pre_len, keep_alive);

    case IPP_OP_GET_JOB_ATTRIBUTES: {
        int stopped;
        ipp_job_rec_t snapshot = job_snapshot(&stopped);
        if (!ipp_job_matches(&snapshot, r->job_id))
            return send_status(w, fd, r, IPP_STATUS_ERR_NOT_FOUND, keep_alive);
        return send_job(w, fd, r, &snapshot, stopped, keep_alive);
    }

    case IPP_OP_GET_JOBS: {
        int stopped;
        ipp_job_rec_t snapshot = job_snapshot(&stopped);
        ipp_resp_begin(&e, resp_version(r), IPP_STATUS_OK, r->request_id);
        ipp_write_op_attrs(&e);
        // An empty list is a complete answer, and the only honest one while no
        // job is in flight.
        if (ipp_job_active(&snapshot)) {
            ipp_job_t j;
            fill_job(&j, &snapshot, stopped);
            ipp_write_job_attrs(&e, &j);
        }
        ipp_end(&e);
        return send_ipp(fd, &e, keep_alive);
    }

    case IPP_OP_CANCEL_JOB: {
        xSemaphoreTake(s_job_lock, portMAX_DELAY);
        ipp_cancel_t c = ipp_job_cancel(&s_job, r->job_id);
        int32_t id = s_job.id;
        // Stop the paper, not just the bookkeeping. Under the lock, so the job
        // cannot finish in between and hand the printer to the next one, which
        // the cancel would then land on.
        if (c == IPP_CANCEL_STOP_PRINT && s_sink.cancel) s_sink.cancel(s_sink.ctx);
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

    ESP_LOGI(TAG, "op 0x%04x id %u from %s", r.op, (unsigned)r.request_id,
             r.requesting_user[0] ? r.requesting_user : "-");

    // Major version 1 and 2 share this encoding; anything else we cannot
    // speak, but the client still needs a well-formed answer.
    if ((r.version >> 8) != 1 && (r.version >> 8) != 2) {
        hbody_drain(&body);
        return send_status(w, fd, &r, IPP_STATUS_ERR_BAD_REQUEST, hr->keep_alive);
    }

    return dispatch(w, fd, &r, &body, w->hdr + r.header_len, have - r.header_len,
                    hr->keep_alive);
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
            // Some clients probe the queue with a GET before they trust it.
            static const char page[] = "AirWriter\n";
            rc = hresp_send(fd, 200, "OK", "text/plain", page, sizeof page - 1,
                            hr.keep_alive);
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

static esp_err_t start_mdns(void)
{
    esp_err_t err = mdns_init();
    if (err != ESP_OK) return err;

    ESP_ERROR_CHECK(mdns_hostname_set(s_hostname));
    ESP_ERROR_CHECK(mdns_instance_name_set(s_cfg.queue_name));

    mdns_txt_item_t txt[] = {
        { "txtvers",  "1" },
        { "rp",       "ipp/print" },
        { "ty",       "Apple ImageWriter II" },
        { "product",  "(AirWriter)" },
        { "pdl",      "image/urf" },
        { "adminurl", s_adminurl },
        { "priority", "10" },
        { "UUID",     s_uuid },
        { "Color",    "F" },
        { "Duplex",   "F" },
        { "URF",      IPP_URF_STRING },
        { "TLS",      "" },
        { "note",     "" },
    };
    // The TXT UUID has no urn:uuid: prefix, unlike the IPP attribute.
    const char *bare_uuid = strstr(s_uuid, "urn:uuid:");
    if (bare_uuid) txt[7].value = bare_uuid + strlen("urn:uuid:");

    // The service has to exist before a subtype can attach to it.
    ESP_ERROR_CHECK(mdns_service_add(NULL, "_ipp", "_tcp", s_cfg.port,
                                     txt, sizeof txt / sizeof txt[0]));

    // Without the _universal subtype iOS never lists the printer, however
    // correct the rest of the advertisement is. SPEC.md 3.2.
    //
    // The first argument is the instance name, not the service type. NULL
    // means the default instance -- passing "_ipp" there looks plausible and
    // fails with "service doesn't exist".
    err = mdns_service_subtype_add_for_host(NULL, "_ipp", "_tcp", NULL, "_universal");
    if (err != ESP_OK) ESP_LOGE(TAG, "no _universal subtype: iOS will not list us (%s)",
                                esp_err_to_name(err));

    // A stub for older clients that probe LPD before IPP. Neither of these is
    // worth failing the printer over, so they only warn.
    if (mdns_service_add(NULL, "_printer", "_tcp", 515, NULL, 0) != ESP_OK)
        ESP_LOGW(TAG, "no _printer._tcp advertisement");
    if (mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0) != ESP_OK)
        ESP_LOGW(TAG, "no _http._tcp advertisement");

    return ESP_OK;
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

esp_err_t ipp_server_start(const ipp_server_cfg_t *cfg)
{
    s_cfg  = *cfg;
    s_sink = cfg->sink;
    if (!s_cfg.port) s_cfg.port = 631;

    strncpy(s_hostname, cfg->hostname, sizeof s_hostname - 1);
    snprintf(s_uri, sizeof s_uri, "ipp://%s.local:%u/ipp/print",
             s_hostname, (unsigned)s_cfg.port);
    snprintf(s_adminurl, sizeof s_adminurl, "http://%s.local/", s_hostname);
    make_uuid(cfg->dev_mode);

    s_job_lock   = xSemaphoreCreateMutex();
    s_print_lock = xSemaphoreCreateBinary();
    if (!s_job_lock || !s_print_lock) return ESP_ERR_NO_MEM;
    xSemaphoreGive(s_print_lock);         // a binary semaphore starts taken
    s_job.state = IPP_JSTATE_COMPLETED;   // no job yet; nothing is pending

    esp_err_t err = start_mdns();
    if (err != ESP_OK) return err;

    err = start_listener();
    if (err != ESP_OK) return err;

    ESP_LOGI(TAG, "listening on %s", s_uri);
    ESP_LOGI(TAG, "uuid %s%s", s_uuid, cfg->dev_mode ? "  (dev mode, new each boot)" : "");
    return ESP_OK;
}

void ipp_server_stop(void)
{
    s_running = 0;
    if (s_listen_fd >= 0) { close(s_listen_fd); s_listen_fd = -1; }
    mdns_free();
}

const char *ipp_server_uri(void) { return s_uri; }

void ipp_server_job_done(int ok)
{
    uint8_t state;
    const char *reason;
    xSemaphoreTake(s_job_lock, portMAX_DELAY);
    int ended = ipp_job_done(&s_job, ok);
    int32_t id = s_job.id;
    ipp_job_view(&s_job, 0, &state, &reason);
    xSemaphoreGive(s_job_lock);

    if (!ended) {
        ESP_LOGE(TAG, "job done reported with no job printing");
        return;
    }
    ESP_LOGI(TAG, "job %d: %s", (int)id, reason);
    xSemaphoreGive(s_print_lock);
}

void ipp_server_set_stopped(const char *reason_or_null, const char *message)
{
    if (!s_job_lock) { s_stop_reason = reason_or_null; s_stop_message = message; return; }
    xSemaphoreTake(s_job_lock, portMAX_DELAY);
    s_stop_reason  = reason_or_null;
    s_stop_message = message;
    xSemaphoreGive(s_job_lock);
    ESP_LOGW(TAG, "printer-state-reasons now %s", reason_or_null ? reason_or_null : "none");
}
