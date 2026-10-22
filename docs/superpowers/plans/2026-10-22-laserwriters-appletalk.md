# AirPrint to LaserWriters on AppleTalk (W-1082) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A LaserWriter heard on the cable or an EtherTalk link can be shared as a second AirPrint printer: iPhone pages are ripped at 300 dpi, sent as Level 1 PostScript `imagemask` bands over a PAP client, with the LaserWriter's status shown on the phone, and the ImageWriter untouched.

**Architecture:** Pure C cores, host-tested in `tools/rip` and `tools/ipp` as the codebase already does, with thin ESP-IDF shells. `printer_caps` replaces the ImageWriter constants in `ipp_attrs` and `ripjob`; `ripjob` gains one output-stage hook that `ps_page` fills. A PAP client rides the existing `lt_node` (cable and EtherTalk alike). A new `laser` component holds the LaserWriter's query, status mapping and policy (pure) plus the firmware glue (`laser.c`). `ipp_server` becomes per-printer, with the ImageWriter as printer 0, byte for byte as today.

**Tech Stack:** C11, ESP-IDF 5.5.1, FreeRTOS, ESP mDNS (multiple instances), Python 3 (etpeer, fixtures), Ghostscript (host only), CUPS `ipptool`.

**Spec:** `docs/superpowers/specs/2026-10-21-laser-printers-design.md` (phase 1). Read it, including "Calls made in this design" and "What only a printer settles", before any task.

## Global Constraints

- The ImageWriter's bytes do not move: `make -C tools/rip regress` (four fixtures), its IPP attribute bytes (Task 1's golden), its TXT record, `printer-uuid` and job ids.
- LaserWriter AirPrint: URF `RS300`, `W8` only; no colour, no fanfold; `media-supported`/`media-ready` only the size the printer reported; margins from its imageable area, rounded outward to whole 1/100 mm.
- `printer-uuid` per LaserWriter: the MAC, a laser epoch, and a hash of the shared printer's NBP name.
- PostScript: DSC header `%!PS-Adobe-3.0`, `%%Creator: AirWriter <version>`, `%%Title:` escaped; Level 1 operators only; no LaserPrep, no filters; per page `save` … `restore showpage`; top-left pixel at the paper's top-left at 72/300 scale; bands of at most 64 rows, ended by a blank row (skipped); each band trimmed to its first and last non-zero byte; `imagemask` polarity `true`; one prolog string read with `readstring` (binary) or `readhexstring` (hex); exactly one LF, never CR LF, between the procedure name and binary data; hex lines broken every 64 digits.
- PAP client: one ATP socket for OpenConn, data and CloseConn (it is the responding socket named in OpenConn), a separate socket for SendStatus; OpenConn data `[socket, quantum 8, wait u16]`; busy (`0xFFFF`) retried every 2 s with the wait in seconds since the first try, giving up at `CONFIG_AIRWRITER_STALL_ABORT` (600 s); Data packets at most 512 bytes, at most 8 per SendData, user bytes `[connID, 4, EOF, 0]`; the printer's sequence numbers start at 1 and wrap skipping 0, a repeated SendData gets a byte-identical reply; tickle every 30 s, dead after 120 s silent; SendStatus `[0, 8, 0, 0]`, reply `[0, 9, 0, 0]` + 4 bytes + Pascal string.
- NBP `=:LaserWriter@*` when the card is opened and every 30 s while one is shared; SendStatus every 5 s while shared; no NBP or status answer for 30 s → `stopped` + `offline-report`.
- NVS namespace `laser`. Sharing is off by default. Docs say LaserWriters are untested.
- Ghostscript is a host test tool only (AGPL): never linked, never shipped. Call it by path (`/opt/homebrew/bin/gs`): `gs` is aliased to `git status` in Wells's shell.
- New source files carry an SPDX header (firmware/tools GPL-3.0-or-later, docs CC-BY-SA-4.0). Nothing from TailTalk or netatalk is copied; they are read as references.
- Bench: never OTA or flash board 1 without the main session's go (bench rules in `AGENTS.md`); ask Wells before any print on the ImageWriter.

## Review Focus

1. **A page bigger than the LaserWriter's paper** (an A4 or Legal URF page shared to a Letter printer): placed top-left, clipped, logged; no row wider than the prolog string. Test in Task 4.
2. **A page with nothing on it, or ending in a band that runs to the last row**: the page still prints (an empty page is `save restore showpage`), and the last band is flushed. Test in Task 4.
3. **Printer output split across Data packets mid-line, CR or LF line ends, `%%[ … ]%%` lines between the answers**: the query parser still reads every answer. Test in Task 6.
4. **NBP names that are not ASCII or are 32 characters** (`Wells’s LaserWriter` in MacRoman): stable UUID, a UTF-8 Bonjour and IPP name, escaped JSON. Test in Task 6 and Task 12.
5. **The shared LaserWriter vanishing mid-job** (cable pulled, tunnel gone, printer silent): the job aborts, the job lock is freed, and the next ImageWriter job prints. Tests in Task 8 (client) and Task 9 (interop `drop`).

## Calls made in this plan (beyond the spec)

- **Toner low does not stop the printer.** The spec lists it among the reasons that set `stopped`; IPP's `-warning` reasons mean "still printing", and a LaserWriter prints on with low toner. `toner-low-warning` is reported with the printer idle or processing. Every `-error` stops it, as the spec says.
- **Routing by the HTTP path.** `/ipp/laser` is the LaserWriter; every other path is the ImageWriter, as every path is today.
- **Job ids per printer**, so the ImageWriter's run 1, 2, 3 as now.
- **No `_printer._tcp` for the LaserWriter.** It is a stub for old LPD probes; one is enough.
- **The icon is an original drawing** of a LaserWriter II in the ImageWriter icon's style, under this repository's licence, not a copy of Apple's.
- **A host interop test** (Task 9): the board's PAP client, built for the host, prints to the fake LaserWriter over a pipe before anything goes to board 1.
- **The fake LaserWriter runs on this Mac** for the bench: Basilisk II runs on the Pi at 10.0.1.110, so UDP 6066 is free here.

---

## File map

