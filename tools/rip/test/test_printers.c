// SPDX-License-Identifier: GPL-3.0-or-later
//
// The printer list (W-1088): what AirWriter offers over AirPrint and to old
// Macs is exactly the printers a person has, found or told. Naming, identity,
// sharing and its cap, notices, migration from a board before W-1088, and the
// stored form.
#include "test_util.h"
#include "printers_core.h"
#include "lw_caps.h"
#include <stdlib.h>

static const uint8_t MAC[6] = { 0x10, 0xBD, 0xA3, 0x6E, 0x24, 0x34 };

static pr_list_t L;
static void fresh(void) { pr_init(&L, MAC, 1); }

static int notes_of(int kind)
{
    int c = 0;
    for (int i = 0; i < L.n_notes; i++) if (L.notes[i].kind == kind) c++;
    return c;
}

static const uint8_t *P(const char *s)      // a Pascal string from C, for tests
{
    static uint8_t b[8][34];
    static int k;
    uint8_t *o = b[k++ % 8];
    size_t n = strlen(s);
    o[0] = (uint8_t)n;
    memcpy(o + 1, s, n);
    return o;
}

static void test_port_identified(void)
{
    fresh();
    int id = pr_port_seen(&L, PR_PORT_IW2);
    CHECK(id >= 0);
    const pr_rec_t *r = pr_get(&L, id);
    CHECK_EQ_INT(r->where, PR_WHERE_PORT);
    CHECK_EQ_INT(r->kind, PR_KIND_IMAGEWRITER);
    CHECK_EQ_INT(r->model, PR_MODEL_IW2);
    CHECK(strcmp(r->name, "ImageWriter II") == 0);
    CHECK_EQ_INT(r->shared, 1);
    CHECK_EQ_INT(r->found_auto, 1);
    CHECK_EQ_INT(notes_of(PR_NOTE_FOUND), 1);
    CHECK_EQ_INT(pr_port_seen(&L, PR_PORT_IW2), id);
    CHECK_EQ_INT(notes_of(PR_NOTE_FOUND), 1);
    CHECK_EQ_INT(L.n, 1);
}

static void test_port_silent(void)
{
    fresh();
    int id = pr_port_seen(&L, PR_PORT_SILENT);
    const pr_rec_t *r = pr_get(&L, id);
    CHECK_EQ_INT(r->kind, PR_KIND_UNKNOWN);
    CHECK_EQ_INT(r->shared, 0);
    CHECK_EQ_INT(notes_of(PR_NOTE_UNKNOWN_PORT), 1);
    CHECK_EQ_INT(pr_set_shared(&L, id, 1), -1);       // nothing to print to yet
    CHECK_EQ_INT(pr_set_model(&L, id, PR_MODEL_IW1), 0);
    r = pr_get(&L, id);
    CHECK_EQ_INT(r->kind, PR_KIND_IMAGEWRITER);
    CHECK(strcmp(r->name, "ImageWriter") == 0);
    CHECK_EQ_INT(r->shared, 1);
    CHECK_EQ_INT(notes_of(PR_NOTE_UNKNOWN_PORT), 0);
    // "Something else": listed, not shared, and the port stays quiet.
    CHECK_EQ_INT(pr_set_model(&L, id, PR_MODEL_NONE), 0);
    CHECK_EQ_INT(pr_get(&L, id)->kind, PR_KIND_UNKNOWN);
    CHECK_EQ_INT(pr_get(&L, id)->shared, 0);
    CHECK_EQ_INT(pr_port_is_imagewriter(&L), 0);
}

