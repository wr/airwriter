// SPDX-License-Identifier: GPL-3.0-or-later
#include "printers_core.h"
#include "lw_caps.h"
#include "lt_proto.h"
#include "sw_session.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <stdlib.h>

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
                                     "appletalk-imagewriter", "stylewriter" };
    return k >= 0 && k <= PR_KIND_STYLEWRITER ? n[k] : "?";
}

int pr_fault_from_sw(int st)
{
    switch (st) {
    case SWS_NO_PAPER:     return PR_FAULT_PAPER;
    case SWS_JAM:          return PR_FAULT_JAM;
    case SWS_CART_MISSING: return PR_FAULT_CARTRIDGE;
    case SWS_CART_JAM:     return PR_FAULT_CART_JAM;
    case SWS_INK_OUT:      return PR_FAULT_INK;
    case SWS_DOOR:         return PR_FAULT_COVER;
    case SWS_WASTE:        return PR_FAULT_WASTE;
    case SWS_FAULT:        return PR_FAULT_OTHER;
    default:               return PR_FAULT_NONE;       // ready, printing, offline
    }
}

// pr_model_t's StyleWriters, in order, as sw_model_t has them.
static const sw_model_t SW_OF[] = { SW_ORIGINAL, SW_II, SW_1200, SW_C1500, SW_C2200, SW_C2400,
                                    SW_C2500 };

sw_model_t pr_model_to_sw(pr_model_t m)
{
    return m >= PR_MODEL_SW1 && m <= PR_MODEL_SWC2500 ? SW_OF[m - PR_MODEL_SW1] : SW_NONE;
}

pr_model_t pr_model_from_sw(sw_model_t m)
{
    for (size_t i = 0; i < sizeof SW_OF / sizeof SW_OF[0]; i++)
        if (SW_OF[i] == m) return (pr_model_t)(PR_MODEL_SW1 + (int)i);
    return PR_MODEL_NONE;
}

const char *pr_model_name(int m)
{
    static const char *const n[] = { "", "ImageWriter II", "ImageWriter LQ", "ImageWriter" };
    if (m >= 0 && m <= PR_MODEL_IW1) return n[m];
    const sw_spec_t *sp = sw_spec(pr_model_to_sw((pr_model_t)m));
    return sp ? sp->name : "";
}

static int model_is_sw(int m) { return m >= PR_MODEL_SW1 && m <= PR_MODEL_SWC2500; }

// A model by the name the page sends (pr_model_name's); -1 for none.
static int model_by_name(const char *s)
{
    for (int m = PR_MODEL_IW2; m <= PR_MODEL_SWC2500; m++)
        if (!strcmp(s, pr_model_name(m))) return m;
    return -1;
}

const char *pr_default_host_fmt(int has_wifi) { return has_wifi ? "imagewriter-%s" : "airwriter-%s"; }
int pr_default_lt_port(int has_wifi) { return has_wifi ? -1 : 1; }

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

int pr_port_is_stylewriter(const pr_list_t *l)
{
    const pr_rec_t *p = pr_port(l);
    return p && p->kind == PR_KIND_STYLEWRITER;
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

// W-1103: the port's printer answered as `model` (a pr_model_t) of family `heard`. An answer
// other than the one whose notice the person dismissed lets that notice be offered again.
static void port_heard(pr_list_t *l, int heard, int model)
{
    l->heard = (uint8_t)heard;
    l->heard_model = (uint8_t)model;
    if (model != l->dismissed) l->dismissed = 0;
}

// The "changed" notice for the port's record `p`, unless the person dismissed it for this model.
static void port_changed(pr_list_t *l, const pr_rec_t *p, int model)
{
    if (model != l->dismissed) note_add(l, PR_NOTE_CHANGED, p->id, model);
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
        char b[PR_NAME_MAX + 8];
        snprintf(b, sizeof b, "%s", base);
        cut_utf8(b, PR_NAME_MAX - 1 - (k < 10 ? 2 : 3));
        size_t n = strlen(b);
        snprintf(b + n, sizeof b - n, " %d", k);
        memcpy(cand, b, strlen(b) + 1);
    }
    snprintf(r->name, sizeof r->name, "%s", cand);
}

