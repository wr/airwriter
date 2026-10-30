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
#include "sw_session.h"
#include <stddef.h>
#include <stdlib.h>

static const uint8_t MAC[6] = { 0x10, 0xBD, 0xA3, 0x6E, 0x24, 0x34 };

static pr_list_t L;
static void fresh(void) { pr_init(&L, MAC, 1); }

// W-1116: an answer naming a model with no record makes one only when it comes twice in a
// row. The set-up for tests that want the record there: the second answer's return.
static int seen_iw(pr_port_seen_t what) { pr_port_seen(&L, what); return pr_port_seen(&L, what); }
static int seen_sw(pr_model_t m) { pr_port_seen_sw(&L, m); return pr_port_seen_sw(&L, m); }

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
    // W-1116: the same answer twice makes the record; one reply may be garbled.
    CHECK_EQ_INT(pr_port_seen(&L, PR_PORT_IW2), PR_SEEN_AGAIN);
    CHECK_EQ_INT(L.n, 0);
    CHECK_EQ_INT(pr_port_heard(&L), PR_HEARD_IMAGEWRITER);     // who answered follows at once
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

// W-1116: the printer port keeps a record for every model that answers on it, one per model,
// and the one that answered last is the one on the port. A different printer answering is
// that printer's record, never a notice, and the record it replaced is as it was.
static void test_port_two_records(void)
{
    fresh();
    int iw = seen_iw(PR_PORT_IW2);
    CHECK(iw >= 0);
    CHECK_EQ_INT(L.n, 1);
    CHECK_EQ_INT(pr_port_count(&L), 1);
    CHECK(pr_port_current(&L) == pr_get(&L, iw));
    pr_rename(&L, iw, "Desk");
    CHECK_EQ_INT(pr_port_seen_sw(&L, PR_MODEL_SW2), PR_SEEN_AGAIN);
    CHECK_EQ_INT(L.n, 1);
    CHECK(pr_port_current(&L) == pr_get(&L, iw));          // no record answers: the oldest
    int sw = pr_port_seen_sw(&L, PR_MODEL_SW2);
    CHECK(sw >= 0 && sw != iw);
    CHECK_EQ_INT(L.n, 2);
    CHECK_EQ_INT(pr_port_count(&L), 2);
    const pr_rec_t *r = pr_get(&L, sw);
    CHECK_EQ_INT(r->where, PR_WHERE_PORT);
    CHECK_EQ_INT(r->kind, PR_KIND_STYLEWRITER);
    CHECK_EQ_INT(r->model, PR_MODEL_SW2);
    CHECK(strcmp(r->name, "StyleWriter II") == 0);
    CHECK_EQ_INT(r->shared, 1);
    CHECK_EQ_INT(r->found_auto, 1);
    CHECK_EQ_INT(r->identified, 1);
    r = pr_get(&L, iw);                                     // the ImageWriter's, as it was
    CHECK_EQ_INT(r->kind, PR_KIND_IMAGEWRITER);
    CHECK_EQ_INT(r->model, PR_MODEL_IW2);
    CHECK_EQ_INT(r->shared, 1);
    CHECK(strcmp(r->name, "Desk") == 0);
    CHECK_EQ_INT(L.n_notes, 0);
    CHECK_EQ_INT(notes_of(PR_NOTE_CHANGED), 0);
    // The one on the port follows each answer; the oldest record stays pr_port's.
    CHECK(pr_port_current(&L) == pr_get(&L, sw));
    CHECK(pr_port(&L) == pr_get(&L, iw));
    CHECK_EQ_INT(pr_port_heard(&L), PR_HEARD_STYLEWRITER);
    CHECK_EQ_INT(pr_port_is_stylewriter(&L), 1);
    CHECK_EQ_INT(pr_port_is_imagewriter(&L), 0);
    CHECK_EQ_INT(pr_port_seen(&L, PR_PORT_IW2), iw);        // a recorded model: at once
    CHECK(pr_port_current(&L) == pr_get(&L, iw));
    CHECK_EQ_INT(pr_port_heard(&L), PR_HEARD_IMAGEWRITER);
    CHECK_EQ_INT(pr_port_is_imagewriter(&L), 1);
    CHECK_EQ_INT(pr_port_is_stylewriter(&L), 0);
    CHECK_EQ_INT(pr_port_seen_sw(&L, PR_MODEL_SW2), sw);
    CHECK(pr_port_current(&L) == pr_get(&L, sw));
    CHECK_EQ_INT(L.n, 2);
    CHECK_EQ_INT(L.n_notes, 0);
    CHECK(pr_port_of(&L, PR_MODEL_IW2) == pr_get(&L, iw));
    CHECK(pr_port_of(&L, PR_MODEL_SW2) == pr_get(&L, sw));
    CHECK(pr_port_of(&L, PR_MODEL_IWLQ) == NULL);
    CHECK(pr_port_of(&L, PR_MODEL_NONE) == NULL);
    CHECK(pr_port_said_other(&L) == NULL);
    // Which family answered last, for the record that must not hear the other's language.
    CHECK_EQ_INT(pr_port_other_family(&L, PR_HEARD_IMAGEWRITER), 1);
    CHECK_EQ_INT(pr_port_other_family(&L, PR_HEARD_STYLEWRITER), 0);
    pr_port_seen(&L, PR_PORT_IW2);
    CHECK_EQ_INT(pr_port_other_family(&L, PR_HEARD_IMAGEWRITER), 0);
    CHECK_EQ_INT(pr_port_other_family(&L, PR_HEARD_STYLEWRITER), 1);
    // An ImageWriter LQ makes a third; an AppleTalk printer is not a port record.
    int lq = seen_iw(PR_PORT_IWLQ);
    CHECK(lq >= 0);
    CHECK_EQ_INT(pr_port_count(&L), 3);
    CHECK(pr_port_current(&L) == pr_get(&L, lq));
    pr_at_seen(&L, PR_WHERE_CABLE, P("Office"), P("LaserWriter"), 0);
    CHECK_EQ_INT(pr_port_count(&L), 3);
    CHECK_EQ_INT(L.n, 4);
    // An answer of its own that names no model (a job that printed) keeps the one on the port;
    // one of the other family with no model named leaves the oldest as the port's.
    pr_port_answered(&L, PR_HEARD_IMAGEWRITER);
    CHECK(pr_port_current(&L) == pr_get(&L, lq));
    pr_port_answered(&L, PR_HEARD_STYLEWRITER);
    CHECK_EQ_INT(pr_port_heard(&L), PR_HEARD_STYLEWRITER);
    CHECK(pr_port_current(&L) == pr_port(&L));
    pr_port_answered(&L, PR_HEARD_NONE);                    // silence is not an answer
    CHECK_EQ_INT(pr_port_heard(&L), PR_HEARD_STYLEWRITER);
    // The line down or nothing said changes no record.
    CHECK_EQ_INT(pr_port_seen(&L, PR_PORT_NOTHING), pr_port_current(&L)->id);
    CHECK_EQ_INT(pr_port_seen(&L, PR_PORT_SILENT), pr_port_current(&L)->id);
    CHECK_EQ_INT(L.n, 4);
    CHECK_EQ_INT(L.n_notes, 0);
    // The page: every port record with its own state, and no swap to confirm.
    pr_live_t live[PR_MAX];
    memset(live, 0, sizeof live);
    live[pr_index(&L, sw)].present = live[pr_index(&L, sw)].ready = 1;
    static char buf[4096];
    CHECK(pr_json(&L, live, buf, sizeof buf) > 0);
    CHECK(strstr(buf, "swapped") == NULL);
    CHECK(strstr(buf, "\"state\":\"ready\"") != NULL);
    CHECK(strstr(buf, "\"state\":\"attention\"") == NULL);
    CHECK(strstr(buf, "\"changed\"") == NULL);
    // Removing the record of the printer on the port: it comes back at the next two answers.
    CHECK_EQ_INT(pr_remove(&L, iw), 0);
    CHECK(pr_port(&L) == pr_get(&L, sw));                   // the oldest left
    CHECK_EQ_INT(pr_port_seen(&L, PR_PORT_IW2), PR_SEEN_AGAIN);
    int again = pr_port_seen(&L, PR_PORT_IW2);
    CHECK(again >= 0 && again != iw);
    CHECK_EQ_INT(pr_port_count(&L), 3);
    // Nothing answered yet: no family is the other.
    fresh();
    CHECK_EQ_INT(pr_port_other_family(&L, PR_HEARD_IMAGEWRITER), 0);
    CHECK_EQ_INT(pr_port_other_family(&L, PR_HEARD_STYLEWRITER), 0);
    CHECK(pr_port_current(&L) == NULL);
    CHECK_EQ_INT(pr_port_count(&L), 0);
}

