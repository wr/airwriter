// SPDX-License-Identifier: GPL-3.0-or-later
//
// What a Mac draws under Options & Supplies, Supply Levels (W-1137): one row for each
// value of marker-names, in the colour and to the level at the same place in the others.
// All six lists have to be the same length. macOS asks for them by name, so they go only
// when named, which keeps a printer's full answer inside the reply buffer.
#include "test_util.h"
#include "ipp_req.h"
#include "ipp_attrs.h"
#include "ipp_walk.h"

static uint8_t buf[6144];

#define BLACK   "#1C1C1E"
#define CYAN    "#0A84C6"
#define MAGENTA "#D6187F"
#define YELLOW  "#FFCC00"

typedef struct {
    const char *name, *colour, *type;
    int level, low;
} row_t;

static const ipp_printer_t BASE = {
    .uri = "ipp://aw.local:631/ipp/print", .name = "AirWriter",
    .info = "Apple ImageWriter II", .uuid = "urn:uuid:41575254-4552-0002-10bd-a36e2434",
    .adminurl = "http://aw.local/", .location = "",
    .state = IPP_PSTATE_IDLE, .state_reason = "none",
    .accepting_jobs = 1, .queued_jobs = 0, .uptime_s = 5,
};

static size_t encode(const ipp_printer_t *p, const ipp_names_t *requested)
{
    ipp_enc_t e;
    ipp_enc_init(&e, buf, sizeof buf);
    ipp_resp_begin(&e, IPP_VERSION_1_1, IPP_STATUS_OK, 1);
    ipp_write_op_attrs(&e);
    ipp_write_printer_attrs(&e, p, requested);
    ipp_end(&e);
    CHECK(ipp_enc_ok(&e));
    return ipp_enc_len(&e);
}

// macOS's list, as the parser leaves it: tag, name, value, the name empty after the first.
static ipp_names_t marker_lists(void)
{
    static const char *const want[] = { "marker-colors", "marker-high-levels", "marker-levels",
                                        "marker-low-levels", "marker-names", "marker-types" };
    static uint8_t b[256];
    size_t o = 0;
    for (size_t i = 0; i < sizeof want / sizeof want[0]; i++) {
        const char *attr = i ? "" : "requested-attributes";
        b[o++] = IPP_TAG_KEYWORD;
        b[o++] = 0; b[o++] = (uint8_t)strlen(attr);
        memcpy(b + o, attr, strlen(attr)); o += strlen(attr);
        b[o++] = 0; b[o++] = (uint8_t)strlen(want[i]);
        memcpy(b + o, want[i], strlen(want[i])); o += strlen(want[i]);
    }
    return (ipp_names_t){ .p = b, .len = o, .n = 6 };
}

// The i-th value of a list: each further value is its tag, an empty name, its length,
// then the value.
static const uint8_t *nth(const ipp_found_t *f, int i, size_t *len)
{
    const uint8_t *v = f->val;
    size_t n = f->vlen;
    for (; i > 0; i--) {
        v += n;
        n = (size_t)((v[3] << 8) | v[4]);
        v += 5;
    }
    *len = n;
    return v;
}

static void check_str(const ipp_found_t *f, int i, const char *want)
{
    size_t n;
    const uint8_t *v = nth(f, i, &n);
    int same = n == strlen(want) && memcmp(v, want, n) == 0;
    if (!same) fprintf(stderr, "  row %d: got \"%.*s\", want \"%s\"\n", i, (int)n, v, want);
    CHECK(same);
}

static void check_int(const ipp_found_t *f, int i, int want)
{
    size_t n;
    const uint8_t *v = nth(f, i, &n);
    CHECK_EQ_INT(n, 4);
    int32_t got = (int32_t)(((uint32_t)v[0] << 24) | ((uint32_t)v[1] << 16) | ((uint32_t)v[2] << 8) | v[3]);
    CHECK_EQ_INT(got, want);
}

static void check_rows(const ipp_printer_t *p, const row_t *want, int n)
{
    ipp_names_t asked = marker_lists();
    size_t len = encode(p, &asked);
    ipp_found_t names, colours, types, levels, lows, highs;
    CHECK(ipp_find(buf, len, "marker-names", &names));
    CHECK(ipp_find(buf, len, "marker-colors", &colours));
    CHECK(ipp_find(buf, len, "marker-types", &types));
    CHECK(ipp_find(buf, len, "marker-levels", &levels));
    CHECK(ipp_find(buf, len, "marker-low-levels", &lows));
    CHECK(ipp_find(buf, len, "marker-high-levels", &highs));
    if (g_failed) return;
    CHECK_EQ_INT(names.nvalues, n);
    CHECK_EQ_INT(colours.nvalues, n);
    CHECK_EQ_INT(types.nvalues, n);
    CHECK_EQ_INT(levels.nvalues, n);
    CHECK_EQ_INT(lows.nvalues, n);
    CHECK_EQ_INT(highs.nvalues, n);
    CHECK_EQ_INT(names.tag, IPP_TAG_NAME);
    CHECK_EQ_INT(colours.tag, IPP_TAG_NAME);
    CHECK_EQ_INT(types.tag, IPP_TAG_KEYWORD);
    CHECK_EQ_INT(levels.tag, IPP_TAG_INTEGER);
    if (names.nvalues != n) return;
    for (int i = 0; i < n; i++) {
        check_str(&names, i, want[i].name);
        check_str(&colours, i, want[i].colour);
        check_str(&types, i, want[i].type);
        check_int(&levels, i, want[i].level);
        check_int(&lows, i, want[i].low);
        check_int(&highs, i, 100);
    }
}

