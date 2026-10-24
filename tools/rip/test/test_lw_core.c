// SPDX-License-Identifier: GPL-3.0-or-later
//
// The AppleTalk printers' policy (W-1082, W-1088): when the board looks for
// printers, which it tells the printer list about, asks each its status,
// runs the query job, and what the phone is told, with a fake clock.
#include "test_util.h"
#include "lw_core.h"

static lt_route_t route(uint8_t node) { lt_route_t r = { { 0, node, 0x85 }, node, 0 }; return r; }

static void found_t(lwc_t *c, const char *name, const char *type, uint8_t link, uint8_t node,
                    uint64_t now)
{
    nbp_tuple_t t = { .addr = { 0, node, 0x85 } };
    nbp_name(t.obj, name);
    nbp_name(t.type, type);
    t.zone[0] = 1; t.zone[1] = '*';
    lt_route_t r = route(node);
    lwc_found(c, &t, link, &r, now);
}

static void found(lwc_t *c, const char *name, uint8_t link, uint8_t node, uint64_t now)
{
    found_t(c, name, "LaserWriter", link, node, now);
}

static void status(lwc_t *c, int ti, const char *s, uint64_t now)
{
    uint8_t p[81];
    p[0] = (uint8_t)strlen(s);
    memcpy(p + 1, s, p[0]);
    lwc_status(c, ti, p, now);
}

static lwc_target_in_t target(uint8_t id, const char *name, uint8_t kind)
{
    lwc_target_in_t t = { .id = id, .kind = kind };
    nbp_name(t.name, name);
    return t;
}

// Lookups at boot and every 30 s for 5 minutes, then every 5 minutes; "Look
// for printers" starts the 30 s run again.
static void test_cadence(void)
{
    static lwc_t c;
    lwc_init(&c, 0);
    uint64_t looks[64];
    int n = 0;
    for (uint64_t t = 0; t <= 1200000 && n < 64; t += 1000)
        if (lwc_tick(&c, t).look) looks[n++] = t;
    static const uint64_t want[] = { 0, 30000, 60000, 90000, 120000, 150000, 180000, 210000,
                                     240000, 270000, 300000, 600000, 900000, 1200000 };
    CHECK_EQ_INT(n, (int)(sizeof want / sizeof want[0]));
    for (int i = 0; i < n && i < (int)(sizeof want / sizeof want[0]); i++)
        CHECK_EQ_INT((long)looks[i], (long)want[i]);
    lwc_look_now(&c, 1250000);
    CHECK(lwc_tick(&c, 1250000).look);
    CHECK(!lwc_tick(&c, 1270000).look);
    CHECK(lwc_tick(&c, 1280000).look);
}

// What was heard is told to the printer list once, a second after it was
// first heard, unless it came from another AirWriter.
static void test_tell(void)
{
    static lwc_t c;
    lwc_init(&c, 0);
    found(&c, "LaserWriter II NT", LW_LINK_TUNNEL, 30, 100);
    found_t(&c, "Den ImageWriter", "ImageWriter", LW_LINK_CABLE, 12, 200);
    found_t(&c, "imagewriter-2434", "AirWriter", LW_LINK_RADIO, 50, 300);
    found(&c, "Office", LW_LINK_RADIO, 50, 400);          // relayed by that AirWriter
    CHECK_EQ_INT(lwc_tick(&c, 500).tell, LWC_NONE);        // too soon
    int told[LW_HEARD_MAX] = { 0 }, n = 0;
    for (uint64_t t = 1100; t < 3000; t += 100) {
        uint8_t i = lwc_tick(&c, t).tell;
        if (i != LWC_NONE) { told[n++] = i; CHECK(c.heard[i].told); }
    }
    CHECK_EQ_INT(n, 2);
    int lw = 0, iw = 0;
    for (int k = 0; k < n; k++) {
        const lw_heard_t *h = &c.heard[told[k]];
        lw |= h->kind == LWC_KIND_LW && h->link == LW_LINK_TUNNEL;
        iw |= h->kind == LWC_KIND_IW && h->link == LW_LINK_CABLE;
    }
    CHECK(lw && iw);
    // Heard again: not told again.
    found(&c, "LaserWriter II NT", LW_LINK_TUNNEL, 30, 5000);
    for (uint64_t t = 6100; t < 7000; t += 100) CHECK_EQ_INT(lwc_tick(&c, t).tell, LWC_NONE);
}

