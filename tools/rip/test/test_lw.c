// SPDX-License-Identifier: GPL-3.0-or-later
//
// The LaserWriter's side of W-1082, pure: the query job and what its answer
// says, the status strings as IPP reasons, and the AirPrint printer a queried
// LaserWriter becomes. With --parse, reads a printer's output on stdin and
// prints what the parser made of it (pscheck.py runs the query job through
// Ghostscript and feeds this).
#include "test_util.h"
#include "lw_query.h"
#include "lw_status.h"
#include "lw_caps.h"

// A LaserWriter II NT's answer, as the board would read it back: CR line
// ends, a status line in among the answers.
static const char k_ntx[] =
    "aw:product=LaserWriter II NT\r"
    "aw:version=47.0\r"
    "%%[ status: busy; source: AppleTalk ]%%\r"
    "aw:revision=1\r"
    "aw:level=1\r"
    "aw:res=300 300\r"
    "aw:area=14.16 8.4 597.84 783.6\r"
    "aw:size=?\r"
    "aw:probe=256 1\r"
    "aw:end\r";

static int parse_stdin(void)
{
    lw_parse_t p;
    lw_parse_init(&p);
    uint8_t buf[512];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, stdin)) > 0) lw_parse_feed(&p, buf, n);
    const lw_info_t *i = &p.info;
    printf("complete=%d product=%s level=%d binary=%d dpi=%d page=%s area=%d %d %d %d\n",
           i->complete, i->product, i->level, i->binary, i->dpi, lw_page_name(i->page),
           (int)i->area[0], (int)i->area[1], (int)i->area[2], (int)i->area[3]);
    return 0;
}

