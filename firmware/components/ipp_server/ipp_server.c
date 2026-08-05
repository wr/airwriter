// SPDX-License-Identifier: GPL-3.0-or-later
#include "ipp_server.h"
#include "ipp_req.h"
#include "ipp_attrs.h"

#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_random.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "esp_http_server.h"
#include "mdns.h"

static const char *TAG = "ipp";

// One job at a time. printer_state owns the real queue later; M4 only needs
// enough to answer Get-Job-Attributes while a job is in flight.
#define JOB_NAME_MAX  IPP_NAME_MAX
#define RESP_MAX      3584   // printer attributes measure ~2.6KB
#define CHUNK         1024

typedef struct {
    int32_t  id;
    uint8_t  state;
    char     name[JOB_NAME_MAX];
    char     user[JOB_NAME_MAX];
    char     uri[160];
    uint32_t created_s;
    int      canceled;
} job_t;

static httpd_handle_t s_httpd;
static ipp_server_cfg_t s_cfg;
static ipp_doc_sink_t   s_sink;
static char s_uri[128];
static char s_adminurl[96];
static char s_uuid[64];
static char s_hostname[40];
static job_t s_job;
static int32_t s_next_job_id = 1;

// Static scratch. These are too big for the httpd task stack and there is only
// ever one request in flight -- the server runs a single worker.
static uint8_t s_resp[RESP_MAX];
static uint8_t s_hdrbuf[2048];

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

