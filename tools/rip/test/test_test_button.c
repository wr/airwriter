// SPDX-License-Identifier: GPL-3.0-or-later
// Host test for the test button: tap, hold, cancel and commit (btn_step).
#include <assert.h>
#include <stdio.h>
#include "test_button_core.h"

#define P BTN_POLL_MS
#define D BTN_DEBOUNCE_MS

// Feeds the button a level every poll from *t up to end, and returns the
// first event other than NONE (NONE if there was none). Asserts that at most
// one such event happens in the stretch.
static btn_ev_t run(btn_t *b, int pressed, uint32_t *t, uint32_t end)
{
    btn_ev_t got = BTN_EV_NONE;
    for (; *t < end; *t += P) {
        btn_ev_t e = btn_step(b, pressed, *t);
        if (e == BTN_EV_NONE) continue;
        assert(got == BTN_EV_NONE);
        got = e;
    }
    return got;
}

// A press of d ms that starts `phase` ms after a poll, read at the poll rate
// from idle to idle again: how many taps it makes. Any other event fails.
static int taps_of_press(uint32_t d, uint32_t phase)
{
    btn_t b = {0};
    uint32_t on = 1000 + phase;
    int taps = 0;
    for (uint32_t t = 0; t < 3000; t += P) {
        btn_ev_t e = btn_step(&b, t >= on && t < on + d, t);
        assert(e == BTN_EV_NONE || e == BTN_EV_TAP);
        taps += e == BTN_EV_TAP;
    }
    return taps;
}

// The raw samples in s, one a poll, then the button left up: how many taps.
static int taps_of_samples(const char *s)
{
    btn_t b = {0};
    uint32_t t = 0;
    int taps = 0;
    assert(run(&b, 0, &t, 500) == BTN_EV_NONE);
    for (; *s; s++, t += P) {
        btn_ev_t e = btn_step(&b, *s == '1', t);
        assert(e == BTN_EV_NONE || e == BTN_EV_TAP);
        taps += e == BTN_EV_TAP;
    }
    btn_ev_t e = run(&b, 0, &t, t + 500);
    assert(e == BTN_EV_NONE || e == BTN_EV_TAP);
    return taps + (e == BTN_EV_TAP);
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
    assert(run(&b, 1, &t, pressed_at + D + 900) == BTN_EV_NONE);
    assert(run(&b, 0, &t, t + 500) == BTN_EV_TAP);

    // Let go just as 1 s comes up: the first sample of the release is not
    // down, so it neither starts the countdown nor eats the tap.
    pressed_at = t;
    assert(run(&b, 1, &t, pressed_at + D + BTN_TAP_MAX_MS) == BTN_EV_NONE);
    assert(run(&b, 0, &t, t + 500) == BTN_EV_TAP);

    // Past 1 s the countdown starts, and releasing cancels with no test page.
    pressed_at = t;
    assert(run(&b, 1, &t, pressed_at + D + BTN_TAP_MAX_MS) == BTN_EV_NONE);
    assert(run(&b, 1, &t, t + P) == BTN_EV_HOLD);           // at exactly 1.0 s
    assert(run(&b, 1, &t, t + 3000) == BTN_EV_NONE);
    assert(run(&b, 0, &t, t + 500) == BTN_EV_CANCEL);

    // Held to the end of the countdown it commits once, at 8 s, and the
    // release afterwards is only a release.
    pressed_at = t;
    assert(run(&b, 1, &t, pressed_at + D + BTN_TAP_MAX_MS) == BTN_EV_NONE);
    assert(run(&b, 1, &t, t + P) == BTN_EV_HOLD);
    assert(run(&b, 1, &t, pressed_at + D + BTN_COMMIT_MS) == BTN_EV_NONE);
    assert(run(&b, 1, &t, t + P) == BTN_EV_COMMIT);
    assert(run(&b, 1, &t, t + 20000) == BTN_EV_NONE);       // no second commit
    assert(run(&b, 0, &t, t + 500) == BTN_EV_RELEASE);

    // Glitches shorter than the debounce change nothing, either way.
    for (int i = 0; i < 20; i++, t += P)
        assert(btn_step(&b, i >= 7 && i < 7 + D / P, t) == BTN_EV_NONE);
    int holds = 0;
    for (int i = 0; i < 2500 / P; i++, t += P) {            // 2.5 s down, one glitch up
        btn_ev_t e = btn_step(&b, !(i >= 120 && i < 120 + D / P), t);
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
    for (uint32_t k = 0; k < 9000 / P; k++, t += P) {
        e = btn_step(&w, 1, t);
        if (e != BTN_EV_NONE) seen[n++] = e;
    }
    assert(n == 2 && seen[0] == BTN_EV_HOLD && seen[1] == BTN_EV_COMMIT);
}

// A quick tap is a tap, wherever it falls between two reads of the pin. A
// tap of 50 to 150 ms once slipped between reads a tenth of a second apart,
// and the button seemed to need a second press.
static void test_quick_taps(void)
{
    static const uint32_t ds[] = { D + P, 50, 80, 100, 120, 150, 200, 400 };
    for (unsigned i = 0; i < sizeof ds / sizeof *ds; i++)
        for (uint32_t phase = 0; phase < P; phase++)
            assert(taps_of_press(ds[i], phase) == 1);
    // Shorter than the debounce, it is a glitch wherever it falls.
    for (uint32_t phase = 0; phase < P; phase++)
        assert(taps_of_press(D - P, phase) == 0);
}

// Contacts that bounce for a read or two as they close and as they open make
// one tap, never two: a second tap would cancel the test page it started.
static void test_bounce(void)
{
    assert(taps_of_samples("10101111111111") == 1);
    assert(taps_of_samples("1111111111" "0110") == 1);
    assert(taps_of_samples("1011111111" "0101") == 1);
    assert(taps_of_samples("1111111111" "0011") == 1);    // 20 ms clear, 20 ms bounce
    // Two real taps, let go for a moment in between: two.
    assert(taps_of_samples("11111111" "00000000" "11111111") == 2);
}

int main(void)
{
    test_button();
    test_quick_taps();
    test_bounce();
    printf("test_test_button: ok\n");
    return 0;
}