static void check_map(const char *status, const char *reason, int stopped, int busy)
{
    lw_state_t s;
    lw_status_map(status, &s);
    if (strcmp(s.reason, reason) || s.stopped != stopped || s.busy != busy)
        fprintf(stderr, "  \"%s\" -> %s stopped %d busy %d\n", status, s.reason, s.stopped, s.busy);
    CHECK(strcmp(s.reason, reason) == 0);
    CHECK_EQ_INT(s.stopped, stopped);
    CHECK_EQ_INT(s.busy, busy);
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "--parse")) return parse_stdin();

    // The answer, one byte at a time (Review Focus 3).
    lw_parse_t p;
    lw_parse_init(&p);
    for (size_t k = 0; k < sizeof k_ntx - 1; k++) lw_parse_feed(&p, (const uint8_t *)k_ntx + k, 1);
    lw_info_t *i = &p.info;
    CHECK(strcmp(i->product, "LaserWriter II NT") == 0);
    CHECK(strcmp(i->version, "47.0") == 0);
    CHECK_EQ_INT(i->revision, 1);
    CHECK_EQ_INT(i->level, 1);
    CHECK_EQ_INT(i->dpi, 300);
    CHECK_EQ_INT(i->area[0], 1416);
    CHECK_EQ_INT(i->area[1], 840);
    CHECK_EQ_INT(i->area[2], 59784);
    CHECK_EQ_INT(i->area[3], 78360);
    CHECK_EQ_INT(i->page, LW_LETTER);
    CHECK_EQ_INT(i->binary, 1);
    CHECK_EQ_INT(i->complete, 1);

    // LF line ends, split mid-line; a failed probe; Level 2 with a page size.
    lw_parse_init(&p);
    const char *a = "aw:prod", *b = "uct=LaserWriter Pro 630\naw:level=2\naw:size=595 842\n"
                    "aw:area=14 8 581 834\naw:probe=200 0\n";
    lw_parse_feed(&p, (const uint8_t *)a, strlen(a));
    lw_parse_feed(&p, (const uint8_t *)b, strlen(b));
    CHECK(strcmp(i->product, "LaserWriter Pro 630") == 0);
    CHECK_EQ_INT(i->level, 2);
    CHECK_EQ_INT(i->page, LW_A4);
    CHECK_EQ_INT(i->binary, 0);
    CHECK_EQ_INT(i->complete, 1);           // the probe is the last question

    // Cut off before the probe: not complete, the defaults stand.
    lw_parse_init(&p);
    const char *cut = "aw:product=LaserWriter Pro 630\naw:level=2\n";
    lw_parse_feed(&p, (const uint8_t *)cut, strlen(cut));
    CHECK_EQ_INT(i->complete, 0);

    // Review I2: a printer that mangles binary answers the probe, then
    // chokes on what follows it; every answer came before the probe, so the
    // query is complete and the printer gets hex.
    lw_parse_init(&p);
    const char *mangled = "aw:product=LaserWriter Plus\raw:level=1\raw:area=18 8 594 784\r"
                          "aw:probe=254 0\r%%[ Error: undefined; OffendingCommand: aw:end ]%%\r";
    lw_parse_feed(&p, (const uint8_t *)mangled, strlen(mangled));
    CHECK_EQ_INT(i->complete, 1);
    CHECK_EQ_INT(i->binary, 0);
    CHECK(strcmp(i->product, "LaserWriter Plus") == 0);

    // A Level 1 machine on Legal paper: the page from the area alone.
    lw_parse_init(&p);
    const char *c = "aw:area=18 18 594 990\r\naw:product=?\r\naw:end\r\n";
    lw_parse_feed(&p, (const uint8_t *)c, strlen(c));
    CHECK_EQ_INT(i->page, LW_LEGAL);
    CHECK(strcmp(i->product, "LaserWriter") == 0);   // '?' keeps the default
    CHECK_EQ_INT(i->complete, 1);
    CHECK_EQ_INT(i->binary, 0);

    // The defaults stand in for a printer that never answered.
    lw_info_t d;
    lw_info_default(&d);
    CHECK_EQ_INT(d.level, 1);
    CHECK_EQ_INT(d.binary, 0);
    CHECK_EQ_INT(d.dpi, 300);
    CHECK_EQ_INT(d.page, LW_LETTER);
    CHECK_EQ_INT(d.area[0], 1800);
    CHECK_EQ_INT(d.area[3], 77400);

    // The query job: Level 1 text around the 256-byte probe, which follows
    // one LF after the procedure that reads it.
    static uint8_t q[4096];
    size_t qn = lw_query_job(q, sizeof q);
    CHECK(qn > 256);
    CHECK(memcmp(q, "%!PS-Adobe-3.0", 14) == 0);
    int probe_at = -1;
    for (size_t k = 0; k + 256 <= qn; k++) {
        int ok = 1;
        for (int v = 0; v < 256 && ok; v++) ok = q[k + (size_t)v] == v;
        if (ok) { probe_at = (int)k; break; }
    }
    CHECK(probe_at > 0);
    if (probe_at > 0) CHECK_EQ_INT(q[probe_at - 1], '\n');
    CHECK(probe_at > 1 && q[probe_at - 2] != '\r');
    CHECK_EQ_INT(lw_query_job(q, 100), 0);

    // Status strings as IPP reasons (spec, "Status and errors").
    check_map("status: idle", "none", 0, 0);
    check_map("status: PrinterError: out of paper", "media-empty-error", 1, 0);
    check_map("status: PrinterError: paper tray empty", "media-empty-error", 1, 0);
    check_map("status: PrinterError: Paper Jam", "media-jam-error", 1, 0);
    check_map("status: PrinterError: cover open", "door-open-error", 1, 0);
    check_map("status: PrinterError: door open", "door-open-error", 1, 0);
    check_map("status: PrinterError: toner low", "toner-low-warning", 0, 0);
    check_map("status: PrinterError: no toner", "toner-empty-error", 1, 0);
    check_map("status: PrinterError: out of toner", "toner-empty-error", 1, 0);
    check_map("status: PrinterError: service call", "other-error", 1, 0);
    check_map("job: Letter; status: busy; source: AppleTalk", "none", 0, 1);
    check_map("status: busy; source: AppleTalk", "none", 0, 1);
    check_map("status: warming up", "none", 0, 1);
    check_map("STATUS: PRINTERERROR: OUT OF PAPER", "media-empty-error", 1, 0);

    // Review I1: the job's own name is not the printer's state.
    check_map("job: Benjamin's letter; status: busy; source: AppleTalk", "none", 0, 1);
    check_map("job: Open Door Policy; status: printing", "none", 0, 1);
    check_map("user: wells; job: Paper out notice; status: busy", "none", 0, 1);
    check_map("job: Pajamas; status: idle", "none", 0, 0);
    check_map("job: Pajamas; status: PrinterError: paper jam", "media-jam-error", 1, 0);
    check_map("%%[ status: PrinterError: out of paper ]%%", "media-empty-error", 1, 0);

    char msg[80];
    // Review M2: ERROR in capitals is still an error, and nothing past the line is read.
    CHECK_EQ_INT(lw_ps_error("%%[ ERROR: limitcheck; OffendingCommand: image ]%%", msg, sizeof msg), 1);
    CHECK(strcmp(msg, "limitcheck; OffendingCommand: image") == 0);
    CHECK_EQ_INT(lw_ps_error("%%[ Error: undefined; OffendingCommand: foo ]%%", msg, sizeof msg), 1);
    CHECK(strcmp(msg, "undefined; OffendingCommand: foo") == 0);
    CHECK_EQ_INT(lw_ps_error("%%[ Flushing: rest of job (to end-of-file) will be ignored ]%%",
                             msg, sizeof msg), 0);
    CHECK_EQ_INT(lw_ps_error("%%[ status: idle ]%%", msg, sizeof msg), 0);

    // The AirPrint printer the II NT becomes.
    lw_parse_init(&p);
    lw_parse_feed(&p, (const uint8_t *)k_ntx, sizeof k_ntx - 1);
    lw_caps_t lc;
    lw_caps_fill(&lc, &p.info);
    const printer_caps_t *cp = &lc.caps;
    CHECK(strcmp(cp->make_model, "Apple LaserWriter II NT") == 0);
    CHECK(strcmp(cp->device_id, "MFG:Apple;MDL:LaserWriter II NT;CMD:URF;") == 0);
    CHECK(strcmp(cp->media, "na_letter_8.5x11in") == 0);
    CHECK_EQ_INT(cp->media_x, 21590);
    CHECK_EQ_INT(cp->media_y, 27940);
    CHECK_EQ_INT(cp->margin_l, 500);        // ceil(14.16 pt in 1/100 mm)
    CHECK_EQ_INT(cp->margin_r, 500);        // 612 - 597.84
    CHECK_EQ_INT(cp->margin_t, 297);        // 792 - 783.6
    CHECK_EQ_INT(cp->margin_b, 297);        // 8.4
    CHECK(strcmp(cp->urf_rs, "RS300") == 0);
    CHECK_EQ_INT(cp->res_x, 300);
    CHECK_EQ_INT(cp->res_y, 300);
    CHECK_EQ_INT(cp->rip_dpi_x, 300);
    CHECK_EQ_INT(cp->rip_dpi_y, 300);
    CHECK_EQ_INT(cp->max_dots, 2550);
    CHECK_EQ_INT(cp->crop_margin, 0);
    CHECK_EQ_INT(lw_paper_h_pt(LW_LETTER), 792);
    CHECK_EQ_INT(lw_paper_h_pt(LW_A4), 842);
    CHECK_EQ_INT(lw_paper_h_pt(LW_LEGAL), 1008);

    lw_info_t qms = p.info;
    strcpy(qms.product, "QMS-PS 410");
    qms.page = LW_A4;
    lw_caps_fill(&lc, &qms);
    CHECK(strcmp(lc.caps.make_model, "QMS-PS 410") == 0);
    CHECK(strcmp(lc.caps.device_id, "MFG:QMS;MDL:QMS-PS 410;CMD:URF;") == 0);
    CHECK(strcmp(lc.caps.media, "iso_a4_210x297mm") == 0);
    CHECK_EQ_INT(lc.caps.max_dots, 2480);

    // UUIDs: stable per name and epoch, different for another (Review Focus 4).
    static const uint8_t mac[6] = { 0xa1, 0xb2, 0xc3, 0xd4, 0xe5, 0xf6 };
    static const uint8_t nt[] = "\x11LaserWriter II NT";
    static const uint8_t ntx[] = "\x12LaserWriter II NTX";
    static const uint8_t wells[] = "\x1fWells\xd5s LaserWriter in the hall";
    char u1[46], u2[46], u3[46], u4[46];
    lw_uuid(u1, mac, 1, nt);
    lw_uuid(u2, mac, 1, nt);
    lw_uuid(u3, mac, 1, ntx);
    lw_uuid(u4, mac, 2, nt);
    CHECK(strcmp(u1, u2) == 0);
    CHECK(strcmp(u1, u3) != 0);
    CHECK(strcmp(u1, u4) != 0);
    CHECK(strncmp(u1, "urn:uuid:41575254-4c57-0001-", 28) == 0);
    CHECK(strcmp(u1 + 32, "-a1b2c3d4e5f6") == 0);
    CHECK_EQ_INT(strlen(u1), 45);
    lw_uuid(u3, mac, 1, wells);
    CHECK_EQ_INT(strlen(u3), 45);

    TEST_END;
}
