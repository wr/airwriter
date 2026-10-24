// SPDX-License-Identifier: GPL-3.0-or-later
#include "printers_core.h"
#include "lw_caps.h"
#include "lt_proto.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>

#define PR_TOP_DEFAULT (-144)      // CONFIG_AIRWRITER_TOP_MARGIN's default, 1/144 in
static const char UNKNOWN_NAME[] = "Unknown printer";

const char *pr_where_name(int w)
{
    static const char *const n[] = { "port", "cable", "network", "tunnel", "zone" };
    return w >= 0 && w <= PR_WHERE_ZONE ? n[w] : "?";
}

const char *pr_kind_name(int k)
{
    static const char *const n[] = { "unknown", "imagewriter", "laserwriter",
                                     "appletalk-imagewriter" };
    return k >= 0 && k <= PR_KIND_AT_IMAGEWRITER ? n[k] : "?";
}

const char *pr_model_name(int m)
{
    static const char *const n[] = { "", "ImageWriter II", "ImageWriter LQ", "ImageWriter" };
    return m >= 0 && m <= PR_MODEL_IW1 ? n[m] : "";
}

void pr_init(pr_list_t *l, const uint8_t mac[6], uint16_t epoch)
{
    memset(l, 0, sizeof *l);
    memcpy(l->mac, mac, 6);
    l->epoch = epoch;
    l->next_id = 1;
}

// ------------------------------------------------------------- lookups

int pr_index(const pr_list_t *l, int id)
{
    for (int i = 0; i < l->n; i++) if (l->r[i].id == id) return i;
    return -1;
}

pr_rec_t *pr_get(pr_list_t *l, int id)
{
    int i = pr_index(l, id);
    return i < 0 ? NULL : &l->r[i];
}

const pr_rec_t *pr_port(const pr_list_t *l)
{
    for (int i = 0; i < l->n; i++) if (l->r[i].where == PR_WHERE_PORT) return &l->r[i];
    return NULL;
}

int pr_port_is_imagewriter(const pr_list_t *l)
{
    const pr_rec_t *p = pr_port(l);
    return p && p->kind == PR_KIND_IMAGEWRITER;
}

static int at_shared(const pr_list_t *l)
{
    int c = 0;
    for (int i = 0; i < l->n; i++) if (l->r[i].where != PR_WHERE_PORT && l->r[i].shared) c++;
    return c;
}

static int can_share(const pr_list_t *l, const pr_rec_t *r)
{
    return r->where == PR_WHERE_PORT || at_shared(l) < PR_AT_SHARE_MAX;
}

// NBP compares names without regard to ASCII case.
static int pstr_eq(const uint8_t *a, const uint8_t *b)
{
    if (a[0] != b[0]) return 0;
    for (int i = 1; i <= a[0]; i++)
        if (tolower(a[i]) != tolower(b[i])) return 0;
    return 1;
}

static int pstr_is(const uint8_t *p, const char *s)
{
    size_t n = strlen(s);
    if (p[0] != n) return 0;
    for (size_t i = 0; i < n; i++) if (tolower(p[1 + i]) != tolower((unsigned char)s[i])) return 0;
    return 1;
}

// ------------------------------------------------------------- notes

static void note_drop(pr_list_t *l, int i)
{
    memmove(&l->notes[i], &l->notes[i + 1], (size_t)(l->n_notes - i - 1) * sizeof l->notes[0]);
    l->n_notes--;
}

static void note_add(pr_list_t *l, int kind, int id, int model)
{
    for (int i = 0; i < l->n_notes; i++)
        if (l->notes[i].kind == kind && l->notes[i].id == id && l->notes[i].model == model) return;
    if (l->n_notes == PR_NOTES_MAX) note_drop(l, 0);          // the oldest goes
    l->notes[l->n_notes++] = (pr_note_t){ (uint8_t)kind, (uint8_t)id, (uint8_t)model };
}

static void notes_clear(pr_list_t *l, int id, int kind)     // kind -1: every kind
{
    for (int i = l->n_notes - 1; i >= 0; i--)
        if (l->notes[i].id == id && (kind < 0 || l->notes[i].kind == kind)) note_drop(l, i);
}

// ------------------------------------------------------------- names

