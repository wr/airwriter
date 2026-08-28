// SPDX-License-Identifier: GPL-3.0-or-later
#include "ipp_server.h"
#include "ipp_req.h"
#include "ipp_attrs.h"
#include "ipp_http.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/socket.h>
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

// One job at a time. printer_state owns the real queue later; M4 only needs
// enough to answer Get-Job-Attributes while a job is in flight.
#define JOB_NAME_MAX  IPP_NAME_MAX
#define RESP_MAX      5120   // printer attributes measure ~3.2KB with media-col
#define HDRBUF_MAX    2048   // the IPP attribute section, before the document
#define CHUNK         1024

// iOS polls Get-Job-Attributes on a second connection while the document is
// still streaming on the first. One worker would make those polls queue behind
// the transfer, and the phone would call the printer unresponsive.
#define WORKERS       3
#define WORKER_STACK  5120

typedef struct {
    int32_t  id;
    uint8_t  state;
    char     name[JOB_NAME_MAX];
    char     user[JOB_NAME_MAX];
    char     uri[160];
    uint32_t created_s;
    int      canceled;
} job_t;

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
static ipp_server_cfg_t s_cfg;
static ipp_doc_sink_t   s_sink;
static char s_uri[128];
static char s_adminurl[96];
static char s_uuid[64];
static char s_hostname[40];

static job_t s_job;
static int32_t s_next_job_id = 1;
static const char *s_stop_reason;   // NULL when the printer is fine
static SemaphoreHandle_t s_job_lock;     // guards s_job and s_next_job_id
static SemaphoreHandle_t s_print_lock;   // one document in the sink at a time

static uint32_t uptime_s(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000000);
}

// printer-uuid must survive a reflash, so it is derived from the eFuse MAC.
// In dev mode it is random every boot instead: iOS caches capabilities against
// the UUID hard enough that a stale media list otherwise outlives any number
// of reflashes. SPEC.md 3.5.
static void make_uuid(int dev_mode)
{
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);

    uint16_t rnd = dev_mode ? (uint16_t)(esp_random() & 0xFFFF) : 0;
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
    // Still accepting: the job queues and prints when paper goes back in,
    // which is what a user expects after clearing the alert.
    p->accepting_jobs = 1;
    p->queued_jobs    = (s_job.state == IPP_JSTATE_PROCESSING) ? 1 : 0;
    p->uptime_s       = uptime_s();
}

