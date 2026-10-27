// SPDX-License-Identifier: GPL-3.0-or-later
//
// The board's side of a StyleWriter on J2 (stylewriter.h, W-1103). The session itself is
// sw_session.c, host-tested against a scripted printer; here are its ops over serial_out,
// the port's questions, and a job's three places on the pipeline: the sink's begin (this
// file's stylewriter_job_begin), the rip's start (job_rip_begin, which resets the printer
// and reads its cartridge before the page stage exists) and the printer task's bytes
// (stylewriter_prn), then the finisher's stylewriter_job_end.
#include "stylewriter.h"
#include "sw_page.h"
#include "sw_status.h"
#include "serial_out.h"
#include "job_pipe.h"

#include <string.h>
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "stylewriter";

static stylewriter_cfg_t s_cfg;
static int               s_link_sw;       // J2 is at a StyleWriter's link; the ImageWriter's at boot
static volatile int      s_cancel;

// ---------------------------------------------------------------- the session's line

static int op_write(void *ctx, const uint8_t *b, size_t n)
{
    (void)ctx;
    // 57600 baud is 5.76 bytes a millisecond: n/2 ms is about three times what the bytes
    // take, and 2 s is a floor for the smallest. A StyleWriter has no flow control, so this
    // only ever fails on a UART that is stuck.
    return serial_out_write_raw(b, n, 2000u + (uint32_t)(n / 2));
}

static int op_read(void *ctx, uint8_t *b, size_t cap, uint32_t first_ms, uint32_t gap_ms)
{
    (void)ctx;
    int n = serial_out_read(b, cap, first_ms, gap_ms);
    return n < 0 ? 0 : n;                  // loopback has nothing to hear
}

static void op_drain(void *ctx)
{
    (void)ctx;
    serial_out_rx_drop();
}

static void op_sleep(void *ctx, uint32_t ms)
{
    (void)ctx;
    TickType_t t = pdMS_TO_TICKS(ms);
    vTaskDelay(t ? t : 1);
}