// The name a printer gives itself: a port printer's model, an AppleTalk
// printer's NBP name.
static void own_name(const pr_rec_t *r, char *out, size_t cap)
{
    if (r->where != PR_WHERE_PORT) nbp_utf8(out, cap, r->nbp);
    else snprintf(out, cap, "%s", r->kind == PR_KIND_UNKNOWN ? UNKNOWN_NAME : pr_model_name(r->model));
}

static int name_taken(const pr_list_t *l, int self, const char *name)
{
    for (int i = 0; i < l->n; i++)
        if (l->r[i].id != self && !strcasecmp(l->r[i].name, name)) return 1;
    return 0;
}

// Cut to fit without splitting a UTF-8 character.
static void cut_utf8(char *s, size_t max)
{
    size_t n = strlen(s);
    if (n <= max) return;
    while (max && ((unsigned char)s[max] & 0xC0) == 0x80) max--;
    s[max] = '\0';
}

static void set_name(pr_list_t *l, pr_rec_t *r, const char *want)
{
    char base[PR_NAME_MAX], cand[PR_NAME_MAX];
    snprintf(base, sizeof base, "%s", want);
    cut_utf8(base, PR_NAME_MAX - 1);
    snprintf(cand, sizeof cand, "%s", base);
    for (int k = 2; name_taken(l, r->id, cand) && k < 100; k++) {
        char b[PR_NAME_MAX];
        snprintf(b, sizeof b, "%s", base);
        cut_utf8(b, PR_NAME_MAX - 1 - (k < 10 ? 2 : 3));
        snprintf(cand, sizeof cand, "%s %d", b, k);
    }
    snprintf(r->name, sizeof r->name, "%s", cand);
}

static int name_is_own(const pr_rec_t *r)
{
    char own[PR_NAME_MAX];
    own_name(r, own, sizeof own);
    size_t n = strlen(own);
    // "ImageWriter II" or "ImageWriter II 2": still the printer's own.
    return !strncasecmp(r->name, own, n) &&
           (r->name[n] == '\0' || (r->name[n] == ' ' && isdigit((unsigned char)r->name[n + 1])));
}

// ------------------------------------------------------------- records

static pr_rec_t *rec_new(pr_list_t *l)
{
    if (l->n == PR_MAX) return NULL;
    uint8_t id = l->next_id;
    while (id == 0 || pr_index(l, id) >= 0) id++;
    l->next_id = (uint8_t)(id + 1);
    pr_rec_t *r = &l->r[l->n++];
    memset(r, 0, sizeof *r);
    r->id = id;
    snprintf(r->dither, sizeof r->dither, "atkinson");
    r->top_margin = PR_TOP_DEFAULT;
    lw_info_default(&r->lw);
    return r;
}

// A new port printer's identity: W-1082's form, the port's own tag in place
// of a name's hash.
static void port_uuid(const pr_list_t *l, char out[46])
{
    static const uint8_t tag[] = { 1, 1 };
    lw_uuid(out, l->mac, l->epoch, tag);
    memcpy(out + strlen("urn:uuid:41575254-4c57-0000-"), "0001", 4);
}

static void announce(pr_list_t *l, pr_rec_t *r)
{
    if (r->announced) return;
    r->announced = 1;
    if (can_share(l, r)) {
        r->shared = 1;
        note_add(l, PR_NOTE_FOUND, r->id, 0);
    } else {
        note_add(l, PR_NOTE_TOO_MANY, r->id, 0);
    }
}

static void become_imagewriter(pr_list_t *l, pr_rec_t *r, int model)
{
    int own = name_is_own(r);
    r->kind = PR_KIND_IMAGEWRITER;
    r->model = (uint8_t)model;
    r->identified = 1;
    if (own) {
        char n[PR_NAME_MAX];
        own_name(r, n, sizeof n);
        set_name(l, r, n);
    }
    notes_clear(l, r->id, PR_NOTE_UNKNOWN_PORT);
    notes_clear(l, r->id, PR_NOTE_CHANGED);
}

