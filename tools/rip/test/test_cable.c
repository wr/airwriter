// SPDX-License-Identifier: GPL-3.0-or-later
// Cable tester core (W-1055): FM0 frames out and back, at the timings the
// ESP32's RMT and TashTalk's PIC actually run.
#include "test_util.h"
#include <stdlib.h>
#include "cable_core.h"
#include "lt_proto.h"

#define RES 40000000u     // the RMT resolution cable_test.c uses

static uint8_t  runs[CT_RUNS_MAX];
static uint16_t t16[CT_RUNS_MAX];
static uint32_t cap[4 * CT_RUNS_MAX];

// A frame on the wire as the RMT would see it: encode, to ticks, widened.
static size_t wire(const uint8_t *f, size_t n, uint32_t *out)
{
    size_t r = ct_fm0_encode(f, n, runs, sizeof runs);
    ct_runs_to_ticks(runs, r, RES, t16);
    for (size_t i = 0; i < r; i++) out[i] = t16[i];
    return r;
}

static void test_encode_shape(void)
{
    static const uint8_t enq[] = { 0x00, 0x00, LLAP_ENQ };
    size_t r = ct_fm0_encode(enq, sizeof enq, runs, sizeof runs);
    CHECK(r > 0);
    // Four 1s, then a flag LSB first: 0 1 1 1 1 1 1 0.
    static const uint8_t head[] = { 2, 2, 2, 2,  1, 1, 2, 2, 2, 2, 2, 2, 1, 1 };
    CHECK_EQ_MEM(runs, head, sizeof head);
    // A closing flag and thirteen 1s.
    static const uint8_t tail[] = { 1, 1, 2, 2, 2, 2, 2, 2, 1, 1,
                                    2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2 };
    CHECK_EQ_MEM(runs + r - sizeof tail, tail, sizeof tail);
    for (size_t i = 0; i < r; i++) CHECK(runs[i] == 1 || runs[i] == 2);

    // Too long, or no room.
    uint8_t big[CT_FRAME_MAX + 1] = { 0 };
    CHECK_EQ_INT(ct_fm0_encode(big, sizeof big, runs, sizeof runs), 0);
    CHECK_EQ_INT(ct_fm0_encode(enq, sizeof enq, runs, 10), 0);
    CHECK_EQ_INT(ct_fm0_encode(enq, 2, runs, sizeof runs), 0);
}

static void test_ticks(void)
{
    static const uint8_t r[] = { 2, 1, 1, 2, 2, 1, 2, 1, 1, 1, 2 };
    uint16_t t[sizeof r];
    ct_runs_to_ticks(r, sizeof r, RES, t);
    // Every transition within half a tick of ideal: no drift.
    uint64_t sum = 0, halves = 0;
    for (size_t i = 0; i < sizeof r; i++) {
        sum += t[i];
        halves += r[i];
        double ideal = (double)halves * RES / CT_HALF_RATE;
        CHECK((double)sum > ideal - 0.51 && (double)sum < ideal + 0.51);
    }
    // 40 MHz: 86 or 87 ticks a half cell, 173 or 174 a whole.
    CHECK(t[1] == 86 || t[1] == 87);
    CHECK(t[0] == 173 || t[0] == 174);
}

static uint8_t got[8][CT_FRAME_MAX];
static size_t  got_len[8];
static int     ngot;
static void on_frame(void *ctx, const uint8_t *f, size_t n)
{
    (void)ctx;
    if (ngot < 8 && n <= CT_FRAME_MAX) { memcpy(got[ngot], f, n); got_len[ngot] = n; }
    ngot++;
}

static void test_roundtrip(void)
{
    // Control frame, then data bytes that need stuffing and look like flags.
    static const uint8_t enq[] = { 0x00, 0x00, LLAP_ENQ };
    static const uint8_t ugly[] = { 0x00, 0x00, LLAP_ENQ, 0xFF, 0x7E, 0xFE, 0x3F, 0x1F, 0xF8 };
    const uint8_t *fs[] = { enq, ugly };
    size_t ns[] = { sizeof enq, sizeof ugly };
    for (int k = 0; k < 2; k++) {
        size_t n = wire(fs[k], ns[k], cap);
        ngot = 0;
        ct_fm0_decode(cap, n, RES, on_frame, NULL);
        CHECK_EQ_INT(ngot, 1);
        CHECK_EQ_INT(got_len[0], ns[k]);
        CHECK_EQ_MEM(got[0], fs[k], ns[k]);
        CHECK(ct_capture_has(cap, n, RES, fs[k], ns[k]));
    }
    // The stuffed 0 is there: 0xFF's eight 1s never reach six in a row.
    size_t n = ct_fm0_encode(ugly, sizeof ugly, runs, sizeof runs);
    int twos = 0, worst = 0;
    for (size_t i = 0; i < n - 13; i++) {      // the trailing 1s are meant to run on
        twos = runs[i] == 2 ? twos + 1 : 0;
        if (twos > worst) worst = twos;
    }
    CHECK_EQ_INT(worst, 6);                    // the flags' six, never more
}

