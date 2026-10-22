// SPDX-License-Identifier: GPL-3.0-or-later
//
// The LaserWriter's policy (W-1082): when the board looks for LaserWriters,
// asks the shared one its status, runs the query job, and what the phone is
// told, with a fake clock.
#include "test_util.h"
#include "lw_core.h"

static lt_route_t route(uint8_t node) { lt_route_t r = { { 0, node, 0x85 }, node, 0 }; return r; }

static nbp_tuple_t tuple(const char *name, uint8_t node)
{
    nbp_tuple_t t = { .addr = { 0, node, 0x85 } };
    nbp_name(t.obj, name);
    nbp_name(t.type, "LaserWriter");
    t.zone[0] = 1; t.zone[1] = '*';
    return t;
}

static void found(lwc_t *c, const char *name, uint8_t link, uint8_t node, uint64_t now)
{
    nbp_tuple_t t = tuple(name, node);
    lt_route_t r = route(node);
    lwc_found(c, &t, link, &r, now);
}

static void status(lwc_t *c, int idx, const char *s, uint64_t now)
{
    uint8_t p[81];
    p[0] = (uint8_t)strlen(s);
    memcpy(p + 1, s, p[0]);
    lwc_status(c, idx, p, now);
}

int main(void)
{
    lwc_t c;
    uint8_t nt[NBP_NAME_MAX + 1];
    nbp_name(nt, "LaserWriter II NT");

    // Nothing shared, the card closed: nothing to do.
    lwc_init(&c, 0);
    lwc_want_t w = lwc_tick(&c, 1000);
    CHECK(!w.look && w.status_idx == LWC_NONE && !w.query);
    lwc_view_t v = lwc_view(&c, 1000);
    CHECK(!v.shared);

    // The card opens: a lookup at once, then not again for 30 s.
    lwc_card(&c, 2000);
    w = lwc_tick(&c, 2000);
    CHECK(w.look);
    w = lwc_tick(&c, 2100);
    CHECK(!w.look);
    CHECK(!lwc_tick(&c, 31999).look);
    CHECK(lwc_tick(&c, 32000).look);           // while the card is open
    CHECK(!lwc_tick(&c, 100000).look);         // closed (no GET for a minute): no more

    // Heard: on the tunnel, then the same name on the cable.
    found(&c, "LaserWriter II NT", LW_LINK_TUNNEL, 30, 100100);
    found(&c, "LaserWriter II NT", LW_LINK_CABLE, 12, 100200);
    found(&c, "Personal LW", LW_LINK_RADIO, 40, 100300);
    size_t n = 0;
    for (int i = 0; i < LW_HEARD_MAX; i++) n += c.heard[i].used;
    CHECK_EQ_INT(n, 3);

    // Shared: the cable's wins; it is online, and its first answer wants the query.
    lwc_share(&c, nt, 100400);
    v = lwc_view(&c, 100400);
    CHECK(v.shared);
    CHECK(v.idx >= 0 && c.heard[v.idx].link == LW_LINK_CABLE && c.heard[v.idx].route.addr.node == 12);
    w = lwc_tick(&c, 100400);
    CHECK(w.look);                             // a share looks at once,
    CHECK(w.query);                            // and, heard already, queries at once
    found(&c, "LaserWriter II NT", LW_LINK_CABLE, 12, 100500);
    CHECK(!lwc_tick(&c, 100500).query);        // asked once
    CHECK(!lwc_tick(&c, 100600).query);
    lwc_queried(&c, 1, 101000);

    // Status every 5 s while shared; every 2 s while printing.
    int idx = lwc_view(&c, 101000).idx;
    w = lwc_tick(&c, 105500);
    CHECK_EQ_INT(w.status_idx, idx);
    status(&c, idx, "status: idle", 105510);
    CHECK_EQ_INT(lwc_tick(&c, 108000).status_idx, LWC_NONE);
    CHECK_EQ_INT(lwc_tick(&c, 110600).status_idx, idx);
    status(&c, idx, "status: idle", 110610);
    v = lwc_view(&c, 110610);
    CHECK(v.online);
    CHECK_EQ_INT(v.ipp_state, LWC_IDLE);
    CHECK(!strcmp(v.reason, "none"));
    lwc_job(&c, 1);
    CHECK_EQ_INT(lwc_tick(&c, 112700).status_idx, idx);
    status(&c, idx, "job: Letter; status: busy; source: AppleTalk", 112710);
    v = lwc_view(&c, 112710);
    CHECK_EQ_INT(v.ipp_state, LWC_PROCESSING);
    CHECK(v.busy);

    // Out of paper: stopped, the printer's own words as the message.
    status(&c, idx, "status: PrinterError: out of paper", 113000);
    v = lwc_view(&c, 113000);
    CHECK_EQ_INT(v.ipp_state, LWC_STOPPED);
    CHECK(!strcmp(v.reason, "media-empty-error"));
    CHECK(!strcmp(v.message, "status: PrinterError: out of paper"));
    lwc_job(&c, 0);
    status(&c, idx, "status: idle", 114000);
    CHECK_EQ_INT(lwc_view(&c, 114000).ipp_state, LWC_IDLE);

    // Silent for 30 s: offline. Back: the query again.
    uint64_t t = 114000;
    for (; t < 144100; t += 500) lwc_tick(&c, t);
    v = lwc_view(&c, t);
    CHECK(!v.online);
    CHECK_EQ_INT(v.ipp_state, LWC_STOPPED);
    CHECK(!strcmp(v.reason, "offline-report"));
    found(&c, "LaserWriter II NT", LW_LINK_CABLE, 12, t + 10);
    CHECK(lwc_view(&c, t + 10).online);
    CHECK(lwc_tick(&c, t + 20).query);

    // Nine heard: the one quiet longest makes room.
    lwc_init(&c, 0);
    char name[16];
    for (int i = 0; i < 8; i++) {
        snprintf(name, sizeof name, "LW %d", i);
        found(&c, name, LW_LINK_RADIO, (uint8_t)(10 + i), 1000 + (uint64_t)i * 10);
    }
    found(&c, "LW 0", LW_LINK_RADIO, 10, 2000);           // LW 1 is now the quietest
    found(&c, "LW 8", LW_LINK_RADIO, 18, 2100);
    int have1 = 0, have8 = 0;
    for (int i = 0; i < LW_HEARD_MAX; i++) {
        if (!c.heard[i].used) continue;
        if (c.heard[i].name[0] == 4 && !memcmp(c.heard[i].name + 1, "LW 1", 4)) have1 = 1;
        if (c.heard[i].name[0] == 4 && !memcmp(c.heard[i].name + 1, "LW 8", 4)) have8 = 1;
    }
    CHECK(!have1 && have8);

    // A printer not heard for 10 minutes leaves the list.
    lwc_tick(&c, 2100 + LW_FORGET_MS + 1000);
    n = 0;
    for (int i = 0; i < LW_HEARD_MAX; i++) n += c.heard[i].used;
    CHECK_EQ_INT(n, 0);

    // Unshared: nothing shared, no more status asks.
    lwc_share(&c, NULL, 900000);
    CHECK(!lwc_view(&c, 900000).shared);

    TEST_END;
}