| Path | Role |
|---|---|
| `firmware/components/printer_caps/` (new) | `printer_caps_t`, the ImageWriter's constant, the URF string builder |
| `firmware/components/ipp_attrs/ipp_attrs.{c,h}` | attributes from a printer's caps |
| `firmware/components/ripjob/ripjob.{c,h}` | geometry from caps; `rip_stage_t` output hook |
| `firmware/components/ps_page/` (new) | 1-bit rows → Level 1 PostScript |
| `firmware/components/laser/lw_query.c`, `lw_status.c`, `lw_caps.c`, `lw_core.c` (new, pure) | query program and parser, status mapping, caps/UUID from the answers, the share/poll/offline policy |
| `firmware/components/laser/laser.c` (new, IDF) | NVS, the spool, the PAP client's owner, the IPP sink, `/api/laser` JSON |
| `firmware/components/localtalk/pap_client.{c,h}` (new, pure) | the PAP client state machine |
| `firmware/components/localtalk/lt_node.{c,h}` | roles (`serve`), NBP lookups out, the client's ATP sockets |
| `firmware/components/localtalk/localtalk.c`, `ethertalk.c` | a link runs for either role; lookups; the client attached |
| `firmware/components/ipp_server/` | per-printer state, routing by path, a second mDNS instance, icons |
| `firmware/components/job_pipe/` | a per-job target (caps, stage, screen) |
| `firmware/main/main.c` | wiring: the laser's IPP sink under the job lock, the panel's line source, `/api/laser` |
| `firmware/components/webui/` | `/api/laser` routes |
| `tools/rip/` | `--printer laser`, `--ps`, `--out-pbm`, `--binary/--hex`; tests; `lwhost` harness; `pscheck` and `interop` targets |
| `tools/ipp/` | golden ImageWriter response; two-printer tests; `gpa_server` serves both |
| `tools/ethertalk/etpeer.py` | `pipe` transport and the `laserwriter` mode; `test_interop.py` |
| `tools/ippcap/ippcap.py` | `--urf` so an iPhone can be captured at `RS300` |
| `tools/icon/laserwriter.txt`, `mkicons.py` | the LaserWriter icon |
| `tools/webui/` | the LaserWriters card and its preview states |
| `docs/`, `AGENTS.md`, `LICENSE.md` | SPEC §3.14, traps, untested note, icon licence row |

---

### Task 1: Pin the ImageWriter's IPP bytes

**Files:**
- Create: `tools/ipp/test/test_ipp_golden.c`, `tools/ipp/test/golden/imagewriter-gpa-all.bin`, `imagewriter-gpa-mcd.bin`, `imagewriter-gpa-colour.bin`, `imagewriter-job.bin`, `imagewriter-txt.txt`
- Modify: `tools/ipp/Makefile` (nothing if the wildcard picks the test up)

**Interfaces:**
- Produces: golden files later tasks must keep passing; `GOLDEN_REGEN=1` rewrites them (used once here only).

- [ ] **Step 1:** Write `test_ipp_golden.c`: build `ipp_printer_t` exactly as `ipp_server.c`'s `fill_printer` does for an idle board (`uri` `ipp://airwriter.local:631/ipp/print`, name `ImageWriter II`, info `Apple ImageWriter II`, uuid `urn:uuid:41575254-4552-0002-a1b2c3d4e5f6`, icons `http://airwriter.local:631/icon-{48,128,512}.png`, state idle, reason `none`, accepting 1, uptime 1). Encode op attrs + printer attrs for (a) no requested-attributes, (b) `media-col-database` named, (c) (a) with `colour = 1`, and (d) op attrs + job attrs for a processing job (id 3, name `Letter`, user `wells`). With `GOLDEN_REGEN=1` write the bytes to `test/golden/`; otherwise `CHECK_EQ_MEM` against them. A second check builds the TXT item list the way `add_print_services` does (factor nothing yet; copy the table into the test) and compares `key=value` lines with `imagewriter-txt.txt`.
- [ ] **Step 2:** `GOLDEN_REGEN=1 make -B -C tools/ipp test`, then `make -B -C tools/ipp test` → all `ok`. Commit the golden files with the test.

```bash
git add tools/ipp/test/test_ipp_golden.c tools/ipp/test/golden
git commit -m "ipp: golden bytes for the ImageWriter's printer attributes (W-1082)"
```

### Task 2: `printer_caps`, and attributes from it

**Files:**
- Create: `firmware/components/printer_caps/{CMakeLists.txt,include/printer_caps.h,printer_caps.c}`
- Modify: `firmware/components/ipp_attrs/{ipp_attrs.c,include/ipp_attrs.h,CMakeLists.txt}`, `firmware/components/ipp_server/ipp_server.c` (TXT `URF` from caps), `tools/ipp/Makefile`, `tools/ipp/gpa_server.c`
- Test: `tools/ipp/test/test_printer_caps.c`; Task 1's golden must still pass

**Interfaces:**
- Produces:
  ```c
  typedef struct {
      const char *make_model;      // printer-make-and-model, TXT ty
      const char *device_id;       // printer-device-id
      const char *urf_rs;          // "RS72-144" | "RS300"
      uint16_t    res_x, res_y;    // printer-resolution-*: 144x72 | 300x300
      const char *media;           // "na_letter_8.5x11in" ...
      int32_t     media_x, media_y;                        // 1/100 mm
      int32_t     margin_l, margin_r, margin_t, margin_b;  // 1/100 mm
      int32_t     ppm;             // pages-per-minute
      uint16_t    rip_dpi_x, rip_dpi_y;   // 144x72 | 300x300
      uint32_t    max_dots;        // widest row the output stage takes; 0 = any
      uint8_t     crop_margin;     // crop up to 1/4 in a side to reach max_dots
  } printer_caps_t;
  extern const printer_caps_t printer_caps_imagewriter;
  // "V1.4,CP1,<rs>,W8[,SRGB24],DM1,IS1,MT1,FN3"; length, 0 if cap is short.
  size_t printer_caps_urf(const printer_caps_t *c, int colour, char *out, size_t cap);
  // The same as keywords for urf-supported; returns the count (<= 9).
  size_t printer_caps_urf_keywords(const printer_caps_t *c, int colour, const char *kw[9]);
  ```
  `ipp_printer_t` gains `const printer_caps_t *caps;` (NULL means `&printer_caps_imagewriter`).

- [ ] **Step 1: Failing test** `test_printer_caps.c`: `printer_caps_urf(&printer_caps_imagewriter, 0, ...)` equals `IPP_URF_STRING`, with colour equals `IPP_URF_STRING_COLOUR`; a caps with `urf_rs = "RS300"` gives `"V1.4,CP1,RS300,W8,DM1,IS1,MT1,FN3"`; keyword count 8 / 9; the ImageWriter constant holds 144/72, 1152 (`IW_MAX_DOTS`), crop 1, Letter 21590×27940, margins 635/635/2540/1270, ppm 1, make-and-model `Apple ImageWriter II`, device id `MFG:Apple;MDL:ImageWriter II;CMD:URF;`.
- [ ] **Step 2:** `make -B -C tools/ipp test` → fails to link.
- [ ] **Step 3:** Implement `printer_caps`; move the ImageWriter's constants out of `ipp_attrs.c` into `printer_caps_imagewriter`; `ipp_write_printer_attrs` and `write_media_col` read `p->caps` (or the ImageWriter's). `IPP_URF_STRING*` stay defined (the regression test and comments use them) and must equal the builder's output. `ipp_server.c`'s TXT `URF` comes from `printer_caps_urf`. Add `printer_caps` to `ipp_attrs`' REQUIRES and to `tools/ipp/Makefile`'s `INCS`/`SRCS`.
- [ ] **Step 4:** `make -B -C tools/ipp test` → all ok, golden unchanged. `idf.py build` clean.
- [ ] **Step 5:** Commit `printer_caps: the printer's capabilities in one struct; the ImageWriter's bytes unchanged (W-1082)`.

### Task 3: `ripjob` geometry from caps, and the output-stage hook

**Files:**
- Modify: `firmware/components/ripjob/{ripjob.c,include/ripjob.h,CMakeLists.txt}`, `tools/rip/Makefile` (printer_caps include/src)
- Test: `tools/rip/test/test_ripjob_stage.c`; `make -C tools/rip regress` unchanged