// A model with no record is believed only when it answers twice in a row, so a garbled reply
// never adds a printer. Any other answer in between starts it over; the line going down
// between the two (the cable moved) does not. Nothing of it is stored.
static void test_twice_seen(void)
{
    fresh();
    CHECK_EQ_INT(pr_port_seen(&L, PR_PORT_IWLQ), PR_SEEN_AGAIN);
    CHECK_EQ_INT(L.n, 0);
    CHECK_EQ_INT(L.n_notes, 0);
    CHECK_EQ_INT(pr_port_seen(&L, PR_PORT_IW2), PR_SEEN_AGAIN);     // another model: over again
    CHECK_EQ_INT(pr_port_seen(&L, PR_PORT_IWLQ), PR_SEEN_AGAIN);
    CHECK_EQ_INT(L.n, 0);
    int lq = pr_port_seen(&L, PR_PORT_IWLQ);
    CHECK(lq >= 0);
    CHECK_EQ_INT(pr_get(&L, lq)->model, PR_MODEL_IWLQ);
    CHECK_EQ_INT(L.n, 1);
    // A StyleWriter's answer resets an ImageWriter's, and the other way round.
    CHECK_EQ_INT(pr_port_seen(&L, PR_PORT_IW2), PR_SEEN_AGAIN);
    CHECK_EQ_INT(pr_port_seen_sw(&L, PR_MODEL_SWC2500), PR_SEEN_AGAIN);
    CHECK_EQ_INT(pr_port_seen(&L, PR_PORT_IW2), PR_SEEN_AGAIN);
    CHECK_EQ_INT(pr_port_seen_sw(&L, PR_MODEL_SWC2500), PR_SEEN_AGAIN);
    CHECK_EQ_INT(L.n, 1);
    // Nothing said with the line up is another answer; the line down is not one.
    CHECK_EQ_INT(pr_port_seen(&L, PR_PORT_IW2), PR_SEEN_AGAIN);
    CHECK_EQ_INT(pr_port_seen(&L, PR_PORT_SILENT), lq);
    CHECK_EQ_INT(pr_port_seen(&L, PR_PORT_IW2), PR_SEEN_AGAIN);
    CHECK_EQ_INT(pr_port_seen(&L, PR_PORT_NOTHING), lq);
    int iw = pr_port_seen(&L, PR_PORT_IW2);
    CHECK(iw >= 0 && iw != lq);
    CHECK_EQ_INT(L.n, 2);
    // A recorded model needs no second answer.
    CHECK_EQ_INT(pr_port_seen(&L, PR_PORT_IWLQ), lq);
    CHECK_EQ_INT(pr_port_seen(&L, PR_PORT_IW2), iw);
    CHECK_EQ_INT(pr_port_seen_sw(&L, PR_MODEL_SWC2500), PR_SEEN_AGAIN);
    int sw = pr_port_seen_sw(&L, PR_MODEL_SWC2500);
    CHECK(sw >= 0);
    CHECK_EQ_INT(pr_port_seen_sw(&L, PR_MODEL_SWC2500), sw);
    CHECK_EQ_INT(L.n, 3);
    CHECK_EQ_INT(L.n_notes, 0);
    // Not a StyleWriter model: ignored.
    CHECK_EQ_INT(pr_port_seen_sw(&L, PR_MODEL_NONE), -1);
    CHECK_EQ_INT(pr_port_seen_sw(&L, PR_MODEL_IW2), -1);
    CHECK_EQ_INT(L.n, 3);
    // The first answer is not stored: a restart between the two starts over.
    fresh();
    CHECK_EQ_INT(pr_port_seen(&L, PR_PORT_IWLQ), PR_SEEN_AGAIN);
    static uint8_t buf[4096];
    size_t n = pr_save(&L, buf, sizeof buf);
    pr_list_t M;
    pr_init(&M, MAC, 1);
    CHECK_EQ_INT(pr_load(&M, buf, n), 0);
    CHECK_EQ_INT(pr_port_seen(&M, PR_PORT_IWLQ), PR_SEEN_AGAIN);
    CHECK_EQ_INT(M.n, 0);
}

static void test_port_nothing(void)
{
    fresh();
    int id = seen_iw(PR_PORT_IW2);
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
    seen_iw(PR_PORT_IW2);
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
    int a = seen_iw(PR_PORT_IW2);
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
    int port = seen_iw(PR_PORT_IW2);
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
    int a = seen_iw(PR_PORT_IW2);
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
    seen_iw(PR_PORT_IW2);
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
    seen_iw(PR_PORT_IW2);
    static uint8_t buf[4096];
    size_t n = pr_save(&L, buf, sizeof buf);
    pr_list_t M;
    pr_init(&M, MAC, 1);
    pr_port_seen(&M, PR_PORT_IWLQ);                         // what must survive a refusal
    pr_port_seen(&M, PR_PORT_IWLQ);
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
    seen_iw(PR_PORT_IW2);
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
    int a = seen_iw(PR_PORT_IW2);
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
    int port = seen_iw(PR_PORT_IW2);
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
// one that is off, never answers by itself. It makes a port record, the
// person's, shared; W-1116: one for each model, beside the records there.
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
    int iw1 = p->id;
    // Again, with that record there: a second record, the first as it was.
    CHECK_EQ_INT(pr_post(&L, "{\"add_port\":\"ImageWriter LQ\"}", &r), 0);
    CHECK_EQ_INT(L.n, 2);
    CHECK_EQ_INT(pr_port_count(&L), 2);
    CHECK_EQ_INT(pr_get(&L, iw1)->model, PR_MODEL_IW1);
    CHECK(pr_port_of(&L, PR_MODEL_IWLQ) != NULL);
    CHECK_EQ_INT(pr_port_of(&L, PR_MODEL_IWLQ)->shared, 1);
    CHECK_EQ_INT(pr_post(&L, "{\"add_port\":\"Daisywheel\"}", &r), -1);
    CHECK(strcmp(r.err, "Unknown model.") == 0);
    // The LQ then answers ESC ?: its record, no churn.
    CHECK_EQ_INT(pr_port_seen(&L, PR_PORT_IWLQ), pr_port_of(&L, PR_MODEL_IWLQ)->id);
    CHECK_EQ_INT(L.n, 2);
    CHECK_EQ_INT(L.n_notes, 0);
}

