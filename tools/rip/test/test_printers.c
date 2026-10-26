// SPDX-License-Identifier: GPL-3.0-or-later
//
// The printer list (W-1088): what AirWriter offers over AirPrint and to old
// Macs is exactly the printers a person has, found or told. Naming, identity,
// sharing and its cap, notices, migration from a board before W-1088, and the
// stored form.
#include "test_util.h"
#include "printers_core.h"
#include "lw_caps.h"
#include "sw_model.h"
#include <stddef.h>
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
    CHECK_EQ_INT(L.n_notes, 0);                             // it just appears in the list
    CHECK_EQ_INT(pr_port_seen(&L, PR_PORT_IW2), id);
    CHECK_EQ_INT(L.n_notes, 0);
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
    pr_accept_change(&L, 0);                                // notes: CHANGED
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
    lw_info_t info;
    lw_info_default(&info);
    strcpy(info.product, "LaserWriter II NT");
    pr_at_queried(&L, id, 1, &info);
    const pr_rec_t *r = pr_get(&L, id);
    CHECK_EQ_INT(r->identified, 1);
    CHECK_EQ_INT(r->shared, 1);
    CHECK(strcmp(r->lw.product, "LaserWriter II NT") == 0);
    CHECK(strcmp(r->name, "Office") == 0);
    CHECK_EQ_INT(L.n_notes, 0);
    char want[46];
    lw_uuid(want, MAC, 1, P("Office"));
    CHECK(strcmp(r->uuid, want) == 0);
    // Asked again later: no re-share after the person unshared.
    pr_set_shared(&L, id, 0);
    pr_at_queried(&L, id, 1, &info);
    CHECK_EQ_INT(pr_get(&L, id)->shared, 0);
    CHECK_EQ_INT(L.n_notes, 0);
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
    // W-1088's AirPrint to an AppleTalk ImageWriter was cut: it is old Macs'
    // (relayed), never shared over AirPrint, and nothing says it is.
    CHECK_EQ_INT(pr_get(&L, id)->shared, 0);
    CHECK_EQ_INT(L.n_notes, 0);
    CHECK_EQ_INT(pr_set_shared(&L, id, 1), -1);
    pr_post_t r;
    char body[64];
    snprintf(body, sizeof body, "{\"id\":%d,\"shared\":true}", id);
    CHECK_EQ_INT(pr_post(&L, body, &r), -1);
    CHECK(strstr(r.err, "can’t be shared with phones") != NULL);
    int ids[PR_MAX];
    CHECK_EQ_INT(pr_offers(&L, PR_WHERE_TUNNEL, ids, PR_MAX), 1);   // old Macs elsewhere see it
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
    lw_info_t info;
    lw_info_default(&info);
    int id = pr_at_seen(&L, PR_WHERE_CABLE, P("Kitchen"), P("LaserWriter"), 0);
    for (int i = 0; i < 3; i++) pr_at_queried(&L, id, 0, NULL);
    CHECK_EQ_INT(notes_of(PR_NOTE_NO_ANSWER), 1);
    CHECK_EQ_INT(pr_remove(&L, id), 0);
    CHECK(pr_get(&L, id) == NULL);
    CHECK_EQ_INT(L.n, 0);
    CHECK_EQ_INT(L.n_notes, 0);                             // its notes go with it
    int again = pr_at_seen(&L, PR_WHERE_CABLE, P("Kitchen"), P("LaserWriter"), 0);
    CHECK(again != id);
    pr_at_queried(&L, again, 1, &info);
    CHECK_EQ_INT(pr_get(&L, again)->shared, 1);             // found again: shared again
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
    int b = pr_at_seen(&L, PR_WHERE_CABLE, P("Kitchen"), P("LaserWriter"), 0);
    lw_info_t info;
    lw_info_default(&info);
    pr_at_queried(&L, b, 1, &info);
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

    // A board from before keeps "found" notices; they go on load, the rest stay.
    L.notes[0] = (pr_note_t){ PR_NOTE_FOUND, (uint8_t)lw, 0 };
    L.notes[1] = (pr_note_t){ PR_NOTE_TOO_MANY, (uint8_t)lw, 0 };
    L.notes[2] = (pr_note_t){ PR_NOTE_FOUND, 1, 0 };
    L.n_notes = 3;
    n = pr_save(&L, buf, sizeof buf);
    CHECK_EQ_INT(pr_load(&M, buf, n), 0);
    CHECK_EQ_INT(M.n_notes, 1);
    CHECK_EQ_INT(M.notes[0].kind, PR_NOTE_TOO_MANY);
    CHECK_EQ_INT(M.notes[0].id, lw);
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