static void test_port_changed(void)
{
    fresh();
    int id = pr_port_seen(&L, PR_PORT_IW2);
    char uuid[46];
    strcpy(uuid, pr_get(&L, id)->uuid);
    pr_rename(&L, id, "Desk");
    CHECK_EQ_INT(pr_port_seen(&L, PR_PORT_IWLQ), id);
    CHECK_EQ_INT(pr_get(&L, id)->model, PR_MODEL_IW2);      // offered, never swapped
    CHECK(strcmp(pr_get(&L, id)->name, "Desk") == 0);
    CHECK_EQ_INT(notes_of(PR_NOTE_CHANGED), 1);
    pr_port_seen(&L, PR_PORT_IWLQ);                         // seen again: one note
    CHECK_EQ_INT(notes_of(PR_NOTE_CHANGED), 1);
    int note = -1;
    for (int i = 0; i < L.n_notes; i++) if (L.notes[i].kind == PR_NOTE_CHANGED) note = i;
    CHECK_EQ_INT(L.notes[note].model, PR_MODEL_IWLQ);
    CHECK_EQ_INT(pr_accept_change(&L, note), 0);
    CHECK_EQ_INT(pr_get(&L, id)->model, PR_MODEL_IWLQ);
    CHECK(strcmp(pr_get(&L, id)->name, "Desk") == 0);       // the person's name stays
    CHECK(strcmp(pr_get(&L, id)->uuid, uuid) == 0);
    CHECK_EQ_INT(notes_of(PR_NOTE_CHANGED), 0);

    // A default name follows the model.
    fresh();
    id = pr_port_seen(&L, PR_PORT_IW2);
    pr_port_seen(&L, PR_PORT_IWLQ);
    pr_accept_change(&L, 1);                                // notes: FOUND, CHANGED
    CHECK(strcmp(pr_get(&L, id)->name, "ImageWriter LQ") == 0);
}

static void test_port_nothing(void)
{
    fresh();
    int id = pr_port_seen(&L, PR_PORT_IW2);
    int notes = L.n_notes;
    CHECK_EQ_INT(pr_port_seen(&L, PR_PORT_NOTHING), id);
    CHECK_EQ_INT(L.n, 1);
    CHECK_EQ_INT(L.n_notes, notes);
    // A silent printer where an ImageWriter was set up (an original that
    // never answers, a self-ID lost on the line) changes nothing either.
    CHECK_EQ_INT(pr_port_seen(&L, PR_PORT_SILENT), id);
    CHECK_EQ_INT(pr_get(&L, id)->kind, PR_KIND_IMAGEWRITER);
    CHECK_EQ_INT(L.n_notes, notes);
}

static void test_at_laserwriter(void)
{
    fresh();
    int id = pr_at_seen(&L, PR_WHERE_NETWORK, P("Office"), P("LaserWriter"), 0);
    CHECK(id >= 0);
    CHECK_EQ_INT(pr_get(&L, id)->kind, PR_KIND_LASERWRITER);
    CHECK_EQ_INT(pr_get(&L, id)->identified, 0);
    CHECK_EQ_INT(pr_get(&L, id)->shared, 0);
    CHECK_EQ_INT(notes_of(PR_NOTE_FOUND), 0);
    lw_info_t info;
    lw_info_default(&info);
    strcpy(info.product, "LaserWriter II NT");
    pr_at_queried(&L, id, 1, &info);
    const pr_rec_t *r = pr_get(&L, id);
    CHECK_EQ_INT(r->identified, 1);
    CHECK_EQ_INT(r->shared, 1);
    CHECK(strcmp(r->lw.product, "LaserWriter II NT") == 0);
    CHECK(strcmp(r->name, "Office") == 0);
    CHECK_EQ_INT(notes_of(PR_NOTE_FOUND), 1);
    char want[46];
    lw_uuid(want, MAC, 1, P("Office"));
    CHECK(strcmp(r->uuid, want) == 0);
    // Asked again later: no second note, no re-share after the person unshared.
    pr_set_shared(&L, id, 0);
    pr_at_queried(&L, id, 1, &info);
    CHECK_EQ_INT(pr_get(&L, id)->shared, 0);
    CHECK_EQ_INT(notes_of(PR_NOTE_FOUND), 1);
}