static int name_is_own(const pr_rec_t *r)
{
    char own[PR_NAME_MAX];
    own_name(r, own, sizeof own);
    size_t n = strlen(own);
    // "ImageWriter II" or "ImageWriter II 2" (a StyleWriter's the same): still the printer's own.
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
// of a name's hash. An ImageWriter's is "0001", a StyleWriter's "0002" (W-1103):
// phones keep a queue's capabilities by uuid, and a StyleWriter is not the
// ImageWriter that was there.
static void port_uuid(const pr_list_t *l, char out[46], int stylewriter)
{
    static const uint8_t tag[] = { 1, 1 };
    lw_uuid(out, l->mac, l->epoch, tag);
    memcpy(out + strlen("urn:uuid:41575254-4c57-0000-"), stylewriter ? "0002" : "0001", 4);
}

// By its form, not by the board's MAC and epoch it was made with.
static int uuid_is_stylewriter(const char *uuid)
{
    static const char head[] = "urn:uuid:41575254-4c57-";
    return strlen(uuid) == 45 && !strncmp(uuid, head, sizeof head - 1) &&
           !strncmp(uuid + strlen("urn:uuid:41575254-4c57-0000-"), "0002", 4);
}

static void announce(pr_list_t *l, pr_rec_t *r)
{
    if (r->announced) return;
    r->announced = 1;
    if (can_share(l, r)) r->shared = 1;
    else note_add(l, PR_NOTE_TOO_MANY, r->id, 0);
}

// The port's record takes a printer of `kind` and `model`: its own name follows
// (a name the person gave stays), and a printer of the other family is a new
// printer to phones, with the uuid of its own. Else the record, its uuid and
// its settings go on. The Apple screen is a StyleWriter's alone.
static void become_port_printer(pr_list_t *l, pr_rec_t *r, int kind, int model)
{
    int own = name_is_own(r);
    r->kind = (uint8_t)kind;
    r->model = (uint8_t)model;
    r->identified = 1;
    if (kind == PR_KIND_STYLEWRITER) {
        if (!uuid_is_stylewriter(r->uuid)) port_uuid(l, r->uuid, 1);
    } else {
        if (uuid_is_stylewriter(r->uuid)) port_uuid(l, r->uuid, 0);
        if (!strcmp(r->dither, "apple")) snprintf(r->dither, sizeof r->dither, "atkinson");
    }
    if (own) {
        char n[PR_NAME_MAX];
        own_name(r, n, sizeof n);
        set_name(l, r, n);
    }
    notes_clear(l, r->id, PR_NOTE_UNKNOWN_PORT);
    notes_clear(l, r->id, PR_NOTE_CHANGED);
}

static void become_imagewriter(pr_list_t *l, pr_rec_t *r, int model)
{
    become_port_printer(l, r, PR_KIND_IMAGEWRITER, model);
}

static void become_stylewriter(pr_list_t *l, pr_rec_t *r, int model)
{
    become_port_printer(l, r, PR_KIND_STYLEWRITER, model);
}

// A model the port's printer has come to be, whichever family.
static void become_model(pr_list_t *l, pr_rec_t *r, int model)
{
    if (model_is_sw(model)) become_stylewriter(l, r, model);
    else become_imagewriter(l, r, model);
}

// The port's record for a printer that named itself on an empty port: found,
// named by its model, shared.
static pr_rec_t *port_new(pr_list_t *l, int model)
{
    pr_rec_t *p = rec_new(l);
    if (!p) return NULL;
    p->where = PR_WHERE_PORT;
    p->found_auto = 1;
    port_uuid(l, p->uuid, model_is_sw(model));
    become_model(l, p, model);
    char n[PR_NAME_MAX];
    own_name(p, n, sizeof n);
    set_name(l, p, n);
    announce(l, p);
    return p;
}

int pr_port_seen(pr_list_t *l, pr_port_seen_t what)
{
    pr_rec_t *p = (pr_rec_t *)pr_port(l);
    if (what == PR_PORT_NOTHING) return p ? p->id : -1;
    if (what == PR_PORT_SILENT) {
        // The original ImageWriter has no self-ID: its line up and nothing said is its answer.
        if (p && p->kind == PR_KIND_IMAGEWRITER && p->model == PR_MODEL_IW1)
            port_heard(l, PR_HEARD_IMAGEWRITER, PR_MODEL_IW1);
        if (p) return p->id;              // an original ImageWriter, a lost answer: no churn
        p = rec_new(l);
        if (!p) return -1;
        p->where = PR_WHERE_PORT;
        p->kind = PR_KIND_UNKNOWN;
        p->found_auto = 1;
        port_uuid(l, p->uuid, 0);
        set_name(l, p, UNKNOWN_NAME);
        note_add(l, PR_NOTE_UNKNOWN_PORT, p->id, 0);
        return p->id;
    }
    int model = what == PR_PORT_IWLQ ? PR_MODEL_IWLQ : PR_MODEL_IW2;
    port_heard(l, PR_HEARD_IMAGEWRITER, model);
    if (!p) {
        p = port_new(l, model);
        return p ? p->id : -1;
    }
    if (p->kind == PR_KIND_UNKNOWN) {
        if (!p->found_auto) return p->id;     // the person said "Something else"
        become_imagewriter(l, p, model);
        announce(l, p);
        return p->id;
    }
    // A different printer than the record's, an ImageWriter of another model or
    // a StyleWriter, is offered, never swapped in.
    if ((p->kind == PR_KIND_IMAGEWRITER && p->model != model) || p->kind == PR_KIND_STYLEWRITER)
        port_changed(l, p, model);
    return p->id;
}

void pr_port_seen_sw(pr_list_t *l, pr_model_t m)
{
    if (!model_is_sw(m)) return;
    port_heard(l, PR_HEARD_STYLEWRITER, m);
    pr_rec_t *p = (pr_rec_t *)pr_port(l);
    if (!p) {
        port_new(l, m);
        return;
    }
    if (p->kind == PR_KIND_UNKNOWN) {
        if (!p->found_auto) return;           // the person said "Something else"
        become_stylewriter(l, p, m);
        announce(l, p);
        return;
    }
    if (p->kind == PR_KIND_IMAGEWRITER || (p->kind == PR_KIND_STYLEWRITER && p->model != m))
        port_changed(l, p, m);
}

int pr_port_sw_found(pr_list_t *l, pr_model_t m)
{
    pr_rec_t *p = (pr_rec_t *)pr_port(l);
    if (!model_is_sw(m) || !p || p->kind != PR_KIND_STYLEWRITER || p->model == m) return 0;
    become_stylewriter(l, p, m);
    return 1;
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
        // AirPrint to it was cut from W-1088: old Macs on other links print
        // to it through the relay, and nothing offers it to phones.
        r->identified = 1;
        r->announced = 1;
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

// ------------------------------------------------------------- W-1098: renamed

static pr_seen_t *seen_find(pr_list_t *l, int id)
{
    for (int i = 0; i < PR_MAX; i++) if (id > 0 && l->seen[i].id == id) return &l->seen[i];
    return NULL;
}

const pr_seen_t *pr_seen_of(const pr_list_t *l, int id)
{
    return seen_find((pr_list_t *)l, id);
}

static void seen_drop(pr_list_t *l, int id)
{
    pr_seen_t *s = seen_find(l, id);
    if (s) memset(s, 0, sizeof *s);
}

// The new record b's printer, as one the list has under an old name: the
// one that answered last at b's address with the same product and fewest
// pages since, its old name silent. Its index, or -1.
static int renamed_from(const pr_list_t *l, const pr_rec_t *b, pr_where_t where, uint16_t net,
                        uint8_t node, int32_t pages, const lw_info_t *info,
                        const pr_live_t live[PR_MAX])
{
    if (b->kind != PR_KIND_LASERWRITER || b->announced || !b->found_auto || pages < 0 || !info)
        return -1;
    int best = -1;
    int32_t best_pages = -1;
    for (int i = 0; i < l->n; i++) {
        const pr_rec_t *c = &l->r[i];
        const pr_seen_t *s = pr_seen_of(l, c->id);
        if (c->id == b->id || c->kind != PR_KIND_LASERWRITER || !c->identified || !s ||
            s->where != where || s->net != net || s->node != node || s->pages < 0 ||
            pages < s->pages || strcmp(c->lw.product, info->product) || live[i].present ||
            pstr_eq(c->nbp, b->nbp))
            continue;
        if (s->pages > best_pages) {
            best = i;
            best_pages = s->pages;
        }
    }
    return best;
}

int pr_at_answered(pr_list_t *l, int id, pr_where_t where, uint16_t net, uint8_t node,
                   int32_t pages, const lw_info_t *info, const pr_live_t live[PR_MAX])
{
    pr_rec_t *b = pr_get(l, id);
    if (!b) return -1;
    int ci = renamed_from(l, b, where, net, node, pages, info, live);
    if (ci >= 0) {
        pr_rec_t *c = &l->r[ci];
        int own = name_is_own(c), cid = c->id;
        memcpy(c->nbp, b->nbp, sizeof c->nbp);
        memcpy(c->nbp_type, b->nbp_type, sizeof c->nbp_type);
        if (b->where < c->where) c->where = b->where;
        pr_remove(l, id);                            // c moves down if it was after b
        c = pr_get(l, cid);
        if (own) {
            char n[PR_NAME_MAX];
            own_name(c, n, sizeof n);
            set_name(l, c, n);
        }
        id = cid;
    }
    pr_seen_t *s = seen_find(l, id);
    int32_t was = s ? s->pages : -1;
    for (int i = 0; i < PR_MAX && !s; i++) if (!l->seen[i].id) s = &l->seen[i];
    if (!s) return id;                               // full: PR_MAX of each, never
    *s = (pr_seen_t){ .id = (uint8_t)id, .where = (uint8_t)where, .node = node, .net = net,
                      .pages = pages >= 0 ? pages : was };
    return id;
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
    if (r->kind == PR_KIND_UNKNOWN || r->kind == PR_KIND_AT_IMAGEWRITER || !r->identified) return -1;
    if (r->shared) return 0;
    if (!can_share(l, r)) return -1;
    r->shared = 1;
    notes_clear(l, id, PR_NOTE_TOO_MANY);
    return 0;
}

int pr_set_model(pr_list_t *l, int id, pr_model_t m)
{
    pr_rec_t *r = pr_get(l, id);
    if (!r || r->where != PR_WHERE_PORT || m > PR_MODEL_SWC2500) return -1;
    r->found_auto = 0;
    if (m == PR_MODEL_NONE) {
        int own = name_is_own(r);
        r->kind = PR_KIND_UNKNOWN;
        r->model = PR_MODEL_NONE;
        r->identified = 0;
        r->shared = 0;
        if (!strcmp(r->dither, "apple")) snprintf(r->dither, sizeof r->dither, "atkinson");
        if (own) set_name(l, r, UNKNOWN_NAME);
        notes_clear(l, id, PR_NOTE_UNKNOWN_PORT);
        notes_clear(l, id, PR_NOTE_CHANGED);
        return 0;
    }
    become_model(l, r, m);
    r->shared = 1;                        // the port always fits under the cap
    r->announced = 1;
    return 0;
}

int pr_add_port(pr_list_t *l, pr_model_t m)
{
    if (m < PR_MODEL_IW2 || m > PR_MODEL_SWC2500) return -1;
    pr_rec_t *p = (pr_rec_t *)pr_port(l);
    if (!p) {
        p = rec_new(l);
        if (!p) return -1;
        p->where = PR_WHERE_PORT;
        port_uuid(l, p->uuid, model_is_sw(m));
        set_name(l, p, UNKNOWN_NAME);             // its own name follows the model, below
        p->kind = PR_KIND_UNKNOWN;
    }
    return pr_set_model(l, p->id, m) ? -1 : p->id;
}

int pr_remove(pr_list_t *l, int id)
{
    int i = pr_index(l, id);
    if (i < 0) return -1;
    notes_clear(l, id, -1);
    seen_drop(l, id);
    memmove(&l->r[i], &l->r[i + 1], (size_t)(l->n - i - 1) * sizeof l->r[0]);
    l->n--;
    return 0;
}

int pr_use_anyway(pr_list_t *l, int id)
{
    pr_rec_t *r = pr_get(l, id);
    // Offered only with the "didn't answer" notice: after three unanswered asks.
    if (!r || r->kind != PR_KIND_LASERWRITER || r->identified || r->query_fails < 3) return -1;
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
    if (!r || n.model < PR_MODEL_IW2 || n.model > PR_MODEL_SWC2500) return -1;
    become_model(l, r, n.model);
    return 0;
}

void pr_dismiss(pr_list_t *l, int note)
{
    if (note < 0 || note >= l->n_notes) return;
    const pr_rec_t *p = pr_port(l);
    if (p && l->notes[note].kind == PR_NOTE_CHANGED && l->notes[note].id == p->id)
        l->dismissed = l->notes[note].model;
    note_drop(l, note);
}

// ------------------------------------------------------------- who answered

int pr_port_heard(const pr_list_t *l)
{
    return l->heard;
}

// The family a port record's kind speaks, PR_HEARD_NONE for one that is neither.
static int family_of_kind(int kind)
{
    return kind == PR_KIND_IMAGEWRITER ? PR_HEARD_IMAGEWRITER
         : kind == PR_KIND_STYLEWRITER ? PR_HEARD_STYLEWRITER : PR_HEARD_NONE;
}

void pr_port_answered(pr_list_t *l, int heard)
{
    if (heard != PR_HEARD_IMAGEWRITER && heard != PR_HEARD_STYLEWRITER) return;
    // The record's own printer: its model, when the family is the record's.
    const pr_rec_t *p = pr_port(l);
    int own = p && family_of_kind(p->kind) == heard;
    port_heard(l, heard, own ? p->model : 0);
}

int pr_port_conflict(const pr_list_t *l)
{
    const pr_rec_t *p = pr_port(l);
    int fam = p ? family_of_kind(p->kind) : PR_HEARD_NONE;
    return fam != PR_HEARD_NONE && l->heard != PR_HEARD_NONE && l->heard != fam;
}

int pr_port_swapped(const pr_list_t *l)
{
    if (pr_port_conflict(l)) return 1;
    const pr_rec_t *p = pr_port(l);
    return p && p->kind == PR_KIND_STYLEWRITER && l->heard == PR_HEARD_STYLEWRITER &&
           l->heard_model && l->heard_model != p->model;
}

// ------------------------------------------------------------- old Macs, the box

int pr_offers(const pr_list_t *l, pr_where_t link, int ids[], int max)
{
    int n = 0;
    for (int i = 0; i < l->n && n < max; i++) {
        const pr_rec_t *r = &l->r[i];
        if (!r->identified || r->kind == PR_KIND_UNKNOWN) continue;
        if (r->kind == PR_KIND_STYLEWRITER) continue;                  // phase 2
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
    if (p && (p->kind == PR_KIND_IMAGEWRITER || p->kind == PR_KIND_STYLEWRITER)) return p->id;
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
    if (!p->uuid[0]) port_uuid(l, p->uuid, 0);
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

// W-1103: the list is stored with the lowest version that can hold it, 1 until
// a record or a notice holds a StyleWriter, so that an OTA rollback to a firmware from
// before StyleWriters (the W-1059 trial) still reads an ImageWriter or LaserWriter
// owner's list and their printers keep their identity on their phones.
static uint8_t blob_version(const pr_list_t *l)
{
    for (int i = 0; i < l->n; i++)
        if (l->r[i].kind > PR_KIND_AT_IMAGEWRITER || l->r[i].model > PR_MODEL_IW1) return 2;
    for (int i = 0; i < l->n_notes; i++)
        if (l->notes[i].model > PR_MODEL_IW1) return 2;
    return 1;
}

size_t pr_save(const pr_list_t *l, uint8_t *buf, size_t cap)
{
    size_t need = HDR + (size_t)l->n * sizeof(pr_rec_t) + (size_t)l->n_notes * sizeof(pr_note_t);
    if (cap < need) return 0;
    buf[0] = 'P'; buf[1] = 'R'; buf[2] = blob_version(l);
    buf[3] = l->n; buf[4] = l->next_id; buf[5] = l->n_notes;
    memcpy(buf + HDR, l->r, (size_t)l->n * sizeof(pr_rec_t));
    memcpy(buf + HDR + (size_t)l->n * sizeof(pr_rec_t), l->notes,
           (size_t)l->n_notes * sizeof(pr_note_t));
    return need;
}

int pr_load(pr_list_t *l, const uint8_t *buf, size_t n)
{
    if (!buf || n < HDR || buf[0] != 'P' || buf[1] != 'R' || buf[2] < 1 || buf[2] > PR_BLOB_VER)
        return -1;
    // W-1103: a version 1 list has the ImageWriter kinds and models only. The
    // layout is the same, so what it may hold is all that differs.
    int max_kind = buf[2] == 1 ? PR_KIND_AT_IMAGEWRITER : PR_KIND_STYLEWRITER;
    int max_model = buf[2] == 1 ? PR_MODEL_IW1 : PR_MODEL_SWC2500;
    uint8_t cnt = buf[3], notes = buf[5];
    if (cnt > PR_MAX || notes > PR_NOTES_MAX) return -1;
    if (n != HDR + (size_t)cnt * sizeof(pr_rec_t) + (size_t)notes * sizeof(pr_note_t)) return -1;
    // Checked a record at a time from buf, so nothing here takes internal RAM.
    uint8_t ids[PR_MAX];
    for (int i = 0; i < cnt; i++) {
        pr_rec_t r;
        memcpy(&r, buf + HDR + (size_t)i * sizeof r, sizeof r);
        if (!r.id || r.kind > max_kind || r.where > PR_WHERE_ZONE || r.model > max_model ||
            (r.kind == PR_KIND_STYLEWRITER) != model_is_sw(r.model) ||
            !memchr(r.name, 0, sizeof r.name) ||
            !memchr(r.uuid, 0, sizeof r.uuid) || !memchr(r.dither, 0, sizeof r.dither))
            return -1;
        for (int j = 0; j < i; j++) if (ids[j] == r.id) return -1;
        ids[i] = r.id;
    }
    pr_note_t nt[PR_NOTES_MAX];
    memcpy(nt, buf + HDR + (size_t)cnt * sizeof(pr_rec_t), (size_t)notes * sizeof(pr_note_t));
    uint8_t kept = 0;
    for (int i = 0; i < notes; i++) {
        if (nt[i].kind > PR_NOTE_TOO_MANY || nt[i].model > max_model) return -1;
        if (nt[i].kind != PR_NOTE_FOUND) nt[kept++] = nt[i];   // a "found" from before
    }
    memcpy(l->r, buf + HDR, (size_t)cnt * sizeof(pr_rec_t));
    memcpy(l->notes, nt, (size_t)kept * sizeof(pr_note_t));
    l->n = cnt;
    l->n_notes = kept;
    l->next_id = buf[4] ? buf[4] : 1;
    // Who answered on the port is not stored, but a "changed" notice still waiting for the
    // person says the other family did: so it holds from the start, as it did before.
    // So is another StyleWriter model's, on a StyleWriter record.
    l->heard = l->heard_model = l->dismissed = 0;
    const pr_rec_t *p = pr_port(l);
    for (int i = 0; p && i < l->n_notes; i++) {
        int fam = model_is_sw(l->notes[i].model) ? PR_HEARD_STYLEWRITER : PR_HEARD_IMAGEWRITER;
        if (l->notes[i].kind != PR_NOTE_CHANGED || l->notes[i].id != p->id) continue;
        if (fam != family_of_kind(p->kind) || p->kind == PR_KIND_STYLEWRITER) {
            l->heard = (uint8_t)fam;
            l->heard_model = l->notes[i].model;
        }
    }
    return 0;
}

int pr_blob_newer(const uint8_t *buf, size_t n)
{
    return buf && n >= 3 && buf[0] == 'P' && buf[1] == 'R' && buf[2] > PR_BLOB_VER;
}

// W-1098: 'P' 'S' 1, a count, then each entry: id, where, node, net and
// pages, little-endian (pages as uint32, -1 as FFFFFFFF).
enum { SEEN_HDR = 4, SEEN_ONE = 9 };

size_t pr_seen_save(const pr_list_t *l, uint8_t *buf, size_t cap)
{
    int n = 0;
    for (int i = 0; i < PR_MAX; i++) n += l->seen[i].id != 0;
    size_t need = SEEN_HDR + (size_t)n * SEEN_ONE;
    if (cap < need) return 0;
    buf[0] = 'P'; buf[1] = 'S'; buf[2] = 1; buf[3] = (uint8_t)n;
    uint8_t *p = buf + SEEN_HDR;
    for (int i = 0; i < PR_MAX; i++) {
        const pr_seen_t *s = &l->seen[i];
        if (!s->id) continue;
        uint32_t pg = (uint32_t)s->pages;
        p[0] = s->id; p[1] = s->where; p[2] = s->node;
        p[3] = (uint8_t)s->net; p[4] = (uint8_t)(s->net >> 8);
        for (int k = 0; k < 4; k++) p[5 + k] = (uint8_t)(pg >> (8 * k));
        p += SEEN_ONE;
    }
    return need;
}

int pr_seen_load(pr_list_t *l, const uint8_t *buf, size_t n)
{
    if (!buf || n < SEEN_HDR || buf[0] != 'P' || buf[1] != 'S' || buf[2] != 1 ||
        buf[3] > PR_MAX || n != SEEN_HDR + (size_t)buf[3] * SEEN_ONE)
        return -1;
    pr_seen_t got[PR_MAX];
    int k = 0;
    for (int i = 0; i < buf[3]; i++) {
        const uint8_t *p = buf + SEEN_HDR + (size_t)i * SEEN_ONE;
        if (!p[0] || p[1] > PR_WHERE_ZONE) return -1;
        if (pr_index(l, p[0]) < 0) continue;          // not one of the list's
        int dup = 0;
        for (int j = 0; j < k; j++) dup |= got[j].id == p[0];
        if (dup) continue;
        uint32_t pg = 0;
        for (int b = 0; b < 4; b++) pg |= (uint32_t)p[5 + b] << (8 * b);
        got[k++] = (pr_seen_t){ .id = p[0], .where = p[1], .node = p[2],
                                .net = (uint16_t)(p[3] | p[4] << 8), .pages = (int32_t)pg };
    }
    memset(l->seen, 0, sizeof l->seen);
    memcpy(l->seen, got, (size_t)k * sizeof got[0]);
    return 0;
}

// ------------------------------------------------------------- the Printers section

// Where "key": starts its value, or NULL.
static const char *json_at(const char *b, const char *key)
{
    char k[32];
    snprintf(k, sizeof k, "\"%s\"", key);
    const char *p = strstr(b, k);
    if (!p) return NULL;
    p += strlen(k);
    while (*p == ' ') p++;
    if (*p != ':') return NULL;
    p++;
    while (*p == ' ') p++;
    return p;
}

static int json_has_int(const char *b, const char *key, long *v)
{
    const char *p = json_at(b, key);
    if (!p || !(*p == '-' || isdigit((unsigned char)*p))) return 0;
    *v = strtol(p, NULL, 10);
    return 1;
}

static int json_has_bool(const char *b, const char *key, int *v)
{
    const char *p = json_at(b, key);
    if (!p) return 0;
    if (!strncmp(p, "true", 4)) { *v = 1; return 1; }
    if (!strncmp(p, "false", 5)) { *v = 0; return 1; }
    return 0;
}

// A JSON string's value as UTF-8; 0 when absent or not a string.
static int json_has_str(const char *b, const char *key, char *out, size_t cap)
{
    const char *p = json_at(b, key);
    if (!p || *p != '"') return 0;
    size_t k = 0;
    for (p++; *p && *p != '"' && k + 4 < cap; p++) {
        if (*p != '\\') { out[k++] = *p; continue; }
        p++;
        if (*p == 'u') {
            unsigned u = 0;
            for (int i = 1; i <= 4; i++) {
                char c = p[i];
                u = u * 16 + (unsigned)(isdigit((unsigned char)c) ? c - '0' : (tolower(c) - 'a' + 10));
            }
            p += 4;
            if (u < 0x80) out[k++] = (char)u;
            else if (u < 0x800) { out[k++] = (char)(0xC0 | u >> 6); out[k++] = (char)(0x80 | (u & 0x3F)); }
            else { out[k++] = (char)(0xE0 | u >> 12); out[k++] = (char)(0x80 | ((u >> 6) & 0x3F));
                   out[k++] = (char)(0x80 | (u & 0x3F)); }
        } else {
            out[k++] = *p == 'n' ? '\n' : *p == 't' ? '\t' : *p;
        }
        if (!*p) break;
    }
    out[k] = '\0';
    return 1;
}

static int post_err(pr_post_t *o, const char *msg)
{
    snprintf(o->err, sizeof o->err, "%s", msg);
    return -1;
}

int pr_post(pr_list_t *l, const char *body, pr_post_t *o)
{
    memset(o, 0, sizeof *o);
    o->screen_id = o->start_page_id = o->start_page = o->clean_id = o->confirm_sw_id = -1;
    const char *b = body ? body : "";
    while (*b == ' ' || *b == '\n' || *b == '\r' || *b == '\t') b++;
    if (*b != '{') return post_err(o, "Unknown request.");
    int v;
    long n;
    if (json_has_bool(b, "look", &v) && v) {
        o->look = 1;
        return 0;
    }
    if (json_has_int(b, "note", &n)) {
        if (n < 0 || n >= l->n_notes) return post_err(o, "No such notice.");
        if (json_has_bool(b, "accept", &v) && v) {
            if (pr_accept_change(l, (int)n)) return post_err(o, "No such notice.");
        } else {
            pr_dismiss(l, (int)n);
        }
        o->changed = 1;
        return 0;
    }
    char s[PR_NAME_MAX * 2];
    if (json_has_str(b, "add_port", s, sizeof s)) {
        int m = model_by_name(s);
        int port = m < 0 ? -1 : pr_add_port(l, (pr_model_t)m);
        if (port < 0) return post_err(o, "Unknown model.");
        o->changed = 1;
        if (model_is_sw(m)) o->confirm_sw_id = port;     // the person's word, to be checked
        return 0;
    }
    if (!json_has_int(b, "id", &n) || !pr_get(l, (int)n)) return post_err(o, "No such printer.");
    int id = (int)n;
    if (json_has_bool(b, "remove", &v) && v) {
        pr_remove(l, id);
        o->changed = 1;
        return 0;
    }
    if (json_has_bool(b, "use_anyway", &v) && v) {
        if (pr_use_anyway(l, id)) return post_err(o, "Only for a LaserWriter that isn’t responding.");
        o->changed = 1;
    }
    if (json_has_str(b, "model", s, sizeof s)) {
        int m = !strcmp(s, "none") ? PR_MODEL_NONE : model_by_name(s);
        if (m < 0 || pr_set_model(l, id, (pr_model_t)m)) return post_err(o, "Unknown model.");
        o->changed = 1;
        if (model_is_sw(m)) o->confirm_sw_id = id;
    }
    if (json_has_str(b, "name", s, sizeof s)) {
        pr_rename(l, id, s);
        o->changed = 1;
    }
    if (json_has_bool(b, "shared", &v)) {
        pr_rec_t *r = pr_get(l, id);
        if (v && r->kind == PR_KIND_AT_IMAGEWRITER)
            return post_err(o, "This printer can’t be shared with phones and computers.");
        if (v && r->kind == PR_KIND_UNKNOWN)
            return post_err(o, "Choose this printer’s model to share it.");
        if (v && !r->identified)
            return post_err(o, "This printer can’t be shared until it responds.");
        if (pr_set_shared(l, id, v)) return post_err(o, "You can share up to 4 printers.");
        o->changed = 1;
    }
    if (json_has_str(b, "dither", s, sizeof s)) {
        static const char *const ok[] = { "atkinson", "floyd", "bluenoise", "bayer", "threshold",
                                          "apple" };
        const char *good = NULL;
        for (size_t i = 0; i < sizeof ok / sizeof ok[0]; i++) if (!strcmp(s, ok[i])) good = ok[i];
        // Apple's screen is the StyleWriter drivers': only a StyleWriter is offered it.
        if (good && !strcmp(good, "apple") && pr_get(l, id)->kind != PR_KIND_STYLEWRITER) good = NULL;
        if (!good) return post_err(o, "Unknown halftone.");
        snprintf(pr_get(l, id)->dither, sizeof pr_get(l, id)->dither, "%s", good);
        o->changed = 1;
        o->screen_id = id;
    }
    if (json_has_int(b, "gain", &n)) {
        if (n < 0 || n > 45) return post_err(o, "Gain is 0 to 45.");
        pr_get(l, id)->gain = (uint8_t)n;
        o->changed = 1;
        o->screen_id = id;
    }
    if (json_has_bool(b, "start_page", &v)) {
        const pr_rec_t *r = pr_get(l, id);
        if (r->kind != PR_KIND_LASERWRITER ||
            (r->lw.start_page != LW_START_ON && r->lw.start_page != LW_START_OFF))
            return post_err(o, "This printer has no start page setting.");
        o->start_page_id = id;
        o->start_page = v ? 1 : 0;
    }
    if (json_has_bool(b, "clean", &v) && v) {
        const pr_rec_t *r = pr_get(l, id);
        const sw_spec_t *sp = r->kind == PR_KIND_STYLEWRITER ? sw_spec(pr_model_to_sw(r->model)) : NULL;
        if (!sp || !sp->cleans) return post_err(o, "This printer can’t clean its print head.");
        o->clean_id = id;
    }
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
        // Its AirPrint path while it is shared: the port's, W-1082's, or its own.
        char rp[16] = "";
        if (r->shared && r->identified && r->where == PR_WHERE_PORT &&
            (r->kind == PR_KIND_IMAGEWRITER || r->kind == PR_KIND_STYLEWRITER))
            snprintf(rp, sizeof rp, "ipp/print");
        else if (r->shared && r->identified && r->kind == PR_KIND_LASERWRITER)
            snprintf(rp, sizeof rp, r->ipp_laser ? "ipp/laser" : "ipp/p%u", r->id);
        // W-1099: a LaserWriter's start page, null where it never said.
        int lw = r->kind == PR_KIND_LASERWRITER;
        const char *sp = lw && r->lw.start_page == LW_START_ON ? "true"
                       : lw && r->lw.start_page == LW_START_OFF ? "false" : "null";
        // W-1103: a StyleWriter's cartridge as the board last read it, said only once it has.
        char swx[28 + 24] = "";   // the cartridge and the fault, as JSON fields
        if (r->kind == PR_KIND_STYLEWRITER && (live[i].cart == 1 || live[i].cart == 2))
            snprintf(swx, sizeof swx, ",\"cartridge\":\"%s\"", live[i].cart == 2 ? "colour" : "black");
        // and the fault it holds, for the page to name in its own words, said only while it has one
        static const char *const faults[] = { "", "paper", "jam", "cartridge", "cartridge-jam",
                                              "ink", "cover", "waste", "other" };
        if (r->kind == PR_KIND_STYLEWRITER && live[i].fault > PR_FAULT_NONE &&
            live[i].fault <= PR_FAULT_OTHER) {
            size_t n = strlen(swx);
            snprintf(swx + n, sizeof swx - n, ",\"fault\":\"%s\"", faults[live[i].fault]);
        }
        // W-1103: the other family answered on the port: its record needs the person.
        int swapped = r->where == PR_WHERE_PORT && pr_port_swapped(l);
        PUT("%s{\"id\":%u,\"name\":\"%s\",\"where\":\"%s\",\"kind\":\"%s\",\"model\":\"%s\","
            "\"shared\":%s,\"identified\":%s,\"found\":\"%s\",\"state\":\"%s\",\"uuid\":\"%s\","
            "\"product\":\"%s\",\"dither\":\"%s\",\"gain\":%u,\"rp\":\"%s\",\"start_page\":%s%s%s}",
            i ? "," : "", r->id, e_name, pr_where_name(r->where), pr_kind_name(r->kind),
            pr_model_name(r->model), r->shared ? "true" : "false",
            r->identified ? "true" : "false", r->found_auto ? "auto" : "person",
            states[swapped ? PR_BOX_ATTENTION : one_state(&live[i])], r->uuid, e_prod, r->dither,
            r->gain, rp, sp, swx, swapped ? ",\"swapped\":true" : "");
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