// A list stored by a newer firmware (a later stored form) is one this
// firmware must not write over: an OTA rollback or the known-good reset brings
// the board back here, and the newer firmware wants its printers back (review I1).
static void test_load_newer(void)
{
    fresh();
    pr_port_seen(&L, PR_PORT_IW2);
    static uint8_t buf[4096];
    size_t n = pr_save(&L, buf, sizeof buf);
    CHECK_EQ_INT(pr_blob_newer(buf, n), 0);                 // ours
    uint8_t bad[4096];
    memcpy(bad, buf, n);
    bad[2] = PR_BLOB_VER + 1;
    pr_list_t M;
    pr_init(&M, MAC, 1);
    CHECK_EQ_INT(pr_load(&M, bad, n), -1);                  // unreadable here
    CHECK_EQ_INT(pr_blob_newer(bad, n), 1);                 // and kept
    CHECK_EQ_INT(pr_blob_newer(bad, 3), 1);                 // the header is enough
    CHECK_EQ_INT(pr_blob_newer(buf, n - 1), 0);             // ours, damaged: replace it
    CHECK_EQ_INT(pr_blob_newer(bad, 2), 0);
    bad[0] = 'X';
    CHECK_EQ_INT(pr_blob_newer(bad, n), 0);                 // not a printer list
    CHECK_EQ_INT(pr_blob_newer(NULL, 0), 0);
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

    // W-1088: each shared printer's AirPrint path, for the AirPrint pane.
    CHECK(strstr(buf, "\"rp\":\"ipp/print\"") != NULL);
    int lw = pr_at_seen(&L, PR_WHERE_TUNNEL, P("Office"), P("LaserWriter"), 0);
    lw_info_t info;
    lw_info_default(&info);
    pr_at_queried(&L, lw, 1, &info);
    char want[32];
    snprintf(want, sizeof want, "\"rp\":\"ipp/p%d\"", lw);
    n = pr_json(&L, live, buf, sizeof buf);
    CHECK(strstr(buf, want) != NULL);
    CHECK(strstr(buf, "\"start_page\":true") == NULL);  // W-1099: it didn't say
    CHECK(strstr(buf, "\"start_page\":false") == NULL);
    CHECK(strstr(buf, "\"start_page\":null") != NULL);
    info.start_page = LW_START_OFF;
    pr_at_queried(&L, lw, 1, &info);
    n = pr_json(&L, live, buf, sizeof buf);
    CHECK(strstr(buf, "\"start_page\":false") != NULL);
    pr_get(&L, lw)->ipp_laser = 1;                    // W-1082's keeps its path
    n = pr_json(&L, live, buf, sizeof buf);
    CHECK(strstr(buf, "\"rp\":\"ipp/laser\"") != NULL);
    pr_set_shared(&L, a, 0);
    n = pr_json(&L, live, buf, sizeof buf);
    CHECK(strstr(buf, "\"rp\":\"ipp/print\"") == NULL);
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
    CHECK(strstr(r.err, "until it responds") != NULL);
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
    snprintf(body, sizeof body, "{\"id\":%d,\"model\":\"Daisywheel\"}", port);
    CHECK_EQ_INT(pr_post(&L, body, &r), -1);
    snprintf(body, sizeof body, "{\"id\":%d,\"model\":\"StyleWriter\"}", port);   // W-1103
    CHECK_EQ_INT(pr_post(&L, body, &r), 0);
    CHECK_EQ_INT(pr_get(&L, port)->kind, PR_KIND_STYLEWRITER);
    CHECK_EQ_INT(pr_get(&L, port)->model, PR_MODEL_SW1);
    CHECK_EQ_INT(r.confirm_sw_id, port);

    snprintf(body, sizeof body, "{\"id\":%d,\"dither\":\"floyd\",\"gain\":20}", lw);
    CHECK_EQ_INT(pr_post(&L, body, &r), 0);
    CHECK(strcmp(pr_get(&L, lw)->dither, "floyd") == 0);
    CHECK_EQ_INT(pr_get(&L, lw)->gain, 20);
    CHECK_EQ_INT(r.screen_id, lw);
    snprintf(body, sizeof body, "{\"id\":%d,\"dither\":\"sparkle\"}", lw);
    CHECK_EQ_INT(pr_post(&L, body, &r), -1);
    snprintf(body, sizeof body, "{\"id\":%d,\"gain\":46}", lw);
    CHECK_EQ_INT(pr_post(&L, body, &r), -1);

    // W-1099: the start page is the printer's own: asked for here, set by a
    // job to it. Only for a LaserWriter that said whether it prints one.
    snprintf(body, sizeof body, "{\"id\":%d,\"start_page\":false}", lw);
    CHECK_EQ_INT(pr_post(&L, body, &r), -1);
    CHECK(strstr(r.err, "start page") != NULL);
    snprintf(body, sizeof body, "{\"id\":%d,\"start_page\":false}", port);
    CHECK_EQ_INT(pr_post(&L, body, &r), -1);
    info.start_page = LW_START_ON;
    pr_at_queried(&L, lw, 1, &info);
    snprintf(body, sizeof body, "{\"id\":%d,\"start_page\":false}", lw);
    CHECK_EQ_INT(pr_post(&L, body, &r), 0);
    CHECK_EQ_INT(r.start_page, 0);
    CHECK_EQ_INT(r.start_page_id, lw);
    CHECK_EQ_INT(r.changed, 0);
    CHECK_EQ_INT(pr_get(&L, lw)->lw.start_page, LW_START_ON);   // until the printer says
    snprintf(body, sizeof body, "{\"id\":%d,\"start_page\":true}", lw);
    CHECK_EQ_INT(pr_post(&L, body, &r), 0);
    CHECK_EQ_INT(r.start_page, 1);
    snprintf(body, sizeof body, "{\"id\":%d,\"name\":\"Office\"}", lw);
    CHECK_EQ_INT(pr_post(&L, body, &r), 0);
    CHECK_EQ_INT(r.start_page, -1);

    CHECK_EQ_INT(pr_post(&L, "{\"look\":true}", &r), 0);
    CHECK_EQ_INT(r.look, 1);
    CHECK_EQ_INT(r.changed, 0);

    int back = pr_at_seen(&L, PR_WHERE_CABLE, P("Back"), P("LaserWriter"), 0);
    for (int i = 0; i < 3; i++) pr_at_queried(&L, back, 0, NULL);    // a NO_ANSWER note
    int notes = L.n_notes;
    CHECK(notes > 0);
    snprintf(body, sizeof body, "{\"note\":%d,\"dismiss\":true}", notes - 1);
    CHECK_EQ_INT(pr_post(&L, body, &r), 0);
    CHECK_EQ_INT(L.n_notes, notes - 1);

    snprintf(body, sizeof body, "{\"id\":%d,\"remove\":true}", lw);
    CHECK_EQ_INT(pr_post(&L, body, &r), 0);
    CHECK(pr_get(&L, lw) == NULL);
    CHECK_EQ_INT(pr_post(&L, "{\"id\":99,\"remove\":true}", &r), -1);
    CHECK(strstr(r.err, "No such printer") != NULL);
    CHECK_EQ_INT(pr_post(&L, "not json", &r), -1);
}

// "Add a Printer…" for the port (W-1088 setup): an original ImageWriter, or
// one that is off, never answers by itself. With nothing there it makes the
// port's record, the person's, shared; with something there it sets its model.
static void test_add_port(void)
{
    fresh();
    pr_post_t r;
    CHECK_EQ_INT(pr_post(&L, "{\"add_port\":\"ImageWriter\"}", &r), 0);
    CHECK_EQ_INT(r.changed, 1);
    const pr_rec_t *p = pr_port(&L);
    CHECK(p != NULL);
    CHECK_EQ_INT(p->kind, PR_KIND_IMAGEWRITER);
    CHECK_EQ_INT(p->model, PR_MODEL_IW1);
    CHECK_EQ_INT(p->shared, 1);
    CHECK_EQ_INT(p->found_auto, 0);
    CHECK(strcmp(p->name, "ImageWriter") == 0);
    CHECK(strlen(p->uuid) == 45);
    CHECK_EQ_INT(L.n, 1);
    // Again, with the port's record there: its model, not a second record.
    CHECK_EQ_INT(pr_post(&L, "{\"add_port\":\"ImageWriter LQ\"}", &r), 0);
    CHECK_EQ_INT(L.n, 1);
    CHECK_EQ_INT(pr_port(&L)->model, PR_MODEL_IWLQ);
    CHECK_EQ_INT(pr_post(&L, "{\"add_port\":\"Daisywheel\"}", &r), -1);
    // It then answers ESC ? as an LQ: no churn.
    pr_port_seen(&L, PR_PORT_IWLQ);
    CHECK_EQ_INT(L.n_notes, 0);
}

// W-1099: the start page rides in the record's lw_info_t. Every answered
// query asks the printer again and brings it, so the record follows the
// printer; a query that fails changes nothing; the stored list keeps it.
static void test_start_page_kept(void)
{
    fresh();
    int lw = pr_at_seen(&L, PR_WHERE_CABLE, P("NTR"), P("LaserWriter"), 0);
    lw_parse_t q;
    lw_parse_init(&q);
    const char *a = "aw:product=LaserWriter Personal NTR\raw:startpage=false\raw:probe=256 1\raw:end\r";
    lw_parse_feed(&q, (const uint8_t *)a, strlen(a));
    pr_at_queried(&L, lw, 1, &q.info);
    CHECK_EQ_INT(pr_get(&L, lw)->lw.start_page, LW_START_OFF);
    lw_parse_init(&q);                                      // switched on again: asked again
    a = "aw:product=LaserWriter Personal NTR\raw:startpage=true\raw:probe=256 1\raw:end\r";
    lw_parse_feed(&q, (const uint8_t *)a, strlen(a));
    pr_at_queried(&L, lw, 1, &q.info);
    CHECK_EQ_INT(pr_get(&L, lw)->lw.start_page, LW_START_ON);
    pr_at_queried(&L, lw, 0, NULL);                         // no answer: as it was
    CHECK_EQ_INT(pr_get(&L, lw)->lw.start_page, LW_START_ON);
    static uint8_t buf[4096];
    size_t n = pr_save(&L, buf, sizeof buf);
    pr_list_t M;
    pr_init(&M, MAC, 1);
    CHECK_EQ_INT(pr_load(&M, buf, n), 0);
    CHECK_EQ_INT(pr_get(&M, lw)->lw.start_page, LW_START_ON);
}

