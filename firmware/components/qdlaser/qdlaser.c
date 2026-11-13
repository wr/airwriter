// SPDX-License-Identifier: GPL-3.0-or-later
// W-1102: the board's side of a Personal LaserWriter 300 (qdlaser.h).
#include "qdlaser.h"
#include "qd_link.h"
#include "qd_page.h"
#include "serial_out.h"
#include "job_pipe.h"

#include <string.h>
#include "sdkconfig.h"
#include "esp_log.h"

static const char *TAG = "qdlaser";

static qdlaser_cfg_t s_cfg;
static volatile int  s_cancel;
static qd_ops_t      s_ops;      // the link's, with this module's cancelled and state

static int ops_cancelled(void *ctx)
{
    (void)ctx;
    return s_cancel || (s_cfg.cancelled && s_cfg.cancelled());
}

static void ops_state(void *ctx, qd_state_t st)
{
    (void)ctx;
    if (s_cfg.state) s_cfg.state(st);
}

void qdlaser_start(const qdlaser_cfg_t *cfg)
{
    s_cfg = *cfg;
}

const qd_ops_t *qdlaser_ops(void)
{
    s_ops = *qd_link_ops();
    s_ops.cancelled = ops_cancelled;
    s_ops.state = ops_state;
    return &s_ops;
}

void qdlaser_cancel(void)
{
    s_cancel = 1;
}

// ---------------------------------------------------------------- the port's questions

int qdlaser_identity(int hz, uint8_t *engine)
{
    if (qd_link_open(hz, 0)) return QD_E_NOMEM;
    uint8_t ctrl = 0;
    int rc = qd_ask_identity(qd_link_ops(), engine, &ctrl);
    qd_link_close();
    return rc;
}

int qdlaser_status(int hz, qd_model_t m, qd_state_t *st, qd_raw_t *raw)
{
    if (qd_link_open(hz, 0)) return QD_E_NOMEM;
    int rc = qd_ask_status(qd_link_ops(), m, 0, st, raw);
    qd_link_close();
    return rc;
}

// ---------------------------------------------------------------- a job

typedef struct {
    qd_session_t *s;
    qd_page_t    *page;
    rip_stage_t   stage;         // the target's by pointer; filled in when the rip begins
    qd_model_t    model;
    ht_algo_t     screen;
    int           gain;
    int           keep;          // the record's screen is not Apple's: keep an Apple page
    int           linked;        // the link is open for the job
    int           began;         // qd_session_begin returned QD_OK
    volatile int  err;           // the first session error
} job_t;

static job_t s_job;

static void job_free(void)
{
    qd_page_free(s_job.page);        // both take NULL
    qd_session_free(s_job.s);
    if (s_job.linked) qd_link_close();
    memset(&s_job, 0, sizeof s_job);
    s_cancel = 0;                    // a cancel is the job's: it ends with it
}

// The job cannot go on: the first error is kept, the printer is told (8A 00, 81 00, 84 01:
// once, and only if the session wrote), and the job pipeline is released. On the rip task.
static void job_failed(int rc)
{
    if (s_job.err) return;
    s_job.err = rc;
    if (s_job.s) qd_session_abort(s_job.s);
    job_pipe_abort();
}

// The page stage's sink is the session.
static int sk_open(void *ctx, const qd_media_t *md)
{
    (void)ctx;
    int rc = s_job.err ? s_job.err : qd_session_open(s_job.s, md);
    if (rc) job_failed(rc);
    return rc;
}

static int sk_row(void *ctx, const uint8_t *codes, size_t n)
{
    (void)ctx;
    int rc = s_job.err ? s_job.err : qd_session_row(s_job.s, codes, n);
    if (rc) job_failed(rc);
    return rc;
}

static int sk_print(void *ctx)
{
    (void)ctx;
    int rc = s_job.err ? s_job.err : qd_session_print(s_job.s);
    if (rc) job_failed(rc);
    return rc;
}

static void sk_note(void *ctx, int page, int apple, uint32_t bytes, int too_big)
{
    (void)ctx;
    if (s_cfg.page) s_cfg.page(page, apple, bytes, too_big);
}