int pr_port_seen(pr_list_t *l, pr_port_seen_t what)
{
    pr_rec_t *p = (pr_rec_t *)pr_port(l);
    if (what == PR_PORT_NOTHING) return p ? p->id : -1;
    if (what == PR_PORT_SILENT) {
        if (p) return p->id;              // an original ImageWriter, a lost answer: no churn
        p = rec_new(l);
        if (!p) return -1;
        p->where = PR_WHERE_PORT;
        p->kind = PR_KIND_UNKNOWN;
        p->found_auto = 1;
        port_uuid(l, p->uuid);
        set_name(l, p, UNKNOWN_NAME);
        note_add(l, PR_NOTE_UNKNOWN_PORT, p->id, 0);
        return p->id;
    }
    int model = what == PR_PORT_IWLQ ? PR_MODEL_IWLQ : PR_MODEL_IW2;
    if (!p) {
        p = rec_new(l);
        if (!p) return -1;
        p->where = PR_WHERE_PORT;
        p->found_auto = 1;
        port_uuid(l, p->uuid);
        become_imagewriter(l, p, model);
        char n[PR_NAME_MAX];
        own_name(p, n, sizeof n);
        set_name(l, p, n);
        announce(l, p);
        return p->id;
    }
    if (p->kind == PR_KIND_UNKNOWN) {
        if (!p->found_auto) return p->id;     // the person said "Something else"
        become_imagewriter(l, p, model);
        announce(l, p);
        return p->id;
    }
    if (p->kind == PR_KIND_IMAGEWRITER && p->model != model)
        note_add(l, PR_NOTE_CHANGED, p->id, model);
    return p->id;
}

int pr_at_seen(pr_list_t *l, pr_where_t link, const uint8_t *nbp_name, const uint8_t *nbp_type,
               int from_airwriter)
{
    if (from_airwriter || link == PR_WHERE_PORT || !nbp_name || !nbp_type || !nbp_name[0])
        return -1;
    int kind = pstr_is(nbp_type, "LaserWriter") ? PR_KIND_LASERWRITER
             : pstr_is(nbp_type, "ImageWriter") ? PR_KIND_AT_IMAGEWRITER : -1;
    if (kind < 0) return -1;
    for (int i = 0; i < l->n; i++) {
        pr_rec_t *r = &l->r[i];
        if (r->where == PR_WHERE_PORT || r->kind != kind || !pstr_eq(r->nbp, nbp_name)) continue;
        if (link < r->where) r->where = (uint8_t)link;      // the cable, then the network ...
        return r->id;
    }
    pr_rec_t *r = rec_new(l);
    if (!r) return -1;
    r->where = (uint8_t)link;
    r->kind = (uint8_t)kind;
    r->found_auto = 1;
    memcpy(r->nbp, nbp_name, 1 + (size_t)(nbp_name[0] > 32 ? 32 : nbp_name[0]));
    r->nbp[0] = nbp_name[0] > 32 ? 32 : nbp_name[0];
    memcpy(r->nbp_type, nbp_type, 1 + (size_t)(nbp_type[0] > 32 ? 32 : nbp_type[0]));
    r->nbp_type[0] = nbp_type[0] > 32 ? 32 : nbp_type[0];
    lw_uuid(r->uuid, l->mac, l->epoch, r->nbp);
    char n[PR_NAME_MAX];
    own_name(r, n, sizeof n);
    set_name(l, r, n);
    if (kind == PR_KIND_AT_IMAGEWRITER) {       // its type says what it is
        r->identified = 1;
        announce(l, r);
    }
    return r->id;
}

void pr_at_queried(pr_list_t *l, int id, int ok, const lw_info_t *info)
{
    pr_rec_t *r = pr_get(l, id);
    if (!r || r->kind != PR_KIND_LASERWRITER) return;
    if (ok) {
        r->identified = 1;
        r->query_fails = 0;
        if (info) r->lw = *info;
        notes_clear(l, id, PR_NOTE_NO_ANSWER);
        announce(l, r);
        return;
    }
    if (r->query_fails < 255) r->query_fails++;
    if (!r->identified && r->query_fails == 3) note_add(l, PR_NOTE_NO_ANSWER, id, 0);
}

// ------------------------------------------------------------- the person

int pr_rename(pr_list_t *l, int id, const char *name)
{
    pr_rec_t *r = pr_get(l, id);
    if (!r) return -1;
    char want[PR_NAME_MAX * 2];
    const char *s = name ? name : "";
    while (*s == ' ') s++;
    snprintf(want, sizeof want, "%s", s);
    size_t n = strlen(want);
    while (n && want[n - 1] == ' ') want[--n] = '\0';
    if (!n) own_name(r, want, sizeof want);
    set_name(l, r, want);
    return 0;
}