static void fill_printer(ipp_printer_t *p)
{
    p->uri            = s_uri;
    p->name           = s_cfg.queue_name;
    p->info           = "Apple ImageWriter II";
    p->uuid           = s_uuid;
    p->adminurl       = s_adminurl;
    p->location       = "";
    p->state          = (s_job.state == IPP_JSTATE_PROCESSING)
                          ? IPP_PSTATE_PROCESSING : IPP_PSTATE_IDLE;
    p->state_reason   = "none";
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

static esp_err_t send_ipp(httpd_req_t *req, const ipp_enc_t *e)
{
    if (!ipp_enc_ok(e)) {
        ESP_LOGE(TAG, "response overflowed %d bytes", RESP_MAX);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "ipp encode");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/ipp");
    return httpd_resp_send(req, (const char *)e->buf, ipp_enc_len(e));
}

// A response carrying nothing but a status. Every error path uses this rather
// than closing the connection, which SPEC.md 3.4 calls for explicitly.
static esp_err_t send_status(httpd_req_t *req, uint16_t status, uint32_t request_id)
{
    ipp_enc_t e;
    ipp_enc_init(&e, s_resp, sizeof s_resp);
    ipp_resp_begin(&e, status, request_id);
    ipp_write_op_attrs(&e);
    ipp_end(&e);
    return send_ipp(req, &e);
}

static int format_supported(const char *fmt)
{
    // An absent or unspecified format means "you decide", which for this
    // printer is URF. Only an explicit foreign format is a refusal.
    return fmt[0] == '\0'
        || strcmp(fmt, "image/urf") == 0
        || strcmp(fmt, "application/octet-stream") == 0;
}

// Streams the rest of the body to the sink. `pre` is the slice of document
// bytes that already arrived in the header buffer.
static esp_err_t stream_document(httpd_req_t *req, const uint8_t *pre, size_t pre_len,
                                 size_t consumed_total)
{
    size_t total = 0;
    esp_err_t err = ESP_OK;

    if (pre_len && s_sink.data) err = s_sink.data(s_sink.ctx, pre, pre_len);
    total += pre_len;

    // content_len is the whole IPP message, header included.
    size_t remaining = (req->content_len > consumed_total)
                     ? req->content_len - consumed_total : 0;

    uint8_t buf[CHUNK];
    while (remaining && err == ESP_OK) {
        int n = httpd_req_recv(req, (char *)buf, remaining < CHUNK ? remaining : CHUNK);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (n <= 0) {
            ESP_LOGW(TAG, "body ended %u bytes early", (unsigned)remaining);
            if (s_sink.end) s_sink.end(s_sink.ctx, 0);
            return ESP_FAIL;
        }
        if (s_sink.data) err = s_sink.data(s_sink.ctx, buf, (size_t)n);
        total += (size_t)n;
        remaining -= (size_t)n;
    }

    if (s_sink.end) s_sink.end(s_sink.ctx, err == ESP_OK);
    ESP_LOGI(TAG, "job %d: %u document bytes", (int)s_job.id, (unsigned)total);
    return err;
}

static esp_err_t op_print_job(httpd_req_t *req, const ipp_req_t *r,
                              const uint8_t *hdr, size_t hdr_have)
{
    if (!format_supported(r->document_format)) {
        ESP_LOGW(TAG, "refusing document-format %s", r->document_format);
        return send_status(req, IPP_STATUS_ERR_FORMAT_NOT_SUPPORTED, r->request_id);
    }

    memset(&s_job, 0, sizeof s_job);
    s_job.id        = s_next_job_id++;
    s_job.state     = IPP_JSTATE_PROCESSING;
    s_job.created_s = uptime_s();
    strncpy(s_job.name, r->job_name, sizeof s_job.name - 1);
    strncpy(s_job.user, r->requesting_user, sizeof s_job.user - 1);
    snprintf(s_job.uri, sizeof s_job.uri, "%s/%d", s_uri, (int)s_job.id);

    if (s_sink.begin) {
        esp_err_t err = s_sink.begin(s_sink.ctx, r->document_format, s_job.name);
        if (err != ESP_OK) {
            s_job.state = IPP_JSTATE_ABORTED;
            return send_status(req, IPP_STATUS_ERR_NOT_ACCEPTING_JOBS, r->request_id);
        }
    }

    // Respond only after the body is drained. A client that sees the response
    // while it is still sending treats the connection as finished and the
    // remaining document is lost.
    esp_err_t err = stream_document(req, hdr + r->header_len, hdr_have - r->header_len,
                                    hdr_have);

    s_job.state = (err == ESP_OK) ? IPP_JSTATE_COMPLETED : IPP_JSTATE_ABORTED;
    if (s_job.canceled) s_job.state = IPP_JSTATE_CANCELED;

    ipp_job_t j;
    fill_job(&j, &s_job);
    ipp_enc_t e;
    ipp_enc_init(&e, s_resp, sizeof s_resp);
    ipp_resp_begin(&e, IPP_STATUS_OK, r->request_id);
    ipp_write_op_attrs(&e);
    ipp_write_job_attrs(&e, &j);
    ipp_end(&e);
    return send_ipp(req, &e);
}

static esp_err_t dispatch(httpd_req_t *req, const ipp_req_t *r,
                          const uint8_t *hdr, size_t hdr_have)
{
    ipp_enc_t e;
    ipp_enc_init(&e, s_resp, sizeof s_resp);

    switch (r->op) {
    case IPP_OP_GET_PRINTER_ATTRIBUTES: {
        ipp_printer_t p;
        fill_printer(&p);
        ipp_resp_begin(&e, IPP_STATUS_OK, r->request_id);
        ipp_write_op_attrs(&e);
        ipp_write_printer_attrs(&e, &p);
        ipp_end(&e);
        return send_ipp(req, &e);
    }

    case IPP_OP_VALIDATE_JOB:
        if (!format_supported(r->document_format))
            return send_status(req, IPP_STATUS_ERR_FORMAT_NOT_SUPPORTED, r->request_id);
        return send_status(req, IPP_STATUS_OK, r->request_id);

    case IPP_OP_PRINT_JOB:
        return op_print_job(req, r, hdr, hdr_have);

    case IPP_OP_GET_JOB_ATTRIBUTES: {
        if (r->job_id >= 0 && r->job_id != s_job.id)
            return send_status(req, IPP_STATUS_ERR_NOT_FOUND, r->request_id);
        ipp_job_t j;
        fill_job(&j, &s_job);
        ipp_resp_begin(&e, IPP_STATUS_OK, r->request_id);
        ipp_write_op_attrs(&e);
        ipp_write_job_attrs(&e, &j);
        ipp_end(&e);
        return send_ipp(req, &e);
    }

    case IPP_OP_GET_JOBS: {
        ipp_resp_begin(&e, IPP_STATUS_OK, r->request_id);
        ipp_write_op_attrs(&e);
        // An empty list is a complete answer, and the only honest one while
        // no job is in flight.
        if (s_job.state == IPP_JSTATE_PROCESSING) {
            ipp_job_t j;
            fill_job(&j, &s_job);
            ipp_write_job_attrs(&e, &j);
        }
        ipp_end(&e);
        return send_ipp(req, &e);
    }

    case IPP_OP_CANCEL_JOB:
        if (r->job_id >= 0 && r->job_id != s_job.id)
            return send_status(req, IPP_STATUS_ERR_NOT_FOUND, r->request_id);
        s_job.canceled = 1;
        if (s_job.state == IPP_JSTATE_PROCESSING) s_job.state = IPP_JSTATE_CANCELED;
        return send_status(req, IPP_STATUS_OK, r->request_id);

    default:
        ESP_LOGW(TAG, "unsupported operation 0x%04x", r->op);
        return send_status(req, IPP_STATUS_ERR_OP_NOT_SUPPORTED, r->request_id);
    }
}

static esp_err_t ipp_post(httpd_req_t *req)
{
    size_t have = 0;
    ipp_req_t r;
    int rc;

    // Read until the attribute section parses. It arrives chunked, so a short
    // read is the normal case and not an error.
    for (;;) {
        rc = ipp_req_parse(&r, s_hdrbuf, have);
        if (rc != IPP_REQ_SHORT) break;
        if (have == sizeof s_hdrbuf) {
            ESP_LOGE(TAG, "attribute section over %u bytes", (unsigned)sizeof s_hdrbuf);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "ipp header too large");
            return ESP_FAIL;
        }
        int n = httpd_req_recv(req, (char *)s_hdrbuf + have, sizeof s_hdrbuf - have);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (n <= 0) return ESP_FAIL;
        have += (size_t)n;
    }

    if (rc != IPP_REQ_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "malformed ipp");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "op 0x%04x id %u from %s", r.op, (unsigned)r.request_id,
             r.requesting_user[0] ? r.requesting_user : "-");

    // Major version 1 and 2 share this encoding; anything else we cannot
    // speak, but the client still needs a well-formed answer.
    if ((r.version >> 8) != 1 && (r.version >> 8) != 2)
        return send_status(req, IPP_STATUS_ERR_BAD_REQUEST, r.request_id);

    return dispatch(req, &r, s_hdrbuf, have);
}