// W-1098: a LaserWriter's query answered on `where` at node, with its page
// count; the id the answers went to.
static int lw_answers(int id, pr_where_t where, uint8_t node, int32_t pages, const char *product,
                      const pr_live_t *live)
{
    lw_info_t info;
    lw_info_default(&info);
    strcpy(info.product, product);
    static const pr_live_t none[PR_MAX];
    int got = pr_at_answered(&L, id, where, 0, node, pages, &info, live ? live : none);
    pr_at_queried(&L, got, 1, &info);
    return got;
}

static const char NTR[] = "LaserWriter Personal NTR";

// A LaserWriter renamed (LaserWriter Utility, or a new owner) comes back at
// the address it had under its new name. Its record goes on -- its uuid, its
// AirPrint printer, its settings -- under the new name, and no printer is
// found.
static void test_at_renamed(void)
{
    fresh();
    int a = pr_at_seen(&L, PR_WHERE_CABLE, P("Personal LaserWriter NTR"), P("LaserWriter"), 0);
    CHECK_EQ_INT(lw_answers(a, PR_WHERE_CABLE, 233, 10287, NTR, NULL), a);
    pr_get(&L, a)->gain = 20;
    char uuid[46];
    strcpy(uuid, pr_get(&L, a)->uuid);
    int notes = L.n_notes;

    int b = pr_at_seen(&L, PR_WHERE_CABLE, P("Wells's LaserWriter"), P("LaserWriter"), 0);
    CHECK(b >= 0 && b != a);
    CHECK_EQ_INT(L.n, 2);
    CHECK_EQ_INT(lw_answers(b, PR_WHERE_CABLE, 233, 10290, NTR, NULL), a);
    CHECK_EQ_INT(L.n, 1);
    CHECK(pr_get(&L, b) == NULL);
    const pr_rec_t *r = pr_get(&L, a);
    CHECK(r->nbp[0] == 19 && !memcmp(r->nbp + 1, "Wells's LaserWriter", 19));
    CHECK(strcmp(r->name, "Wells's LaserWriter") == 0);   // its own name follows
    CHECK(strcmp(r->uuid, uuid) == 0);
    CHECK_EQ_INT(r->shared, 1);
    CHECK_EQ_INT(r->gain, 20);
    CHECK_EQ_INT(L.n_notes, notes);                        // nothing found
    const pr_seen_t *s = pr_seen_of(&L, a);
    CHECK(s && s->pages == 10290 && s->node == 233 && s->where == PR_WHERE_CABLE);
    CHECK(pr_seen_of(&L, b) == NULL);

    // Renamed again, and its count not given this time: the last one stands.
    int c = pr_at_seen(&L, PR_WHERE_CABLE, P("Den"), P("LaserWriter"), 0);
    CHECK_EQ_INT(lw_answers(c, PR_WHERE_CABLE, 233, 10301, NTR, NULL), a);
    CHECK_EQ_INT(lw_answers(a, PR_WHERE_CABLE, 233, -1, NTR, NULL), a);
    CHECK_EQ_INT(pr_seen_of(&L, a)->pages, 10301);
    CHECK(strcmp(pr_get(&L, a)->name, "Den") == 0);
}

// A name the person gave it stays.
static void test_at_renamed_named(void)
{
    fresh();
    int a = pr_at_seen(&L, PR_WHERE_CABLE, P("Personal LaserWriter NTR"), P("LaserWriter"), 0);
    lw_answers(a, PR_WHERE_CABLE, 233, 10287, NTR, NULL);
    CHECK_EQ_INT(pr_rename(&L, a, "Front Office"), 0);
    int b = pr_at_seen(&L, PR_WHERE_CABLE, P("Wells's LaserWriter"), P("LaserWriter"), 0);
    CHECK_EQ_INT(lw_answers(b, PR_WHERE_CABLE, 233, 10290, NTR, NULL), a);
    CHECK(strcmp(pr_get(&L, a)->name, "Front Office") == 0);
    CHECK(pr_get(&L, a)->nbp[0] == 19);
}

// Not a rename: fewer pages, another product, link or address, the old name
// still answering, a count not given now or then, or a printer the list
// already had.
static void test_at_not_renamed(void)
{
    static const struct {
        pr_where_t where;
        uint8_t node;
        int32_t pages, old_pages;
        const char *product;
        int live;
    } cases[] = {
        { PR_WHERE_CABLE, 233, 10000, 10287, NTR, 0 },
        { PR_WHERE_CABLE, 233, 10290, 10287, "LaserWriter II NT", 0 },
        { PR_WHERE_NETWORK, 233, 10290, 10287, NTR, 0 },
        { PR_WHERE_CABLE, 234, 10290, 10287, NTR, 0 },
        { PR_WHERE_CABLE, 233, 10290, 10287, NTR, 1 },
        { PR_WHERE_CABLE, 233, -1, 10287, NTR, 0 },
        { PR_WHERE_CABLE, 233, 10290, -1, NTR, 0 },
    };
    for (size_t k = 0; k < sizeof cases / sizeof cases[0]; k++) {
        fresh();
        int a = pr_at_seen(&L, PR_WHERE_CABLE, P("Personal LaserWriter NTR"), P("LaserWriter"), 0);
        lw_answers(a, PR_WHERE_CABLE, 233, cases[k].old_pages, NTR, NULL);
        int b = pr_at_seen(&L, cases[k].where, P("Wells's LaserWriter"), P("LaserWriter"), 0);
        pr_live_t live[PR_MAX] = { 0 };
        if (cases[k].live) live[pr_index(&L, a)].present = 1;
        if (lw_answers(b, cases[k].where, cases[k].node, cases[k].pages, cases[k].product, live) != b)
            fprintf(stderr, "  case %zu taken for a rename\n", k);
        CHECK_EQ_INT(L.n, 2);
        CHECK(pr_get(&L, a)->nbp[0] == 24);
        CHECK_EQ_INT(pr_get(&L, b)->identified, 1);
    }

    fresh();
    int a = pr_at_seen(&L, PR_WHERE_CABLE, P("Personal LaserWriter NTR"), P("LaserWriter"), 0);
    lw_answers(a, PR_WHERE_CABLE, 233, 10287, NTR, NULL);
    int b = pr_at_seen(&L, PR_WHERE_CABLE, P("Office"), P("LaserWriter"), 0);
    lw_answers(b, PR_WHERE_CABLE, 40, 500, NTR, NULL);
    CHECK_EQ_INT(lw_answers(b, PR_WHERE_CABLE, 233, 20000, NTR, NULL), b);   // it moved
    CHECK_EQ_INT(L.n, 2);
}

