// SPDX-License-Identifier: GPL-3.0-or-later
// ESP glue: a mutex-guarded ps_t on the esp_timer clock, plus the poll task.
#include "printer_state.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static ps_t s_ps;
static SemaphoreHandle_t s_mtx;
static void (*s_cb)(const ps_view_t *);

static uint64_t now_ms(void) { return (uint64_t)esp_timer_get_time() / 1000; }

static void poll_task(void *arg)
{
    (void)arg;
    int told = -1;
    for (;;) {
        ps_view_t v = printer_state_view();
        if ((int)v.state != told) {
            told = (int)v.state;
            if (s_cb) s_cb(&v);
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void printer_state_start(const ps_timing_t *tm, void (*on_change)(const ps_view_t *))
{
    s_mtx = xSemaphoreCreateMutex();
    ps_init(&s_ps, tm, now_ms());
    s_cb = on_change;
    xTaskCreate(poll_task, "ps_poll", 3072, NULL, 4, NULL);
}

void printer_state_post(ps_event_t ev)
{
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    ps_post(&s_ps, ev, now_ms());
    xSemaphoreGive(s_mtx);
}

void printer_state_set_ap_stations(unsigned n)
{
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    ps_set_ap_stations(&s_ps, n);
    xSemaphoreGive(s_mtx);
}

ps_view_t printer_state_view(void)
{
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    ps_view_t v = ps_update(&s_ps, now_ms());
    xSemaphoreGive(s_mtx);
    return v;
}

int printer_state_want_kick(void)
{
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    int want = ps_want_kick(&s_ps, now_ms());
    xSemaphoreGive(s_mtx);
    return want;
}

void printer_state_kicked(void)
{
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    ps_kicked(&s_ps, now_ms());
    xSemaphoreGive(s_mtx);
}
