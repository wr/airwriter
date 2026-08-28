// SPDX-License-Identifier: GPL-3.0-or-later
// ESP glue: a mutex-guarded ps_t plus a change callback fired outside the lock.
#include "printer_state.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static ps_t s_ps;
static SemaphoreHandle_t s_mtx;
static void (*s_cb)(ps_state_t);

void printer_state_start(void (*on_change)(ps_state_t))
{
    s_mtx = xSemaphoreCreateMutex();
    ps_init(&s_ps);
    s_cb = on_change;
}

void printer_state_post_ev(ps_event_t ev)
{
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    ps_state_t before = ps_state(&s_ps);
    ps_state_t after  = ps_post(&s_ps, ev);
    xSemaphoreGive(s_mtx);
    if (after != before && s_cb) s_cb(after);
}

ps_state_t printer_state_get(void)
{
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    ps_state_t st = ps_state(&s_ps);
    xSemaphoreGive(s_mtx);
    return st;
}