static void test_at_no_answer(void)
{
    fresh();
    int id = pr_at_seen(&L, PR_WHERE_CABLE, P("Back Room"), P("LaserWriter"), 0);
    pr_at_queried(&L, id, 0, NULL);
    pr_at_queried(&L, id, 0, NULL);
    CHECK_EQ_INT(notes_of(PR_NOTE_NO_ANSWER), 0);
    pr_at_queried(&L, id, 0, NULL);
    CHECK_EQ_INT(notes_of(PR_NOTE_NO_ANSWER), 1);
    CHECK_EQ_INT(pr_get(&L, id)->shared, 0);
    CHECK_EQ_INT(pr_use_anyway(&L, id), 0);
    const pr_rec_t *r = pr_get(&L, id);
    CHECK_EQ_INT(r->identified, 1);
    CHECK_EQ_INT(r->shared, 1);
    CHECK_EQ_INT(r->lw.page, LW_LETTER);
    CHECK_EQ_INT(r->lw.dpi, 300);
    CHECK_EQ_INT(r->lw.binary, 0);
    CHECK_EQ_INT(notes_of(PR_NOTE_NO_ANSWER), 0);
}

static void test_at_imagewriter(void)
{
    fresh();
    int id = pr_at_seen(&L, PR_WHERE_CABLE, P("Kitchen"), P("ImageWriter"), 0);
    CHECK_EQ_INT(pr_get(&L, id)->kind, PR_KIND_AT_IMAGEWRITER);
    CHECK_EQ_INT(pr_get(&L, id)->identified, 1);
    CHECK_EQ_INT(pr_get(&L, id)->shared, 1);
    CHECK_EQ_INT(notes_of(PR_NOTE_FOUND), 1);
    // Other types are not printers AirWriter drives.
    CHECK_EQ_INT(pr_at_seen(&L, PR_WHERE_CABLE, P("Fred"), P("Workstation"), 0), -1);
}

static void test_link_preference(void)
{
    fresh();
    int id = pr_at_seen(&L, PR_WHERE_TUNNEL, P("Office"), P("LaserWriter"), 0);
    CHECK_EQ_INT(pr_at_seen(&L, PR_WHERE_CABLE, P("office"), P("LaserWriter"), 0), id);
    CHECK_EQ_INT(pr_get(&L, id)->where, PR_WHERE_CABLE);
    CHECK_EQ_INT(pr_at_seen(&L, PR_WHERE_NETWORK, P("Office"), P("LaserWriter"), 0), id);
    CHECK_EQ_INT(pr_get(&L, id)->where, PR_WHERE_CABLE);
    CHECK_EQ_INT(L.n, 1);
}

static void test_skip_airwriter(void)
{
    fresh();
    CHECK_EQ_INT(pr_at_seen(&L, PR_WHERE_NETWORK, P("ImageWriter II"), P("ImageWriter"), 1), -1);
    CHECK_EQ_INT(L.n, 0);
}

static void test_cap(void)
{
    fresh();
    lw_info_t info;
    lw_info_default(&info);
    pr_port_seen(&L, PR_PORT_IW2);
    int id[4];
    const char *names[4] = { "A", "B", "C", "D" };
    for (int i = 0; i < 4; i++) {
        id[i] = pr_at_seen(&L, PR_WHERE_NETWORK, P(names[i]), P("LaserWriter"), 0);
        pr_at_queried(&L, id[i], 1, &info);
    }
    CHECK_EQ_INT(pr_get(&L, id[0])->shared + pr_get(&L, id[1])->shared + pr_get(&L, id[2])->shared, 3);
    CHECK_EQ_INT(pr_get(&L, id[3])->shared, 0);
    CHECK_EQ_INT(notes_of(PR_NOTE_TOO_MANY), 1);
    CHECK_EQ_INT(pr_set_shared(&L, id[3], 1), -1);
    CHECK_EQ_INT(pr_set_shared(&L, id[1], 0), 0);
    CHECK_EQ_INT(pr_set_shared(&L, id[3], 1), 0);
}

static void test_names_unique(void)
{
    fresh();
    int a = pr_port_seen(&L, PR_PORT_IW2);
    int b = pr_at_seen(&L, PR_WHERE_CABLE, P("ImageWriter II"), P("ImageWriter"), 0);
    CHECK(strcmp(pr_get(&L, a)->name, "ImageWriter II") == 0);
    CHECK(strcmp(pr_get(&L, b)->name, "ImageWriter II 2") == 0);
    CHECK_EQ_INT(pr_rename(&L, a, "X"), 0);
    CHECK_EQ_INT(pr_rename(&L, b, "x"), 0);
    CHECK(strcmp(pr_get(&L, b)->name, "x 2") == 0);
    CHECK_EQ_INT(pr_rename(&L, b, ""), 0);                  // empty: back to its own
    CHECK(strcmp(pr_get(&L, b)->name, "ImageWriter II") == 0);
}