// The addresses and counts are stored apart from the list (its blob is as
// it was, so a firmware without them reads it); a printer removed takes its
// own with it, and one for a printer not in the list is not read back.
static void test_seen_save_load(void)
{
    fresh();
    int a = pr_at_seen(&L, PR_WHERE_CABLE, P("Personal LaserWriter NTR"), P("LaserWriter"), 0);
    lw_answers(a, PR_WHERE_CABLE, 233, 10287, NTR, NULL);
    int b = pr_at_seen(&L, PR_WHERE_NETWORK, P("Office"), P("LaserWriter"), 0);
    lw_info_t info;
    lw_info_default(&info);
    static const pr_live_t none[PR_MAX];
    pr_at_answered(&L, b, PR_WHERE_NETWORK, 0xFF2A, 12, 500, &info, none);
    pr_at_queried(&L, b, 1, &info);

    static uint8_t list[4096], seen[256], bare[4096];
    size_t ln = pr_save(&L, list, sizeof list);
    size_t sn = pr_seen_save(&L, seen, sizeof seen);
    CHECK(ln > 0 && sn > 0);
    pr_list_t M = L;
    memset(M.seen, 0, sizeof M.seen);
    CHECK_EQ_INT(pr_save(&M, bare, sizeof bare), ln);     // the list's blob is the same
    CHECK(!memcmp(list, bare, ln));

    pr_init(&M, MAC, 1);
    CHECK_EQ_INT(pr_load(&M, list, ln), 0);
    CHECK_EQ_INT(pr_seen_load(&M, seen, sn), 0);
    const pr_seen_t *s = pr_seen_of(&M, b);
    CHECK(s && s->where == PR_WHERE_NETWORK && s->net == 0xFF2A && s->node == 12 && s->pages == 500);
    CHECK(pr_seen_of(&M, a) && pr_seen_of(&M, a)->pages == 10287);

    CHECK_EQ_INT(pr_remove(&M, a), 0);
    CHECK(pr_seen_of(&M, a) == NULL);
    pr_list_t N;
    pr_init(&N, MAC, 1);
    CHECK_EQ_INT(pr_load(&N, list, ln), 0);
    CHECK_EQ_INT(pr_remove(&N, b), 0);
    CHECK_EQ_INT(pr_seen_load(&N, seen, sn), 0);
    CHECK(pr_seen_of(&N, b) == NULL);                      // not in the list
    CHECK(pr_seen_of(&N, a) != NULL);

    CHECK_EQ_INT(pr_seen_load(&N, (const uint8_t *)"PS", 2), -1);
    seen[2] = 99;                                          // a newer form
    CHECK_EQ_INT(pr_seen_load(&N, seen, sn), -1);
    CHECK_EQ_INT(pr_seen_save(&L, seen, 4), 0);            // too small: nothing
}

// ------------------------------------------------------------- W-1103: StyleWriters

static const pr_model_t SW_ALL[] = { PR_MODEL_SW1, PR_MODEL_SW2, PR_MODEL_SW1200, PR_MODEL_SWC1500,
                                     PR_MODEL_SWC2200, PR_MODEL_SWC2400, PR_MODEL_SWC2500 };
static const char *const SW_NAMES[] = { "StyleWriter", "StyleWriter II", "StyleWriter 1200",
                                        "Color StyleWriter 1500", "Color StyleWriter 2200",
                                        "Color StyleWriter 2400", "Color StyleWriter 2500" };
static const sw_model_t SW_TO[] = { SW_ORIGINAL, SW_II, SW_1200, SW_C1500, SW_C2200, SW_C2400,
                                    SW_C2500 };

// The seven models are appended after the ImageWriters' (stored numbers keep
// theirs), named as sw_model names them, and map both ways to sw_model_t.
static void test_sw_models(void)
{
    CHECK_EQ_INT(PR_KIND_STYLEWRITER, 4);                   // appended: stored kinds keep theirs
    CHECK_EQ_INT(PR_MODEL_IW1, 3);
    CHECK_EQ_INT(PR_MODEL_SW1, 4);
    CHECK_EQ_INT(PR_MODEL_SWC2500, 10);
    CHECK(strcmp(pr_kind_name(PR_KIND_STYLEWRITER), "stylewriter") == 0);
    for (size_t i = 0; i < sizeof SW_ALL / sizeof SW_ALL[0]; i++) {
        CHECK(strcmp(pr_model_name(SW_ALL[i]), SW_NAMES[i]) == 0);
        CHECK(strcmp(pr_model_name(SW_ALL[i]), sw_spec(SW_TO[i])->name) == 0);
        CHECK_EQ_INT(pr_model_to_sw(SW_ALL[i]), SW_TO[i]);
        CHECK_EQ_INT(pr_model_from_sw(SW_TO[i]), SW_ALL[i]);
    }
    CHECK_EQ_INT(pr_model_to_sw(PR_MODEL_NONE), SW_NONE);
    CHECK_EQ_INT(pr_model_to_sw(PR_MODEL_IW2), SW_NONE);
    CHECK_EQ_INT(pr_model_to_sw((pr_model_t)(PR_MODEL_SWC2500 + 1)), SW_NONE);
    CHECK_EQ_INT(pr_model_from_sw(SW_NONE), PR_MODEL_NONE);
    CHECK(strcmp(pr_model_name(PR_MODEL_IW2), "ImageWriter II") == 0);   // as it was
    CHECK(strcmp(pr_model_name(PR_MODEL_SWC2500 + 1), "") == 0);
}

// A StyleWriter that answers on an empty port: a shared record named by its model.
static void test_sw_port_found(void)
{
    fresh();
    pr_port_seen_sw(&L, PR_MODEL_SW2);
    const pr_rec_t *r = pr_port(&L);
    CHECK(r != NULL);
    CHECK_EQ_INT(L.n, 1);
    CHECK_EQ_INT(r->where, PR_WHERE_PORT);
    CHECK_EQ_INT(r->kind, PR_KIND_STYLEWRITER);
    CHECK_EQ_INT(r->model, PR_MODEL_SW2);
    CHECK(strcmp(r->name, "StyleWriter II") == 0);
    CHECK_EQ_INT(r->shared, 1);
    CHECK_EQ_INT(r->identified, 1);
    CHECK_EQ_INT(r->found_auto, 1);
    CHECK_EQ_INT(strlen(r->uuid), 45);
    CHECK_EQ_INT(L.n_notes, 0);
    CHECK_EQ_INT(pr_port_is_stylewriter(&L), 1);
    CHECK_EQ_INT(pr_port_is_imagewriter(&L), 0);
    int id = r->id;
    pr_port_seen_sw(&L, PR_MODEL_SW2);                      // again: no churn
    CHECK_EQ_INT(L.n, 1);
    CHECK_EQ_INT(L.n_notes, 0);
    CHECK_EQ_INT(pr_port_seen(&L, PR_PORT_NOTHING), id);
    CHECK_EQ_INT(pr_port_seen(&L, PR_PORT_SILENT), id);     // a lost answer changes nothing
    CHECK_EQ_INT(pr_get(&L, id)->kind, PR_KIND_STYLEWRITER);
    CHECK_EQ_INT(L.n_notes, 0);
    pr_port_seen_sw(&L, PR_MODEL_NONE);                     // not a StyleWriter model: ignored
    pr_port_seen_sw(&L, PR_MODEL_IW2);
    CHECK_EQ_INT(L.n_notes, 0);
    CHECK_EQ_INT(pr_get(&L, id)->model, PR_MODEL_SW2);

    // The unknown printer that answers: found, shared, named by its model.
    fresh();
    int u = pr_port_seen(&L, PR_PORT_SILENT);
    CHECK_EQ_INT(notes_of(PR_NOTE_UNKNOWN_PORT), 1);
    pr_port_seen_sw(&L, PR_MODEL_SWC2500);
    CHECK_EQ_INT(pr_get(&L, u)->kind, PR_KIND_STYLEWRITER);
    CHECK(strcmp(pr_get(&L, u)->name, "Color StyleWriter 2500") == 0);
    CHECK_EQ_INT(pr_get(&L, u)->shared, 1);
    CHECK_EQ_INT(notes_of(PR_NOTE_UNKNOWN_PORT), 0);
    // ... unless the person said "Something else".
    fresh();
    u = pr_port_seen(&L, PR_PORT_SILENT);
    CHECK_EQ_INT(pr_set_model(&L, u, PR_MODEL_NONE), 0);
    pr_port_seen_sw(&L, PR_MODEL_SW2);
    CHECK_EQ_INT(pr_get(&L, u)->kind, PR_KIND_UNKNOWN);
    CHECK_EQ_INT(pr_get(&L, u)->shared, 0);
}

