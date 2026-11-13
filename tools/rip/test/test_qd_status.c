// SPDX-License-Identifier: GPL-3.0-or-later
// W-1102: how a Personal LaserWriter 300's state reads from its status replies, what the phone
// is told for each, and the front lights the board sets for it. The bytes are the bench's
// (board 1, 2026-11-12): idle 83 01 -> 01, 83 02 -> 01, 83 2A -> 20, 82 00 -> A4.
#include "test_util.h"
#include "qd_status.h"

static qd_raw_t idle(void)
{
    return (qd_raw_t){ .s01 = 0x01, .s02 = 0x01, .s04 = 0x01, .s07 = 0x01, .s2a = 0x20, .c82 = 0xA4 };
}

static void test_state_from(void)
{
    const qd_spec_t *sp = qd_spec(QD_PLW300);
    qd_raw_t r = idle();
    CHECK_EQ_INT(qd_state_from(sp, &r, 0), QDS_READY);

    r = idle(); r.s01 = 0x20;                       // paper moving
    CHECK_EQ_INT(qd_state_from(sp, &r, 0), QDS_PRINTING);
    r = idle(); r.s01 = 0x21;
    CHECK_EQ_INT(qd_state_from(sp, &r, 0), QDS_PRINTING);

    r = idle(); r.s2a = 0x00;                       // no paper in the cassette
    CHECK_EQ_INT(qd_state_from(sp, &r, 0), QDS_NO_PAPER);
    r = idle();                                     // cassette full, the manual slot asked for and empty
    CHECK_EQ_INT(qd_state_from(sp, &r, 1), QDS_MANUAL);
    r = idle(); r.s2a = 0x40;                       // a sheet in the slot, none in the cassette
    CHECK_EQ_INT(qd_state_from(sp, &r, 1), QDS_READY);
    CHECK_EQ_INT(qd_state_from(sp, &r, 0), QDS_NO_PAPER);
    r = idle(); r.s2a = 0x60;
    CHECK_EQ_INT(qd_state_from(sp, &r, 1), QDS_READY);

    r = idle(); r.s02 = 0x21;                       // cassette not in properly
    CHECK_EQ_INT(qd_state_from(sp, &r, 0), QDS_NO_CASSETTE);
    r = idle(); r.s02 = 0x05;                       // cover open or no toner cartridge
    CHECK_EQ_INT(qd_state_from(sp, &r, 0), QDS_COVER);
    r = idle(); r.s02 = 0x09;                       // jam
    CHECK_EQ_INT(qd_state_from(sp, &r, 0), QDS_JAM);
    r = idle(); r.s01 = 0x03;                       // a fault stored
    CHECK_EQ_INT(qd_state_from(sp, &r, 0), QDS_ENGINE);

    // First match wins: jam over cover over cassette over engine over paper over printing.
    r = idle(); r.s02 = 0x2D; r.s01 = 0x23; r.s2a = 0;
    CHECK_EQ_INT(qd_state_from(sp, &r, 0), QDS_JAM);
    r.s02 = 0x25;
    CHECK_EQ_INT(qd_state_from(sp, &r, 0), QDS_COVER);
    r.s02 = 0x21;
    CHECK_EQ_INT(qd_state_from(sp, &r, 0), QDS_NO_CASSETTE);
    r.s02 = 0x01;
    CHECK_EQ_INT(qd_state_from(sp, &r, 0), QDS_ENGINE);
    r.s01 = 0x21;
    CHECK_EQ_INT(qd_state_from(sp, &r, 0), QDS_NO_PAPER);
    CHECK_EQ_INT(qd_state_from(sp, &r, 1), QDS_MANUAL);
}

// 83 02's 10 is no paper where the printer was told to take it from, and 83 01 sets its fault
// bit with it, as board 1's printer answered (2026-11-13). It is paper, not an engine fault.
static void test_no_paper_at_the_source_as_the_bench_read_it(void)
{
    const qd_spec_t *sp = qd_spec(QD_PLW300);
    qd_raw_t r = idle();                            // the cassette pulled out
    r.s01 = 0x02; r.s02 = 0x10; r.s2a = 0x01; r.c82 = 0xAC;
    CHECK_EQ_INT(qd_state_from(sp, &r, 0), QDS_NO_PAPER);
    r = idle();                                     // a page from the slot done, the slot empty
    r.s01 = 0x02; r.s02 = 0x10; r.s2a = 0x20; r.c82 = 0xAC;
    CHECK_EQ_INT(qd_state_from(sp, &r, 0), QDS_READY);
    CHECK_EQ_INT(qd_state_from(sp, &r, 1), QDS_MANUAL);
}

static void test_ipp(void)
{
    static const struct { qd_state_t st; const char *reason, *message; } T[] = {
        { QDS_NO_PAPER,    "media-empty-error",        "Needs attention: check paper" },
        { QDS_NO_CASSETTE, "input-tray-missing-error", "Needs attention: check the paper cassette" },
        { QDS_MANUAL,      "media-needed-report",      "Needs attention: load the manual feed slot" },
        { QDS_JAM,         "media-jam-error",          "Needs attention: clear the paper jam" },
        { QDS_COVER,       "door-open-error",          "Needs attention: close the cover" },
        { QDS_ENGINE,      "other-error",              "Needs attention: check the printer" },
        { QDS_OFFLINE,     "other",                    "Offline" },
    };
    for (size_t i = 0; i < sizeof T / sizeof T[0]; i++) {
        qd_ipp_t p = qd_state_ipp(T[i].st);
        CHECK(p.reason && !strcmp(p.reason, T[i].reason));
        CHECK(p.message && !strcmp(p.message, T[i].message));
        CHECK(!qd_state_ready(T[i].st));
        CHECK_EQ_INT(qd_state_fault(T[i].st), T[i].st != QDS_OFFLINE);
    }
    CHECK(qd_state_ipp(QDS_READY).reason == NULL && qd_state_ipp(QDS_READY).message == NULL);
    CHECK(qd_state_ipp(QDS_PRINTING).reason == NULL && qd_state_ipp(QDS_PRINTING).message == NULL);
    CHECK(qd_state_ready(QDS_READY) && qd_state_ready(QDS_PRINTING));
    CHECK(!qd_state_fault(QDS_READY) && !qd_state_fault(QDS_PRINTING));
    // Literals the server compares by address.
    CHECK(qd_state_ipp(QDS_JAM).reason == qd_state_ipp(QDS_JAM).reason);
}

static void test_lights(void)
{
    for (int b = 0; b < 2; b++) {
        CHECK_EQ_INT(qd_lights(QDS_READY, b), 0x01);
        CHECK_EQ_INT(qd_lights(QDS_NO_PAPER, b), 0x02);
        CHECK_EQ_INT(qd_lights(QDS_NO_CASSETTE, b), 0x02);
        CHECK_EQ_INT(qd_lights(QDS_MANUAL, b), 0x02);
        CHECK_EQ_INT(qd_lights(QDS_JAM, b), 0x04);
        CHECK_EQ_INT(qd_lights(QDS_COVER, b), 0x04);
        CHECK_EQ_INT(qd_lights(QDS_ENGINE, b), 0x00);
        CHECK_EQ_INT(qd_lights(QDS_OFFLINE, b), 0x00);
    }
    CHECK_EQ_INT(qd_lights(QDS_PRINTING, 1), 0x01);
    CHECK_EQ_INT(qd_lights(QDS_PRINTING, 0), 0x00);
}

int main(void)
{
    test_state_from();
    test_no_paper_at_the_source_as_the_bench_read_it();
    test_ipp();
    test_lights();
    TEST_END;
}