#define ROWS(p, ...) do { \
    static const row_t w_[] = { __VA_ARGS__ }; \
    check_rows(&(p), w_, (int)(sizeof w_ / sizeof w_[0])); } while (0)

static ipp_printer_t with(ipp_supply_t supply)
{
    ipp_printer_t p = BASE;
    p.supply = supply;
    return p;
}

// Nothing to show: none of the lists goes out, since an IPP attribute needs a value, and no
// link. macOS says Information Not Available.
static void check_no_rows(const ipp_printer_t *p)
{
    ipp_names_t asked = marker_lists();
    size_t len = encode(p, &asked);
    ipp_found_t f;
    CHECK(!ipp_find(buf, len, "marker-names", &f));
    CHECK(!ipp_find(buf, len, "marker-levels", &f));
    len = encode(p, NULL);
    CHECK(!ipp_find(buf, len, "printer-supply-info-uri", &f));
}

int main(void)
{
    // An ImageWriter's ribbon: four stripes in one bar, or one black bar.
    ipp_printer_t p = with(IPP_SUPPLY_COLOUR_RIBBON);
    ROWS(p, { "Color Ribbon", YELLOW MAGENTA CYAN BLACK, "ink-ribbon", 100, 10 });
    p = with(IPP_SUPPLY_BLACK_RIBBON);
    ROWS(p, { "Black Ribbon", BLACK, "ink-ribbon", 100, 10 });

    // A StyleWriter's cartridge: each ink its own row.
    p = with(IPP_SUPPLY_COLOUR_INK);
    ROWS(p, { "Black Ink", BLACK, "ink-cartridge", 100, 10 },
            { "Cyan Ink", CYAN, "ink-cartridge", 100, 10 },
            { "Magenta Ink", MAGENTA, "ink-cartridge", 100, 10 },
            { "Yellow Ink", YELLOW, "ink-cartridge", 100, 10 });
    p = with(IPP_SUPPLY_BLACK_INK);
    ROWS(p, { "Black Ink", BLACK, "ink-cartridge", 100, 10 });

    // A LaserWriter: toner, its name carrying the printer's own page count.
    p = with(IPP_SUPPLY_TONER);
    p.pages = 10277;
    ROWS(p, { "Toner, 10,277 Pages Printed", BLACK, "toner", 100, 10 });
    p.pages = 999;
    ROWS(p, { "Toner, 999 Pages Printed", BLACK, "toner", 100, 10 });
    p.pages = 1234567;
    ROWS(p, { "Toner, 1,234,567 Pages Printed", BLACK, "toner", 100, 10 });
    p.pages = 2147483647;           // the longest a count can be
    ROWS(p, { "Toner, 2,147,483,647 Pages Printed", BLACK, "toner", 100, 10 });
    p.pages = 0;                    // none reported
    ROWS(p, { "Toner", BLACK, "toner", 100, 10 });

    // A printer that says it is low or empty: that row says so. macOS adds its warning
    // triangle at or below the low level.
    p = with(IPP_SUPPLY_TONER);
    p.pages = 10277;
    p.state_reason = "toner-low-warning";
    ROWS(p, { "Toner, 10,277 Pages Printed", BLACK, "toner", 10, 10 });
    p.state_reason = "toner-empty-error";
    ROWS(p, { "Toner, 10,277 Pages Printed", BLACK, "toner", 0, 10 });
    p = with(IPP_SUPPLY_COLOUR_INK);
    p.state_reason = "marker-supply-empty-error";
    ROWS(p, { "Black Ink", BLACK, "ink-cartridge", 0, 10 },
            { "Cyan Ink", CYAN, "ink-cartridge", 0, 10 },
            { "Magenta Ink", MAGENTA, "ink-cartridge", 0, 10 },
            { "Yellow Ink", YELLOW, "ink-cartridge", 0, 10 });

    // Nothing known to be in the printer, no cartridge, or a printer that isn't answering:
    // nothing is claimed as installed.
    p = with(IPP_SUPPLY_NONE);
    check_no_rows(&p);
    p = with(IPP_SUPPLY_COLOUR_INK);
    p.state_reason = "marker-supply-missing-error";
    check_no_rows(&p);
    p = with(IPP_SUPPLY_TONER);
    p.pages = 10277;
    p.state_reason = "offline-report";
    check_no_rows(&p);

    // Only asked-for attributes go back, and macOS asks by name. More Info is the settings page.
    {
        size_t len;
        ipp_found_t f;
        p = with(IPP_SUPPLY_BLACK_RIBBON);
        len = encode(&p, NULL);
        CHECK(ipp_find(buf, len, "printer-supply-info-uri", &f));
        CHECK(ipp_val_is(&f, "http://aw.local/"));
        CHECK_EQ_INT(f.tag, IPP_TAG_URI);
        CHECK(!ipp_find(buf, len, "marker-names", &f));     // not in a full answer

        // A list of just marker-names, as the parser leaves it: tag, name, value.
        static const uint8_t list[] = {
            0x44, 0, 20, 'r','e','q','u','e','s','t','e','d','-','a','t','t','r','i','b','u','t','e','s',
                  0, 12, 'm','a','r','k','e','r','-','n','a','m','e','s',
        };
        ipp_names_t one = { .p = list, .len = sizeof list, .n = 1 };
        len = encode(&p, &one);
        CHECK(ipp_find(buf, len, "marker-names", &f));
        CHECK(!ipp_find(buf, len, "marker-levels", &f));
        CHECK(!ipp_find(buf, len, "printer-name", &f));
    }

    TEST_END;
}