// W-1116: Add a Printer… adds a record for a model not in the list; a model that is in it
// is refused, by the page's sentence, and so is a record's Model set to one another record
// has. An "Unknown printer" or "Something else" record is the one record it converts, as
// before.
static void test_add_port_adds(void)
{
    fresh();
    int iw = seen_iw(PR_PORT_IW2);
    int sw = pr_add_port(&L, PR_MODEL_SW2);
    CHECK(sw >= 0 && sw != iw);
    CHECK_EQ_INT(L.n, 2);
    const pr_rec_t *r = pr_get(&L, sw);
    CHECK_EQ_INT(r->where, PR_WHERE_PORT);
    CHECK_EQ_INT(r->kind, PR_KIND_STYLEWRITER);
    CHECK_EQ_INT(r->model, PR_MODEL_SW2);
    CHECK_EQ_INT(r->shared, 1);
    CHECK_EQ_INT(r->found_auto, 0);
    CHECK_EQ_INT(r->identified, 1);
    CHECK(strcmp(r->name, "StyleWriter II") == 0);
    CHECK_EQ_INT(pr_get(&L, iw)->model, PR_MODEL_IW2);         // the ImageWriter's, as it was
    CHECK(pr_port(&L) == pr_get(&L, iw));
    // One record per model.
    CHECK_EQ_INT(pr_add_port(&L, PR_MODEL_IW2), -2);
    CHECK_EQ_INT(pr_add_port(&L, PR_MODEL_SW2), -2);
    CHECK_EQ_INT(L.n, 2);
    CHECK_EQ_INT(pr_set_model(&L, sw, PR_MODEL_IW2), -2);
    CHECK_EQ_INT(pr_get(&L, sw)->model, PR_MODEL_SW2);
    CHECK_EQ_INT(pr_set_model(&L, sw, PR_MODEL_SW2), 0);        // its own model again
    CHECK_EQ_INT(pr_set_model(&L, sw, PR_MODEL_SW1200), 0);     // a model nobody has
    CHECK_EQ_INT(pr_get(&L, sw)->model, PR_MODEL_SW1200);
    int sw2 = pr_add_port(&L, PR_MODEL_SW2);                   // free again
    CHECK(sw2 >= 0 && sw2 != sw);
    CHECK_EQ_INT(L.n, 3);
    CHECK_EQ_INT(pr_add_port(&L, PR_MODEL_NONE), -1);
    CHECK_EQ_INT(pr_add_port(&L, (pr_model_t)(PR_MODEL_SWC2500 + 1)), -1);
    // The page's sentence, for Add a Printer… and for a record's Model.
    pr_post_t o;
    char body[80];
    CHECK_EQ_INT(pr_post(&L, "{\"add_port\":\"ImageWriter II\"}", &o), -1);
    CHECK(strcmp(o.err, "ImageWriter II is already in Printers.") == 0);
    CHECK_EQ_INT(o.changed, 0);
    snprintf(body, sizeof body, "{\"id\":%d,\"model\":\"StyleWriter II\"}", sw);
    CHECK_EQ_INT(pr_post(&L, body, &o), -1);
    CHECK(strcmp(o.err, "StyleWriter II is already in Printers.") == 0);
    CHECK_EQ_INT(o.confirm_sw_id, -1);
    CHECK_EQ_INT(pr_post(&L, "{\"add_port\":\"ImageWriter LQ\"}", &o), 0);
    CHECK_EQ_INT(L.n, 4);
    CHECK_EQ_INT(pr_post(&L, "{\"add_port\":\"Daisywheel\"}", &o), -1);
    CHECK(strcmp(o.err, "Unknown model.") == 0);
    // An Unknown printer the board found, or a Something else, converts, as before.
    fresh();
    int u = pr_port_seen(&L, PR_PORT_SILENT);
    CHECK_EQ_INT(pr_add_port(&L, PR_MODEL_IW1), u);
    CHECK_EQ_INT(L.n, 1);
    CHECK_EQ_INT(pr_get(&L, u)->model, PR_MODEL_IW1);
    CHECK_EQ_INT(pr_get(&L, u)->found_auto, 0);
    CHECK_EQ_INT(notes_of(PR_NOTE_UNKNOWN_PORT), 0);
    CHECK_EQ_INT(pr_set_model(&L, u, PR_MODEL_NONE), 0);
    CHECK(pr_port_said_other(&L) == pr_get(&L, u));
    CHECK_EQ_INT(pr_add_port(&L, PR_MODEL_SW2), u);
    CHECK_EQ_INT(L.n, 1);
    CHECK_EQ_INT(pr_get(&L, u)->kind, PR_KIND_STYLEWRITER);
    CHECK(pr_port_said_other(&L) == NULL);
    // Beside another record too.
    int iw2 = pr_add_port(&L, PR_MODEL_IW2);
    CHECK(iw2 > 0);
    CHECK_EQ_INT(pr_set_model(&L, u, PR_MODEL_NONE), 0);
    CHECK(pr_port_said_other(&L) == pr_get(&L, u));
    CHECK_EQ_INT(pr_add_port(&L, PR_MODEL_SWC2500), u);
    CHECK(pr_port_said_other(&L) == NULL);
    CHECK_EQ_INT(L.n, 2);
    CHECK_EQ_INT(pr_add_port(&L, PR_MODEL_IW2), -2);
}

// An "Unknown printer" record is the port's printer before it was identified: an identified
// answer (twice) converts it, its uuid kept, as before; where that model has a record
// already, the Unknown record and its notice go instead.
static void test_unknown_goes(void)
{
    fresh();
    int u = pr_port_seen(&L, PR_PORT_SILENT);
    char uuid[46];
    strcpy(uuid, pr_get(&L, u)->uuid);
    CHECK_EQ_INT(notes_of(PR_NOTE_UNKNOWN_PORT), 1);
    CHECK_EQ_INT(pr_port_seen(&L, PR_PORT_IWLQ), PR_SEEN_AGAIN);
    CHECK_EQ_INT(pr_get(&L, u)->kind, PR_KIND_UNKNOWN);
    CHECK_EQ_INT(notes_of(PR_NOTE_UNKNOWN_PORT), 1);
    CHECK_EQ_INT(pr_port_seen(&L, PR_PORT_IWLQ), u);
    const pr_rec_t *r = pr_get(&L, u);
    CHECK_EQ_INT(r->kind, PR_KIND_IMAGEWRITER);
    CHECK_EQ_INT(r->model, PR_MODEL_IWLQ);
    CHECK(strcmp(r->name, "ImageWriter LQ") == 0);
    CHECK(strcmp(r->uuid, uuid) == 0);
    CHECK_EQ_INT(r->shared, 1);
    CHECK_EQ_INT(r->found_auto, 1);
    CHECK_EQ_INT(notes_of(PR_NOTE_UNKNOWN_PORT), 0);
    CHECK_EQ_INT(L.n, 1);
    // A StyleWriter the same.
    fresh();
    u = pr_port_seen(&L, PR_PORT_SILENT);
    strcpy(uuid, pr_get(&L, u)->uuid);
    CHECK_EQ_INT(seen_sw(PR_MODEL_SWC2500), u);
    r = pr_get(&L, u);
    CHECK_EQ_INT(r->kind, PR_KIND_STYLEWRITER);
    CHECK(strcmp(r->name, "Color StyleWriter 2500") == 0);
    CHECK(strcmp(r->uuid, uuid) == 0);
    CHECK_EQ_INT(r->shared, 1);
    CHECK_EQ_INT(notes_of(PR_NOTE_UNKNOWN_PORT), 0);
    CHECK_EQ_INT(L.n, 1);
    // With the LQ listed already, the Unknown printer was the LQ: it goes, with its notice.
    // Nothing said where a record is set up makes no record (as before), so the list is
    // made by hand.
    fresh();
    int lq = seen_iw(PR_PORT_IWLQ);
    L.r[1] = L.r[0];
    L.r[1].id = 7;
    L.r[1].kind = PR_KIND_UNKNOWN;
    L.r[1].model = PR_MODEL_NONE;
    L.r[1].shared = L.r[1].identified = 0;
    snprintf(L.r[1].name, sizeof L.r[1].name, "Unknown printer");
    L.n = 2;
    L.notes[0] = (pr_note_t){ PR_NOTE_UNKNOWN_PORT, 7, 0 };
    L.n_notes = 1;
    CHECK_EQ_INT(pr_port_seen(&L, PR_PORT_IWLQ), lq);         // its own record: at once
    CHECK_EQ_INT(L.n, 2);                                     // but the Unknown waits for the second
    CHECK_EQ_INT(pr_port_seen(&L, PR_PORT_IWLQ), lq);
    CHECK_EQ_INT(L.n, 1);
    CHECK(pr_get(&L, 7) == NULL);
    CHECK_EQ_INT(L.n_notes, 0);
    CHECK_EQ_INT(pr_get(&L, lq)->model, PR_MODEL_IWLQ);
    CHECK_EQ_INT(pr_port_seen(&L, PR_PORT_IWLQ), lq);
    CHECK_EQ_INT(L.n, 1);
}

// "Something else" is the person's word that the port is theirs to leave alone. A printer
// that answers the person's own Look for Printers replaces that record (W-1116; before, the
// answer was ignored).
static void test_something_else_replaced(void)
{
    fresh();
    int u = pr_port_seen(&L, PR_PORT_SILENT);
    CHECK_EQ_INT(pr_set_model(&L, u, PR_MODEL_NONE), 0);
    CHECK(pr_port_said_other(&L) == pr_get(&L, u));
    CHECK_EQ_INT(pr_port_seen(&L, PR_PORT_IW2), PR_SEEN_AGAIN);       // once: nothing
    CHECK_EQ_INT(pr_get(&L, u)->kind, PR_KIND_UNKNOWN);
    CHECK_EQ_INT(L.n, 1);
    int iw = pr_port_seen(&L, PR_PORT_IW2);
    CHECK(iw >= 0 && iw != u);
    CHECK(pr_get(&L, u) == NULL);
    CHECK(pr_port_said_other(&L) == NULL);
    CHECK_EQ_INT(L.n, 1);
    const pr_rec_t *r = pr_get(&L, iw);
    CHECK_EQ_INT(r->kind, PR_KIND_IMAGEWRITER);
    CHECK_EQ_INT(r->model, PR_MODEL_IW2);
    CHECK_EQ_INT(r->shared, 1);
    CHECK_EQ_INT(r->found_auto, 1);
    CHECK(strcmp(r->name, "ImageWriter II") == 0);
    CHECK_EQ_INT(L.n_notes, 0);
    // A StyleWriter the same, beside a record of another model.
    int sw = pr_add_port(&L, PR_MODEL_SW2);
    CHECK_EQ_INT(pr_set_model(&L, sw, PR_MODEL_NONE), 0);
    CHECK(pr_port_said_other(&L) == pr_get(&L, sw));
    int cs = seen_sw(PR_MODEL_SWC2400);
    CHECK(cs >= 0 && cs != sw);
    CHECK(pr_get(&L, sw) == NULL);
    CHECK_EQ_INT(L.n, 2);
    CHECK_EQ_INT(pr_get(&L, cs)->model, PR_MODEL_SWC2400);
    CHECK_EQ_INT(pr_get(&L, iw)->model, PR_MODEL_IW2);
    // Replaced by a model that has its record already: it just goes.
    int x = pr_add_port(&L, PR_MODEL_SW1200);
    CHECK_EQ_INT(pr_set_model(&L, x, PR_MODEL_NONE), 0);
    CHECK_EQ_INT(L.n, 3);
    CHECK_EQ_INT(pr_port_seen(&L, PR_PORT_IW2), iw);
    CHECK_EQ_INT(L.n, 3);
    CHECK_EQ_INT(pr_port_seen(&L, PR_PORT_IW2), iw);
    CHECK_EQ_INT(L.n, 2);
    CHECK(pr_get(&L, x) == NULL);
    CHECK(pr_port_said_other(&L) == NULL);
}