static void test_macroman_name(void)
{
    fresh();
    static const uint8_t nbp[] = { 10, 'C', 'a', 'f', 0x8E, ' ', 'L', 'a', 's', 'e', 'r' };
    int id = pr_at_seen(&L, PR_WHERE_CABLE, nbp, P("LaserWriter"), 0);
    CHECK(strcmp(pr_get(&L, id)->name, "Caf\xc3\xa9 Laser") == 0);
    CHECK_EQ_MEM(pr_get(&L, id)->nbp, nbp, sizeof nbp);
}

static void test_remove(void)
{
    fresh();
    int id = pr_at_seen(&L, PR_WHERE_CABLE, P("Kitchen"), P("ImageWriter"), 0);
    CHECK_EQ_INT(pr_remove(&L, id), 0);
    CHECK(pr_get(&L, id) == NULL);
    CHECK_EQ_INT(L.n, 0);
    CHECK_EQ_INT(notes_of(PR_NOTE_FOUND), 0);               // its notes go with it
    int again = pr_at_seen(&L, PR_WHERE_CABLE, P("Kitchen"), P("ImageWriter"), 0);
    CHECK(again != id);
    CHECK_EQ_INT(notes_of(PR_NOTE_FOUND), 1);
}

static void test_offers(void)
{
    fresh();
    lw_info_t info;
    lw_info_default(&info);
    int port = pr_port_seen(&L, PR_PORT_IW2);
    int lw = pr_at_seen(&L, PR_WHERE_CABLE, P("Office"), P("LaserWriter"), 0);
    pr_at_queried(&L, lw, 1, &info);
    int iw = pr_at_seen(&L, PR_WHERE_TUNNEL, P("Kitchen"), P("ImageWriter"), 0);
    int raw = pr_at_seen(&L, PR_WHERE_NETWORK, P("Raw"), P("LaserWriter"), 0);   // unidentified
    int ids[8], n;
    n = pr_offers(&L, PR_WHERE_CABLE, ids, 8);
    CHECK_EQ_INT(n, 2); CHECK_EQ_INT(ids[0], port); CHECK_EQ_INT(ids[1], iw);
    n = pr_offers(&L, PR_WHERE_TUNNEL, ids, 8);
    CHECK_EQ_INT(n, 2); CHECK_EQ_INT(ids[0], port); CHECK_EQ_INT(ids[1], lw);
    n = pr_offers(&L, PR_WHERE_NETWORK, ids, 8);
    CHECK_EQ_INT(n, 3);
    pr_set_shared(&L, lw, 0);                               // AirPrint only
    CHECK_EQ_INT(pr_offers(&L, PR_WHERE_NETWORK, ids, 8), 3);
    for (int i = 0; i < 3; i++) CHECK(ids[i] != raw);
}

static void test_box_state(void)
{
    fresh();
    int a = pr_port_seen(&L, PR_PORT_IW2);
    int b = pr_at_seen(&L, PR_WHERE_CABLE, P("Kitchen"), P("ImageWriter"), 0);
    pr_live_t live[PR_MAX];
    memset(live, 0, sizeof live);
    CHECK_EQ_INT(pr_box_state(&L, live), PR_BOX_OFFLINE);
    live[pr_index(&L, a)].present = live[pr_index(&L, a)].ready = 1;
    CHECK_EQ_INT(pr_box_state(&L, live), PR_BOX_READY);
    live[pr_index(&L, b)].present = 1;
    live[pr_index(&L, b)].attention = 1;
    CHECK_EQ_INT(pr_box_state(&L, live), PR_BOX_ATTENTION);
    live[pr_index(&L, a)].failed = 1;
    CHECK_EQ_INT(pr_box_state(&L, live), PR_BOX_FAILED);
    CHECK_EQ_INT(pr_test_target(&L, live), a);
    pr_remove(&L, a);
    CHECK_EQ_INT(pr_test_target(&L, live), b);
}

