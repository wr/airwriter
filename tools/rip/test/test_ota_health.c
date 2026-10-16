// SPDX-License-Identifier: GPL-3.0-or-later
// Host test for the rollback health gate (ota_health_core, W-1046): when a new
// OTA image is kept, when it is rolled back, and that each is decided once.
#include <assert.h>
#include <stdio.h>
#include "ota_health_core.h"

static const oh_timing_t TM = OH_TIMING_DEFAULT;
#define ALL_STA (OH_STA_UP | OH_IPP_UP | OH_WEB_UP)

// Steps the gate every 100 ms over [*t, end) and returns the first decision.
static oh_action_t run(oh_gate_t *g, uint32_t *t, uint32_t end)
{
    oh_action_t got = OH_WAIT;
    for (; *t < end; *t += 100) {
        oh_action_t a = oh_update(g, *t);
        if (a == OH_WAIT) continue;
        assert(got == OH_WAIT);         // decided once
        got = a;
    }
    return got;
}

int main(void)
{
    oh_gate_t g;
    uint32_t t;

    // With credentials: Wi-Fi, IPP and the web UI up at 20 s, kept 15 s later.
    t = 1000;
    oh_init(&g, &TM, 1, t);
    assert(run(&g, &t, 20000) == OH_WAIT);
    oh_set(&g, OH_STA_UP, 1, t);
    oh_set(&g, OH_WEB_UP, 1, t);
    assert(run(&g, &t, 22000) == OH_WAIT);
    oh_set(&g, OH_IPP_UP, 1, t);                        // healthy from 22.0 s
    assert(run(&g, &t, 22000 + TM.settle_ms) == OH_WAIT);
    assert(run(&g, &t, t + 100) == OH_MARK_VALID);
    assert(run(&g, &t, t + 300000) == OH_WAIT);         // never again
    oh_set(&g, OH_STA_UP, 0, t);                        // later losses don't matter
    assert(run(&g, &t, t + 300000) == OH_WAIT);

    // IPP never comes up: rolled back at the deadline, not a tick before.
    t = 0;
    oh_init(&g, &TM, 1, t);
    assert(run(&g, &t, 5000) == OH_WAIT);
    oh_set(&g, OH_STA_UP | OH_WEB_UP, 1, t);
    assert(run(&g, &t, TM.deadline_ms) == OH_WAIT);
    assert(run(&g, &t, t + 100) == OH_ROLLBACK);
    assert(run(&g, &t, t + 60000) == OH_WAIT);

    // Credentials that no longer join: the board falls back to the setup AP,
    // which is not enough for an image on trial. Rolled back.
    t = 0;
    oh_init(&g, &TM, 1, t);
    assert(run(&g, &t, 60000) == OH_WAIT);
    oh_set(&g, OH_AP_UP | OH_WEB_UP, 1, t);
    assert(run(&g, &t, TM.deadline_ms + 100) == OH_ROLLBACK);

    // A fresh board (no credentials): setup AP and web UI up is healthy.
    t = 0;
    oh_init(&g, &TM, 0, t);
    assert(run(&g, &t, 3000) == OH_WAIT);
    oh_set(&g, OH_AP_UP, 1, t);
    assert(run(&g, &t, 3500) == OH_WAIT);
    oh_set(&g, OH_WEB_UP, 1, t);
    assert(run(&g, &t, 3500 + TM.settle_ms) == OH_WAIT);
    assert(run(&g, &t, t + 100) == OH_MARK_VALID);

    // A break during the settle restarts it.
    t = 0;
    oh_init(&g, &TM, 1, t);
    assert(run(&g, &t, 10000) == OH_WAIT);
    oh_set(&g, ALL_STA, 1, t);
    assert(run(&g, &t, 20000) == OH_WAIT);
    oh_set(&g, OH_STA_UP, 0, t);                        // Wi-Fi drops at 20 s
    assert(run(&g, &t, 21000) == OH_WAIT);
    oh_set(&g, OH_STA_UP, 1, t);                        // back at 21 s
    assert(run(&g, &t, 21000 + TM.settle_ms) == OH_WAIT);
    assert(run(&g, &t, t + 100) == OH_MARK_VALID);

    // Healthy just before the deadline: the settle runs past it, and the
    // image is kept rather than rolled back for being late.
    t = 0;
    oh_init(&g, &TM, 1, t);
    assert(run(&g, &t, TM.deadline_ms - 2000) == OH_WAIT);
    oh_set(&g, ALL_STA, 1, t);
    assert(run(&g, &t, TM.deadline_ms - 2000 + TM.settle_ms) == OH_WAIT);
    assert(run(&g, &t, t + 100) == OH_MARK_VALID);

    // ... but unhealthy again after the deadline is a rollback at once.
    t = 0;
    oh_init(&g, &TM, 1, t);
    assert(run(&g, &t, TM.deadline_ms - 2000) == OH_WAIT);
    oh_set(&g, ALL_STA, 1, t);
    assert(run(&g, &t, TM.deadline_ms + 5000) == OH_WAIT);
    oh_set(&g, OH_IPP_UP, 0, t);
    assert(run(&g, &t, t + 100) == OH_ROLLBACK);

    // Bits the board's mode doesn't need are ignored: an AP blip on a board
    // with credentials, or a stray STA bit on a fresh one.
    t = 0;
    oh_init(&g, &TM, 0, t);
    oh_set(&g, OH_AP_UP | OH_WEB_UP | OH_STA_UP, 1, t);
    assert(run(&g, &t, 4000) == OH_WAIT);
    oh_set(&g, OH_STA_UP, 0, t);
    assert(run(&g, &t, TM.settle_ms + 100) == OH_MARK_VALID);

    // The millisecond clock wrapping during the trial.
    t = 0xFFFFFFFFu - 10000;
    oh_init(&g, &TM, 1, t);
    oh_set(&g, ALL_STA, 1, t);
    uint32_t start = t;
    int n = 0;
    for (uint32_t k = 0; k < 300; k++, t += 100)
        if (oh_update(&g, t) == OH_MARK_VALID) { n++; assert(t - start >= TM.settle_ms); }
    assert(n == 1);

    printf("ok  test_ota_health\n");
    return 0;
}