int pr_set_shared(pr_list_t *l, int id, int on)
{
    pr_rec_t *r = pr_get(l, id);
    if (!r) return -1;
    if (!on) {
        r->shared = 0;
        return 0;
    }
    if (r->kind == PR_KIND_UNKNOWN || !r->identified) return -1;
    if (r->shared) return 0;
    if (!can_share(l, r)) return -1;
    r->shared = 1;
    notes_clear(l, id, PR_NOTE_TOO_MANY);
    return 0;
}

int pr_set_model(pr_list_t *l, int id, pr_model_t m)
{
    pr_rec_t *r = pr_get(l, id);
    if (!r || r->where != PR_WHERE_PORT || m > PR_MODEL_IW1) return -1;
    r->found_auto = 0;
    if (m == PR_MODEL_NONE) {
        int own = name_is_own(r);
        r->kind = PR_KIND_UNKNOWN;
        r->model = PR_MODEL_NONE;
        r->identified = 0;
        r->shared = 0;
        if (own) set_name(l, r, UNKNOWN_NAME);
        notes_clear(l, id, PR_NOTE_UNKNOWN_PORT);
        notes_clear(l, id, PR_NOTE_CHANGED);
        return 0;
    }
    become_imagewriter(l, r, m);
    r->shared = 1;                        // the port always fits under the cap
    r->announced = 1;
    return 0;
}

int pr_remove(pr_list_t *l, int id)
{
    int i = pr_index(l, id);
    if (i < 0) return -1;
    notes_clear(l, id, -1);
    memmove(&l->r[i], &l->r[i + 1], (size_t)(l->n - i - 1) * sizeof l->r[0]);
    l->n--;
    return 0;
}

int pr_use_anyway(pr_list_t *l, int id)
{
    pr_rec_t *r = pr_get(l, id);
    if (!r || r->kind != PR_KIND_LASERWRITER || r->identified) return -1;
    lw_info_default(&r->lw);
    r->identified = 1;
    notes_clear(l, id, PR_NOTE_NO_ANSWER);
    if (can_share(l, r)) r->shared = 1;
    else note_add(l, PR_NOTE_TOO_MANY, id, 0);
    r->announced = 1;
    return 0;
}

int pr_accept_change(pr_list_t *l, int note)
{
    if (note < 0 || note >= l->n_notes || l->notes[note].kind != PR_NOTE_CHANGED) return -1;
    pr_note_t n = l->notes[note];
    pr_rec_t *r = pr_get(l, n.id);
    note_drop(l, note);
    if (!r) return -1;
    become_imagewriter(l, r, n.model);
    return 0;
}

void pr_dismiss(pr_list_t *l, int note)
{
    if (note >= 0 && note < l->n_notes) note_drop(l, note);
}

// ------------------------------------------------------------- old Macs, the box

int pr_offers(const pr_list_t *l, pr_where_t link, int ids[], int max)
{
    int n = 0;
    for (int i = 0; i < l->n && n < max; i++) {
        const pr_rec_t *r = &l->r[i];
        if (!r->identified || r->kind == PR_KIND_UNKNOWN) continue;
        if (r->where != PR_WHERE_PORT && r->where == link) continue;   // it speaks for itself
        ids[n++] = r->id;
    }
    return n;
}

static pr_box_t one_state(const pr_live_t *v)
{
    if (v->failed) return PR_BOX_FAILED;
    if (v->attention) return PR_BOX_ATTENTION;
    if (v->printing) return PR_BOX_PRINTING;
    if (v->present && v->ready) return PR_BOX_READY;
    return PR_BOX_OFFLINE;
}

pr_box_t pr_box_state(const pr_list_t *l, const pr_live_t live[PR_MAX])
{
    int any_shared = 0;
    for (int i = 0; i < l->n; i++) any_shared |= l->r[i].shared;
    pr_box_t s = PR_BOX_OFFLINE;
    for (int i = 0; i < l->n; i++) {
        if (any_shared ? !l->r[i].shared : !l->r[i].identified) continue;
        pr_box_t o = one_state(&live[i]);
        if (o > s) s = o;
    }
    return s;
}