// A target heard on two links rides the cable's; its first answer wants the
// query; status every 5 s, 2 s while printing; the phone's view.
static void test_one(void)
{
    static lwc_t c;
    lwc_init(&c, 0);
    lwc_target_in_t in = target(2, "LaserWriter II NT", LWC_KIND_LW);
    lwc_targets(&c, &in, 1, 0);
    int ti = lwc_target(&c, 2);
    CHECK(ti >= 0);
    lwc_view_t v = lwc_view(&c, ti, 0);
    CHECK(!v.online);
    CHECK_EQ_INT(v.ipp_state, LWC_STOPPED);
    CHECK(!strcmp(v.reason, "offline-report"));

    found(&c, "LaserWriter II NT", LW_LINK_TUNNEL, 30, 100100);
    found(&c, "LaserWriter II NT", LW_LINK_CABLE, 12, 100200);
    v = lwc_view(&c, ti, 100200);
    CHECK(v.online);
    CHECK(v.idx >= 0 && c.heard[v.idx].link == LW_LINK_CABLE && c.heard[v.idx].route.addr.node == 12);
    lwc_want_t w = lwc_tick(&c, 100400);
    CHECK_EQ_INT(w.query_ti, ti);                      // back after boot: asked again
    CHECK_EQ_INT(lwc_tick(&c, 100500).query_ti, LWC_NONE);
    lwc_queried(&c, ti, 1, 101000);

    w = lwc_tick(&c, 105500);
    CHECK_EQ_INT(w.status_ti, ti);
    status(&c, ti, "status: idle", 105510);
    CHECK_EQ_INT(lwc_tick(&c, 108000).status_ti, LWC_NONE);
    CHECK_EQ_INT(lwc_tick(&c, 110600).status_ti, ti);
    status(&c, ti, "status: idle", 110610);
    v = lwc_view(&c, ti, 110610);
    CHECK_EQ_INT(v.ipp_state, LWC_IDLE);
    CHECK(!strcmp(v.reason, "none"));
    lwc_job(&c, ti, 1);
    CHECK_EQ_INT(lwc_tick(&c, 112700).status_ti, ti);
    status(&c, ti, "job: Letter; status: busy; source: AppleTalk", 112710);
    v = lwc_view(&c, ti, 112710);
    CHECK_EQ_INT(v.ipp_state, LWC_PROCESSING);
    CHECK(v.busy);

    status(&c, ti, "status: PrinterError: out of paper", 113000);
    v = lwc_view(&c, ti, 113000);
    CHECK_EQ_INT(v.ipp_state, LWC_STOPPED);
    CHECK(!strcmp(v.reason, "media-empty-error"));
    CHECK(!strcmp(v.message, "status: PrinterError: out of paper"));
    lwc_job(&c, ti, 0);
    status(&c, ti, "status: idle", 114000);
    CHECK_EQ_INT(lwc_view(&c, ti, 114000).ipp_state, LWC_IDLE);

    // Review I3: a warning with the printer idle; warming up keeps it processing.
    status(&c, ti, "status: PrinterError: toner low", 114100);
    v = lwc_view(&c, ti, 114100);
    CHECK_EQ_INT(v.ipp_state, LWC_IDLE);
    CHECK(!strcmp(v.reason, "toner-low-warning"));
    CHECK(!strcmp(v.message, "status: PrinterError: toner low"));
    status(&c, ti, "status: warming up", 114200);
    v = lwc_view(&c, ti, 114200);
    CHECK_EQ_INT(v.ipp_state, LWC_PROCESSING);

    // Review I4: a classic Mac's job name (MacRoman) comes out UTF-8.
    status(&c, ti, "job: Wells\xd5s Letter; status: busy; source: AppleTalk", 114300);
    CHECK(strstr(lwc_view(&c, ti, 114300).message, "Wells\xe2\x80\x99s Letter") != NULL);
    status(&c, ti, "status: idle", 114400);

    // Silent for 30 s: offline. Back: the query again.
    uint64_t t = 114400;
    for (; t < 144500; t += 500) lwc_tick(&c, t);
    v = lwc_view(&c, ti, t);
    CHECK(!v.online);
    CHECK(!strcmp(v.reason, "offline-report"));
    found(&c, "LaserWriter II NT", LW_LINK_CABLE, 12, t + 10);
    CHECK(lwc_view(&c, ti, t + 10).online);
    CHECK_EQ_INT(lwc_tick(&c, t + 20).query_ti, ti);

    // Set again with the same id and name: its state stays.
    lwc_targets(&c, &in, 1, t + 30);
    CHECK(lwc_view(&c, lwc_target(&c, 2), t + 30).online);
    // Gone from the list: no target.
    lwc_targets(&c, NULL, 0, t + 40);
    CHECK_EQ_INT(lwc_target(&c, 2), -1);
}