// A different printer on the port is a notice, never a swap, both ways; accepting it
// is a new printer to phones (a new uuid), and an ImageWriter's again is its own.
static void test_sw_changed(void)
{
    fresh();
    int id = pr_port_seen(&L, PR_PORT_IW2);
    char iw_uuid[46];
    strcpy(iw_uuid, pr_get(&L, id)->uuid);
    pr_port_seen_sw(&L, PR_MODEL_SW2);
    CHECK_EQ_INT(pr_get(&L, id)->kind, PR_KIND_IMAGEWRITER);   // offered, never swapped
    CHECK_EQ_INT(notes_of(PR_NOTE_CHANGED), 1);
    pr_port_seen_sw(&L, PR_MODEL_SW2);                         // seen again: one note
    CHECK_EQ_INT(notes_of(PR_NOTE_CHANGED), 1);
    CHECK_EQ_INT(L.notes[0].kind, PR_NOTE_CHANGED);
    CHECK_EQ_INT(L.notes[0].model, PR_MODEL_SW2);
    static char buf[4096];
    pr_live_t live[PR_MAX];
    memset(live, 0, sizeof live);
    pr_json(&L, live, buf, sizeof buf);
    CHECK(strstr(buf, "{\"kind\":\"changed\",\"id\":1,\"model\":\"StyleWriter II\"}") != NULL);
    CHECK_EQ_INT(pr_accept_change(&L, 0), 0);
    const pr_rec_t *r = pr_get(&L, id);
    CHECK_EQ_INT(r->kind, PR_KIND_STYLEWRITER);
    CHECK_EQ_INT(r->model, PR_MODEL_SW2);
    CHECK(strcmp(r->name, "StyleWriter II") == 0);             // its own name follows
    CHECK(strcmp(r->uuid, iw_uuid) != 0);
    CHECK_EQ_INT(strlen(r->uuid), 45);
    CHECK_EQ_INT(r->shared, 1);
    CHECK_EQ_INT(L.n, 1);
    CHECK_EQ_INT(L.n_notes, 0);
    char sw_uuid[46];
    strcpy(sw_uuid, r->uuid);

    // The same model seen again later raises nothing; another StyleWriter does, and keeps the uuid.
    pr_port_seen_sw(&L, PR_MODEL_SW2);
    CHECK_EQ_INT(L.n_notes, 0);
    pr_port_seen_sw(&L, PR_MODEL_SWC2200);
    CHECK_EQ_INT(notes_of(PR_NOTE_CHANGED), 1);
    CHECK_EQ_INT(pr_accept_change(&L, 0), 0);
    CHECK_EQ_INT(pr_get(&L, id)->model, PR_MODEL_SWC2200);
    CHECK(strcmp(pr_get(&L, id)->name, "Color StyleWriter 2200") == 0);
    CHECK(strcmp(pr_get(&L, id)->uuid, sw_uuid) == 0);         // a StyleWriter is a StyleWriter

    // The reverse: an ImageWriter answering on a StyleWriter record.
    pr_port_seen(&L, PR_PORT_IWLQ);
    CHECK_EQ_INT(pr_get(&L, id)->kind, PR_KIND_STYLEWRITER);
    CHECK_EQ_INT(notes_of(PR_NOTE_CHANGED), 1);
    CHECK_EQ_INT(L.notes[0].model, PR_MODEL_IWLQ);
    pr_port_seen(&L, PR_PORT_IWLQ);
    CHECK_EQ_INT(notes_of(PR_NOTE_CHANGED), 1);
    strcpy(pr_get(&L, id)->dither, "apple");                  // the Apple screen is a StyleWriter's
    CHECK_EQ_INT(pr_accept_change(&L, 0), 0);
    r = pr_get(&L, id);
    CHECK_EQ_INT(r->kind, PR_KIND_IMAGEWRITER);
    CHECK_EQ_INT(r->model, PR_MODEL_IWLQ);
    CHECK(strcmp(r->name, "ImageWriter LQ") == 0);
    CHECK(strcmp(r->uuid, sw_uuid) != 0);
    CHECK(strcmp(r->uuid, iw_uuid) == 0);                      // the ImageWriter's, as it was made
    CHECK(strcmp(r->dither, "atkinson") == 0);                 // no Apple screen on an ImageWriter
    CHECK_EQ_INT(L.n_notes, 0);

    // A name the person gave stays through both.
    fresh();
    id = pr_port_seen(&L, PR_PORT_IW2);
    pr_rename(&L, id, "Desk");
    pr_port_seen_sw(&L, PR_MODEL_SW1200);
    CHECK_EQ_INT(pr_accept_change(&L, 0), 0);
    CHECK(strcmp(pr_get(&L, id)->name, "Desk") == 0);
    CHECK_EQ_INT(pr_get(&L, id)->model, PR_MODEL_SW1200);

    // A note from a list that cannot be read back is refused, not applied.
    fresh();
    id = pr_port_seen(&L, PR_PORT_IW2);
    L.notes[0] = (pr_note_t){ PR_NOTE_CHANGED, (uint8_t)id, PR_MODEL_SWC2500 + 1 };
    L.n_notes = 1;
    CHECK_EQ_INT(pr_accept_change(&L, 0), -1);
    CHECK_EQ_INT(pr_get(&L, id)->kind, PR_KIND_IMAGEWRITER);
}

// The full sequence says the person's pick was another StyleWriter: the record takes the
// printer's word, quietly, and keeps what the person decided about it.
static void test_sw_found(void)
{
    fresh();
    pr_post_t r;
    CHECK_EQ_INT(pr_post(&L, "{\"add_port\":\"Color StyleWriter 2200\"}", &r), 0);
    int id = pr_port(&L)->id;
    char uuid[46];
    strcpy(uuid, pr_port(&L)->uuid);
    pr_set_shared(&L, id, 0);
    CHECK_EQ_INT(pr_port_sw_found(&L, PR_MODEL_SWC2200), 0);          // as picked: nothing
    CHECK_EQ_INT(pr_port_sw_found(&L, PR_MODEL_SWC2500), 1);
    const pr_rec_t *p = pr_port(&L);
    CHECK_EQ_INT(p->model, PR_MODEL_SWC2500);
    CHECK(strcmp(p->name, "Color StyleWriter 2500") == 0);
    CHECK_EQ_INT(p->shared, 0);                                       // still off, as the person left it
    CHECK_EQ_INT(p->found_auto, 0);
    CHECK(strcmp(p->uuid, uuid) == 0);
    CHECK_EQ_INT(L.n_notes, 0);
    pr_rename(&L, id, "Desk");
    CHECK_EQ_INT(pr_port_sw_found(&L, PR_MODEL_SW2), 1);
    CHECK(strcmp(pr_port(&L)->name, "Desk") == 0);
    // Not a StyleWriter model, not a StyleWriter record, no port.
    CHECK_EQ_INT(pr_port_sw_found(&L, PR_MODEL_IW2), 0);
    CHECK_EQ_INT(pr_port_sw_found(&L, PR_MODEL_NONE), 0);
    pr_set_model(&L, id, PR_MODEL_IW2);
    CHECK_EQ_INT(pr_port_sw_found(&L, PR_MODEL_SW2), 0);
    CHECK_EQ_INT(pr_get(&L, id)->kind, PR_KIND_IMAGEWRITER);
    fresh();
    CHECK_EQ_INT(pr_port_sw_found(&L, PR_MODEL_SW2), 0);
}

