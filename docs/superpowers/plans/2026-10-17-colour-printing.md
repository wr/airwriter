# Colour printing over AirPrint — implementation plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** With the colour ribbon in, an iPhone or Mac prints a colour page in colour over AirPrint; with a black ribbon nothing changes.

**Architecture:** Pure-C cores first, host-tested in `tools/rip`: self-ID parsing and a four-ink strip writer in `imagewriter`, a multi-channel resampler, an RGB-to-ink split in `ripjob`, and a colour branch in `ripjob_run`. Then the firmware glue: a self-ID query on the printer's receive line, a saved ribbon state, and IPP attributes and mDNS TXT that follow it under today's UUID.

**Tech Stack:** C11, ESP-IDF 5.5.1, host tests via `make -C tools/rip test` and `make -B -C tools/ipp test`.

**Spec:** `docs/superpowers/specs/2026-10-17-colour-printing-design.md` (W-1064).

## Global Constraints

- Gray pages keep their bytes: `make -C tools/rip regress` passes unchanged (checker crc32 `8ee84844`).
- Black ribbon or unknown: IPP attributes and TXT exactly as today, same UUID.
- Ink order in a strip: yellow, magenta, cyan, black (`ESC K1`, `ESC K2`, `ESC K3`, `ESC K0`).
- Passes are separated by CR with no LF; the LF to the next strip is held back as the gray writer does.
- A page ends with black selected. `ESC c` at page start already resets the ribbon to black
  (Table A-2), and the Mac driver sends `ESC K0` itself (`fixtures/pap-iw701-page.bin`), so the
  W-1041 resync is unchanged.
- Self-ID is `ESC ?`; reply `IW10CF`/`IW15`…, `C` = colour ribbon (Table 6-7).
- New files carry `// SPDX-License-Identifier: GPL-3.0-or-later`.

## Review Focus

1. A colour URF from a client that cached colour while a black ribbon is in: prints gray, never four passes (Task 4 test `rgb_without_colour_prints_gray`).
2. A strip with no ink at all (blank band in a colour page) still advances the paper by one strip (Task 2 test `blank_strip_holds_lf`).
3. A self-ID reply with noise, parity bit set, or a late partial reply (`IW1` only): parse as unknown or right, never as colour by accident (Task 1 tests).
4. CIELab (URF colorspace 2) and CMYK pages: refused cleanly as today, not ripped as RGB (Task 4 test `cielab_refused`).
5. Query while the link is dirty (last job cut mid-graphics): skipped, so `ESC ?` is never swallowed as graphics data (Task 6, by construction in `ribbon_check`).

---

### Task 1: Self-ID parsing

**Files:** Modify `firmware/components/imagewriter/include/imagewriter.h`, `imagewriter.c`; Test `tools/rip/test/test_imagewriter.c`.

**Produces:**
```c
typedef enum { IW_RIBBON_UNKNOWN = 0, IW_RIBBON_BLACK = 1, IW_RIBBON_COLOUR = 2 } iw_ribbon_t;
iw_ribbon_t iw_self_id_ribbon(const uint8_t *reply, size_t n);
```
Masks bit 7 of each byte; finds `IW` followed by two digits; `C` among the bytes after the digits means colour, otherwise black; no `IWdd` means unknown.

- [ ] Tests: `"IW10CF"`→colour, `"IW10C"`→colour, `"IW10F"`→black, `"IW10"`→black, `"IW15C"`→colour, `"\x00\x00IW10C"`→colour, `"IW1"`→unknown, `""`→unknown, `"XX10C"`→unknown, `"IW10C"` with bit 7 set on every byte→colour.
- [ ] Run `make -C tools/rip build/test_imagewriter && ./tools/rip/build/test_imagewriter`: fails; implement; passes.

### Task 2: Four-ink strip writer

**Files:** `imagewriter.h`, `imagewriter.c`; Test `tools/rip/test/test_imagewriter.c`.

**Produces:**
```c
typedef enum { IW_YELLOW, IW_MAGENTA, IW_CYAN, IW_BLACK, IW_INKS } iw_ink_t;  // print order
typedef struct {
    uint32_t  width_dots;
    iw_out_fn out;
    void     *ctx;
    uint8_t   rows[IW_INKS][8][(IW_MAX_DOTS + 7) / 8];
    uint32_t  nrows;
    uint8_t   cols[IW_MAX_DOTS];
    uint32_t  held_lf;   // LFs owed before the next strip that prints anything
    int       ink;       // ink selected now; -1 before the first select
} iwc_t;
void iwc_init(iwc_t *w, uint32_t width_dots, iw_out_fn out, void *ctx);
void iwc_begin_page(iwc_t *w);                         // same 8 bytes as iw_begin_page; ink = -1
void iwc_row(iwc_t *w, const uint8_t *const bits[IW_INKS]);
void iwc_end_page(iwc_t *w);                            // flush, ESC K0 if ink != black, FF
```
Strip flush: for each ink in order, transpose its 8 rows; `last` = last non-zero column; skip the ink if none. Before the strip's first pass emit `held_lf` LFs and zero it. Per pass: `ESC K <digit>` only if the ink differs from `w->ink`, then `ESC G` with `last + 1` as four digits, the columns, CR. After the strip, `held_lf += 1` (a strip with no pass adds its LF too). The digit per ink: yellow `1`, magenta `2`, cyan `3`, black `0`.