// A query that fails is tried again at +60 s and +120 s, then every 5 minutes.
static void test_query_retry(void)
{
    static lwc_t c;
    lwc_init(&c, 0);
    lwc_target_in_t in = target(3, "Office", LWC_KIND_LW);
    lwc_targets(&c, &in, 1, 0);
    int ti = lwc_target(&c, 3);
    uint64_t asked[8];
    int n = 0;
    for (uint64_t t = 1000; t <= 800000 && n < 8; t += 500) {
        if (t % 20000 == 1000) found(&c, "Office", LW_LINK_TUNNEL, 30, t);   // keeps answering
        if (lwc_tick(&c, t).query_ti == ti) {
            asked[n++] = t;
            lwc_queried(&c, ti, 0, t + 400);
        }
    }
    CHECK(n >= 5);
    CHECK_EQ_INT((long)asked[0], 1000);
    CHECK(asked[1] >= 61400 && asked[1] < 62000);
    CHECK(asked[2] >= 121800 && asked[2] < 123000);
    CHECK(asked[3] >= 422000 && asked[3] < 424000);    // three failed: every 5 minutes
    // An ImageWriter on AppleTalk is never queried: its type says what it is.
    lwc_init(&c, 0);
    in = target(4, "Den ImageWriter", LWC_KIND_IW);
    lwc_targets(&c, &in, 1, 0);
    found_t(&c, "Den ImageWriter", "ImageWriter", LW_LINK_CABLE, 12, 1000);
    for (uint64_t t = 1000; t < 70000; t += 500) CHECK_EQ_INT(lwc_tick(&c, t).query_ti, LWC_NONE);
}

// Two printers asked their status in turn, each every 5 s; a job's every 2 s.
static void test_two_lw_status(void)
{
    static lwc_t c;
    lwc_init(&c, 0);
    lwc_target_in_t in[2] = { target(2, "A", LWC_KIND_LW), target(5, "B", LWC_KIND_LW) };
    lwc_targets(&c, in, 2, 0);
    int a = lwc_target(&c, 2), b = lwc_target(&c, 5);
    found(&c, "A", LW_LINK_TUNNEL, 30, 0);
    found(&c, "B", LW_LINK_TUNNEL, 31, 0);
    int na = 0, nb = 0, last = -1, alternated = 1;
    for (uint64_t t = 100; t <= 30000; t += 100) {
        lwc_want_t w = lwc_tick(&c, t);
        if (w.query_ti != LWC_NONE) lwc_queried(&c, w.query_ti, 1, t);
        if (w.status_ti == LWC_NONE) continue;
        if (w.status_ti == last) alternated = 0;
        last = w.status_ti;
        na += w.status_ti == a;
        nb += w.status_ti == b;
        status(&c, w.status_ti, "status: idle", t + 10);
    }
    CHECK(alternated);
    CHECK(na >= 5 && na <= 6 && nb >= 5 && nb <= 6);
    lwc_job(&c, a, 1);
    na = nb = 0;
    for (uint64_t t = 30100; t <= 50000; t += 100) {
        lwc_want_t w = lwc_tick(&c, t);
        if (w.status_ti == LWC_NONE) continue;
        na += w.status_ti == a;
        nb += w.status_ti == b;
        status(&c, w.status_ti, "status: busy", t + 10);
    }
    CHECK(na >= 9 && na <= 11);
    CHECK(nb >= 3 && nb <= 5);
}

// The heard list: full, the quietest goes; ten minutes unheard, forgotten.
static void test_heard_list(void)
{
    static lwc_t c;
    lwc_init(&c, 0);
    char name[16];
    for (int i = 0; i < LW_HEARD_MAX; i++) {
        snprintf(name, sizeof name, "LW %d", i);
        found(&c, name, LW_LINK_RADIO, (uint8_t)(10 + i), 1000 + (uint64_t)i * 10);
    }
    found(&c, "LW 0", LW_LINK_RADIO, 10, 2000);           // LW 1 is now the quietest
    found(&c, "LW 99", LW_LINK_RADIO, 99, 2100);
    int have1 = 0, have99 = 0;
    for (int i = 0; i < LW_HEARD_MAX; i++) {
        if (!c.heard[i].used) continue;
        if (c.heard[i].name[0] == 4 && !memcmp(c.heard[i].name + 1, "LW 1", 4)) have1 = 1;
        if (c.heard[i].name[0] == 5 && !memcmp(c.heard[i].name + 1, "LW 99", 5)) have99 = 1;
    }
    CHECK(!have1 && have99);
    lwc_tick(&c, 2100 + LW_FORGET_MS + 1000);
    int n = 0;
    for (int i = 0; i < LW_HEARD_MAX; i++) n += c.heard[i].used;
    CHECK_EQ_INT(n, 0);
}

int main(void)
{
    test_cadence();
    test_tell();
    test_one();
    test_query_retry();
    test_two_lw_status();
    test_heard_list();
    TEST_END;
}