// TashTalk's PIC sends a bit every 35 cycles at 8 MHz (4.375 us, 0.8% slow)
// off an internal oscillator good to a couple of percent; the RMT samples
// with its own jitter. Noise before and after: a floating RxD pair.
static void test_tolerance(void)
{
    static const uint8_t f[] = { 0x00, 0x00, LLAP_ENQ, 0x12, 0x34, 0x56, 0x78 };
    srand(1055);
    const double scales[] = { 0.97, 0.992, 1.0, 1.008, 1.03 };
    for (size_t s = 0; s < sizeof scales / sizeof scales[0]; s++) {
        size_t pre = 0;
        for (int i = 0; i < 40; i++) cap[pre++] = 5 + (uint32_t)(rand() % 900);    // noise
        cap[pre++] = 3000;                                                         // idle
        size_t r = ct_fm0_encode(f, sizeof f, runs, sizeof runs);
        ct_runs_to_ticks(runs, r, RES, t16);
        for (size_t i = 0; i < r; i++) {
            double j = 1.0 + ((rand() % 17) - 8) / 100.0;                          // +-8%
            cap[pre + i] = (uint32_t)(t16[i] * scales[s] * j + 0.5);
        }
        size_t n = pre + r;
        cap[n++] = 4000;
        for (int i = 0; i < 40; i++) cap[n++] = 5 + (uint32_t)(rand() % 900);
        CHECK(ct_capture_has(cap, n, RES, f, sizeof f));
    }
}

static void test_corrupt(void)
{
    static const uint8_t f[] = { 0x00, 0x00, LLAP_ENQ, 0xA5, 0x5A };
    size_t n = wire(f, sizeof f, cap);
    CHECK(ct_capture_has(cap, n, RES, f, sizeof f));
    // Turn a 0 in the frame's body into a 1: two half runs merged into one
    // whole run. The FCS catches it.
    size_t body = 4 + 3 * 10 + 4;              // past the preamble, into dst
    for (size_t i = body; i + 1 < n; i++) {
        if (cap[i] < 120 && cap[i + 1] < 120) {
            cap[i] += cap[i + 1];
            memmove(cap + i + 1, cap + i + 2, (n - i - 2) * sizeof cap[0]);
            n--;
            break;
        }
    }
    CHECK(!ct_capture_has(cap, n, RES, f, sizeof f));
    // The wrong frame is not the one we want.
    static const uint8_t other[] = { 0x00, 0x00, LLAP_ENQ, 0xA5, 0x5B };
    n = wire(f, sizeof f, cap);
    CHECK(!ct_capture_has(cap, n, RES, other, sizeof other));
    // Nothing at all.
    CHECK(!ct_capture_has(cap, 0, RES, f, sizeof f));
}

static void test_two_frames(void)
{
    // A capture can hold the frame after another one (the node acquiring).
    static const uint8_t a[] = { 0x81, 0x81, LLAP_ENQ };
    static const uint8_t b[] = { 0x00, 0x00, LLAP_ENQ };
    size_t n = wire(a, sizeof a, cap);
    cap[n++] = 2000;
    n += wire(b, sizeof b, cap + n);
    ngot = 0;
    ct_fm0_decode(cap, n, RES, on_frame, NULL);
    CHECK_EQ_INT(ngot, 2);
    CHECK_EQ_MEM(got[0], a, sizeof a);
    CHECK_EQ_MEM(got[1], b, sizeof b);
}

static void test_verdict(void)
{
    CHECK_EQ_INT(ct_verdict(CT_TT_OK, 1, 1), CT_PASS);
    CHECK_EQ_INT(ct_verdict(CT_TT_OK, 0, 0), CT_NO_LOOP);
    CHECK_EQ_INT(ct_verdict(CT_TT_OK, 0, 1), CT_NO_LOOP);     // fwd is never tried without back
    CHECK_EQ_INT(ct_verdict(CT_TT_OK, 1, 0), CT_ONE_WAY);
    CHECK(!strcmp(ct_verdict_name(CT_PASS), "pass"));
    CHECK(!strcmp(ct_verdict_name(CT_NO_LOOP), "no_loop"));
    CHECK(!strcmp(ct_verdict_name(CT_ONE_WAY), "one_way"));
    CHECK(strlen(ct_verdict_text(CT_ONE_WAY)) > 10);

    // W-1063: TashTalk silent means no cable was tested, whatever the legs
    // say, and the verdict names what U7 holds instead of blaming the cable.
    const ct_tt_t tt[] = { CT_TT_NO_CHIP, CT_TT_NO_TASHTALK, CT_TT_UNCHECKED };
    const ct_verdict_t want[] = { CT_NO_CHIP, CT_NO_TASHTALK, CT_CHIP_SILENT };
    const char *name[] = { "no_chip", "no_tashtalk", "chip_silent" };
    for (int k = 0; k < 3; k++) {
        for (int legs = 0; legs < 4; legs++)
            CHECK_EQ_INT(ct_verdict(tt[k], legs & 1, legs >> 1), want[k]);
        CHECK(!ct_verdict_tested(want[k]));
        CHECK(!strcmp(ct_verdict_name(want[k]), name[k]));
        CHECK(strstr(ct_verdict_text(want[k]), "Couldn’t test the cable") != NULL);
        CHECK(!strchr(ct_verdict_text(want[k]), '"'));       // it goes into JSON as is
    }
    CHECK(ct_verdict_tested(CT_PASS) && ct_verdict_tested(CT_NO_LOOP) && ct_verdict_tested(CT_ONE_WAY));
    CHECK(strstr(ct_verdict_text(CT_NO_TASHTALK), "Flash TashTalk") != NULL);
}

int main(void)
{
    test_encode_shape();
    test_ticks();
    test_roundtrip();
    test_tolerance();
    test_corrupt();
    test_two_frames();
    test_verdict();
    TEST_END;
}