static pr_legacy_t legacy(int airprint)
{
    static lw_info_t info;
    lw_info_default(&info);
    strcpy(info.product, "LaserWriter II NT");
    info.binary = 1;
    pr_legacy_t o = {
        .model = "ImageWriter II", .name = "", .airprint = airprint, .dither = "atkinson",
        .gain = 0, .top_margin = -126, .ribbon = 2,
        .uuid = "urn:uuid:41575254-4552-0001-10bd-a36e2434",
        .laser_shared = 1, .laser_nbp = P("Office"), .laser_link = 1 /* AT_LINK_TUNNEL */,
        .laser_info = &info, .laser_dither = "bluenoise", .laser_gain = 10,
    };
    return o;
}

static void test_migrate(void)
{
    fresh();
    pr_legacy_t o = legacy(1);
    pr_migrate(&L, &o);
    CHECK_EQ_INT(L.n, 2);
    CHECK_EQ_INT(L.n_notes, 0);
    const pr_rec_t *p = pr_port(&L);
    CHECK(strcmp(p->name, "ImageWriter II") == 0);
    CHECK(strcmp(p->uuid, "urn:uuid:41575254-4552-0001-10bd-a36e2434") == 0);
    CHECK_EQ_INT(p->found_auto, 0);
    CHECK_EQ_INT(p->shared, 1);
    CHECK_EQ_INT(p->top_margin, -126);
    CHECK_EQ_INT(p->ribbon, 2);
    const pr_rec_t *lw = NULL;
    for (int i = 0; i < L.n; i++) if (L.r[i].kind == PR_KIND_LASERWRITER) lw = &L.r[i];
    CHECK(lw != NULL);
    CHECK_EQ_INT(lw->ipp_laser, 1);
    CHECK_EQ_INT(lw->shared, 1);
    CHECK_EQ_INT(lw->where, PR_WHERE_TUNNEL);
    CHECK_EQ_INT(lw->lw.binary, 1);
    CHECK(strcmp(lw->dither, "bluenoise") == 0);
    char want[46];
    lw_uuid(want, MAC, 1, P("Office"));
    CHECK(strcmp(lw->uuid, want) == 0);
    // A named printer keeps its name.
    fresh();
    o.name = "Den";
    pr_migrate(&L, &o);
    CHECK(strcmp(pr_port(&L)->name, "Den") == 0);
}

static void test_migrate_port_off(void)
{
    fresh();
    pr_legacy_t o = legacy(0);
    o.laser_shared = 0;
    pr_migrate(&L, &o);
    CHECK_EQ_INT(L.n, 1);
    int id = pr_port(&L)->id;
    CHECK_EQ_INT(pr_port(&L)->shared, 0);
    int ids[8];
    CHECK_EQ_INT(pr_offers(&L, PR_WHERE_CABLE, ids, 8), 1);    // still in the Chooser
    CHECK_EQ_INT(pr_port_seen(&L, PR_PORT_IW2), id);
    CHECK_EQ_INT(L.n_notes, 0);
    CHECK_EQ_INT(pr_port_seen(&L, PR_PORT_NOTHING), id);
    CHECK_EQ_INT(pr_port_is_imagewriter(&L), 1);
}

static void test_save_load(void)
{
    fresh();
    lw_info_t info;
    lw_info_default(&info);
    pr_port_seen(&L, PR_PORT_IW2);
    int lw = pr_at_seen(&L, PR_WHERE_CABLE, P("Office"), P("LaserWriter"), 0);
    pr_at_queried(&L, lw, 1, &info);
    pr_rename(&L, lw, "Front");
    static uint8_t buf[4096];
    size_t n = pr_save(&L, buf, sizeof buf);
    CHECK(n > 0);
    pr_list_t M;
    pr_init(&M, MAC, 1);
    CHECK_EQ_INT(pr_load(&M, buf, n), 0);
    CHECK_EQ_INT(M.n, L.n);
    CHECK_EQ_INT(M.next_id, L.next_id);
    CHECK_EQ_INT(M.n_notes, L.n_notes);
    CHECK(memcmp(M.r, L.r, sizeof L.r[0] * L.n) == 0);
    CHECK_EQ_INT(pr_save(&L, buf, 10), 0);                  // too small: nothing
}