// A model the person picks reaches the same places, and the uuid follows the family.
static void test_sw_set_model(void)
{
    fresh();
    int id = pr_port_seen(&L, PR_PORT_IW2);
    char iw_uuid[46];
    strcpy(iw_uuid, pr_get(&L, id)->uuid);
    CHECK_EQ_INT(pr_set_model(&L, id, PR_MODEL_SWC1500), 0);
    const pr_rec_t *r = pr_get(&L, id);
    CHECK_EQ_INT(r->kind, PR_KIND_STYLEWRITER);
    CHECK(strcmp(r->name, "Color StyleWriter 1500") == 0);
    CHECK(strcmp(r->uuid, iw_uuid) != 0);
    CHECK_EQ_INT(r->shared, 1);
    CHECK_EQ_INT(r->found_auto, 0);
    CHECK_EQ_INT(pr_set_model(&L, id, (pr_model_t)(PR_MODEL_SWC2500 + 1)), -1);
    CHECK_EQ_INT(pr_set_model(&L, id, PR_MODEL_NONE), 0);        // "Something else"
    CHECK_EQ_INT(pr_get(&L, id)->kind, PR_KIND_UNKNOWN);
    CHECK_EQ_INT(pr_get(&L, id)->shared, 0);
    CHECK_EQ_INT(pr_port_is_stylewriter(&L), 0);
    CHECK_EQ_INT(pr_set_model(&L, id, PR_MODEL_IW2), 0);
    CHECK(strcmp(pr_get(&L, id)->uuid, iw_uuid) == 0);

    // A migrated board's old uuid is an ImageWriter's: a model change within ImageWriters leaves it.
    fresh();
    pr_legacy_t o = legacy(1);
    pr_migrate(&L, &o);
    id = pr_port(&L)->id;
    CHECK_EQ_INT(pr_set_model(&L, id, PR_MODEL_IWLQ), 0);
    CHECK(strcmp(pr_get(&L, id)->uuid, "urn:uuid:41575254-4552-0001-10bd-a36e2434") == 0);

    // An AppleTalk printer takes no model.
    int lw = pr_at_seen(&L, PR_WHERE_CABLE, P("Office"), P("LaserWriter"), 0);
    CHECK_EQ_INT(pr_set_model(&L, lw, PR_MODEL_SW2), -1);
}

static void test_sw_add_port(void)
{
    fresh();
    pr_post_t r;
    CHECK_EQ_INT(pr_post(&L, "{\"add_port\":\"Color StyleWriter 2500\"}", &r), 0);
    CHECK_EQ_INT(r.changed, 1);
    const pr_rec_t *p = pr_port(&L);
    CHECK(p != NULL);
    CHECK_EQ_INT(p->kind, PR_KIND_STYLEWRITER);
    CHECK_EQ_INT(p->model, PR_MODEL_SWC2500);
    CHECK_EQ_INT(p->shared, 1);
    CHECK_EQ_INT(p->found_auto, 0);
    CHECK(strcmp(p->name, "Color StyleWriter 2500") == 0);
    CHECK_EQ_INT(strlen(p->uuid), 45);
    CHECK_EQ_INT(r.confirm_sw_id, p->id);                    // the full sequence confirms the pick
    CHECK_EQ_INT(r.clean_id, -1);
    CHECK_EQ_INT(L.n, 1);
    int id = p->id;
    // Again, with the port's record there: its model, not a second record.
    CHECK_EQ_INT(pr_post(&L, "{\"add_port\":\"StyleWriter 1200\"}", &r), 0);
    CHECK_EQ_INT(L.n, 1);
    CHECK_EQ_INT(pr_port(&L)->model, PR_MODEL_SW1200);
    CHECK_EQ_INT(r.confirm_sw_id, id);
    // An ImageWriter's pick confirms nothing (ESC ? is its own, at line-up).
    CHECK_EQ_INT(pr_post(&L, "{\"add_port\":\"ImageWriter LQ\"}", &r), 0);
    CHECK_EQ_INT(r.confirm_sw_id, -1);
    CHECK_EQ_INT(pr_port(&L)->kind, PR_KIND_IMAGEWRITER);
    CHECK_EQ_INT(pr_post(&L, "{\"add_port\":\"Color StyleWriter 9000\"}", &r), -1);
    CHECK_EQ_INT(pr_post(&L, "{\"add_port\":\"none\"}", &r), -1);
    // Every one of the seven by the name the page sends.
    for (size_t i = 0; i < sizeof SW_ALL / sizeof SW_ALL[0]; i++) {
        char body[80];
        snprintf(body, sizeof body, "{\"add_port\":\"%s\"}", SW_NAMES[i]);
        CHECK_EQ_INT(pr_post(&L, body, &r), 0);
        CHECK_EQ_INT(pr_port(&L)->model, SW_ALL[i]);
        CHECK_EQ_INT(r.confirm_sw_id, id);
    }

    // The unknown printer's notice: the model picked in its detail.
    fresh();
    id = pr_port_seen(&L, PR_PORT_SILENT);
    char body[80];
    snprintf(body, sizeof body, "{\"id\":%d,\"model\":\"StyleWriter\"}", id);
    CHECK_EQ_INT(pr_post(&L, body, &r), 0);
    CHECK_EQ_INT(pr_get(&L, id)->kind, PR_KIND_STYLEWRITER);
    CHECK_EQ_INT(pr_get(&L, id)->model, PR_MODEL_SW1);
    CHECK(strcmp(pr_get(&L, id)->name, "StyleWriter") == 0);
    CHECK_EQ_INT(r.confirm_sw_id, id);
    CHECK_EQ_INT(notes_of(PR_NOTE_UNKNOWN_PORT), 0);
    snprintf(body, sizeof body, "{\"id\":%d,\"name\":\"Desk\"}", id);
    CHECK_EQ_INT(pr_post(&L, body, &r), 0);
    CHECK_EQ_INT(r.confirm_sw_id, -1);                       // only a model picked confirms
}

// "Clean Print Head": one cleaning, for a StyleWriter that has one.
static void test_sw_clean(void)
{
    fresh();
    pr_post_t r;
    char body[80];
    int id = pr_port_seen(&L, PR_PORT_IW2);
    snprintf(body, sizeof body, "{\"id\":%d,\"clean\":true}", id);
    CHECK_EQ_INT(pr_post(&L, body, &r), -1);                 // an ImageWriter has none
    CHECK(strstr(r.err, "clean") != NULL);
    pr_set_model(&L, id, PR_MODEL_SW1);
    CHECK_EQ_INT(pr_post(&L, body, &r), -1);                 // nor the original StyleWriter
    CHECK_EQ_INT(r.clean_id, -1);
    for (size_t i = 1; i < sizeof SW_ALL / sizeof SW_ALL[0]; i++) {
        pr_set_model(&L, id, SW_ALL[i]);
        CHECK_EQ_INT(pr_post(&L, body, &r), 0);
        CHECK_EQ_INT(r.clean_id, id);
        CHECK_EQ_INT(r.changed, 0);                          // it changes no record
    }
    snprintf(body, sizeof body, "{\"id\":%d,\"clean\":false}", id);
    CHECK_EQ_INT(pr_post(&L, body, &r), 0);
    CHECK_EQ_INT(r.clean_id, -1);
    snprintf(body, sizeof body, "{\"id\":%d,\"name\":\"Den\"}", id);
    CHECK_EQ_INT(pr_post(&L, body, &r), 0);
    CHECK_EQ_INT(r.clean_id, -1);
    // With other changes: they are made, and the cleaning is asked for.
    snprintf(body, sizeof body, "{\"id\":%d,\"name\":\"Den\",\"clean\":true}", id);
    CHECK_EQ_INT(pr_post(&L, body, &r), 0);
    CHECK_EQ_INT(r.clean_id, id);
    CHECK_EQ_INT(r.changed, 1);
    // Not a LaserWriter, not an unknown printer, not a printer that is not there.
    int lw = pr_at_seen(&L, PR_WHERE_CABLE, P("Office"), P("LaserWriter"), 0);
    snprintf(body, sizeof body, "{\"id\":%d,\"clean\":true}", lw);
    CHECK_EQ_INT(pr_post(&L, body, &r), -1);
    pr_set_model(&L, id, PR_MODEL_NONE);
    snprintf(body, sizeof body, "{\"id\":%d,\"clean\":true}", id);
    CHECK_EQ_INT(pr_post(&L, body, &r), -1);
    CHECK_EQ_INT(pr_post(&L, "{\"id\":99,\"clean\":true}", &r), -1);
    // A post that asks for nothing clean leaves the new field unset.
    CHECK_EQ_INT(pr_post(&L, "{\"look\":true}", &r), 0);
    CHECK_EQ_INT(r.clean_id, -1);
    CHECK_EQ_INT(r.confirm_sw_id, -1);
}