static uint32_t op_now(void *ctx)
{
    (void)ctx;
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static int op_cancelled(void *ctx)
{
    (void)ctx;
    return s_cancel || (s_cfg.cancelled && s_cfg.cancelled());
}

static void op_state(void *ctx, sw_state_t st)
{
    (void)ctx;
    if (s_cfg.state) s_cfg.state(st);
}

static const sw_ops_t k_ops = {
    .write = op_write, .read = op_read, .drain = op_drain, .sleep_ms = op_sleep,
    .now_ms = op_now, .cancelled = op_cancelled, .state = op_state, .ctx = NULL,
};

const sw_ops_t *stylewriter_ops(void) { return &k_ops; }

void stylewriter_start(const stylewriter_cfg_t *cfg)
{
    s_cfg = *cfg;
}

void stylewriter_cancel(void)
{
    s_cancel = 1;
}

// ---------------------------------------------------------------- the link

void stylewriter_link(int sw)
{
    sw = sw ? 1 : 0;
    if (sw == s_link_sw) return;
    if (sw) serial_out_set_link(57600, 0);
    else    serial_out_set_link(CONFIG_AIRWRITER_BAUD, 1);
    s_link_sw = sw;
}

void stylewriter_link_record(void)
{
    stylewriter_link(s_cfg.record_is_sw && s_cfg.record_is_sw());
}

// ---------------------------------------------------------------- the port's questions

sw_model_t stylewriter_ask_plain(void)
{
    stylewriter_link(1);
    sw_model_t m = sw_ask_plain(&k_ops);
    stylewriter_link_record();
    return m;
}

sw_model_t stylewriter_ask_full(void)
{
    stylewriter_link(1);
    sw_model_t m = sw_ask_full(&k_ops);
    stylewriter_link_record();
    return m;
}

int stylewriter_status(sw_model_t m, sw_state_t *st)
{
    stylewriter_link(1);
    int rc = sw_ask_status(&k_ops, m, st);
    stylewriter_link_record();
    return rc;
}

int stylewriter_cart(sw_model_t m)
{
    stylewriter_link(1);
    int rc = sw_ask_cart(&k_ops, m);
    stylewriter_link_record();
    return rc;
}

int stylewriter_clean(sw_model_t m)
{
    s_cancel = 0;
    stylewriter_link(1);
    int rc = sw_clean(&k_ops, m);
    stylewriter_link_record();
    ESP_LOGI(TAG, "cleaning: %s (%d)", rc == SW_OK ? "done" : "failed", rc);
    return rc;
}

int stylewriter_clean_check(sw_model_t m, int busy)
{
    if (busy) return 409;
    const sw_spec_t *sp = sw_spec(m);
    if (!sp || !sp->cleans) return 502;
    sw_state_t st;
    if (stylewriter_status(m, &st) != SW_OK || sw_state_fault(st)) return 502;
    return 0;
}

// ---------------------------------------------------------------- the test page

typedef struct {
    sw_session_t *s;
    int           err;           // the first session error
} tp_ctx_t;

// The stage's frames go straight to the session, which waits while the printer's buffer is full.
static void tp_frame(void *ctx, const uint8_t *frame, size_t n)
{
    tp_ctx_t *c = ctx;
    if (c->err) return;
    int rc = sw_session_feed(c->s, frame, n);
    if (rc) c->err = rc;
}

// The page is not made any further once the session has failed or been asked to stop.
static int tp_stop(void *ctx)
{
    tp_ctx_t *c = ctx;
    return c->err || op_cancelled(NULL);
}

int stylewriter_test_page(sw_model_t m, const sw_tp_info_t *info)
{
    const sw_spec_t *sp = sw_spec(m);
    if (!sp || !info) return SW_E_FAULT;
    s_cancel = 0;                // a cancel a job left behind is not this page's
    stylewriter_link(1);
    const sw_job_cfg_t jc = { .model = m, .quality = 4, .colour_job = sp->colour,
                              .stall_ms = CONFIG_AIRWRITER_STALL_ABORT * 1000u };
    sw_session_t *s = sw_session_new(&k_ops, &jc);
    sw_page_t *pg = NULL;
    int rc = s ? sw_session_begin(s) : SW_E_NOMEM;
    tp_ctx_t c = { .s = s };
    if (!rc) {
        int cart = sw_session_cart(s);
        ESP_LOGI(TAG, "test page begins: %s cartridge", cart ? "colour" : "black");
        if (s_cfg.cart) s_cfg.cart(cart);
        sw_page_cfg_t pc = { .model = m, .colour_cart = cart, .out = tp_frame, .ctx = &c };
        pg = sw_page_new(&pc);
        if (!pg) {
            rc = SW_E_NOMEM;
        } else {
            rip_stage_t st;
            sw_page_stage(pg, &st);
            sw_tp_info_t ti = *info;
            ti.model = m;
            ti.colour_cart = cart;
            ti.stop = tp_stop;
            ti.stop_ctx = &c;
            int drawn = sw_testpage(&ti, &st);
            rc = c.err ? c.err
               : drawn == SW_TP_NOMEM ? SW_E_NOMEM
               : drawn == SW_TP_STAGE ? SW_E_FAULT
               : drawn == SW_TP_STOPPED ? SW_E_CANCEL
               : sw_session_end(s);
        }
    }
    if (rc && s) sw_session_abort(s);      // `E` and the reset, once; a no-op if it sent nothing
    ESP_LOGI(TAG, "test page: %s (%d)", rc ? "not printed" : "printed", rc);
    sw_page_free(pg);                      // both take NULL
    sw_session_free(s);
    s_cancel = 0;
    stylewriter_link_record();
    return rc;
}

// ---------------------------------------------------------------- a job

typedef struct {
    sw_session_t *s;
    sw_page_t    *page;
    rip_stage_t   stage;         // the target's by pointer; filled in when the rip begins
    sw_model_t    model;
    int           began;         // sw_session_begin returned 0
    volatile int  err;           // the first session error
} job_t;

static job_t s_job;

static void job_free(void)
{
    sw_page_free(s_job.page);        // both take NULL
    sw_session_free(s_job.s);
    memset(&s_job, 0, sizeof s_job);
    s_cancel = 0;                    // a cancel is the job's: it ends with it, for the next session
}

// The job cannot go on: the first error is kept, the printer is told (`E`, the reset: once,
// and only if the session wrote), and the rip task, which may be blocked in job_pipe_out
// with no timeout of its own, is released. From the rip task or the printer task.
static void job_failed(int rc)
{
    if (!s_job.err) s_job.err = rc;
    sw_session_abort(s_job.s);
    job_pipe_abort();
}

// What the stage is until the rip begins has made it: a page it is offered is refused.
static int stage_refuse(void *ctx, int index, uint32_t w, uint32_t h)
{
    (void)ctx; (void)index; (void)w; (void)h;
    return -1;
}

static void frame_out(void *ctx, const uint8_t *frame, size_t n)
{
    (void)ctx;
    job_pipe_out(frame, n);
}

// The rip begins (job_pipe_target_t.colour_ok), on the pipeline task under the job lock and
// before its first row: the printer is reset and settled, `?` must name the model, and the
// cartridge is read, so that the page stage is made for it and a colour page is only ripped
// as four inks when a colour cartridge is in. A black cartridge in a colour model gets its
// colour pages as gray, through the screen.
static int job_rip_begin(void *ctx)
{
    (void)ctx;
    if (!s_job.s) { job_failed(SW_E_FAULT); return 0; }
    stylewriter_link(1);
    int rc = sw_session_begin(s_job.s);
    if (rc) {
        ESP_LOGE(TAG, "the printer would not start the job: %d", rc);
        job_failed(rc);
        return 0;
    }
    s_job.began = 1;
    int cart = sw_session_cart(s_job.s);
    ESP_LOGI(TAG, "job begins: %s cartridge", cart ? "colour" : "black");
    if (s_cfg.cart) s_cfg.cart(cart);
    sw_page_cfg_t pc = { .model = s_job.model, .colour_cart = cart, .out = frame_out };
    s_job.page = sw_page_new(&pc);
    if (!s_job.page) {
        ESP_LOGE(TAG, "no memory for the page");
        job_failed(SW_E_NOMEM);
        return 0;
    }
    sw_page_stage(s_job.page, &s_job.stage);
    return cart && sw_spec(s_job.model)->colour;
}

int stylewriter_job_begin(sw_model_t m, int quality, ht_algo_t dither, int gain,
                          job_pipe_target_t *t)
{
    const sw_spec_t *sp = sw_spec(m);
    const printer_caps_t *caps = sw_caps(m);
    if (!sp || !caps || !t) return -1;
    job_free();                              // nothing should be held; belt and braces
    s_cancel = 0;
    const sw_job_cfg_t jc = { .model = m, .quality = quality, .colour_job = sp->colour,
                              .stall_ms = CONFIG_AIRWRITER_STALL_ABORT * 1000u };
    s_job.s = sw_session_new(&k_ops, &jc);
    if (!s_job.s) {
        ESP_LOGE(TAG, "no memory for the session");
        return -1;
    }
    s_job.model = m;
    s_job.stage.page_begin = stage_refuse;
    *t = (job_pipe_target_t){
        .caps = caps, .stage = &s_job.stage, .dither = dither, .gain = gain,
        .colour = sp->colour, .buffered = 1, .prn = stylewriter_prn, .tone = sw_tone(m, dither),
        .colour_ok = job_rip_begin,
    };
    return 0;
}

void stylewriter_prn(void *ctx, const uint8_t *buf, size_t n)
{
    (void)ctx;
    if (!s_job.s || s_job.err) return;       // a dead job's bytes go nowhere
    int rc = sw_session_feed(s_job.s, buf, n);
    if (rc) job_failed(rc);
}

int stylewriter_job_error(void)
{
    return s_job.err;
}

int stylewriter_job_end(int aborted)
{
    int rc = s_job.err;
    if (!rc && !aborted && s_job.began) {
        rc = sw_session_end(s_job.s);
        if (rc) s_job.err = rc;
    } else if (!rc) {
        rc = aborted ? SW_E_CANCEL : SW_E_FAULT;  // ended short with no error of the printer's own
    }
    if (rc) sw_session_abort(s_job.s);       // `E` and the reset, once; a no-op if it sent nothing
    if (s_job.s) ESP_LOGI(TAG, "job over: %s (%d)", rc ? "not printed" : "printed", rc);
    job_free();
    return rc;
}
