// SPDX-License-Identifier: GPL-3.0-or-later
// Host test for the DIP bank decode + debounce (dip_decode, dip_stable).
#include <assert.h>
#include <stdio.h>
#include "dip_config_core.h"

// Feeds the button a level every 100 ms (the poll rate) from *t up to end,
// and returns the first event other than NONE (NONE if there was none).
// Asserts that at most one such event happens in the stretch.
static btn_ev_t run(btn_t *b, int pressed, uint32_t *t, uint32_t end)
{
    btn_ev_t got = BTN_EV_NONE;
    for (; *t < end; *t += 100) {
        btn_ev_t e = btn_step(b, pressed, *t);
        if (e == BTN_EV_NONE) continue;
        assert(got == BTN_EV_NONE);
        got = e;
    }
    return got;
}

// The test button (W-1045): tap, hold, cancel, commit, and the press held
// through power-up that belongs to the bootloader.
static void test_button(void)
{
    btn_t b = {0};
    uint32_t t = 0;
    assert(run(&b, 0, &t, 1000) == BTN_EV_NONE);           // idle

    // A tap: the test page comes on release, not on press.
    assert(run(&b, 1, &t, 1300) == BTN_EV_NONE);
    assert(run(&b, 0, &t, 2000) == BTN_EV_TAP);

    // Down 0.9 s from the debounced press: still a tap.
    uint32_t pressed_at = t;
    assert(run(&b, 1, &t, pressed_at + 100 + 900) == BTN_EV_NONE);
    assert(run(&b, 0, &t, t + 500) == BTN_EV_TAP);

    // Past 1 s the countdown starts, and releasing cancels with no test page.
    pressed_at = t;
    assert(run(&b, 1, &t, pressed_at + 100 + BTN_TAP_MAX_MS) == BTN_EV_NONE);
    assert(run(&b, 1, &t, t + 100) == BTN_EV_HOLD);         // at exactly 1.0 s
    assert(run(&b, 1, &t, t + 3000) == BTN_EV_NONE);
    assert(run(&b, 0, &t, t + 500) == BTN_EV_CANCEL);

    // Held to the end of the countdown it commits once, at 8 s, and the
    // release afterwards is only a release.
    pressed_at = t;
    assert(run(&b, 1, &t, pressed_at + 100 + BTN_TAP_MAX_MS) == BTN_EV_NONE);
    assert(run(&b, 1, &t, t + 100) == BTN_EV_HOLD);
    assert(run(&b, 1, &t, pressed_at + 100 + BTN_COMMIT_MS) == BTN_EV_NONE);
    assert(run(&b, 1, &t, t + 100) == BTN_EV_COMMIT);
    assert(run(&b, 1, &t, t + 20000) == BTN_EV_NONE);       // no second commit
    assert(run(&b, 0, &t, t + 500) == BTN_EV_RELEASE);

    // One-sample glitches change nothing, either way.
    for (int i = 0; i < 20; i++, t += 100)
        assert(btn_step(&b, i == 7, t) == BTN_EV_NONE);
    int holds = 0;
    for (int i = 0; i < 25; i++, t += 100) {                // 2.5 s down, one glitch up
        btn_ev_t e = btn_step(&b, i != 12, t);
        assert(e == BTN_EV_NONE || e == BTN_EV_HOLD);
        holds += e == BTN_EV_HOLD;
    }
    assert(holds == 1);
    assert(run(&b, 0, &t, t + 500) == BTN_EV_CANCEL);

    // Held through power-up (the bootloader's reset gesture): no tap, no
    // countdown, however long, until it is let go; then it works as normal.
    btn_t h = {0};
    t = 0;
    assert(run(&h, 1, &t, 30000) == BTN_EV_NONE);
    assert(run(&h, 0, &t, t + 500) == BTN_EV_NONE);
    assert(run(&h, 1, &t, t + 300) == BTN_EV_NONE);
    assert(run(&h, 0, &t, t + 500) == BTN_EV_TAP);

    // Timestamps that wrap the 32-bit millisecond clock.
    btn_t w = {0};
    t = 0xFFFFFFFFu - 4000;
    assert(run(&w, 0, &t, t + 500) == BTN_EV_NONE);
    pressed_at = t;
    btn_ev_t e = BTN_EV_NONE, seen[4] = {0};
    int n = 0;
    for (uint32_t k = 0; k < 90; k++, t += 100) {
        e = btn_step(&w, 1, t);
        if (e != BTN_EV_NONE) seen[n++] = e;
    }
    assert(n == 2 && seen[0] == BTN_EV_HOLD && seen[1] == BTN_EV_COMMIT);
}

int main(void)
{
    dip_config_t c;
    // Rev A: S1 pin n sits under the switch labelled 9-n, so net DIP1
    // (S1 pin 1, IO38) is the switch labelled 8. dip_physical() turns net
    // bits into label bits; every assignment below is by label.
    assert(dip_physical(0x01) == 0x80);
    assert(dip_physical(0x80) == 0x01);
    assert(dip_physical(0x40) == 0x02);
    assert(dip_physical(0xF0) == 0x0F);
    assert(dip_physical(0x00) == 0x00);

    dip_decode(0x00, &c);
    assert(c.mode == 0 && !c.dev_mode && !c.test_on_boot && c.dither_ovr == 0);
    dip_decode(0x01, &c); assert(c.mode == 1);            // DIP1
    dip_decode(0x02, &c); assert(c.mode == 2);            // DIP2
    dip_decode(0x04, &c); assert(c.dev_mode == 1);        // DIP3
    dip_decode(0x08, &c); assert(c.test_on_boot == 1);    // DIP4
    dip_decode(0x30, &c); assert(c.dither_ovr == 3);      // DIP5+6
    dip_decode(0x40, &c); assert(c.gamer == 1 && c.spare == 0);   // DIP7: gamer mode
    dip_decode(0x80, &c); assert(c.spare == 1 && c.gamer == 0);   // DIP8: spare
    dip_decode(0xC0, &c); assert(c.gamer == 1 && c.spare == 1);

    // Debounce: a value must be seen 3 polls straight; report change once.
    dip_deb_t d = {0};
    uint8_t v = 0xFF;
    assert(dip_stable(&d, 0x05, &v) == 0);
    assert(dip_stable(&d, 0x04, &v) == 0);     // bounce restarts agreement
    assert(dip_stable(&d, 0x04, &v) == 0);
    assert(dip_stable(&d, 0x04, &v) == 1 && v == 0x04);
    assert(dip_stable(&d, 0x04, &v) == 0);     // stable, no re-report

    test_button();
    printf("test_dip_config: ok\n");
    return 0;
}