// Some clients probe the queue with a GET before they trust it.
static esp_err_t root_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, "AirWriter\n");
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

    ESP_ERROR_CHECK(mdns_service_add(NULL, "_ipp", "_tcp", s_cfg.port,
                                     txt, sizeof txt / sizeof txt[0]));

    // Without the _universal subtype iOS never lists the printer, however
    // correct the rest of the advertisement is. SPEC.md 3.2.
    ESP_ERROR_CHECK(mdns_service_subtype_add_for_host("_ipp", "_ipp", "_tcp", NULL,
                                                      "_universal"));
    ESP_ERROR_CHECK(mdns_service_subtype_add_for_host("_ipp", "_ipp", "_tcp", NULL,
                                                      "_print"));

    // A stub for older clients that probe LPD before IPP.
    ESP_ERROR_CHECK(mdns_service_add(NULL, "_printer", "_tcp", 515, NULL, 0));
    ESP_ERROR_CHECK(mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0));

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

    s_job.state = IPP_JSTATE_COMPLETED;   // no job yet; nothing is pending

    esp_err_t err = start_mdns();
    if (err != ESP_OK) return err;

    httpd_config_t hc = HTTPD_DEFAULT_CONFIG();
    hc.server_port      = s_cfg.port;
    hc.ctrl_port        = 32631;
    hc.max_uri_handlers = 8;
    hc.stack_size       = 8192;
    hc.lru_purge_enable = true;
    // Get-Printer-Attributes must answer inside two seconds or the print sheet
    // gives up, but a Print-Job body can stall much longer than that between
    // chunks while the phone renders the next page.
    hc.recv_wait_timeout = 30;
    hc.send_wait_timeout = 30;

    err = httpd_start(&s_httpd, &hc);
    if (err != ESP_OK) return err;

    static const httpd_uri_t u_print = {
        .uri = "/ipp/print", .method = HTTP_POST, .handler = ipp_post
    };
    static const httpd_uri_t u_root_post = {
        .uri = "/", .method = HTTP_POST, .handler = ipp_post
    };
    static const httpd_uri_t u_root_get = {
        .uri = "/", .method = HTTP_GET, .handler = root_get
    };
    static const httpd_uri_t u_print_get = {
        .uri = "/ipp/print", .method = HTTP_GET, .handler = root_get
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_httpd, &u_print));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_httpd, &u_root_post));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_httpd, &u_root_get));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_httpd, &u_print_get));

    ESP_LOGI(TAG, "listening on %s", s_uri);
    ESP_LOGI(TAG, "uuid %s%s", s_uuid, cfg->dev_mode ? "  (dev mode, new each boot)" : "");
    return ESP_OK;
}

void ipp_server_stop(void)
{
    if (s_httpd) { httpd_stop(s_httpd); s_httpd = NULL; }
    mdns_free();
}

const char *ipp_server_uri(void) { return s_uri; }