// Apple's screen is a StyleWriter's alone.
static void test_sw_apple_screen(void)
{
    fresh();
    pr_post_t r;
    char body[96];
    int iw = pr_port_seen(&L, PR_PORT_IW2);
    int lw = pr_at_seen(&L, PR_WHERE_CABLE, P("Office"), P("LaserWriter"), 0);
    snprintf(body, sizeof body, "{\"id\":%d,\"dither\":\"apple\"}", iw);
    CHECK_EQ_INT(pr_post(&L, body, &r), -1);
    CHECK(strstr(r.err, "halftone") != NULL);
    CHECK(strcmp(pr_get(&L, iw)->dither, "atkinson") == 0);
    snprintf(body, sizeof body, "{\"id\":%d,\"dither\":\"apple\"}", lw);
    CHECK_EQ_INT(pr_post(&L, body, &r), -1);
    pr_set_model(&L, iw, PR_MODEL_SW2);
    snprintf(body, sizeof body, "{\"id\":%d,\"dither\":\"apple\"}", iw);
    CHECK_EQ_INT(pr_post(&L, body, &r), 0);
    CHECK(strcmp(pr_get(&L, iw)->dither, "apple") == 0);
    CHECK_EQ_INT(r.screen_id, iw);
    snprintf(body, sizeof body, "{\"id\":%d,\"dither\":\"bluenoise\",\"gain\":10}", iw);
    CHECK_EQ_INT(pr_post(&L, body, &r), 0);                  // the five stay available
    snprintf(body, sizeof body, "{\"id\":%d,\"dither\":\"sparkle\"}", iw);
    CHECK_EQ_INT(pr_post(&L, body, &r), -1);
    // The model and the screen in one post: the model first.
    pr_set_model(&L, iw, PR_MODEL_IW2);
    snprintf(body, sizeof body, "{\"id\":%d,\"model\":\"StyleWriter II\",\"dither\":\"apple\"}", iw);
    CHECK_EQ_INT(pr_post(&L, body, &r), 0);
    CHECK(strcmp(pr_get(&L, iw)->dither, "apple") == 0);
    // Leaving StyleWriters drops it with "Something else", too.
    snprintf(body, sizeof body, "{\"id\":%d,\"model\":\"none\"}", iw);
    CHECK_EQ_INT(pr_post(&L, body, &r), 0);
    CHECK(strcmp(pr_get(&L, iw)->dither, "atkinson") == 0);
}

static void test_sw_json(void)
{
    fresh();
    pr_port_seen_sw(&L, PR_MODEL_SWC2200);
    pr_live_t live[PR_MAX];
    memset(live, 0, sizeof live);
    static char buf[4096];
    pr_json(&L, live, buf, sizeof buf);
    CHECK(strstr(buf, "\"kind\":\"stylewriter\"") != NULL);
    CHECK(strstr(buf, "\"model\":\"Color StyleWriter 2200\"") != NULL);
    CHECK(strstr(buf, "\"name\":\"Color StyleWriter 2200\"") != NULL);
    CHECK(strstr(buf, "\"rp\":\"ipp/print\"") != NULL);
    CHECK(strstr(buf, "\"shared\":true") != NULL);
    CHECK(strstr(buf, "\"identified\":true") != NULL);
    CHECK(strstr(buf, "cartridge") == NULL);                 // not read yet: not said
    live[0].present = live[0].ready = 1;
    live[0].cart = 1;
    pr_json(&L, live, buf, sizeof buf);
    CHECK(strstr(buf, "\"cartridge\":\"black\"") != NULL);
    CHECK(strstr(buf, "\"state\":\"ready\"") != NULL);
    live[0].cart = 2;
    pr_json(&L, live, buf, sizeof buf);
    CHECK(strstr(buf, "\"cartridge\":\"colour\"") != NULL);
    CHECK(strstr(buf, "\"cartridge\":\"black\"") == NULL);

    // Only a StyleWriter has one: an ImageWriter's ribbon is not this, and a LaserWriter has none.
    fresh();
    pr_port_seen(&L, PR_PORT_IW2);
    pr_at_seen(&L, PR_WHERE_CABLE, P("Office"), P("LaserWriter"), 0);
    memset(live, 0, sizeof live);
    live[0].cart = live[1].cart = 2;
    pr_json(&L, live, buf, sizeof buf);
    CHECK(strstr(buf, "cartridge") == NULL);
    // The record is the same size as before: the cartridge is not stored in it.
    CHECK_EQ_INT(sizeof(pr_rec_t), 284);
}

static void test_sw_test_target(void)
{
    fresh();
    pr_live_t live[PR_MAX];
    memset(live, 0, sizeof live);
    lw_info_t info;
    lw_info_default(&info);
    int lw = pr_at_seen(&L, PR_WHERE_CABLE, P("Office"), P("LaserWriter"), 0);
    pr_at_queried(&L, lw, 1, &info);
    CHECK_EQ_INT(pr_test_target(&L, live), lw);              // the first shared printer
    pr_port_seen_sw(&L, PR_MODEL_SW1200);
    CHECK_EQ_INT(pr_test_target(&L, live), pr_port(&L)->id); // the port's StyleWriter, as its ImageWriter
    pr_set_shared(&L, pr_port(&L)->id, 0);
    CHECK_EQ_INT(pr_test_target(&L, live), pr_port(&L)->id); // shared or not
    // A removed one hands the page back.
    pr_remove(&L, pr_port(&L)->id);
    CHECK_EQ_INT(pr_test_target(&L, live), lw);
}

// Vintage Macs are offered nothing for a port StyleWriter until phase 2.
static void test_sw_offers(void)
{
    fresh();
    lw_info_t info;
    lw_info_default(&info);
    int id = pr_port_seen(&L, PR_PORT_IW2);
    int lw = pr_at_seen(&L, PR_WHERE_CABLE, P("Office"), P("LaserWriter"), 0);
    pr_at_queried(&L, lw, 1, &info);
    int ids[8];
    CHECK_EQ_INT(pr_offers(&L, PR_WHERE_TUNNEL, ids, 8), 2);   // the ImageWriter and the LaserWriter
    pr_set_model(&L, id, PR_MODEL_SW2);
    int n = pr_offers(&L, PR_WHERE_TUNNEL, ids, 8);
    CHECK_EQ_INT(n, 1);
    CHECK_EQ_INT(ids[0], lw);
    CHECK_EQ_INT(pr_offers(&L, PR_WHERE_CABLE, ids, 8), 0);
}