**Interfaces:**
- Consumes: `printer_caps_t` (Task 2).
- Produces, in `ripjob.h`:
  ```c
  typedef struct {
      int  (*page_begin)(void *ctx, int index, uint32_t w, uint32_t h);  // 0, or abort the job
      void (*row)(void *ctx, const uint8_t *bits);   // 1bpp, MSB = leftmost pixel, set = ink
      int  (*page_end)(void *ctx);                    // 0, or abort the job
      void  *ctx;
  } rip_stage_t;
  ```
  `ripjob_cfg_t` gains `const printer_caps_t *caps;` (NULL: ImageWriter) and `const rip_stage_t *stage;` (NULL: the ImageWriter's strips through `out`). With a stage, RGB pages print as gray (`colour` ignored) and `top_margin` is ignored.

- [ ] **Step 1: Failing test** `test_ripjob_stage.c`: a recording stage over `fixtures/checker.urf` with a 300×300 caps (`max_dots` 2550, `crop_margin` 0) checks one `page_begin` with `w == src_w * 300 / res` and `h == src_h * 300 / res`, `h` rows each `(w+7)/8` bytes, one `page_end`, and that `out` was never called; a second run with `caps = NULL, stage = NULL` produces the same bytes as `fixtures/checker.expected.bin`; a third with an A4-wide page (2480 px at 300) into Letter caps whose `max_dots` is 2400 (forcing it) gets `w == 2400` and a log line saying it clipped (Review Focus 1: a stage never gets a row wider than `max_dots`, and with `crop_margin` 0 it is clipped on the right, not scaled).
- [ ] **Step 2:** `make -C tools/rip build/test_ripjob_stage && ./tools/rip/build/test_ripjob_stage` → fails.
- [ ] **Step 3:** In `ripjob_run`, take `RIPJOB_DPI_*`, `IW_MAX_DOTS` and the quarter-inch crop from `caps`; with a stage and a page wider than `max_dots`, keep the source's left `max_dots` worth (no crop margin, no scaling) and log it. Route rows to `stage->row` instead of `iw_row`. Keep `RIPJOB_DPI_X/Y` as the ImageWriter caps' values for existing callers.
- [ ] **Step 4:** `make -C tools/rip test regress` → all ok, four regression lines unchanged.
- [ ] **Step 5:** Commit `ripjob: geometry from printer_caps and an output-stage hook; ImageWriter streams unchanged (W-1082)`.

### Task 4: `ps_page`, `rip --ps`, and Ghostscript bit for bit

**Files:**
- Create: `firmware/components/ps_page/{CMakeLists.txt,include/ps_page.h,ps_page.c}`, `tools/rip/test/test_ps_page.c`, `tools/rip/test/pscheck.py`, `tools/rip/test/ps_level1_ops.txt`
- Modify: `tools/rip/main.c` (`--printer imagewriter|laser`, `--ps FILE`, `--hex`, `--out-pbm FILE`, `--paper letter|a4|legal`), `tools/rip/Makefile` (`pscheck` target, ps_page in `SRCS`)

**Interfaces:**
- Consumes: `rip_stage_t` (Task 3).
- Produces:
  ```c
  #define PS_BAND_ROWS 64
  #define PS_ROW_MAX   320       // bytes: the prolog string; 2560 px at 300 dpi
  typedef struct {
      void      (*out)(void *ctx, const uint8_t *buf, size_t n);
      void       *ctx;
      int         binary;      // 1: readstring, raw bytes; 0: readhexstring, hex
      uint32_t    paper_h_pt;  // the paper's height in points: 792, 842, 1008
      const char *title;       // the job name, UTF-8; escaped into %%Title
      const char *creator;     // "AirWriter 0.3.0"
  } ps_cfg_t;
  typedef struct ps ps_t;      // opaque; ps_new allocates it and its band (PSRAM on the board)
  ps_t *ps_new(const ps_cfg_t *cfg);
  void  ps_job_begin(ps_t *ps);           // header and prolog
  void  ps_job_end(ps_t *ps);             // %%Trailer, %%Pages: n, %%EOF
  void  ps_free(ps_t *ps);
  void  ps_stage(ps_t *ps, rip_stage_t *st);   // fills a rip stage that writes pages
  ```
  `rip --printer laser` uses a Letter caps at 300 dpi built in `main.c` (the board's will come from Task 6).

- [ ] **Step 1: Failing unit tests** in `test_ps_page.c` (no Ghostscript): the job starts with `%!PS-Adobe-3.0\n`, has `%%Creator: AirWriter test`, `%%Title: (a \(b\) \\c)` for title `a (b) \c` and non-ASCII as `\ooo` octal; ends with `%%Trailer`, `%%Pages: 2`, `%%EOF`; a page of 130 rows where rows 64–65 are blank emits bands of 64 and 64 rows (the blank rows end the first band and are skipped) and the bands' x offset and width match the trimmed bytes; an all-blank page emits `save`…`restore showpage` and no `imagemask` call (Review Focus 2); a page whose ink reaches its last row flushes that band; in binary each band's procedure line ends in exactly `\n` and the next `h * wbytes` bytes are the rows; in hex no line exceeds 64 digits and the digits decode to the rows; a 2560-px row is accepted, a wider one is clipped to `PS_ROW_MAX` with nothing past it written.
- [ ] **Step 2:** Build and run → fails.
- [ ] **Step 3:** Implement `ps_page.c`. Prolog defines one string (`PS_ROW_MAX` long) and one band procedure that takes `x y w h wbytes`, sets a `getinterval` of the string `wbytes` long, `translate`s, and runs `w h true [1 0 0 1 0 0] {currentfile s readstring pop} imagemask` (or `readhexstring`). Page setup: `0 <paper_h_pt> translate 72 300 div dup neg scale`. The exact prolog text is the implementer's, Level 1 only.
- [ ] **Step 4:** Add `rip --ps` and `--out-pbm` (the rip's own 1-bit page, 1 = ink, as raw PBM). Write `pscheck.py`: for each of `fixtures/checker.urf` and the Task 5 pages (once they exist; checker only until then), in binary and hex, run `rip --printer laser --ps a.ps --out-pbm a.pbm`, render with `/opt/homebrew/bin/gs -q -dSAFER -dBATCH -dNOPAUSE -sDEVICE=pbmraw -r300 -dDEVICEWIDTHPOINTS=612 -dDEVICEHEIGHTPOINTS=792 -dFIXEDMEDIA -sOutputFile=g.pbm a.ps` and require `g.pbm` equal to `a.pbm` bit for bit (diff the first differing row on failure). Then run the same file under `-c "1 .setlanguagelevel" -f a.ps` and require the same bits. Also tokenise the prolog and require every executable name to be in `ps_level1_ops.txt` (the PLRM 1st edition operator list) or defined by the prolog. Exit non-zero with the reason. Make target `pscheck` runs it and says `brew install ghostscript` when the binary is missing.
- [ ] **Step 5:** `brew install ghostscript` (if absent), `make -C tools/rip test pscheck` → all ok.
- [ ] **Step 6:** Commit `ps_page: 1-bit pages as Level 1 PostScript imagemask bands, checked against Ghostscript bit for bit (W-1082)`.

### Task 5: 300 dpi pages, and a way to capture one from an iPhone

**Files:**
- Create: `tools/rip/test/make_300dpi_fixture.py`
- Modify: `tools/rip/test/pscheck.py` (add the pages), `tools/ippcap/ippcap.py` (`--urf STRING`, used in the attributes and printed in the `dns-sd` line it suggests), `fixtures/README.md`

**Interfaces:**
- Produces: `make_300dpi_fixture.py text|photo OUT.urf` writes a Letter page at 300 dpi (2550×3300, 8-bit gray, URF with its RLE), deterministic: `text` is rows of glyph-like bars on white with 1 in margins (about 50 % ink-free rows), `photo` is a full-bleed gradient with seeded noise. Not committed (generated at test time, like the checker).

- [ ] **Step 1:** Write the script; check with `rip --in text.urf --printer laser --ps t.ps` that it rips (pages 1, 2550×3300).
- [ ] **Step 2:** Add both pages to `pscheck.py`; print each page's PostScript size against its full 1-bit size (970 KB), so the text page's 40–50 % claim is measured. `make -C tools/rip pscheck` → ok.
- [ ] **Step 3:** `ippcap.py --urf V1.4,CP1,RS300,W8,DM1,IS1,MT1,FN3` advertises that in `urf-supported` and its suggested `dns-sd` line. (Capturing a real page is Wells's step in Task 15.)
- [ ] **Step 4:** Commit `fixtures: synthetic 300 dpi text and photo pages; ippcap can advertise RS300 (W-1082)`.

### Task 6: The query job, the status words, caps and UUID from the answers (pure)

**Files:**
- Create: `firmware/components/laser/{CMakeLists.txt,include/lw_query.h,include/lw_status.h,include/lw_caps.h,lw_query.c,lw_status.c,lw_caps.c}`, `tools/rip/test/test_lw.c`, `tools/rip/test/lwquery_gs.sh`
- Modify: `tools/rip/Makefile` (`build/test_lw`; `pscheck` also runs `lwquery_gs.sh`)

**Interfaces:**
- Consumes: `printer_caps_t` (Task 2).
- Produces:
  ```c
  typedef enum { LW_LETTER = 0, LW_A4, LW_LEGAL } lw_page_t;
  typedef struct {
      char      product[64], version[16];
      int32_t   revision;
      uint8_t   level;            // 1, 2 or 3
      uint8_t   binary;           // the probe came back 256 bytes in order
      uint16_t  dpi;              // device resolution
      lw_page_t page;
      int32_t   area[4];          // imageable area llx lly urx ury, 1/100 pt
      uint8_t   complete;         // aw:end was seen
  } lw_info_t;
  void   lw_info_default(lw_info_t *i);       // LaserWriter, level 1, hex, 300, Letter, 0.25 in all round
  // The query job, the 256-byte binary probe included. Its length, 0 if cap is short.
  size_t lw_query_job(uint8_t *out, size_t cap);
  // Feeds the printer's output as it comes; lines may split across calls.
  typedef struct { lw_info_t info; char line[160]; size_t n; } lw_parse_t;
  void   lw_parse_init(lw_parse_t *p);
  void   lw_parse_feed(lw_parse_t *p, const uint8_t *d, size_t n);

  typedef struct {
      const char *reason;   // IPP keyword: "none", "media-empty-error", ...
      uint8_t     stopped;  // an -error: printer stopped, job processing-stopped
      uint8_t     busy;     // another job holds it ("busy", "printing", "job:")
  } lw_state_t;
  void lw_status_map(const char *status, lw_state_t *out);
  // A %%[ Error: ...; OffendingCommand: ... ]%% line: 1 and the text for job-state-message.
  int  lw_ps_error(const char *line, char *msg, size_t cap);

  // The AirPrint side of a queried LaserWriter.
  typedef struct { char make_model[72], device_id[112], media[24]; printer_caps_t caps; } lw_caps_t;
  void lw_caps_fill(lw_caps_t *c, const lw_info_t *i);
  // urn:uuid:41575254-4c57-<epoch>-<hash>-<mac>, hash = FNV-1a 32 of the Pascal name, folded to 16 bits.
  void lw_uuid(char out[46], const uint8_t mac[6], uint16_t epoch, const uint8_t *pascal_name);
  ```
  Query output format, one answer per line, every question inside `stopped` with a default: `aw:product=`, `aw:version=`, `aw:revision=`, `aw:level=`, `aw:res=X Y`, `aw:area=llx lly urx ury`, `aw:size=W H` (Level 2 only), `aw:probe=COUNT INORDER` (`INORDER` 1 when byte i is i), `aw:end`. Page from `aw:size`, else from the area (ury > 842 → Legal, > 792 → A4, else Letter).

- [ ] **Step 1: Failing tests** in `test_lw.c`:
  - parse a canned LaserWriter II NT answer fed one byte at a time, with `\r` line ends and a `%%[ status: busy ]%%` line between answers → product `LaserWriter II NT`, version `47.0`, level 1, dpi 300, area `{1416, 840, 59784, 78360}`, Letter, binary 1, complete (Review Focus 3);
  - a probe line `aw:probe=200 0` → binary 0; no `aw:end` → complete 0, defaults kept;
  - `lw_status_map`: `status: idle` → none/0/0; `status: PrinterError: out of paper` and `paper tray empty` → `media-empty-error` stopped; `paper jam` → `media-jam-error`; `cover open`, `door open` → `door-open-error`; `toner low` → `toner-low-warning` not stopped; `no toner`, `out of toner` → `toner-empty-error` stopped; `PrinterError: service call` → `other-error` stopped; `job: x; status: busy; source: AppleTalk` → busy, not stopped; `status: warming up` → none, not stopped; matching is case-insensitive;
  - `lw_ps_error("%%[ Error: undefined; OffendingCommand: foo ]%%")` → 1, `undefined; OffendingCommand: foo`;
  - `lw_caps_fill` on that answer → make-and-model `Apple LaserWriter II NT`, device id `MFG:Apple;MDL:LaserWriter II NT;CMD:URF;`, `na_letter_8.5x11in` 21590×27940, margins left 500, right 500, top 297, bottom 297 (each `ceil(points × 2540 / 72)`: 14.16, 612 − 597.84, 792 − 783.60, 8.40), `RS300`, 300×300, `max_dots` 2550, `crop_margin` 0; a product `QMS-PS 410` → make-and-model `QMS-PS 410`, `MFG:QMS`;
  - `lw_uuid` is stable for the same name, differs for another name or epoch, and takes MacRoman bytes (`Wells\xd5s LaserWriter`, 32 bytes long) without trouble (Review Focus 4).
- [ ] **Step 2:** Build and run → fails.
- [ ] **Step 3:** Implement. The query program text is the implementer's; TailTalk's `dump-statusdict` is the model. Margins round outward (ceil).
- [ ] **Step 4:** `lwquery_gs.sh` runs `lw_query_job`'s bytes (dumped by a tiny `rip --query-job FILE` option) through `/opt/homebrew/bin/gs -q -dNODISPLAY -dBATCH -dNOPAUSE -` and through `-c "1 .setlanguagelevel"`, feeds the output to a `test_lw --parse` mode, and requires `complete`, level 3 then 1, `binary` 1, a Letter page.
- [ ] **Step 5:** `make -C tools/rip test pscheck` → ok. Commit `laser: the query job, its parser, the status words and the LaserWriter's caps and UUID (W-1082)`.

### Task 7: `lt_node` roles, NBP lookups out, and room for a client

**Files:**
- Modify: `firmware/components/localtalk/{lt_node.c,include/lt_node.h,lt_proto.c,include/lt_proto.h}`
- Test: `tools/rip/test/test_localtalk.c`, `tools/rip/test/test_ethertalk.c` (new cases; existing ones unchanged)

**Interfaces:**
- Produces:
  ```c
  // lt_ops_t gains:
  void (*found)(void *ctx, const nbp_tuple_t *t, const lt_route_t *r);  // a LkUp-Reply tuple (not a name ask's)
  // lt_node_t gains: uint8_t serve; struct pap_client *client; uint8_t lkup_id;
  void     lt_node_set_serve(lt_node_t *n, int serve);   // init: 1
  void     lt_node_lookup(lt_node_t *n, const char *type, uint64_t now);   // =:type@* to every node
  void     lt_node_send_atp(lt_node_t *n, const lt_route_t *to, uint8_t src_sock, uint8_t ctl,
                            uint8_t bitmap, uint16_t tid, const uint8_t user[4],
                            const uint8_t *data, size_t len);
  uint16_t lt_node_next_tid(lt_node_t *n);
  void     lt_node_attach(lt_node_t *n, struct pap_client *c);   // NULL detaches
  ```
  The route in `found` is the one to reach the tuple's socket: a long header through the replying router (`via = llap_src`) when the reply came long from another network, as `on_nbp` answers today. ATP for `PAPC_SOCK`/`PAPC_SOCK_ST` (Task 8) goes to the attached client; `lt_node_tick` ticks it.

- [ ] **Step 1: Failing tests:** with `serve = 0`, a LkUp for `ImageWriter II:ImageWriter@*` gets no reply, a SendStatus and an OpenConn to SLS get nothing, AEP still echoes, and the node still acquires its ID; `lt_node_lookup` on LLAP sends one frame to LLAP 0xFF, DDP short, dst socket 2, NBP LkUp `=:LaserWriter@*` with our NIS address in the tuple; on ELAP the same as a long header to net 0 node 0xFF, which `et_link` sends to the AppleTalk multicast; a LkUp-Reply tuple `LaserWriter II NT:LaserWriter@*` at node 12 socket 0x85 reaches `found` with that address and `via` 12, while a Workstation name reply still goes to the name table and not `found`.
- [ ] **Step 2:** `make -C tools/rip build/test_localtalk build/test_ethertalk` and run → fail.
- [ ] **Step 3:** Implement; `pap_client` is a forward-declared struct here (Task 8 fills it). Leave all existing timers alone.
- [ ] **Step 4:** `make -C tools/rip test` → all ok. Commit `localtalk: roles, NBP lookups out, and a socket pair for a PAP client (W-1082)`.

### Task 8: The PAP client

**Files:**
- Create: `firmware/components/localtalk/{pap_client.c,include/pap_client.h}`, `tools/rip/test/test_pap_client.c`
- Modify: `tools/rip/Makefile`, `firmware/components/localtalk/CMakeLists.txt`

**Interfaces:**
- Consumes: Task 7's node API.
- Produces:
  ```c
  #define PAPC_SOCK        0x83     // OpenConn, data, CloseConn: our responding socket
  #define PAPC_SOCK_ST     0x84     // SendStatus
  #define PAPC_FLOW        8
  #define PAPC_BUSY_RETRY_MS  2000
  #define PAPC_TICKLE_MS     30000
  #define PAPC_DEAD_MS      120000
  #define PAPC_RETRY_MS       2000  // OpenConn, CloseConn, SendStatus ATP retries
  #define PAPC_READ_RETRY_MS 15000  // our SendData to the printer
  #define PAPC_EOF_WAIT_MS   30000  // after our EOF, for the printer's
  #define PAPC_HOLD_MS         500  // a SendData held for a fuller answer, at most
  typedef enum { PAPC_END_OK, PAPC_END_CANCELED, PAPC_END_BUSY, PAPC_END_DEAD,
                 PAPC_END_REFUSED, PAPC_END_DROPPED } papc_end_t;
  typedef struct {
      void   *ctx;
      size_t (*read)(void *ctx, uint8_t *buf, size_t max, int *eof);   // never blocks
      void   (*output)(void *ctx, const uint8_t *d, size_t n);         // the printer's output
      void   (*status)(void *ctx, const uint8_t *pstr);                // SendStatus, or busy OpenConn
      void   (*opened)(void *ctx);
      void   (*ended)(void *ctx, papc_end_t how);
      void   (*log)(void *ctx, const char *line);
  } papc_ops_t;
  typedef struct pap_client pap_client_t;   // defined in the header; ~5 KB (the last reply kept)
  void papc_init(pap_client_t *c, const papc_ops_t *ops, uint32_t seed);
  int  papc_open(pap_client_t *c, const lt_route_t *to, uint32_t busy_ms, uint64_t now); // 0, -1 if not idle or detached
  void papc_cancel(pap_client_t *c, uint64_t now);    // EOF in the next Data reply; before open: ends CANCELED
  void papc_status(pap_client_t *c, const lt_route_t *to, uint64_t now);
  int  papc_idle(const pap_client_t *c);
  void papc_link_down(pap_client_t *c, uint64_t now);  // ends DROPPED
  void papc_set_dead_ms(pap_client_t *c, uint32_t ms); // tests only; init sets PAPC_DEAD_MS
  // From lt_node only:
  void papc_rx(pap_client_t *c, const ddp_t *d, const atp_t *a, const lt_route_t *from, uint64_t now);
  void papc_tick(pap_client_t *c, uint64_t now);
  ```

- [ ] **Step 1: Failing tests** in `test_pap_client.c`, driving a node with a fake clock and a scripted printer (frames in, frames captured): OpenConn goes XO to the printer's SLS from 0x83 with data `[0x83, 8, 0, 0]`; a busy reply (`0xFFFF`, status `busy; source: AppleTalk`) calls `status` and retries at +2 s with wait `2`, +4 s with `4`; at 600 s ends `BUSY`; a cancel while busy ends `CANCELED` with no second OpenConn; an OK reply calls `opened`; the printer's SendData seq 1 with 10 KB queued gets 8 packets of 512 bytes, user `[id, 4, 0, 0]`, the last with EOM; with only 700 bytes and no EOF the answer is held up to 500 ms, then 2 packets (512 + 188); after EOF the last packet carries `[id, 4, 1, 0]`; the same SendData again (same TID, or same seq with a new TID) gets byte-identical packets and reads nothing new; seq 65535 then 1 (0 skipped) are both new; our SendData to the printer's session socket starts at seq 1 and the printer's Data comes out through `output`; after our EOF a further SendData gets an empty EOF reply; the printer's EOF then sends CloseConn and ends `OK`; no printer EOF in 30 s also closes, ends `OK` and logs it; a tickle goes out every 30 s; 120 s of silence ends `DEAD` (Review Focus 5); `papc_link_down` ends `DROPPED`; SendStatus goes from 0x84 to SLS with `[0, 8, 0, 0]` and the reply's string reaches `status`; no Data packet is ever over 512 bytes.
- [ ] **Step 2:** Build, run → fail.
- [ ] **Step 3:** Implement against TailTalk's `pap.rs` behaviour (reference only). Check each constant above against it and note any disagreement in the commit message.
- [ ] **Step 4:** `make -C tools/rip test` → ok. Commit `localtalk: a PAP client beside the server, host-tested (W-1082)`.

### Task 9: A fake LaserWriter, and the client against it on the host

**Files:**
- Create: `tools/rip/lwhost.c` (built as `tools/rip/build/lwhost`), `tools/ethertalk/test_interop.py`
- Modify: `tools/ethertalk/etpeer.py`, `tools/ethertalk/README.md`, `tools/rip/Makefile` (`lwhost`, `interop` target)

**Interfaces:**
- Consumes: Tasks 4, 6, 7, 8.
- Produces:
  - Frame pipe format, both directions: big-endian u16 length, then one Ethernet frame (as `et_link` sends and receives them).
  - `lwhost lookup | query NAME | print NAME FILE.ps [--cancel-after BYTES] | status NAME`: one ELAP `et_link` (MAC `B2:00:00:00:00:02`) with a PAP client, frames on stdin/stdout, results as lines on stderr: `found <name> <net>.<node>:<sock>`, `status <text>`, `query product=… level=… binary=0|1 page=…`, `ended ok|canceled|busy|dead|refused|dropped`, `output <line>`. Exit 0 on `ended ok`.
  - `etpeer.py tunnel BOARD_IP laserwriter NAME [--hex-only] [--out DIR]` and `etpeer.py pipe laserwriter NAME …` (frames on stdin/stdout): AARP, registers `NAME:LaserWriter@*`, answers LkUp (`=` and exact), an ATP responder with an XO cache, a PAP server (OpenConn with its socket and quantum 8; pulls with SendData seq starting at 1 and wrapping past 0; answers the client's SendData with its output and then EOF; tickles; CloseConn), SendStatus with its current status. Commands on its stdin (tunnel mode) or programmatically: `busy SECONDS`, `paper`, `jam`, `ok`, `error` (the next job gets `%%[ Error: undefined; OffendingCommand: aw_x ]%%` then `%%[ Flushing: rest of job (to end-of-file) will be ignored ]%%`), `drop` (stops answering anything). It recognises the AirWriter query job and answers from canned II NT values, the probe in order (or, with `--hex-only`, `aw:probe=256 0`, as a Level 1 machine that mangles binary). Each print job is saved as `job-N.ps` and, when Ghostscript is installed, rendered to `job-N.png` at 300 dpi.

- [ ] **Step 1: Failing interop test** `test_interop.py` (spawns `lwhost` with pipes, runs `etpeer`'s fake in the same process over the `pipe` transport): `lookup` finds the fake; `query` reports binary 1, and binary 0 with `--hex-only`; `print` of `rip --printer laser --ps` output of the checker ends `ok` and `job-1.ps` equals the file byte for byte; `busy 5` then print waits, reports the busy status, then ends `ok`; `paper` during a print reports the out-of-paper status line, `ok` lets it finish; `error` ends with the PostScript error on an `output` line; `--cancel-after 4096` ends `canceled` and the saved job is shorter; `drop` mid-job ends `dead` (run with `lwhost --dead-ms 5000`, which calls `papc_set_dead_ms`).
- [ ] **Step 2:** `make -C tools/rip interop` → fails.
- [ ] **Step 3:** Implement `lwhost.c`, the `Pipe` transport and the `LaserWriter` class in `etpeer.py` (TailTalk's `PapServer` and netatalk's `papd` as references), keeping etpeer's existing modes working.
- [ ] **Step 4:** `make -C tools/rip interop` → ok; `python3 tools/ethertalk/etpeer.py tunnel 10.0.1.115 lookup` still works (read-only, no print). Commit `etpeer: a fake LaserWriter; the PAP client proven against it on the host (W-1082)`.

### Task 10: `ipp_server` per printer, the ImageWriter unchanged

**Files:**
- Modify: `firmware/components/ipp_server/{ipp_server.c,include/ipp_server.h,CMakeLists.txt}`, `firmware/components/ipp_attrs/include/ipp_attrs.h`, `tools/ipp/gpa_server.c`
- Test: `tools/ipp/test/test_ipp_golden.c` (unchanged expectations), `tools/ipp/test/test_ipp_route.c` (new)

**Interfaces:**
- Consumes: `printer_caps_t`, `lw_caps_t` (Tasks 2, 6).
- Produces:
  ```c
  #define IPP_PRINTERS 2   // 0 the ImageWriter (ipp_server_cfg_t), 1 a shared LaserWriter
  typedef struct {
      const char *resource;     // "ipp/laser": the path, and TXT rp
      const char *name;         // Bonjour instance, printer-name (UTF-8)
      const char *info;         // printer-info: the NBP name
      const char *uuid;         // urn:uuid:...
      const printer_caps_t *caps;
      const char *icon;         // "laser": served as /laser-icon-48.png ...
      ipp_doc_sink_t sink;
  } ipp_printer_cfg_t;
  // Puts printer idx on the network (strings copied), replaces it, or with NULL takes it
  // off. ESP_ERR_INVALID_STATE while its job holds the printer.
  esp_err_t ipp_server_set_printer(int idx, const ipp_printer_cfg_t *cfg);
  void ipp_server_job_done_for(int idx, int ok);
  void ipp_server_set_stopped_for(int idx, const char *reason, const char *message);  // message copied
  // Pure, for the host test: which printer a POST path names; -1 for none.
  int  ipp_route(const char *path, const char *const resources[IPP_PRINTERS]);
  ```
  The existing `ipp_server_job_done`, `_set_stopped`, `_set_colour`, `_set_airprint` act on printer 0. One print lock and one waiter across both printers (SPEC §3.4); job slot, ids, stop reason and message per printer.
  `ipp_job_t` gains `const char *state_message`, written as `job-state-message` only when non-empty (the ImageWriter never sets it, so Task 1's job golden holds); `void ipp_server_job_message_for(int idx, const char *msg)` sets it (copied) for the job in the slot, which is how a PostScript error reaches the phone.

- [ ] **Step 1: Failing test** `test_ipp_route.c`: `/ipp/print`, `/`, `/anything` → 0; `/ipp/laser`, `/ipp/laser/` → 1; with no resource for 1, `/ipp/laser` → -1 (answered `client-error-not-found`). `ipp_route` lives in `ipp_req.c` so the host links it. In `test_ipp_attrs.c`, a job with `state_message` "undefined; OffendingCommand: foo" carries `job-state-message` with it, and one without carries none.
- [ ] **Step 2:** `make -B -C tools/ipp test` → fails.
- [ ] **Step 3:** Refactor `ipp_server.c`: per-printer struct (uri, uuid, name copies, caps, sink, job slot, next id, stop reason/message buffer, airprint, colour, mDNS registered); `dispatch` takes the printer; `fill_printer` builds from it (printer 0 exactly as today); printer 1's mDNS is `mdns_service_add_for_host(name, "_ipp", "_tcp", NULL, 631, txt…)` + `mdns_service_subtype_add_for_host(name, …, "_universal")`, removed with `mdns_service_remove_for_host`, TXT `rp=ipp/laser`, `ty`, `URF` (from caps), `Color=F`, `UUID` bare, no `_printer._tcp`; GET serves `/<icon>-icon-<size>.png`. `gpa_server` takes `--laser` to answer as a canned LaserWriter (Task 6's caps on the II NT answer) at `/ipp/laser` beside the ImageWriter.
- [ ] **Step 4:** `make -B -C tools/ipp test` → ok, golden unchanged. Then `make -C tools/ipp build/gpa_server && tools/ipp/build/gpa_server 8631 --laser &` and `ipptool -tI ipp://127.0.0.1:8631/ipp/print /usr/share/cups/ipptool/get-printer-attributes-suite.test` and the same for `/ipp/laser` → both pass. `idf.py build` clean.
- [ ] **Step 5:** Commit `ipp_server: a printer per resource; the ImageWriter answers as before, a LaserWriter beside it (W-1082)`.

### Task 11: The LaserWriter's policy (pure)

**Files:**
- Create: `firmware/components/laser/{lw_core.c,include/lw_core.h}`, `tools/rip/test/test_lw_core.c`

**Interfaces:**
- Consumes: `lw_state_t`, `lw_status_map` (Task 6), `lt_route_t`, `nbp_tuple_t`.
- Produces:
  ```c
  #define LW_HEARD_MAX     8
  #define LW_LOOK_MS   30000   // NBP while shared
  #define LW_STATUS_MS  5000   // SendStatus while shared (2 s while printing)
  #define LW_GONE_MS   30000   // no answer: offline-report
  #define LW_CARD_MS   60000   // the card counts as open this long after a GET
  typedef struct { uint8_t name[33]; uint8_t link; lt_route_t route; uint64_t heard, asked;
                   char status[80]; } lw_heard_t;
  typedef struct { /* heard[], shared name+link, queried, online, last status, job flag ... */ } lwc_t;
  typedef struct { uint8_t look, status_idx /* 0xFF none */, query; } lwc_want_t;
  typedef struct { int shared, online, idx; uint8_t ipp_state; const char *reason;
                   const char *message; uint8_t busy; } lwc_view_t;
  void       lwc_init(lwc_t *c, uint64_t now);
  void       lwc_card(lwc_t *c, uint64_t now);                                   // GET /api/laser
  void       lwc_share(lwc_t *c, const uint8_t *pascal_name_or_null, uint64_t now);
  void       lwc_found(lwc_t *c, const nbp_tuple_t *t, uint8_t link, const lt_route_t *r, uint64_t now);
  void       lwc_status(lwc_t *c, int idx, const uint8_t *pstr, uint64_t now);
  void       lwc_queried(lwc_t *c, int ok, uint64_t now);
  void       lwc_job(lwc_t *c, int printing);
  lwc_want_t lwc_tick(lwc_t *c, uint64_t now);
  lwc_view_t lwc_view(const lwc_t *c, uint64_t now);
  ```

- [ ] **Step 1: Failing tests:** a fresh core wants a lookup when the card opens and not again for 30 s unless shared; shared, it looks every 30 s and asks status every 5 s (2 s while printing); the shared printer heard on both the tunnel and the cable resolves to the cable; a shared printer's first answer after boot, and its first answer after being gone, want a query; no answer for 30 s gives `stopped`, `offline-report`; `status: PrinterError: out of paper` gives `stopped`, `media-empty-error` and the printer's words as the message; `busy` keeps it `processing`; a ninth printer replaces the one quiet longest; a heard printer not answering for 10 minutes leaves the list.
- [ ] **Step 2:** Build, run → fail. **Step 3:** Implement. **Step 4:** `make -C tools/rip test` → ok.
- [ ] **Step 5:** Commit `laser: when to look, ask and query, and what the phone is told (W-1082)`.

### Task 12: The board: links, `laser.c`, the print path, `/api/laser`

**Files:**
- Create: `firmware/components/laser/laser.c`, `firmware/components/laser/include/laser.h`
- Modify: `firmware/components/localtalk/{localtalk.c,include/localtalk.h,ethertalk.c,include/ethertalk.h}`, `firmware/components/job_pipe/{job_pipe.c,include/job_pipe.h}`, `firmware/components/webui/{webui.c,include/webui.h}`, `firmware/main/{main.c,Kconfig.projbuild,CMakeLists.txt}`, `firmware/components/ipp_server/CMakeLists.txt` (laser icons, from Task 13)

**Interfaces:**
- Consumes: everything above.
- Produces:
  ```c
  // localtalk.h / ethertalk.h
  void localtalk_set_roles(int serve, int laser);   // neither: quiet as today; laser alone: runs, answers as nothing
  void localtalk_lookup(const char *type);
  void localtalk_attach(pap_client_t *c);           // NULL detaches; under the link's lock
  void ethertalk_set_laser(at_link_t side, int on); // runs that side for the laser whatever its switch
  void ethertalk_lookup(const char *type);           // every running side
  void ethertalk_attach(at_link_t side, pap_client_t *c);
  // both links: void (*found)(at_link_t, const nbp_tuple_t *, const lt_route_t *) in their cfg
  // job_pipe.h
  typedef struct { const printer_caps_t *caps; const rip_stage_t *stage; ht_algo_t dither; int gain; } job_pipe_target_t;
  esp_err_t job_pipe_begin_to(const char *job_name, const job_pipe_target_t *t);  // NULL: job_pipe_begin
  // laser.h
  typedef struct { uint8_t mac[6]; const char *hostname, *version; ipp_doc_sink_t sink; } laser_cfg_t;
  esp_err_t laser_start(const laser_cfg_t *cfg);
  int   laser_job_begin(const char *name, job_pipe_target_t *t);  // 0 and t filled (caps, ps stage, screen); opens PAP; -1 if none shared
  int   laser_job_end(int ok);         // trailer, EOF, waits for the printer's EOF (stall limit); 1 printed
  void  laser_job_cancel(void);
  int   laser_ready(void);             // -1 none shared, 0 stopped or offline, 1 ready: the panel's line
  void  laser_json(char *buf, size_t cap);
  int   laser_post(const char *body, char *buf, size_t cap);   // 0 ok, else 400
  void  laser_status_json(char *buf, size_t cap);              // /api/status "laser"
  ```
  NVS `laser`: `share` u8 (default 0), `name` blob (Pascal), `link` u8, `info` blob (`lw_info_t` with a version byte), `dither` str, `gain` u8. `CONFIG_AIRWRITER_LASER_UUID_EPOCH` (default 1).

- [ ] **Step 1:** Links: roles and lookups in `localtalk.c` and `ethertalk.c` (a link runs for either role; `serve` is the card switch as today), `found` callbacks, client attach/detach under each link's lock, `papc_link_down` from `et_link_down` and a quiet cable. `idf.py build` clean.
- [ ] **Step 2:** `laser.c`: a task running `lwc_tick` every 250 ms and acting on its wants (lookups on the links the core names, `papc_status`, the query job through the client with `lw_parse`), the 16 KB PSRAM spool (rip task writes, blocking, aborting at the stall limit; the client's `read` takes without blocking), the shared printer's IPP side via `ipp_server_set_printer(1, …)` built from `lw_caps_fill` and `lw_uuid`, its state via `ipp_server_set_stopped_for(1, …)` from `lwc_view`, the PostScript-error abort, `/api/laser` GET (calls `lwc_card`) and POST (`share`, `dither`, `gain`), sharing saved in NVS. Unsharing while a job prints is refused (409).
- [ ] **Step 3:** `main.c`: the laser sink takes `s_job_lock` as `doc_begin` does, calls `laser_job_begin`, starts `job_pipe_begin_to`, and finishes through `pipe_done` → `laser_job_end` → `ipp_server_job_done_for(1, …)`; cancel goes to `job_pipe_abort` and `laser_job_cancel`; a laser job sets `LG_SRC_AIRPRINT` and posts the job events as an IPP job does, so Macs on AppleTalk are told busy. The panel's line source: the HSKo line, except while a laser job runs or when the ImageWriter's AirPrint is off and a LaserWriter is shared, when `laser_ready()` feeds `PS_EV_LINE_READY`/`NOT_READY` instead (CTRL-X kicks only with the ImageWriter's line). With the laser as the source no line event comes before its first answer, so the quiet power-on's Spin waits for Wi-Fi and that answer, ended by the existing 30 s boot cap. `/api/status` gains `"laser":{…}`; `webui` routes `/api/laser`.
- [ ] **Step 4:** `idf.py build` clean; `make -C tools/rip test regress` and `make -B -C tools/ipp test` still ok.
- [ ] **Step 5:** Commit `laser: share a LaserWriter as a second AirPrint printer (W-1082)`.

### Task 13: The LaserWriter icon

**Files:**
- Create: `tools/icon/laserwriter.txt`, `firmware/components/ipp_server/icons/laser-{48,128,512}.png`
- Modify: `tools/icon/mkicons.py` (both icons; `--only` unchanged output for the ImageWriter), `firmware/components/ipp_server/CMakeLists.txt` (EMBED_FILES), `LICENSE.md`

- [ ] **Step 1:** Draw a 32×32 LaserWriter II in the ImageWriter icon's palette and shading (an original drawing of the printer, not Apple's icon, so it carries this repository's licence). `python3 tools/icon/mkicons.py`; the ImageWriter PNGs are byte-identical (`git diff --stat` shows only the new files).
- [ ] **Step 2:** `LICENSE.md` row for it; `idf.py build` clean. Commit `icon: a LaserWriter for the second printer (W-1082)`.

### Task 14: The LaserWriters card

**Files:**
- Modify: `tools/webui/{body.html,app.js,style.css,states.json}`, `firmware/components/webui/webui_page.h` (generated by `mkheader.py`)

- [ ] **Step 1:** Before editing, `git log origin/main -5 -- tools/webui firmware/components/settings` and ask the main session whether anyone is mid-edit there; read `docs/superpowers/specs/2026-10-20-settings-redesign-design.md`.
- [ ] **Step 2:** In the AppleTalk pane, a LaserWriters card after the four ways in: one row per LaserWriter heard (NBP name, product once queried, which link, its status line), a share switch per row (one at a time; turning one on turns the other off), and under the shared row its screen picker and gain; an empty state ("No LaserWriters heard yet. Looking on the AppleTalk port and this network."); a footnote that LaserWriters are untested. The card polls `GET /api/laser` every 3 s while its pane shows. Names go in as text, never HTML (Review Focus 4).
- [ ] **Step 3:** `states.json` gains: none heard, two heard (one with a MacRoman name), one shared and idle, shared and printing, shared out of paper, shared offline, busy with another Mac. `python3 tools/webui/preview.py` → every state at desktop and phone width, light and dark, at http://localhost:8079/_states, checked in Chrome (Claude in Chrome).
- [ ] **Step 4:** `python3 tools/webui/mkheader.py`, `idf.py build`. Commit `webui: the LaserWriters card (W-1082)`.

### Task 15: Docs

**Files:**
- Modify: `docs/SPEC.md` (new §3.14), `AGENTS.md` (State, Not built, Open work, Verification, Layout), `docs/traps.md`, `docs/superpowers/specs/2026-08-04-printer-support-scope-design.md` ("LaserWriter — later" points at the laser spec), `tools/ethertalk/README.md`, `README.md` if it lists printers

- [ ] **Step 1:** Write them, current state only: built, tested against a fake LaserWriter only, untested on a real one, sharing off by default; `make -C tools/rip pscheck` and `interop` in Verification; traps: `gs` alias, one LF before binary, 512-byte packets, one ATP socket for the connection.
- [ ] **Step 2:** Commit `docs: LaserWriters over AppleTalk, built and untested on a real printer (W-1082)`.

### Task 16: Bench, then merge

No code. Each step that touches board 1 waits for the main session's go; each ImageWriter print waits for Wells's yes.

- [ ] **Step 1:** Ask Wells to capture one iPhone page: `python3 tools/ippcap/ippcap.py /tmp/cap --urf V1.4,CP1,RS300,W8,DM1,IS1,MT1,FN3` plus its `dns-sd` line, print any page to "AirWriterCap". Nothing prints. If iOS sent 300 dpi, add it as `fixtures/ios-300-<date>.urf` to `pscheck`; if it did not honour `RS300`, stop and tell Wells (the AirPrint printer's resolution is then the open question).
- [ ] **Step 2:** Record board 1's ImageWriter `printer-uuid` and a Get-Printer-Attributes dump. With the main session's go and the bench rules (prints held, `printing=false`), OTA the branch build; wait for `"trial":false`.
- [ ] **Step 3:** ImageWriter unchanged: same UUID and attribute dump (save `printer-up-time`); `ipptool` `get-printer-attributes-suite.test` passes; with Wells's yes, top margin 0, the checker Print-Job gives crc32 `8ee84844`, then top margin back to −126.
- [ ] **Step 4:** On this Mac (Basilisk II runs on the Pi, not here): `python3 tools/ethertalk/etpeer.py tunnel 10.0.1.115 laserwriter "LaserWriter II NT" --out <scratch>`. On the page: the card lists it; share it; the query runs; `ipptool` suite against `/ipp/laser` passes; the iPhone shows "LaserWriter II NT" beside the ImageWriter; print a page and compare the fake's PNG with the page; `busy 20` then print (the phone shows its status, then it prints); `paper` mid-job (the phone shows out of paper; `ok` finishes it); cancel from the phone mid-job; `drop` mid-job (the job aborts, then an ImageWriter test page prints, with Wells's yes).
- [ ] **Step 5:** Unshare; confirm the second printer leaves Bonjour and the ImageWriter is untouched.
- [ ] **Step 6:** With every host test, the regression and steps 3–4 passing, sharing off by default and the docs saying untested, merge to main per Wells's 2026-10-22 rule, push, update `W-1082 AirPrint to LaserWriters on AppleTalk` in Linear (merged; the real-printer bench list stays open there).