static void test_load_garbage(void)
{
    fresh();
    pr_port_seen(&L, PR_PORT_IW2);
    static uint8_t buf[4096];
    size_t n = pr_save(&L, buf, sizeof buf);
    pr_list_t M;
    pr_init(&M, MAC, 1);
    pr_port_seen(&M, PR_PORT_IWLQ);                         // what must survive a refusal
    pr_list_t keep = M;
    CHECK_EQ_INT(pr_load(&M, buf, 0), -1);
    CHECK_EQ_INT(pr_load(&M, buf, 5), -1);
    CHECK_EQ_INT(pr_load(&M, buf, n - 1), -1);
    uint8_t bad[4096];
    memcpy(bad, buf, n);
    bad[2] = 99;                                            // the version
    CHECK_EQ_INT(pr_load(&M, bad, n), -1);
    memcpy(bad, buf, n);
    bad[3] = PR_MAX + 1;                                    // the count
    CHECK_EQ_INT(pr_load(&M, bad, n), -1);
    CHECK(memcmp(&M, &keep, sizeof M) == 0);
}

static void test_json(void)
{
    fresh();
    int a = pr_port_seen(&L, PR_PORT_IW2);
    pr_rename(&L, a, "Wells' \"Desk\"");
    pr_at_seen(&L, PR_WHERE_CABLE, P("Kitchen"), P("ImageWriter"), 0);
    pr_live_t live[PR_MAX];
    memset(live, 0, sizeof live);
    live[0].present = live[0].ready = 1;
    static char buf[4096];
    size_t n = pr_json(&L, live, buf, sizeof buf);
    CHECK(n > 0 && n < sizeof buf);
    CHECK(strstr(buf, "\"name\":\"Wells' \\\"Desk\\\"\"") != NULL);
    CHECK(strstr(buf, "\"where\":\"port\"") != NULL);
    CHECK(strstr(buf, "\"where\":\"cable\"") != NULL);
    CHECK(strstr(buf, "\"kind\":\"appletalk-imagewriter\"") != NULL);
    CHECK(strstr(buf, "\"state\":\"ready\"") != NULL);
    CHECK(strstr(buf, "\"state\":\"offline\"") != NULL);
    CHECK(strstr(buf, "\"notes\":[") != NULL);
    CHECK(strstr(buf, "\"max_shared\":4") != NULL);
}

// A board's first boot on W-1088 fixes its host name and AppleTalk port for
// good: one already set up (Wi-Fi saved) keeps imagewriter-xxxx and its DIP
// rule; a new one is airwriter-xxxx with the AppleTalk port on.
static void test_defaults(void)
{
    CHECK(strcmp(pr_default_host_fmt(1), "imagewriter-%s") == 0);
    CHECK(strcmp(pr_default_host_fmt(0), "airwriter-%s") == 0);
    CHECK_EQ_INT(pr_default_lt_port(1), -1);
    CHECK_EQ_INT(pr_default_lt_port(0), 1);
}