// What the stage is until the rip begins has made it: a page it is offered is refused.
static int stage_refuse(void *ctx, int index, uint32_t w, uint32_t h)
{
    (void)ctx; (void)index; (void)w; (void)h;
    return -1;
}

// The rip begins (job_pipe_target_t.colour_ok), on the rip task under the job lock and before
// its first row: the link opens for pages, the session begins (the identity must be the
// model's), and the page stage is made, with the pool's second half to keep a page in.
static int job_rip_begin(void *ctx)
{
    (void)ctx;
    int hz = 0;
    if (!serial_out_clock(&hz)) { job_failed(QD_E_OFFLINE); return 0; }
    if (qd_link_open(hz, 1)) {
        ESP_LOGE(TAG, "no memory for the link");
        job_failed(QD_E_NOMEM);
        return 0;
    }
    s_job.linked = 1;
    int rc = qd_session_begin(s_job.s);
    if (rc) {
        ESP_LOGE(TAG, "the printer would not start the job: %d", rc);
        job_failed(rc);
        return 0;
    }
    s_job.began = 1;
    size_t cap = 0;
    uint8_t *keep = s_job.keep ? job_pipe_keep(&cap) : NULL;
    if (s_job.keep && !keep)
        ESP_LOGW(TAG, "no room to keep a page: one too big for the printer will fail");
    qd_page_cfg_t pc = {
        .model = s_job.model, .screen = s_job.screen, .gain = s_job.gain,
        .keep = keep, .keep_cap = cap,
        .sink = { .open = sk_open, .row = sk_row, .print = sk_print, .note = sk_note },
    };
    s_job.page = qd_page_new(&pc);
    if (!s_job.page) {
        ESP_LOGE(TAG, "no memory for the page");
        job_failed(QD_E_NOMEM);
        return 0;
    }
    qd_page_stage(s_job.page, &s_job.stage);
    return 0;                        // gray: the printer is black only
}

int qdlaser_job_begin(qd_model_t m, ht_algo_t screen, int gain, int manual, job_pipe_target_t *t)
{
    const printer_caps_t *caps = qd_caps(m);
    if (!caps || !t) return -1;
    job_free();                      // nothing should be held; belt and braces
    if (!serial_out_clock(NULL)) return -1;
    const qd_job_cfg_t jc = { .model = m, .manual = manual,
                              .stall_ms = CONFIG_AIRWRITER_STALL_ABORT * 1000u };
    s_job.s = qd_session_new(qdlaser_ops(), &jc);
    if (!s_job.s) {
        ESP_LOGE(TAG, "no memory for the session");
        return -1;
    }
    const int apple = screen == HT_APPLE || screen == HT_APPLE_LW;
    s_job.model = m;
    s_job.screen = screen;
    s_job.gain = gain;
    s_job.keep = !apple;
    s_job.stage.page_begin = stage_refuse;
    *t = (job_pipe_target_t){
        .caps = caps, .stage = &s_job.stage, .dither = apple ? HT_APPLE_LW : screen,
        .gain = gain, .keep = !apple, .colour_ok = job_rip_begin,
    };
    return 0;
}

int qdlaser_job_error(void)
{
    return s_job.err;
}

int qdlaser_job_failed_pages(void)
{
    return s_job.page ? qd_page_failed(s_job.page) : 0;
}

int qdlaser_job_end(int aborted)
{
    int rc = s_job.err;
    const int failed = qdlaser_job_failed_pages();
    if (!rc && !aborted && s_job.began) {
        rc = qd_session_end(s_job.s);
        if (rc) s_job.err = rc;
    } else if (!rc) {
        rc = aborted ? QD_E_CANCEL : QD_E_FAULT;   // ended short with no error of its own
    }
    if (rc && s_job.s) qd_session_abort(s_job.s);
    if (!rc && failed) rc = QD_E_FAULT;              // a page too detailed for the printer
    if (s_job.s) ESP_LOGI(TAG, "job over: %s (%d)", rc ? "not printed" : "printed", rc);
    job_free();
    return rc;
}