int pr_test_target(const pr_list_t *l, const pr_live_t live[PR_MAX])
{
    (void)live;
    const pr_rec_t *p = pr_port(l);
    if (p && p->kind == PR_KIND_IMAGEWRITER) return p->id;
    for (int i = 0; i < l->n; i++) if (l->r[i].shared) return l->r[i].id;
    return -1;
}

// ------------------------------------------------------------- before W-1088

void pr_migrate(pr_list_t *l, const pr_legacy_t *o)
{
    pr_rec_t *p = rec_new(l);
    if (!p) return;
    p->where = PR_WHERE_PORT;
    int model = o->model && !strcmp(o->model, "ImageWriter LQ") ? PR_MODEL_IWLQ
              : o->model && !strcmp(o->model, "ImageWriter") ? PR_MODEL_IW1 : PR_MODEL_IW2;
    p->kind = PR_KIND_IMAGEWRITER;
    p->model = (uint8_t)model;
    p->identified = p->announced = 1;
    p->shared = o->airprint ? 1 : 0;
    snprintf(p->uuid, sizeof p->uuid, "%s", o->uuid ? o->uuid : "");
    if (!p->uuid[0]) port_uuid(l, p->uuid);
    snprintf(p->dither, sizeof p->dither, "%s", o->dither && o->dither[0] ? o->dither : "atkinson");
    p->gain = o->gain;
    p->top_margin = o->top_margin;
    p->ribbon = o->ribbon;
    set_name(l, p, o->name && o->name[0] ? o->name : pr_model_name(model));

    if (!o->laser_shared || !o->laser_nbp || !o->laser_nbp[0]) return;
    static const uint8_t lw_type[] = { 11, 'L', 'a', 's', 'e', 'r', 'W', 'r', 'i', 't', 'e', 'r' };
    static const uint8_t link_where[] = { PR_WHERE_CABLE, PR_WHERE_TUNNEL, PR_WHERE_ZONE,
                                          PR_WHERE_NETWORK };   // at_link_t order
    pr_rec_t *r = rec_new(l);
    if (!r) return;
    r->where = o->laser_link >= 0 && o->laser_link < 4 ? link_where[o->laser_link] : PR_WHERE_CABLE;
    r->kind = PR_KIND_LASERWRITER;
    size_t nl = o->laser_nbp[0] > 32 ? 32 : o->laser_nbp[0];
    memcpy(r->nbp + 1, o->laser_nbp + 1, nl);
    r->nbp[0] = (uint8_t)nl;
    memcpy(r->nbp_type, lw_type, sizeof lw_type);
    lw_uuid(r->uuid, l->mac, l->epoch, r->nbp);
    r->identified = 1;                    // shared and printing before: keep it so
    if (o->laser_info) r->lw = *o->laser_info;
    r->shared = r->announced = r->ipp_laser = 1;
    snprintf(r->dither, sizeof r->dither, "%s",
             o->laser_dither && o->laser_dither[0] ? o->laser_dither : "atkinson");
    r->gain = o->laser_gain;
    char n[PR_NAME_MAX];
    own_name(r, n, sizeof n);
    set_name(l, r, n);
}

// ------------------------------------------------------------- stored form

enum { HDR = 6 };

size_t pr_save(const pr_list_t *l, uint8_t *buf, size_t cap)
{
    size_t need = HDR + (size_t)l->n * sizeof(pr_rec_t) + (size_t)l->n_notes * sizeof(pr_note_t);
    if (cap < need) return 0;
    buf[0] = 'P'; buf[1] = 'R'; buf[2] = PR_BLOB_VER;
    buf[3] = l->n; buf[4] = l->next_id; buf[5] = l->n_notes;
    memcpy(buf + HDR, l->r, (size_t)l->n * sizeof(pr_rec_t));
    memcpy(buf + HDR + (size_t)l->n * sizeof(pr_rec_t), l->notes,
           (size_t)l->n_notes * sizeof(pr_note_t));
    return need;
}