// What the Printers section sends, flat JSON: {"id":N, ...one action...}.
static void test_post(void)
{
    fresh();
    lw_info_t info;
    lw_info_default(&info);
    int port = pr_port_seen(&L, PR_PORT_IW2);
    int lw = pr_at_seen(&L, PR_WHERE_CABLE, P("Office"), P("LaserWriter"), 0);
    char body[160];
    pr_post_t r;

    snprintf(body, sizeof body, "{\"id\":%d,\"name\":\"Den \\\"Two\\\"\"}", port);
    CHECK_EQ_INT(pr_post(&L, body, &r), 0);
    CHECK(strcmp(pr_get(&L, port)->name, "Den \"Two\"") == 0);
    CHECK_EQ_INT(r.changed, 1);

    snprintf(body, sizeof body, "{\"id\":%d,\"shared\":true}", lw);   // unidentified yet
    CHECK_EQ_INT(pr_post(&L, body, &r), -1);
    CHECK(strstr(r.err, "can't be shared") != NULL);
    snprintf(body, sizeof body, "{\"id\":%d,\"use_anyway\":true}", lw);
    CHECK_EQ_INT(pr_post(&L, body, &r), -1);                 // only after it didn't answer
    pr_at_queried(&L, lw, 0, NULL); pr_at_queried(&L, lw, 0, NULL); pr_at_queried(&L, lw, 0, NULL);
    CHECK_EQ_INT(pr_post(&L, body, &r), 0);
    CHECK_EQ_INT(pr_get(&L, lw)->shared, 1);
    snprintf(body, sizeof body, "{\"id\":%d,\"shared\":false}", lw);
    CHECK_EQ_INT(pr_post(&L, body, &r), 0);
    CHECK_EQ_INT(pr_get(&L, lw)->shared, 0);

    snprintf(body, sizeof body, "{\"id\":%d,\"model\":\"ImageWriter LQ\"}", port);
    CHECK_EQ_INT(pr_post(&L, body, &r), 0);
    CHECK_EQ_INT(pr_get(&L, port)->model, PR_MODEL_IWLQ);
    snprintf(body, sizeof body, "{\"id\":%d,\"model\":\"none\"}", port);
    CHECK_EQ_INT(pr_post(&L, body, &r), 0);
    CHECK_EQ_INT(pr_get(&L, port)->kind, PR_KIND_UNKNOWN);
    snprintf(body, sizeof body, "{\"id\":%d,\"model\":\"StyleWriter\"}", port);
    CHECK_EQ_INT(pr_post(&L, body, &r), -1);

    snprintf(body, sizeof body, "{\"id\":%d,\"dither\":\"floyd\",\"gain\":20}", lw);
    CHECK_EQ_INT(pr_post(&L, body, &r), 0);
    CHECK(strcmp(pr_get(&L, lw)->dither, "floyd") == 0);
    CHECK_EQ_INT(pr_get(&L, lw)->gain, 20);
    CHECK_EQ_INT(r.screen_id, lw);
    snprintf(body, sizeof body, "{\"id\":%d,\"dither\":\"sparkle\"}", lw);
    CHECK_EQ_INT(pr_post(&L, body, &r), -1);
    snprintf(body, sizeof body, "{\"id\":%d,\"gain\":46}", lw);
    CHECK_EQ_INT(pr_post(&L, body, &r), -1);

    CHECK_EQ_INT(pr_post(&L, "{\"look\":true}", &r), 0);
    CHECK_EQ_INT(r.look, 1);
    CHECK_EQ_INT(r.changed, 0);

    pr_port_seen(&L, PR_PORT_NOTHING);
    int found = L.n_notes;                                    // a FOUND note for Office
    CHECK(found > 0);
    snprintf(body, sizeof body, "{\"note\":%d,\"dismiss\":true}", found - 1);
    CHECK_EQ_INT(pr_post(&L, body, &r), 0);
    CHECK_EQ_INT(L.n_notes, found - 1);

    snprintf(body, sizeof body, "{\"id\":%d,\"remove\":true}", lw);
    CHECK_EQ_INT(pr_post(&L, body, &r), 0);
    CHECK(pr_get(&L, lw) == NULL);
    CHECK_EQ_INT(pr_post(&L, "{\"id\":99,\"remove\":true}", &r), -1);
    CHECK(strstr(r.err, "No such printer") != NULL);
    CHECK_EQ_INT(pr_post(&L, "not json", &r), -1);
}

int main(void)
{
    test_defaults();
    test_post();
    test_port_identified();
    test_port_silent();
    test_port_changed();
    test_port_nothing();
    test_at_laserwriter();
    test_at_no_answer();
    test_at_imagewriter();
    test_link_preference();
    test_skip_airwriter();
    test_cap();
    test_names_unique();
    test_macroman_name();
    test_remove();
    test_offers();
    test_box_state();
    test_migrate();
    test_migrate_port_off();
    test_save_load();
    test_load_garbage();
    test_json();
    TEST_END;
}