static void fill_job(ipp_job_t *j, const job_t *src)
{
    j->id               = src->id;
    j->uri              = src->uri;
    j->printer_uri      = s_uri;
    j->name             = src->name[0] ? src->name : "Untitled";
    j->user             = src->user[0] ? src->user : "anonymous";
    j->state            = src->state;
    j->state_reason     = src->state == IPP_JSTATE_COMPLETED ? "job-completed-successfully"
                        : src->state == IPP_JSTATE_CANCELED  ? "job-canceled-by-user"
                        : src->state == IPP_JSTATE_PROCESSING ? "job-printing"
                        : "none";
    j->impressions_done = 0;
    j->time_at_creation = src->created_s;
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

static int format_supported(const char *fmt)
{
    // An absent or unspecified format means "you decide", which for this
    // printer is URF. Only an explicit foreign format is a refusal.
    return fmt[0] == '\0'
        || strcmp(fmt, "image/urf") == 0
        || strcmp(fmt, "application/octet-stream") == 0;
}

static int op_print_job(work_t *w, int fd, const ipp_req_t *r, hbody_t *body,
                        const uint8_t *pre, size_t pre_len, int keep_alive)
{
    if (!format_supported(r->document_format)) {
        ESP_LOGW(TAG, "refusing document-format %s", r->document_format);
        hbody_drain(body);
        return send_status(w, fd, r, IPP_STATUS_ERR_FORMAT_NOT_SUPPORTED, keep_alive);
    }

    // Only one document may be in the sink at a time. A second job waits here
    // rather than interleaving its raster into the same printer.
    if (xSemaphoreTake(s_print_lock, pdMS_TO_TICKS(30000)) != pdTRUE) {
        hbody_drain(body);
        return send_status(w, fd, r, IPP_STATUS_ERR_NOT_ACCEPTING_JOBS, keep_alive);
    }

    xSemaphoreTake(s_job_lock, portMAX_DELAY);
    memset(&s_job, 0, sizeof s_job);
    s_job.id        = s_next_job_id++;
    s_job.state     = IPP_JSTATE_PROCESSING;
    s_job.created_s = uptime_s();
    strncpy(s_job.name, r->job_name, sizeof s_job.name - 1);
    strncpy(s_job.user, r->requesting_user, sizeof s_job.user - 1);
    snprintf(s_job.uri, sizeof s_job.uri, "%s/%d", s_uri, (int)s_job.id);
    int32_t job_id = s_job.id;
    xSemaphoreGive(s_job_lock);

    int ok = 1;
    if (s_sink.begin && s_sink.begin(s_sink.ctx, r->document_format, s_job.name) != ESP_OK)
        ok = 0;

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
                ESP_LOGW(TAG, "job %d: body truncated", (int)job_id);
                ok = 0;
                break;
            }
            if (s_sink.data && s_sink.data(s_sink.ctx, w->chunk, (size_t)n) != ESP_OK)
                ok = 0;
            total += (size_t)n;
        }
    }
    if (s_sink.end) s_sink.end(s_sink.ctx, ok);

    xSemaphoreTake(s_job_lock, portMAX_DELAY);
    s_job.state = s_job.canceled ? IPP_JSTATE_CANCELED
                : ok             ? IPP_JSTATE_COMPLETED
                                 : IPP_JSTATE_ABORTED;
    job_t snapshot = s_job;
    xSemaphoreGive(s_job_lock);
    xSemaphoreGive(s_print_lock);

    ESP_LOGI(TAG, "job %d: %u document bytes, %s",
             (int)job_id, (unsigned)total, ok ? "complete" : "failed");

    // Respond only now. A client that sees the response while it is still
    // sending treats the connection as finished, and the rest is lost.
    ipp_job_t j;
    fill_job(&j, &snapshot);
    ipp_enc_t e;
    ipp_enc_init(&e, w->resp, RESP_MAX);
    ipp_resp_begin(&e, resp_version(r), IPP_STATUS_OK, r->request_id);
    ipp_write_op_attrs(&e);
    ipp_write_job_attrs(&e, &j);
    ipp_end(&e);
    return send_ipp(fd, &e, keep_alive);
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
        ipp_write_printer_attrs(&e, &p);
        ipp_end(&e);
        return send_ipp(fd, &e, keep_alive);
    }

    case IPP_OP_VALIDATE_JOB:
        if (!format_supported(r->document_format))
            return send_status(w, fd, r, IPP_STATUS_ERR_FORMAT_NOT_SUPPORTED, keep_alive);
        return send_status(w, fd, r, IPP_STATUS_OK, keep_alive);

    case IPP_OP_PRINT_JOB:
        return op_print_job(w, fd, r, body, pre, pre_len, keep_alive);

    case IPP_OP_GET_JOB_ATTRIBUTES: {
        xSemaphoreTake(s_job_lock, portMAX_DELAY);
        int found = (r->job_id < 0 || r->job_id == s_job.id);
        job_t snapshot = s_job;
        xSemaphoreGive(s_job_lock);
        if (!found)
            return send_status(w, fd, r, IPP_STATUS_ERR_NOT_FOUND, keep_alive);
        ipp_job_t j;
        fill_job(&j, &snapshot);
        ipp_resp_begin(&e, resp_version(r), IPP_STATUS_OK, r->request_id);
        ipp_write_op_attrs(&e);
        ipp_write_job_attrs(&e, &j);
        ipp_end(&e);
        return send_ipp(fd, &e, keep_alive);
    }

    case IPP_OP_GET_JOBS: {
        xSemaphoreTake(s_job_lock, portMAX_DELAY);
        job_t snapshot = s_job;
        xSemaphoreGive(s_job_lock);
        ipp_resp_begin(&e, resp_version(r), IPP_STATUS_OK, r->request_id);
        ipp_write_op_attrs(&e);
        // An empty list is a complete answer, and the only honest one while no
        // job is in flight.
        if (snapshot.state == IPP_JSTATE_PROCESSING) {
            ipp_job_t j;
            fill_job(&j, &snapshot);
            ipp_write_job_attrs(&e, &j);
        }
        ipp_end(&e);
        return send_ipp(fd, &e, keep_alive);
    }

    case IPP_OP_CANCEL_JOB: {
        xSemaphoreTake(s_job_lock, portMAX_DELAY);
        int found = (r->job_id < 0 || r->job_id == s_job.id);
        int was_printing = found && s_job.state == IPP_JSTATE_PROCESSING;
        if (found) {
            s_job.canceled = 1;
            if (was_printing) s_job.state = IPP_JSTATE_CANCELED;
        }
        xSemaphoreGive(s_job_lock);

        // Stop the paper, not just the bookkeeping. Called outside the lock:
        // the pipeline takes it too as it unwinds.
        if (was_printing && s_sink.cancel) s_sink.cancel(s_sink.ctx);
        return send_status(w, fd, r,
                           found ? IPP_STATUS_OK : IPP_STATUS_ERR_NOT_FOUND, keep_alive);
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

static void handle_conn(work_t *w, int fd)
{
    struct timeval tv = { .tv_sec = 30, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);

    hrb_init(&w->rb, fd);

    for (;;) {
        hreq_t hr;
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
        int fd = accept(s_listen_fd, (struct sockaddr *)&peer, &plen);
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
    if (listen(s_listen_fd, WORKERS + 2) < 0) {
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
    s_print_lock = xSemaphoreCreateMutex();
    if (!s_job_lock || !s_print_lock) return ESP_ERR_NO_MEM;
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

void ipp_server_set_stopped(const char *reason_or_null)
{
    if (!s_job_lock) { s_stop_reason = reason_or_null; return; }
    xSemaphoreTake(s_job_lock, portMAX_DELAY);
    s_stop_reason = reason_or_null;
    xSemaphoreGive(s_job_lock);
    ESP_LOGW(TAG, "printer-state-reasons now %s", reason_or_null ? reason_or_null : "none");
}