- [ ] Tests: `one_yellow_dot` (exact bytes: init, `ESC K1`, `ESC G0001`, `0x01`, CR, then `ESC K0`, FF); `pass_order_and_trim` (dots in all four inks at different last columns: K1,K2,K3,K0 in order, counts trimmed); `same_ink_not_reselected` (two strips of black only: one `ESC K0`); `blank_strip_holds_lf` (ink, blank, ink: exactly two LFs before the third strip's first pass, none after the last strip); `ends_on_black` (last pass cyan: `ESC K0` before FF; last pass black: no extra select).
- [ ] Fail, implement, pass. Commit Tasks 1–2: `imagewriter: self-ID parse and a four-ink strip writer`.

### Task 3: Multi-channel resampler

**Files:** `firmware/components/resample/include/resample.h`, `resample.c`; Test `tools/rip/test/test_resample.c`.

**Produces:**
```c
int rs_init_n(rs_t *r, uint32_t src_w, uint32_t src_h, uint32_t dst_w, uint32_t dst_h,
              uint32_t channels, rs_row_fn emit, void *ctx);   // interleaved pixels
```
`rs_init` becomes `rs_init_n(..., 1, ...)`. `rs_t` gains `uint32_t ch`; `acc`/`out` are `dst_w * ch`; `emit`'s `width` stays pixels. Arithmetic per channel identical to today's, so one channel is byte-identical.

- [ ] Test `three_channels_match_three_singles`: a 7×5 RGB source to 3×2 equals three 1-channel resamples of its planes, per channel.
- [ ] Fail, implement, `make -C tools/rip test regress` passes. Commit: `resample: interleaved channels`.

### Task 4: Ink split and the colour branch of the rip

**Files:** Create `firmware/components/ripjob/include/ink.h`, `firmware/components/ripjob/ink.c`; Modify `ripjob.h`, `ripjob.c`, `firmware/components/ripjob/CMakeLists.txt` (SRCS += `ink.c`); Test `tools/rip/test/test_ink.c`, `tools/rip/test/test_ripjob_colour.c`.

**Produces:**
```c
// ink.h. Planes are in halftone's convention: 0 = full ink, 255 = none.
void ink_split(const uint8_t *rgb, uint32_t w, uint8_t *const planes[IW_INKS]);
void ink_gray(const uint8_t *rgb, uint32_t w, uint8_t *gray);   // (77R + 150G + 29B + 128) >> 8
// ripjob_cfg_t gains:
int   colour;   // RGB pages print in four inks; 0 prints them as gray
void (*ink_row)(void *ctx, const uint8_t *const bits[IW_INKS], uint32_t w, uint32_t y);
```
Split, with `mx = max(R,G,B)`: black = `mx`; cyan = `255 - (mx - R)`; magenta = `255 - (mx - G)`; yellow = `255 - (mx - B)`.

`ripjob_run` accepts 8 bpp/1 colour (as today) or 24 bpp/3 colours with colorspace 1, 3 or 5 (sRGB, AdobeRGB, RGB); anything else is refused with the existing message, now naming both. RGB with `colour == 0`: `ink_gray` each cropped source row, then the gray path unchanged. RGB with `colour`: one 3-channel `rs_init_n` on the cropped RGB; each emitted row → `ink_split` into four `dst_w` planes → four `ht_row`s (one `ht_t` per ink) → `ink_row` callback → `iwc_row`. `iwc_t` and the planes are heap, freed at `page_cleanup`.

- [ ] `test_ink.c`: white → all 255; black → K 0, CMY 255; mid grey 128 → K 128 only; red (255,0,0) → M 0, Y 0, C 255, K 255; `ink_gray` white 255, black 0.
- [ ] `test_ripjob_colour.c`, from in-memory URFs built in the test: `rgb_without_colour_prints_gray` (an RGB page of grey levels with `colour = 0` gives the same stream as the same levels sent as W8); `colour_page_selects_inks` (a pure-red page with `colour = 1`: stream contains `ESC K1` and `ESC K2`, no `ESC K3`); `cielab_refused` (colorspace 2, 24 bpp → negative return).
- [ ] `tools/rip/main.c`: `--colour` sets `cfg.colour`; with `--out-preview` and colour, the preview is RGB (yellow, magenta, cyan, black dots subtracted from white) via a new `png_write_rgb(path, pix, w, h)` in `png_write.c`.
- [ ] `tools/rip/test/make_colour_fixture.py OUT`: a 576×288 sRGB URF at 144 dpi (bars of white, Y, M, C, R, G, B, K and a grey ramp). `regress` gains: rip it with `--colour` and without, compare to `fixtures/colour.expected.bin` and `fixtures/colour.gray.expected.bin` (generated once from this build, preview checked by eye).
- [ ] `make -C tools/rip test regress` passes. Commit: `ripjob: RGB pages print in four inks, or as gray on a black ribbon`.

### Task 5: Colour attributes and TXT

**Files:** `firmware/components/ipp_attrs/include/ipp_attrs.h`, `ipp_attrs.c`; `firmware/components/ipp_server/include/ipp_server.h`, `ipp_server.c`; Test `tools/ipp/test/test_ipp_attrs.c`.

**Produces:**
```c
#define IPP_URF_STRING_COLOUR IPP_URF_VERSION ",CP1,RS72-144,W8,SRGB24,DM1,IS1,MT1,FN3"
// ipp_printer_t gains: int colour;
// ipp_server_cfg_t gains: int colour;   (initial value, from the saved ribbon)
void ipp_server_set_colour(int on);   // attributes at once; TXT Color and URF re-announced
```
With `colour`: `color-supported` true, `print-color-mode-supported` `monochrome, color`, `print-color-mode-default` `color`, `urf-supported` adds `SRGB24` after `W8`. Without: today's values exactly. TXT via `mdns_service_txt_item_set("_ipp", "_tcp", ...)`.

- [ ] Tests: colour off writes today's values (existing tests unchanged); colour on writes the four above.
- [ ] `make -B -C tools/ipp test` passes. Commit: `ipp: advertise colour when the colour ribbon is in`.

### Task 6: The ribbon on board

**Files:** `serial_out.h/.c`, `settings.h/.c`, `job_pipe.h/.c`, `firmware/main/main.c`, web UI source in `tools/webui/` (then `mkheader.py`).

**Produces:**
```c
void serial_out_rx_drop(void);                     // discard anything received
int  serial_out_query(const uint8_t *cmd, size_t n, uint8_t *reply, size_t cap,
                      uint32_t wait_ms);           // queue cmd (uart_tx_chars), wait for a first byte,
                                                   // read until 50 ms quiet; bytes read, -1 loopback/no CTS
// settings_t gains uint8_t ribbon (iw_ribbon_t); NVS key "ribbon"
esp_err_t settings_save_ribbon(uint8_t ribbon);
// job_pipe_cfg_t gains:
int (*colour_ok)(void *ctx);   // pipeline task, before each job's rip: print RGB in colour?
void *colour_ctx;
```
`main.c`: `s_ribbon` from settings at boot, passed to `ipp_server_cfg_t.colour`. `ribbon_check()` (caller holds the job lock): skip if `s_link.dirty`, loopback, or `!serial_out_clear()`; else `serial_out_rx_drop()`, query `ESC ?` with 500 ms, and if nothing came, query CR with 1000 ms; log the reply in hex; `iw_self_id_ribbon`; on a change from the stored state (unknown never overwrites a known one), save, `ipp_server_set_colour(state == colour)`, log it. Called from `colour_ok` (returns `s_ribbon == IW_RIBBON_COLOUR` after the check) and from `kick_task` 2 s after the printer line goes ready while no job holds the lock. `/api/status` gains `"ribbon":"colour"|"black"|"unknown"`; the web page shows it beside the printer state.

- [ ] `idf.py build` clean (signing lines present in the worktree's sdkconfig). Commit: `main: ask the printer for its ribbon and follow it`.

### Task 7: Docs and checks

- [ ] SPEC §3.2 (TXT `Color` follows the ribbon), §3.3 (`SRGB24`), §3.5 (colour attributes), §3.8 (four-ink strips); AGENTS.md state and open work; `docs/traps.md` gets the self-ID findings after the bench.
- [ ] `make -C tools/rip test regress`, `make -B -C tools/ipp test`, `idf.py build`. Commit.
- [ ] Bench (Wells): OTA, `/api/status` `ribbon`, iPhone print sheet offers colour after reopening, a colour print, the same with Black & White on, the checker still `8ee84844`.