// Each port record has a uuid of its own: the list's epoch and MAC with the model and the
// family at offset 28, made once and never changed. A new record whose model form is taken
// (by a record since corrected to another model) sets the model byte's top bit.
static void test_port_uuid_per_model(void)
{
    fresh();
    int iw = seen_iw(PR_PORT_IW2);
    int lq = seen_iw(PR_PORT_IWLQ);
    int sw = seen_sw(PR_MODEL_SW2);
    const char *a = pr_get(&L, iw)->uuid, *b = pr_get(&L, lq)->uuid, *c = pr_get(&L, sw)->uuid;
    CHECK_EQ_INT(strlen(a), 45);
    CHECK(strncmp(a, "urn:uuid:41575254-4c57-0001-", 28) == 0);     // the epoch, 1
    CHECK(strncmp(a + 28, "0101", 4) == 0);
    CHECK(strncmp(b + 28, "0201", 4) == 0);
    CHECK(strncmp(c + 28, "0502", 4) == 0);
    CHECK(strcmp(a + 32, b + 32) == 0 && strcmp(a + 32, c + 32) == 0);   // the MAC
    CHECK(strcmp(a, b) != 0 && strcmp(a, c) != 0 && strcmp(b, c) != 0);
    // Corrected to another model, a record keeps its uuid; a new record of the model it had
    // takes the form with the top bit, and a third the next.
    CHECK_EQ_INT(pr_remove(&L, lq), 0);
    CHECK_EQ_INT(pr_set_model(&L, iw, PR_MODEL_IWLQ), 0);
    CHECK(strcmp(pr_get(&L, iw)->uuid, a) == 0);
    int iw2 = seen_iw(PR_PORT_IW2);
    CHECK(strncmp(pr_get(&L, iw2)->uuid + 28, "8101", 4) == 0);
    CHECK(strcmp(pr_get(&L, iw2)->uuid + 32, a + 32) == 0);
    CHECK_EQ_INT(pr_set_model(&L, iw2, PR_MODEL_IW1), 0);
    CHECK(strncmp(pr_get(&L, iw2)->uuid + 28, "8101", 4) == 0);
    int iw3 = seen_iw(PR_PORT_IW2);
    CHECK(strncmp(pr_get(&L, iw3)->uuid + 28, "4101", 4) == 0);
    // A family the person changes keeps it too: the record is the printer it was to phones.
    fresh();
    iw = seen_iw(PR_PORT_IW2);
    char u[46];
    strcpy(u, pr_get(&L, iw)->uuid);
    CHECK_EQ_INT(pr_set_model(&L, iw, PR_MODEL_SW2), 0);
    CHECK(strcmp(pr_get(&L, iw)->uuid, u) == 0);
    CHECK_EQ_INT(pr_set_model(&L, iw, PR_MODEL_NONE), 0);
    CHECK(strcmp(pr_get(&L, iw)->uuid, u) == 0);
    CHECK_EQ_INT(pr_set_model(&L, iw, PR_MODEL_IWLQ), 0);
    CHECK(strcmp(pr_get(&L, iw)->uuid, u) == 0);
    // Add a Printer… and the Unknown printer get theirs the same way.
    fresh();
    int cs = pr_add_port(&L, PR_MODEL_SWC2500);
    CHECK(strncmp(pr_get(&L, cs)->uuid + 28, "0a02", 4) == 0);
    int o = pr_add_port(&L, PR_MODEL_IW1);
    CHECK(strncmp(pr_get(&L, o)->uuid + 28, "0301", 4) == 0);
    fresh();
    int un = pr_port_seen(&L, PR_PORT_SILENT);
    CHECK(strncmp(pr_get(&L, un)->uuid + 28, "0001", 4) == 0);
    strcpy(u, pr_get(&L, un)->uuid);
    CHECK_EQ_INT(seen_iw(PR_PORT_IW2), un);
    CHECK(strcmp(pr_get(&L, un)->uuid, u) == 0);
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

// A StyleWriter that answers on an empty port (twice, W-1116): a shared record named by its
// model.
static void test_sw_port_found(void)
{
    fresh();
    CHECK_EQ_INT(pr_port_seen_sw(&L, PR_MODEL_SW2), PR_SEEN_AGAIN);
    CHECK_EQ_INT(L.n, 0);
    int found = pr_port_seen_sw(&L, PR_MODEL_SW2);
    const pr_rec_t *r = pr_port(&L);
    CHECK(r != NULL && r->id == found);
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
    CHECK_EQ_INT(pr_port_seen_sw(&L, PR_MODEL_SW2), id);    // again: no churn
    CHECK_EQ_INT(L.n, 1);
    CHECK_EQ_INT(L.n_notes, 0);
    CHECK_EQ_INT(pr_port_seen(&L, PR_PORT_NOTHING), id);
    CHECK_EQ_INT(pr_port_seen(&L, PR_PORT_SILENT), id);     // a lost answer changes nothing
    CHECK_EQ_INT(pr_get(&L, id)->kind, PR_KIND_STYLEWRITER);
    CHECK_EQ_INT(L.n_notes, 0);
    CHECK_EQ_INT(pr_port_seen_sw(&L, PR_MODEL_NONE), -1);   // not a StyleWriter model: ignored
    CHECK_EQ_INT(pr_port_seen_sw(&L, PR_MODEL_IW2), -1);
    CHECK_EQ_INT(L.n_notes, 0);
    CHECK_EQ_INT(pr_get(&L, id)->model, PR_MODEL_SW2);

    // The unknown printer that answers: found, shared, named by its model (test_unknown_goes
    // has the rest; the person's "Something else" is test_something_else_replaced's).
    fresh();
    int u = pr_port_seen(&L, PR_PORT_SILENT);
    CHECK_EQ_INT(notes_of(PR_NOTE_UNKNOWN_PORT), 1);
    CHECK_EQ_INT(seen_sw(PR_MODEL_SWC2500), u);
    CHECK_EQ_INT(pr_get(&L, u)->kind, PR_KIND_STYLEWRITER);
    CHECK(strcmp(pr_get(&L, u)->name, "Color StyleWriter 2500") == 0);
    CHECK_EQ_INT(pr_get(&L, u)->shared, 1);
    CHECK_EQ_INT(notes_of(PR_NOTE_UNKNOWN_PORT), 0);
    // The Apple screen is a StyleWriter's: a record the person makes an ImageWriter loses it.
    strcpy(pr_get(&L, u)->dither, "apple");
    CHECK_EQ_INT(pr_set_model(&L, u, PR_MODEL_IWLQ), 0);
    CHECK(strcmp(pr_get(&L, u)->dither, "atkinson") == 0);
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
    // W-1116: a model that has its own record already is not taken by another; the
    // StyleWriter record it corrects is the one on the port when that is a StyleWriter.
    int a = pr_add_port(&L, PR_MODEL_SWC2200);
    int b = pr_add_port(&L, PR_MODEL_SW2);
    CHECK_EQ_INT(pr_port_sw_found(&L, PR_MODEL_SW2), 0);
    CHECK_EQ_INT(pr_get(&L, a)->model, PR_MODEL_SWC2200);
    CHECK_EQ_INT(pr_get(&L, b)->model, PR_MODEL_SW2);
    seen_sw(PR_MODEL_SW2);
    CHECK_EQ_INT(pr_port_sw_found(&L, PR_MODEL_SW1200), 1);
    CHECK_EQ_INT(pr_get(&L, b)->model, PR_MODEL_SW1200);
    CHECK_EQ_INT(pr_get(&L, a)->model, PR_MODEL_SWC2200);
}

// A model the person picks reaches the same places; the uuid is the record's for its life
// (W-1116: the family no longer makes a new one).
static void test_sw_set_model(void)
{
    fresh();
    int id = seen_iw(PR_PORT_IW2);
    char iw_uuid[46];
    strcpy(iw_uuid, pr_get(&L, id)->uuid);
    CHECK_EQ_INT(pr_set_model(&L, id, PR_MODEL_SWC1500), 0);
    const pr_rec_t *r = pr_get(&L, id);
    CHECK_EQ_INT(r->kind, PR_KIND_STYLEWRITER);
    CHECK(strcmp(r->name, "Color StyleWriter 1500") == 0);
    CHECK(strcmp(r->uuid, iw_uuid) == 0);
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
    // Again, with that record there (W-1116): a second record, and its own id to confirm.
    CHECK_EQ_INT(pr_post(&L, "{\"add_port\":\"StyleWriter 1200\"}", &r), 0);
    CHECK_EQ_INT(L.n, 2);
    CHECK_EQ_INT(pr_get(&L, id)->model, PR_MODEL_SWC2500);
    const pr_rec_t *q = pr_port_of(&L, PR_MODEL_SW1200);
    CHECK(q != NULL && q->id != id);
    CHECK_EQ_INT(r.confirm_sw_id, q->id);
    // An ImageWriter's pick confirms nothing (ESC ? is its own, at line-up).
    CHECK_EQ_INT(pr_post(&L, "{\"add_port\":\"ImageWriter LQ\"}", &r), 0);
    CHECK_EQ_INT(r.confirm_sw_id, -1);
    CHECK_EQ_INT(L.n, 3);
    CHECK_EQ_INT(pr_port(&L)->kind, PR_KIND_STYLEWRITER);     // the oldest stays the port's
    CHECK_EQ_INT(pr_post(&L, "{\"add_port\":\"Color StyleWriter 9000\"}", &r), -1);
    CHECK_EQ_INT(pr_post(&L, "{\"add_port\":\"none\"}", &r), -1);
    // Every one of the seven by the name the page sends: the two there already are refused
    // with the page's sentence, the rest add, up to the list's 8.
    for (size_t i = 0; i < sizeof SW_ALL / sizeof SW_ALL[0]; i++) {
        char body[80];
        snprintf(body, sizeof body, "{\"add_port\":\"%s\"}", SW_NAMES[i]);
        int there = SW_ALL[i] == PR_MODEL_SWC2500 || SW_ALL[i] == PR_MODEL_SW1200;
        CHECK_EQ_INT(pr_post(&L, body, &r), there ? -1 : 0);
        CHECK(pr_port_of(&L, SW_ALL[i]) != NULL);
        if (there) {
            char want[80];
            snprintf(want, sizeof want, "%s is already in Printers.", SW_NAMES[i]);
            CHECK(strcmp(r.err, want) == 0);
        } else {
            CHECK_EQ_INT(r.confirm_sw_id, pr_port_of(&L, SW_ALL[i])->id);
        }
    }
    CHECK_EQ_INT(L.n, PR_MAX);
    CHECK_EQ_INT(pr_port_count(&L), PR_MAX);

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
    int id = seen_iw(PR_PORT_IW2);
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
    int iw = seen_iw(PR_PORT_IW2);
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
    seen_sw(PR_MODEL_SWC2200);
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
    seen_iw(PR_PORT_IW2);
    pr_at_seen(&L, PR_WHERE_CABLE, P("Office"), P("LaserWriter"), 0);
    memset(live, 0, sizeof live);
    live[0].cart = live[1].cart = 2;
    pr_json(&L, live, buf, sizeof buf);
    CHECK(strstr(buf, "cartridge") == NULL);
    // The record is the same size as before: the cartridge is not stored in it.
    CHECK_EQ_INT(sizeof(pr_rec_t), 284);
}

// The fault a StyleWriter holds, named for the page in its own words.
static void test_sw_fault(void)
{
    // Each state the session reports, as the page's word for it.
    CHECK_EQ_INT(pr_fault_from_sw(SWS_READY), PR_FAULT_NONE);
    CHECK_EQ_INT(pr_fault_from_sw(SWS_PRINTING), PR_FAULT_NONE);
    CHECK_EQ_INT(pr_fault_from_sw(SWS_OFFLINE), PR_FAULT_NONE);   // offline is not a fault
    CHECK_EQ_INT(pr_fault_from_sw(SWS_NO_PAPER), PR_FAULT_PAPER);
    CHECK_EQ_INT(pr_fault_from_sw(SWS_JAM), PR_FAULT_JAM);
    CHECK_EQ_INT(pr_fault_from_sw(SWS_CART_MISSING), PR_FAULT_CARTRIDGE);
    CHECK_EQ_INT(pr_fault_from_sw(SWS_CART_JAM), PR_FAULT_CART_JAM);
    CHECK_EQ_INT(pr_fault_from_sw(SWS_INK_OUT), PR_FAULT_INK);
    CHECK_EQ_INT(pr_fault_from_sw(SWS_DOOR), PR_FAULT_COVER);
    CHECK_EQ_INT(pr_fault_from_sw(SWS_WASTE), PR_FAULT_WASTE);
    CHECK_EQ_INT(pr_fault_from_sw(SWS_FAULT), PR_FAULT_OTHER);
    // Every fault state is a fault, and no other state is one (the board sets fault with attention).
    for (int st = SWS_READY; st <= SWS_FAULT; st++)
        CHECK_EQ_INT(pr_fault_from_sw(st) != PR_FAULT_NONE,
                     st != SWS_READY && st != SWS_PRINTING && st != SWS_OFFLINE);

    static const struct { int fault; const char *word; } K[] = {
        { PR_FAULT_PAPER, "paper" }, { PR_FAULT_JAM, "jam" }, { PR_FAULT_CARTRIDGE, "cartridge" },
        { PR_FAULT_CART_JAM, "cartridge-jam" }, { PR_FAULT_INK, "ink" }, { PR_FAULT_COVER, "cover" },
        { PR_FAULT_WASTE, "waste" }, { PR_FAULT_OTHER, "other" } };
    fresh();
    seen_sw(PR_MODEL_SWC2400);
    pr_live_t live[PR_MAX];
    memset(live, 0, sizeof live);
    static char buf[4096];
    live[0].present = 1;
    live[0].attention = 1;
    pr_json(&L, live, buf, sizeof buf);
    CHECK(strstr(buf, "\"fault\"") == NULL);                 // none held: not said
    for (size_t k = 0; k < sizeof K / sizeof K[0]; k++) {
        char want[40];
        snprintf(want, sizeof want, "\"fault\":\"%s\"", K[k].word);
        live[0].fault = (uint8_t)K[k].fault;
        pr_json(&L, live, buf, sizeof buf);
        CHECK(strstr(buf, want) != NULL);
        CHECK(strstr(buf, "\"state\":\"attention\"") != NULL);
        live[0].cart = 2;                                      // beside the cartridge, both valid
        pr_json(&L, live, buf, sizeof buf);
        CHECK(strstr(buf, want) != NULL);
        CHECK(strstr(buf, "\"cartridge\":\"colour\"") != NULL);
        live[0].cart = 0;
        // said once
        CHECK(strstr(strstr(buf, want) + 1, "\"fault\"") == NULL);
    }
    live[0].fault = 99;                                        // out of range: not said
    pr_json(&L, live, buf, sizeof buf);
    CHECK(strstr(buf, "\"fault\"") == NULL);

    // Only a StyleWriter record carries it: not an ImageWriter, not a LaserWriter.
    fresh();
    seen_iw(PR_PORT_IW2);
    pr_at_seen(&L, PR_WHERE_CABLE, P("Office"), P("LaserWriter"), 0);
    memset(live, 0, sizeof live);
    live[0].fault = live[1].fault = PR_FAULT_PAPER;
    pr_json(&L, live, buf, sizeof buf);
    CHECK(strstr(buf, "fault") == NULL);
    CHECK_EQ_INT(sizeof(pr_rec_t), 284);                       // and it is not stored
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
    seen_sw(PR_MODEL_SW1200);
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
    int id = seen_iw(PR_PORT_IW2);
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
    seen_sw(PR_MODEL_SW2);
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
// W-1116: version 3 is a second port record (test_blob_v3); the layout is still the same.
static void test_sw_blob(void)
{
    enum { HDR = 6 };
    static uint8_t buf[4096], v1[4096], v2[4096], bad[4096];
    CHECK_EQ_INT(PR_BLOB_VER, 3);

    // A list with a StyleWriter in it round-trips as version 2, its notices with it.
    fresh();
    seen_sw(PR_MODEL_SWC2400);
    int lw = pr_at_seen(&L, PR_WHERE_CABLE, P("Office"), P("LaserWriter"), 0);
    lw_info_t info;
    lw_info_default(&info);
    pr_at_queried(&L, lw, 1, &info);
    int back = pr_at_seen(&L, PR_WHERE_TUNNEL, P("Back"), P("LaserWriter"), 0);
    for (int i = 0; i < 3; i++) pr_at_queried(&L, back, 0, NULL);   // a "no answer" notice
    CHECK_EQ_INT(L.n_notes, 1);
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
    memcpy(v2, buf, n);
    size_t n2 = n;

    // A version 1 list (what every board has now) loads, nothing lost, and is saved again as 1.
    fresh();
    int a = seen_iw(PR_PORT_IWLQ);
    pr_rename(&L, a, "Desk");
    pr_at_queried(&L, pr_at_seen(&L, PR_WHERE_TUNNEL, P("Back"), P("LaserWriter"), 0), 1, &info);
    n = pr_save(&L, v1, sizeof v1);
    CHECK_EQ_INT(v1[2], 1);                                  // nothing here needs 2
    pr_init(&M, MAC, 1);
    CHECK_EQ_INT(pr_load(&M, v1, n), 0);
    CHECK_EQ_INT(M.n, L.n);
    CHECK(memcmp(M.r, L.r, sizeof L.r[0] * L.n) == 0);
    CHECK_EQ_INT(pr_blob_newer(v1, n), 0);
    size_t m = pr_save(&M, buf, sizeof buf);
    CHECK_EQ_INT(m, n);
    CHECK(memcmp(buf, v1, n) == 0);

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

    // Version 4 is a newer firmware's: not read here, and kept.
    memcpy(bad, buf, n);
    bad[2] = 4;
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
    CHECK_EQ_INT(v2[2], 2);
    CHECK(v2[2] > 1);
    (void)n2;

    // A note whose model nobody has is refused with the rest (no notice made now names a
    // model, 3.15; one stored by an older build still has the byte).
    fresh();
    int iw = seen_iw(PR_PORT_IW2);
    L.notes[0] = (pr_note_t){ PR_NOTE_TOO_MANY, (uint8_t)iw, PR_MODEL_SW2 };
    L.n_notes = 1;
    n = pr_save(&L, buf, sizeof buf);
    CHECK_EQ_INT(buf[2], 2);
    memcpy(bad, buf, n);
    bad[n - 1] = PR_MODEL_SWC2500 + 1;                       // the note's model, its last byte
    CHECK_EQ_INT(pr_load(&M, bad, n), -1);
    bad[2] = 1;
    bad[n - 1] = PR_MODEL_SW2;                               // a StyleWriter in a version 1 note
    CHECK_EQ_INT(pr_load(&M, bad, n), -1);
}

// What a version 1 reader (a firmware before W-1103, as an OTA rollback brings back) accepts,
// by the limits it had: its version, the old kinds and models, notes that name an ImageWriter.
static int old_reader_accepts(const uint8_t *b, size_t n)
{
    enum { HDR = 6 };
    if (n < HDR || b[0] != 'P' || b[1] != 'R' || b[2] != 1) return 0;
    uint8_t cnt = b[3], notes = b[5];
    if (cnt > PR_MAX || notes > PR_NOTES_MAX) return 0;
    if (n != HDR + (size_t)cnt * sizeof(pr_rec_t) + (size_t)notes * sizeof(pr_note_t)) return 0;
    for (int i = 0; i < cnt; i++) {
        pr_rec_t r;
        memcpy(&r, b + HDR + (size_t)i * sizeof r, sizeof r);
        if (r.kind > PR_KIND_AT_IMAGEWRITER || r.model > PR_MODEL_IW1) return 0;
    }
    for (int i = 0; i < notes; i++) {
        pr_note_t nt;
        memcpy(&nt, b + HDR + (size_t)cnt * sizeof(pr_rec_t) + (size_t)i * sizeof nt, sizeof nt);
        if (nt.kind > PR_NOTE_TOO_MANY || nt.model > PR_MODEL_IW1) return 0;
    }
    return 1;
}

// The list is stored as version 1 while nothing in it needs 2, so a rollback to a firmware
// from before StyleWriters (the W-1059 trial) reads it and a person's printers keep their
// identity on their phones. A StyleWriter in a record, or a notice that names one, makes it 2.
static void test_sw_blob_stamp(void)
{
    static uint8_t buf[4096], again[4096];
    lw_info_t info;
    lw_info_default(&info);

    // Nothing at all, an ImageWriter, a LaserWriter, both: version 1, read by the old limits,
    // and read back here identical.
    fresh();
    size_t n = pr_save(&L, buf, sizeof buf);
    CHECK_EQ_INT(buf[2], 1);
    CHECK(old_reader_accepts(buf, n));
    int iw = seen_iw(PR_PORT_IWLQ);
    pr_rename(&L, iw, "Desk");
    pr_dismiss(&L, 0);                                       // no notice: nothing
    int lw = pr_at_seen(&L, PR_WHERE_CABLE, P("Office"), P("LaserWriter"), 0);
    pr_at_queried(&L, lw, 1, &info);
    int ak = pr_at_seen(&L, PR_WHERE_TUNNEL, P("Kitchen"), P("ImageWriter"), 0);
    (void)ak;
    pr_port_seen(&L, PR_PORT_IW2);                           // another ImageWriter once: nothing
    CHECK_EQ_INT(L.n_notes, 0);
    CHECK_EQ_INT(L.n, 3);
    n = pr_save(&L, buf, sizeof buf);
    CHECK_EQ_INT(buf[2], 1);
    CHECK(old_reader_accepts(buf, n));
    pr_list_t M;
    pr_init(&M, MAC, 1);
    CHECK_EQ_INT(pr_load(&M, buf, n), 0);
    CHECK_EQ_INT(M.n, L.n);
    CHECK(memcmp(M.r, L.r, sizeof L.r[0] * L.n) == 0);
    CHECK_EQ_INT(M.n_notes, L.n_notes);
    CHECK(memcmp(M.notes, L.notes, sizeof L.notes[0] * L.n_notes) == 0);
    CHECK_EQ_INT(M.next_id, L.next_id);
    CHECK_EQ_INT(pr_save(&M, again, sizeof again), n);       // and saved again the same, byte for byte
    CHECK(memcmp(buf, again, n) == 0);

    // A StyleWriter record makes it 2: the old limits refuse it (the old firmware would keep it).
    CHECK_EQ_INT(pr_set_model(&L, iw, PR_MODEL_SW2), 0);
    n = pr_save(&L, buf, sizeof buf);
    CHECK_EQ_INT(buf[2], 2);
    CHECK(!old_reader_accepts(buf, n));
    pr_init(&M, MAC, 1);
    CHECK_EQ_INT(pr_load(&M, buf, n), 0);
    CHECK(memcmp(M.r, L.r, sizeof L.r[0] * L.n) == 0);
    // Taking it away makes it 1 again, by removing the record or by the person's model.
    pr_list_t keep = L;
    CHECK_EQ_INT(pr_remove(&L, iw), 0);
    n = pr_save(&L, buf, sizeof buf);
    CHECK_EQ_INT(buf[2], 1);
    CHECK(old_reader_accepts(buf, n));
    L = keep;
    CHECK_EQ_INT(pr_set_model(&L, iw, PR_MODEL_IW2), 0);
    n = pr_save(&L, buf, sizeof buf);
    CHECK_EQ_INT(buf[2], 1);
    CHECK(old_reader_accepts(buf, n));

    // The same through the front door: a StyleWriter that answers, then the person deletes it.
    fresh();
    seen_sw(PR_MODEL_SWC2200);
    n = pr_save(&L, buf, sizeof buf);
    CHECK_EQ_INT(buf[2], 2);
    CHECK_EQ_INT(pr_remove(&L, pr_port(&L)->id), 0);
    n = pr_save(&L, buf, sizeof buf);
    CHECK_EQ_INT(buf[2], 1);
    CHECK(old_reader_accepts(buf, n));

    // A notice that names a StyleWriter is enough on its own (the old firmware would take its
    // model for an ImageWriter's); none made now carries a model (W-1116), but one from an
    // older build may. Dismissed, 1 again.
    fresh();
    iw = seen_iw(PR_PORT_IW2);
    L.notes[0] = (pr_note_t){ PR_NOTE_TOO_MANY, (uint8_t)iw, PR_MODEL_SW1200 };
    L.n_notes = 1;
    n = pr_save(&L, buf, sizeof buf);
    CHECK_EQ_INT(buf[2], 2);
    CHECK(!old_reader_accepts(buf, n));
    pr_dismiss(&L, 0);
    CHECK_EQ_INT(L.n_notes, 0);
    n = pr_save(&L, buf, sizeof buf);
    CHECK_EQ_INT(buf[2], 1);
    CHECK(old_reader_accepts(buf, n));

    // Everything in version 1's range still loads whichever way it is stamped by hand.
    fresh();
    seen_iw(PR_PORT_IW2);
    n = pr_save(&L, buf, sizeof buf);
    buf[2] = 2;
    pr_init(&M, MAC, 1);
    CHECK_EQ_INT(pr_load(&M, buf, n), 0);
    buf[2] = 3;
    CHECK_EQ_INT(pr_load(&M, buf, n), 0);
}

// W-1116 (review focus 3): a board's one-printer list from before is stored as it was. A
// version 1 list (an ImageWriter) and a version 2 one (a StyleWriter) each load, keep their
// uuid and name, and save again byte for byte at their own version.
static void test_one_port_unchanged(void)
{
    enum { HDR = 6 };
    static uint8_t v1[4096], v2[4096], again[4096];
    lw_info_t info;
    lw_info_default(&info);
    fresh();
    int iw = seen_iw(PR_PORT_IW2);
    pr_rename(&L, iw, "Desk");
    int lw = pr_at_seen(&L, PR_WHERE_CABLE, P("Office"), P("LaserWriter"), 0);
    pr_at_queried(&L, lw, 1, &info);
    size_t n1 = pr_save(&L, v1, sizeof v1);
    CHECK_EQ_INT(v1[2], 1);
    CHECK(old_reader_accepts(v1, n1));
    pr_list_t M;
    pr_init(&M, MAC, 1);
    CHECK_EQ_INT(pr_load(&M, v1, n1), 0);
    CHECK_EQ_INT(pr_port_count(&M), 1);
    CHECK(strcmp(pr_get(&M, iw)->uuid, pr_get(&L, iw)->uuid) == 0);
    CHECK(strcmp(pr_get(&M, iw)->name, "Desk") == 0);
    CHECK_EQ_INT(pr_save(&M, again, sizeof again), n1);
    CHECK(memcmp(again, v1, n1) == 0);

    // The StyleWriter branch's version 2, with the uuid form it made (the family tag alone).
    fresh();
    int sw = seen_sw(PR_MODEL_SW2);
    pr_rename(&L, sw, "Den");
    size_t n2 = pr_save(&L, v2, sizeof v2);
    CHECK_EQ_INT(v2[2], 2);
    memcpy(v2 + HDR + offsetof(pr_rec_t, uuid) + 28, "0002", 4);
    pr_init(&M, MAC, 1);
    CHECK_EQ_INT(pr_load(&M, v2, n2), 0);
    CHECK_EQ_INT(pr_port_count(&M), 1);
    CHECK(strncmp(pr_get(&M, sw)->uuid + 28, "0002", 4) == 0);
    CHECK(strcmp(pr_get(&M, sw)->name, "Den") == 0);
    CHECK_EQ_INT(pr_get(&M, sw)->model, PR_MODEL_SW2);
    CHECK_EQ_INT(pr_save(&M, again, sizeof again), n2);
    CHECK(memcmp(again, v2, n2) == 0);
    // Its printer answering, a job printing, a look: still as it was.
    CHECK_EQ_INT(pr_port_seen_sw(&M, PR_MODEL_SW2), sw);
    pr_port_answered(&M, PR_HEARD_STYLEWRITER);
    CHECK_EQ_INT(pr_port_seen(&M, PR_PORT_NOTHING), sw);
    CHECK_EQ_INT(pr_save(&M, again, sizeof again), n2);
    CHECK(memcmp(again, v2, n2) == 0);
}

// W-1116 (review focus 5): two port records are version 3. A firmware before W-1116 refuses
// it (a PR_BLOB_VER 1 reader by its limits, a PR_BLOB_VER 2 reader by the stamp alone) and
// keeps it unwritten (pr_blob_newer) for the next update; here it reads back whole.
static void test_blob_v3(void)
{
    static uint8_t buf[4096], again[4096], bad[4096];
    CHECK_EQ_INT(PR_BLOB_VER, 3);
    fresh();
    int iw = seen_iw(PR_PORT_IW2);
    size_t n = pr_save(&L, buf, sizeof buf);
    CHECK_EQ_INT(buf[2], 1);
    int lq = seen_iw(PR_PORT_IWLQ);
    n = pr_save(&L, buf, sizeof buf);
    CHECK_EQ_INT(buf[2], 3);                                 // two ImageWriters: 3, never 2
    CHECK(!old_reader_accepts(buf, n));
    CHECK(buf[2] > 2);                                       // a PR_BLOB_VER 2 reader: newer
    CHECK_EQ_INT(pr_blob_newer(buf, n), 0);                  // ours
    pr_list_t M;
    pr_init(&M, MAC, 1);
    CHECK_EQ_INT(pr_load(&M, buf, n), 0);
    CHECK_EQ_INT(M.n, 2);
    CHECK_EQ_INT(pr_port_count(&M), 2);
    CHECK(memcmp(M.r, L.r, sizeof L.r[0] * L.n) == 0);
    CHECK_EQ_INT(M.next_id, L.next_id);
    CHECK_EQ_INT(pr_save(&M, again, sizeof again), n);
    CHECK(memcmp(again, buf, n) == 0);
    CHECK(pr_port(&M) == pr_get(&M, iw));                    // the oldest first, as stored
    // Back to one port record: 1 again; a StyleWriter beside an ImageWriter: 3; alone: 2.
    CHECK_EQ_INT(pr_remove(&L, lq), 0);
    n = pr_save(&L, buf, sizeof buf);
    CHECK_EQ_INT(buf[2], 1);
    seen_sw(PR_MODEL_SW2);
    n = pr_save(&L, buf, sizeof buf);
    CHECK_EQ_INT(buf[2], 3);
    CHECK_EQ_INT(pr_remove(&L, iw), 0);
    n = pr_save(&L, buf, sizeof buf);
    CHECK_EQ_INT(buf[2], 2);
    // An Unknown printer or a Something else beside a record is a second port record too.
    int sw = pr_port(&L)->id;
    int x = pr_add_port(&L, PR_MODEL_IW1);
    CHECK_EQ_INT(pr_set_model(&L, x, PR_MODEL_NONE), 0);
    n = pr_save(&L, buf, sizeof buf);
    CHECK_EQ_INT(buf[2], 3);
    CHECK_EQ_INT(pr_remove(&L, sw), 0);
    n = pr_save(&L, buf, sizeof buf);
    CHECK_EQ_INT(buf[2], 1);
    // AppleTalk printers never count.
    lw_info_t info;
    lw_info_default(&info);
    pr_at_queried(&L, pr_at_seen(&L, PR_WHERE_CABLE, P("Office"), P("LaserWriter"), 0), 1, &info);
    pr_at_queried(&L, pr_at_seen(&L, PR_WHERE_TUNNEL, P("Back"), P("LaserWriter"), 0), 1, &info);
    n = pr_save(&L, buf, sizeof buf);
    CHECK_EQ_INT(buf[2], 1);
    // A version 3 stamp is read by version 2's limits: a kind or model nobody has is refused.
    fresh();
    seen_iw(PR_PORT_IW2);
    seen_iw(PR_PORT_IWLQ);
    n = pr_save(&L, buf, sizeof buf);
    memcpy(bad, buf, n);
    bad[6 + offsetof(pr_rec_t, kind)] = PR_KIND_STYLEWRITER + 1;
    CHECK_EQ_INT(pr_load(&M, bad, n), -1);
    memcpy(bad, buf, n);
    bad[6 + offsetof(pr_rec_t, model)] = PR_MODEL_SWC2500 + 1;
    CHECK_EQ_INT(pr_load(&M, bad, n), -1);
}

// A "changed" notice stored by a firmware before W-1116 is dropped on load, as a "found" one
// is, and nothing of who answered on the port is rebuilt from it. The page shows none, and
// a note is only ever dismissed.
static void test_load_drops_changed(void)
{
    static uint8_t buf[4096];
    static char js[4096];
    fresh();
    int iw = seen_iw(PR_PORT_IW2);
    L.notes[0] = (pr_note_t){ PR_NOTE_CHANGED, (uint8_t)iw, PR_MODEL_SW2 };
    L.notes[1] = (pr_note_t){ PR_NOTE_TOO_MANY, (uint8_t)iw, 0 };
    L.notes[2] = (pr_note_t){ PR_NOTE_CHANGED, (uint8_t)iw, PR_MODEL_IWLQ };
    L.n_notes = 3;
    size_t n = pr_save(&L, buf, sizeof buf);
    CHECK_EQ_INT(buf[2], 2);                                 // as the StyleWriter branch stored it
    pr_list_t M;
    pr_init(&M, MAC, 1);
    CHECK_EQ_INT(pr_port_seen_sw(&M, PR_MODEL_SW2), PR_SEEN_AGAIN);   // something pending, to go too
    CHECK_EQ_INT(pr_load(&M, buf, n), 0);
    CHECK_EQ_INT(M.n_notes, 1);
    CHECK_EQ_INT(M.notes[0].kind, PR_NOTE_TOO_MANY);
    CHECK_EQ_INT(pr_port_heard(&M), PR_HEARD_NONE);
    CHECK_EQ_INT(M.heard_model, 0);
    CHECK(pr_port_current(&M) == pr_port(&M));
    CHECK_EQ_INT(pr_port_other_family(&M, PR_HEARD_IMAGEWRITER), 0);
    CHECK_EQ_INT(pr_port_other_family(&M, PR_HEARD_STYLEWRITER), 0);
    CHECK_EQ_INT(pr_port_seen_sw(&M, PR_MODEL_SW2), PR_SEEN_AGAIN);
    CHECK_EQ_INT(M.n, 1);
    pr_live_t live[PR_MAX];
    memset(live, 0, sizeof live);
    pr_json(&M, live, js, sizeof js);
    CHECK(strstr(js, "\"changed\"") == NULL);
    CHECK(strstr(js, "{\"kind\":\"too_many\"") != NULL);
    pr_post_t o;
    CHECK_EQ_INT(pr_post(&M, "{\"note\":0,\"dismiss\":true}", &o), 0);
    CHECK_EQ_INT(M.n_notes, 0);
    CHECK_EQ_INT(o.changed, 1);
    CHECK_EQ_INT(pr_post(&M, "{\"note\":0,\"dismiss\":true}", &o), -1);
    CHECK(strcmp(o.err, "No such notice.") == 0);
    // A version 1 list with such a note (before StyleWriters, an ImageWriter LQ's) the same.
    fresh();
    iw = seen_iw(PR_PORT_IW2);
    L.notes[0] = (pr_note_t){ PR_NOTE_CHANGED, (uint8_t)iw, PR_MODEL_IWLQ };
    L.n_notes = 1;
    n = pr_save(&L, buf, sizeof buf);
    CHECK_EQ_INT(buf[2], 1);
    pr_init(&M, MAC, 1);
    CHECK_EQ_INT(pr_load(&M, buf, n), 0);
    CHECK_EQ_INT(M.n_notes, 0);
}

// W-1116 (review focus 2): what an ImageWriter job on record `id` does after its own ESC ?
// (and the plain question): go when its own model answered, wait while nothing answers,
// and stop when another printer is there. The original ImageWriter's answer is its line up
// and nothing said.
static void test_job_gate(void)
{
    fresh();
    int iw = seen_iw(PR_PORT_IW2);
    int lq = seen_iw(PR_PORT_IWLQ);
    int iw1 = pr_add_port(&L, PR_MODEL_IW1);
    int sw = pr_add_port(&L, PR_MODEL_SW2);
    CHECK_EQ_INT(pr_job_gate(&L, iw, PR_HEARD_IMAGEWRITER, PR_MODEL_IW2, 1), PR_GATE_GO);
    CHECK_EQ_INT(pr_job_gate(&L, iw, PR_HEARD_IMAGEWRITER, PR_MODEL_IWLQ, 1), PR_GATE_OTHER);
    CHECK_EQ_INT(pr_job_gate(&L, lq, PR_HEARD_IMAGEWRITER, PR_MODEL_IW2, 1), PR_GATE_OTHER);
    CHECK_EQ_INT(pr_job_gate(&L, lq, PR_HEARD_IMAGEWRITER, PR_MODEL_IWLQ, 1), PR_GATE_GO);
    CHECK_EQ_INT(pr_job_gate(&L, iw, PR_HEARD_STYLEWRITER, PR_MODEL_SW2, 1), PR_GATE_OTHER);
    CHECK_EQ_INT(pr_job_gate(&L, iw, PR_HEARD_STYLEWRITER, PR_MODEL_SW2, 0), PR_GATE_OTHER);
    CHECK_EQ_INT(pr_job_gate(&L, iw, PR_HEARD_STYLEWRITER, PR_MODEL_NONE, 1), PR_GATE_OTHER);
    CHECK_EQ_INT(pr_job_gate(&L, iw, PR_HEARD_NONE, PR_MODEL_NONE, 0), PR_GATE_WAIT);
    CHECK_EQ_INT(pr_job_gate(&L, iw, PR_HEARD_NONE, PR_MODEL_NONE, 1), PR_GATE_WAIT);   // up, nothing said: not a II
    CHECK_EQ_INT(pr_job_gate(&L, lq, PR_HEARD_NONE, PR_MODEL_NONE, 1), PR_GATE_WAIT);
    // The original ImageWriter has no self-ID: the line up and nothing said is its answer.
    CHECK_EQ_INT(pr_job_gate(&L, iw1, PR_HEARD_NONE, PR_MODEL_NONE, 1), PR_GATE_GO);
    CHECK_EQ_INT(pr_job_gate(&L, iw1, PR_HEARD_NONE, PR_MODEL_NONE, 0), PR_GATE_WAIT);
    CHECK_EQ_INT(pr_job_gate(&L, iw1, PR_HEARD_IMAGEWRITER, PR_MODEL_IW1, 1), PR_GATE_GO);
    CHECK_EQ_INT(pr_job_gate(&L, iw1, PR_HEARD_IMAGEWRITER, PR_MODEL_IW2, 1), PR_GATE_OTHER);
    CHECK_EQ_INT(pr_job_gate(&L, iw1, PR_HEARD_STYLEWRITER, PR_MODEL_SW2, 1), PR_GATE_OTHER);
    // Not an ImageWriter job: a StyleWriter's record, no such record, an AppleTalk printer's.
    CHECK_EQ_INT(pr_job_gate(&L, sw, PR_HEARD_STYLEWRITER, PR_MODEL_SW2, 1), PR_GATE_OTHER);
    CHECK_EQ_INT(pr_job_gate(&L, 99, PR_HEARD_IMAGEWRITER, PR_MODEL_IW2, 1), PR_GATE_OTHER);
    int lw = pr_at_seen(&L, PR_WHERE_CABLE, P("Office"), P("LaserWriter"), 0);
    CHECK_EQ_INT(pr_job_gate(&L, lw, PR_HEARD_IMAGEWRITER, PR_MODEL_IW2, 1), PR_GATE_OTHER);
    // The answer the job just got decides, not the list's last.
    seen_sw(PR_MODEL_SW2);
    CHECK_EQ_INT(pr_port_heard(&L), PR_HEARD_STYLEWRITER);
    CHECK_EQ_INT(pr_job_gate(&L, iw, PR_HEARD_IMAGEWRITER, PR_MODEL_IW2, 1), PR_GATE_GO);
    CHECK_EQ_INT(pr_job_gate(&L, iw, PR_HEARD_NONE, PR_MODEL_NONE, 0), PR_GATE_WAIT);
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
    test_port_two_records();
    test_twice_seen();
    test_add_port_adds();
    test_unknown_goes();
    test_something_else_replaced();
    test_port_uuid_per_model();
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
    test_sw_found();
    test_sw_set_model();
    test_sw_add_port();
    test_sw_clean();
    test_sw_apple_screen();
    test_sw_json();
    test_sw_fault();
    test_sw_test_target();
    test_sw_offers();
    test_sw_box_state();
    test_sw_blob();
    test_sw_blob_stamp();
    test_one_port_unchanged();
    test_blob_v3();
    test_load_drops_changed();
    test_job_gate();
    TEST_END;
}