int pr_load(pr_list_t *l, const uint8_t *buf, size_t n)
{
    if (!buf || n < HDR || buf[0] != 'P' || buf[1] != 'R' || buf[2] != PR_BLOB_VER) return -1;
    uint8_t cnt = buf[3], notes = buf[5];
    if (cnt > PR_MAX || notes > PR_NOTES_MAX) return -1;
    if (n != HDR + (size_t)cnt * sizeof(pr_rec_t) + (size_t)notes * sizeof(pr_note_t)) return -1;
    static pr_rec_t r[PR_MAX];
    pr_note_t nt[PR_NOTES_MAX];
    memcpy(r, buf + HDR, (size_t)cnt * sizeof(pr_rec_t));
    memcpy(nt, buf + HDR + (size_t)cnt * sizeof(pr_rec_t), (size_t)notes * sizeof(pr_note_t));
    for (int i = 0; i < cnt; i++) {
        if (!r[i].id || r[i].kind > PR_KIND_AT_IMAGEWRITER || r[i].where > PR_WHERE_ZONE ||
            r[i].model > PR_MODEL_IW1 || !memchr(r[i].name, 0, sizeof r[i].name) ||
            !memchr(r[i].uuid, 0, sizeof r[i].uuid) || !memchr(r[i].dither, 0, sizeof r[i].dither))
            return -1;
        for (int j = 0; j < i; j++) if (r[j].id == r[i].id) return -1;
    }
    for (int i = 0; i < notes; i++) if (nt[i].kind > PR_NOTE_TOO_MANY) return -1;
    memcpy(l->r, r, (size_t)cnt * sizeof(pr_rec_t));
    memcpy(l->notes, nt, (size_t)notes * sizeof(pr_note_t));
    l->n = cnt;
    l->n_notes = notes;
    l->next_id = buf[4] ? buf[4] : 1;
    return 0;
}

// ------------------------------------------------------------- the page

static size_t esc(char *out, size_t cap, const char *s)
{
    size_t k = 0;
    for (; *s && k + 7 < cap; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { out[k++] = '\\'; out[k++] = (char)c; }
        else if (c < 0x20) k += (size_t)snprintf(out + k, cap - k, "\\u%04x", c);
        else out[k++] = (char)c;
    }
    out[k] = '\0';
    return k;
}

size_t pr_json(const pr_list_t *l, const pr_live_t live[PR_MAX], char *buf, size_t cap)
{
    static const char *const states[] = { "offline", "ready", "printing", "attention", "failed" };
    static const char *const notes[] = { "found", "unknown_port", "changed", "no_answer",
                                         "too_many" };
    size_t k = 0;
    char e_name[PR_NAME_MAX * 2], e_prod[sizeof l->r[0].lw.product * 2];
#define PUT(...) do { if (k < cap) k += (size_t)snprintf(buf + k, cap - k, __VA_ARGS__); } while (0)
    PUT("{\"printers\":[");
    for (int i = 0; i < l->n; i++) {
        const pr_rec_t *r = &l->r[i];
        esc(e_name, sizeof e_name, r->name);
        esc(e_prod, sizeof e_prod, r->kind == PR_KIND_LASERWRITER ? r->lw.product : "");
        PUT("%s{\"id\":%u,\"name\":\"%s\",\"where\":\"%s\",\"kind\":\"%s\",\"model\":\"%s\","
            "\"shared\":%s,\"identified\":%s,\"found\":\"%s\",\"state\":\"%s\",\"uuid\":\"%s\","
            "\"product\":\"%s\",\"dither\":\"%s\",\"gain\":%u}",
            i ? "," : "", r->id, e_name, pr_where_name(r->where), pr_kind_name(r->kind),
            pr_model_name(r->model), r->shared ? "true" : "false",
            r->identified ? "true" : "false", r->found_auto ? "auto" : "person",
            states[one_state(&live[i])], r->uuid, e_prod, r->dither, r->gain);
    }
    PUT("],\"notes\":[");
    for (int i = 0; i < l->n_notes; i++)
        PUT("%s{\"kind\":\"%s\",\"id\":%u,\"model\":\"%s\"}", i ? "," : "",
            notes[l->notes[i].kind], l->notes[i].id, pr_model_name(l->notes[i].model));
    PUT("],\"max_shared\":%d}", PR_SHARE_MAX);
#undef PUT
    if (k >= cap) {
        if (cap) buf[0] = '\0';
        return 0;
    }
    return k;
}