// The box follows a shared StyleWriter as it follows an ImageWriter.
static void test_sw_box_state(void)
{
    fresh();
    pr_port_seen_sw(&L, PR_MODEL_SW2);
    pr_live_t live[PR_MAX];
    memset(live, 0, sizeof live);
    CHECK_EQ_INT(pr_box_state(&L, live), PR_BOX_OFFLINE);
    live[0].present = live[0].ready = 1;
    CHECK_EQ_INT(pr_box_state(&L, live), PR_BOX_READY);
    live[0].attention = 1;
    CHECK_EQ_INT(pr_box_state(&L, live), PR_BOX_ATTENTION);
}

// The stored form (W-1103): version 2 adds the StyleWriter kind and models and keeps
// the record's layout, so a version 1 list reads as it always did and a newer one is kept.
static void test_sw_blob(void)
{
    enum { HDR = 6 };
    static uint8_t buf[4096], v1[4096], bad[4096];
    CHECK_EQ_INT(PR_BLOB_VER, 2);

    // A list with a StyleWriter in it round-trips as version 2.
    fresh();
    pr_port_seen_sw(&L, PR_MODEL_SWC2400);
    int lw = pr_at_seen(&L, PR_WHERE_CABLE, P("Office"), P("LaserWriter"), 0);
    lw_info_t info;
    lw_info_default(&info);
    pr_at_queried(&L, lw, 1, &info);
    pr_port_seen(&L, PR_PORT_IW2);                           // two changed notices, one naming a
    pr_port_seen_sw(&L, PR_MODEL_SW2);                       // StyleWriter model
    size_t n = pr_save(&L, buf, sizeof buf);
    CHECK(n > 0);
    CHECK_EQ_INT(buf[2], 2);
    pr_list_t M;
    pr_init(&M, MAC, 1);
    CHECK_EQ_INT(pr_load(&M, buf, n), 0);
    CHECK_EQ_INT(M.n, L.n);
    CHECK(memcmp(M.r, L.r, sizeof L.r[0] * L.n) == 0);
    CHECK_EQ_INT(M.n_notes, L.n_notes);
    CHECK(memcmp(M.notes, L.notes, sizeof L.notes[0] * L.n_notes) == 0);
    CHECK_EQ_INT(pr_port(&M)->kind, PR_KIND_STYLEWRITER);
    CHECK_EQ_INT(pr_port(&M)->model, PR_MODEL_SWC2400);
    CHECK_EQ_INT(pr_blob_newer(buf, n), 0);                  // ours

    // A version 1 list (what every board has now) loads, nothing lost, and is saved as 2.
    fresh();
    int a = pr_port_seen(&L, PR_PORT_IWLQ);
    pr_rename(&L, a, "Desk");
    pr_at_queried(&L, pr_at_seen(&L, PR_WHERE_TUNNEL, P("Back"), P("LaserWriter"), 0), 1, &info);
    n = pr_save(&L, v1, sizeof v1);
    v1[2] = 1;                                               // the layout is the same, the stamp is 1
    pr_init(&M, MAC, 1);
    CHECK_EQ_INT(pr_load(&M, v1, n), 0);
    CHECK_EQ_INT(M.n, L.n);
    CHECK(memcmp(M.r, L.r, sizeof L.r[0] * L.n) == 0);
    CHECK_EQ_INT(pr_blob_newer(v1, n), 0);
    size_t m = pr_save(&M, buf, sizeof buf);
    CHECK_EQ_INT(m, n);
    CHECK_EQ_INT(buf[2], 2);
    CHECK(memcmp(buf + 3, v1 + 3, n - 3) == 0);

    // Version 1 means the old kinds and models: nothing newer is believed.
    const size_t kind_at = HDR + offsetof(pr_rec_t, kind), model_at = HDR + offsetof(pr_rec_t, model);
    pr_init(&M, MAC, 1);
    memcpy(bad, v1, n);
    bad[kind_at] = PR_KIND_STYLEWRITER;
    bad[model_at] = PR_MODEL_SW2;
    CHECK_EQ_INT(pr_load(&M, bad, n), -1);                   // stamped 1: no such kind
    bad[2] = 2;
    CHECK_EQ_INT(pr_load(&M, bad, n), 0);                    // stamped 2: a StyleWriter
    memcpy(bad, v1, n);
    bad[model_at] = PR_MODEL_SW2;                            // an ImageWriter that is a StyleWriter II
    bad[2] = 1;
    CHECK_EQ_INT(pr_load(&M, bad, n), -1);
    bad[2] = 2;
    CHECK_EQ_INT(pr_load(&M, bad, n), -1);                   // and not in 2 either
    memcpy(bad, v1, n);
    bad[kind_at] = PR_KIND_STYLEWRITER;                      // a StyleWriter that is an ImageWriter LQ
    bad[2] = 2;
    CHECK_EQ_INT(pr_load(&M, bad, n), -1);
    memcpy(bad, v1, n);
    bad[kind_at] = PR_KIND_STYLEWRITER + 1;                  // a kind nobody has
    bad[2] = 2;
    CHECK_EQ_INT(pr_load(&M, bad, n), -1);
    memcpy(bad, v1, n);
    bad[kind_at] = PR_KIND_STYLEWRITER;
    bad[model_at] = PR_MODEL_SWC2500 + 1;                    // a model nobody has
    bad[2] = 2;
    CHECK_EQ_INT(pr_load(&M, bad, n), -1);

    // Version 3 is a newer firmware's: not read here, and kept.
    memcpy(bad, buf, n);
    bad[2] = 3;
    pr_init(&M, MAC, 1);
    pr_list_t keep = M;
    CHECK_EQ_INT(pr_load(&M, bad, n), -1);
    CHECK_EQ_INT(pr_blob_newer(bad, n), 1);
    CHECK_EQ_INT(pr_blob_newer(bad, 3), 1);
    CHECK(memcmp(&M, &keep, sizeof M) == 0);
    // The version before any, and a damaged one, are neither.
    bad[2] = 0;
    CHECK_EQ_INT(pr_load(&M, bad, n), -1);
    CHECK_EQ_INT(pr_blob_newer(bad, n), 0);
    // An old firmware (PR_BLOB_VER 1) sees our version 2 as newer: that is `stamp > its version`.
    CHECK(buf[2] > 1);

    // A note whose model nobody has is refused with the rest.
    fresh();
    pr_port_seen(&L, PR_PORT_IW2);
    pr_port_seen_sw(&L, PR_MODEL_SW2);
    n = pr_save(&L, buf, sizeof buf);
    CHECK_EQ_INT(L.n_notes, 1);
    memcpy(bad, buf, n);
    bad[n - 1] = PR_MODEL_SWC2500 + 1;                       // the note's model, its last byte
    CHECK_EQ_INT(pr_load(&M, bad, n), -1);
    bad[2] = 1;
    bad[n - 1] = PR_MODEL_SW2;                               // a StyleWriter in a version 1 note
    CHECK_EQ_INT(pr_load(&M, bad, n), -1);
}

int main(void)
{
    test_at_renamed();
    test_at_renamed_named();
    test_at_not_renamed();
    test_seen_save_load();
    test_add_port();
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
    test_load_newer();
    test_json();
    test_start_page_kept();
    test_sw_models();
    test_sw_port_found();
    test_sw_changed();
    test_sw_found();
    test_sw_set_model();
    test_sw_add_port();
    test_sw_clean();
    test_sw_apple_screen();
    test_sw_json();
    test_sw_test_target();
    test_sw_offers();
    test_sw_box_state();
    test_sw_blob();
    TEST_END;
}
