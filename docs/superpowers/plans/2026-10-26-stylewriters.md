# StyleWriters on the printer port, phase 1 (W-1103) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A StyleWriter (original, II, 1200, Color 1500, 2200, 2400, 2500) on J2 is found, listed under its model's name and shared over AirPrint at 360 dpi, in colour with a colour cartridge, with its status on the phone, a test page, a cleaning key, and the ImageWriter and LaserWriter paths unchanged.

**Architecture:** Pure C cores in a new `stylewriter` component, host-tested in `tools/rip` like `ps_page` and the PAP client: the model table and caps, the run coder, a rip stage that turns 360 dpi rows into framed bands, and the session that talks to the printer through an ops table. A Python software StyleWriter (`swfake.py`) is the printer for host interop, `swprint`, the capture rig and the bench. The board side (`stylewriter.c`) switches J2 between the ImageWriter's 9600 with CTS and the StyleWriter's 57600 without, runs the session on the `prn` task, and a pure port policy decides what the idle port is asked. Apple's tone curves and 16 × 16 screen come out of Apple's drivers through our own script.

**Tech Stack:** C11, ESP-IDF 5.5.1, FreeRTOS, Python 3.14 (numpy, Pillow), CUPS `ipptool`, Basilisk II (capture rig).

**Spec:** `docs/superpowers/specs/2026-10-25-stylewriters-design.md`, phase 1. Read it whole, including "What the printer and the board allow" and "Calls made in this design", before any task. Phase 2 (ADSP, the adapter) gets its own plan after an owner's print.

## Global Constraints

- The ImageWriter's bytes do not move: `make -C tools/rip regress` (four fixtures, `checker.expected.bin` crc32 `8ee84844`), `tools/ipp/test/golden/*.bin`, its TXT record, `printer-uuid` and job ids. The LaserWriter's do not either: `make -C tools/rip pscheck interop`.
- J2 for a StyleWriter: 57600 baud, 8N1, no flow control. Back to 9600 with CTS whenever the port's record is not a StyleWriter.
- The *plain question* is the one byte `?` (0x3F) at 57600 and nothing else in that write. The *full sequence* is `00 FF FF FF 49`, settle 2 s (8 s on its second try), drain, `?`, read to CR at 250 ms a byte (a reply ending without CR counts), and only after `CS` the query `FF FF FF 70`. The full sequence goes only to a StyleWriter record (before each job) or to a port the person has just named a StyleWriter.
- `?` replies: `IJ10` original, `SW` II, `SW3` 1200, `CS` colour; `p` byte 01 2400, 02 2200, 04 1500, 05 2500, other → 2200.
- Queries are `FF FF FF` + one letter, one byte back: `1` status (01 busy, 02 error, 04 hardware, 08 page done, 40 door, 80 waste ink), `2` errors (01 cartridge jammed, 02 ink out, 04 no paper, 08 paper jam, 10 error, 20 cartridge missing, 40 paper too small, 80 ignored on the 2400 and 2500), `B` buffer, `D` then `H` cartridge (CS 0x81 colour, 0x01 black, else abort; 1500 bit 0x80 colour), `R`, `S` resume (no reply).
- Free buffer bytes = `B` × 256 − 2048, the original `B` × 64 − 2048. `B` reads empty at: original A0, II F8, 1200 F8, 1500 87 (colour) / 9F (black), 2200 `B & A0 == A0`, 2400 F8, 2500 80.
- Geometry at 360 dpi (from TailTalk `tailtalk/src/stylewriter/family.rs`, which read the drivers' `ENGC`): CS family (2200/2400/2500) 2919 dots across, margins L 72 R 72 T 45 B 99, bands 400 rows, rect x offset 0, block cap 0x7E77 black / 0x79FA colour; 1200 family (II/1200/1500) 2880 across, margins 72/72/72/114, bands 800, rect x +16, block cap 0x76ED − planes × row bytes; original as the 1200 family with rect x +0, cap 0x2000, and no row delta. Printable width = min(dots across, paper width − 144). Rect coordinates are relative to the printable area's top-left, inclusive, little-endian u16.
- Wire format: rect `R` (black) or `c` (four inks) + l, t, r, b; then blocks `G` + u16 LE length + data + `00`. Rows run-coded: low 6 bits a count 1–62; top bits 00 literal (n bytes follow), 01 repeat the next byte n times, 10 n white bytes, 11 n black bytes; `80` alone is an all-white plane row. Each plane XORs the same plane of the row before within a band; a band's first row raw. Colour rows C, M, Y, K. `0C` ends a page. A block of 0x8000 stalls a CS printer.
- Media: Letter, A4, Legal; plain paper; `print-quality` 3/4/5 → draft/normal/best. IPP margins rounded outward to whole 1/100 mm.
- No cleaning, test page or reset the person did not ask for. One Clean Print Head press is one cleaning (`P` once). None on the original.
- Apple's driver files and the Apple-driver captures are never committed (`~/.config/airwriter/sw-drivers/`, `~/.config/airwriter/sw-captures/`); host tests that need them skip without them. Downloading the drivers waits for Wells's yes (file, source, size).
- TailTalk is GPL-3.0 with no "or later" grant: read, never copied. lpstyl (BSD-2-Clause, Monroe Williams) may be ported with its notice in `LICENSE.md`. InkTalk's firmware (GPL-3.0-or-later) may be ported with credit.
- Memory: nothing new in internal RAM. Every buffer over a few hundred bytes through `bigmem.h` (`docs/traps.md`: any `malloc` under 16 KB lands internal); the rip task's stack is 7 KB. Check `/api/mem` across a colour page on the bench.
- New source files carry an SPDX header. Copy follows `docs/voice-and-tone.md`; no forum is named as the audience. Docs say "not yet confirmed" (`Yes*`) until an owner prints on a model.
- Bench: never OTA or flash board 1 without the main session's go; bench rules in `AGENTS.md`; ask Wells before any print on the ImageWriter.

## Review Focus

1. **A StyleWriter switched off or unplugged mid-page.** The job stops sending at once, shows offline ("check power and cable"), and after 30 s of silence aborts (a power-cycled printer has lost the page); the next job starts with the reset and prints. Test in Task 6 (`silent_mid_page`).
2. **A page that is not the paper it claims** (an A4 URF to a Letter-sized pick, a 4 × 6 photo page, a Legal page): rows are kept from the printable area's left edge for the printable width and the page's own height less the margins; nothing is written past the printable width or a rect past the page. Test in Task 4 (`odd_sizes`).
3. **The cartridge changed since the printer was advertised** (colour job, `H` now says black): the page prints in gray, the session reports black, and the board turns IPP colour off before the next job. Test in Task 6 (`cart_swapped`).
4. **Answers that arrive late, early or wrong** (a stray byte in RX before the `?` reply, a reply without CR, `B` reading 00 or above the model's empty value): RX is drained before every query, a CR-less reply is used, `B` = 00 is "no room, ask again", and nothing is ever sent past the tracked free space. Test in Task 6 (`garbled_answers`).
5. **Cancel from the phone in the middle of a band.** The block being written finishes (a cut-off `G` block wedges the printer), then `E` (black-only models) and the reset; the job lock frees and the next job prints. Tests in Task 6 (`cancel_mid_band`) and Task 7 (interop `cancel`).

## Calls made in this plan (beyond the spec)

- **The cartridge reuses the ribbon.** A StyleWriter port's cartridge (black or colour) lives where the ImageWriter's ribbon does (`s_ribbon`, `settings_save_ribbon`, `ipp_server_set_colour`, `/api/status` `ribbon`); no new NVS key. `/api/printers` shows it as `cartridge` for a StyleWriter record.
- **The printer list blob goes to version 2** for the new kind and models, with a version 1 reader. An older or rolled-back firmware sees a newer blob and keeps it untouched (`pr_blob_newer`), rather than rejecting the unknown kind and migrating over it.
- **The session's frames ride the existing printer-bytes ring.** `sw_page` writes `[type][u16 LE len][payload]` frames into `job_pipe`'s 512 KB `s_out`; the `prn` task hands the bytes to the StyleWriter's consumer, which reassembles frames and runs the conversation. No second buffer.
- **Silence mid-job aborts after 30 s;** a fault the printer reports (paper, jam, door, ink) waits up to `CONFIG_AIRWRITER_STALL_ABORT` (600 s), as the ImageWriter's paper-out does.
- **The quality reaches the rip through a new options argument** on `ipp_doc_sink_t.begin`; today no Job-group attribute is read at all.
- **The test page's text is a 5 × 7 face drawn for this repo** (GPL, `sw_font.h`), scaled at 360 dpi. There is no bitmap face in the repo, and the ImageWriter's fonts are Apple's and never committed.
- **The IPP icon is Apple's StyleWriter icon** out of the Color SW 2500 driver's `icl8` resource, by the same script as the tables, as the ImageWriter's icon is Apple's (`LICENSE.md` row). The settings page's row symbol is drawn for the page, in its 16-unit style.
- **The idle CTRL-X kick goes to an unrecognised port only after the plain question went unanswered**, so a StyleWriter never hears it.
- **The test button's cable-test branch** (`test_button`, handshake line low) is skipped for a StyleWriter record, whose line means nothing.

---

## File map

| Path | Role |
|---|---|
| `firmware/components/stylewriter/sw_model.{c,h}` (new, pure) | `sw_model_t`, per-model spec, `?`/`p`/`H`/`B` readers, block caps, `sw_caps()` |
| `firmware/components/stylewriter/sw_code.{c,h}` (new, pure) | row run coder and decoder |
| `firmware/components/stylewriter/sw_page.{c,h}` (new, pure) | the rip stage: margins, bands, rects, blocks, frames |
| `firmware/components/stylewriter/sw_session.{c,h}` (new, pure) | recognition, idle status, a job's conversation, pacing, faults, cancel, cleaning |
| `firmware/components/stylewriter/sw_testpage.{c,h}`, `sw_font.h`, `sw_art.h` (new, pure; `sw_art.h` generated) | the raster test page |
| `firmware/components/stylewriter/stylewriter.{c,h}` (new, IDF) | J2 ops for the session, the job's `prn` consumer, idle polls, clean, test page task |
| `firmware/components/printers/port_policy.{c,h}` (new, pure) | what the idle port is asked, and when |
| `firmware/components/printers/printers_core.{c,h}`, `printers.{c,h}` | StyleWriter kind and models, blob v2, notices, picker, test target, `clean`, `cartridge` |
| `firmware/components/serial_out/serial_out.{c,h}` | runtime link (baud, flow), raw read and write |
| `firmware/components/halftone/` | `HT_APPLE`, `ht_set_tone`, generated `apple_tables.h` |
| `firmware/components/ripjob/` | four-plane rows to a stage, per-media crop, tone curve, identity resample fast path |
| `firmware/components/printer_caps/`, `ipp_attrs/` | a media list with per-media margins |
| `firmware/components/ipp_server/` | Job-group `print-quality` into the sink's options; StyleWriter icon |
| `firmware/components/job_pipe/` | target `colour`, `buffered`, `prn` consumer; `job_pipe_out()` |
| `firmware/components/printer_state/` | a fault input that is attention at once |
| `firmware/main/main.c` | wiring: port policy in `kick_task`, the StyleWriter sink and finisher, state messages, test page and clean paths |
| `tools/stylewriter/swfake.py`, `mktables.py`, `test_*.py`, `README.md` (new) | software StyleWriter and decoder, Apple tables and icon, owner instructions |
| `tools/rip/swhost.c`, `tools/rip/test/test_sw_*.c`, `test_port_policy.c`, `lpstyl_encode.{c,h}`, `swcheck.py` (new) | host builds and tests |
| `tools/webui/` | StyleWriter row, detail, picker, Apple screen, states |
| `docs/SPEC.md`, `README.md`, `docs/prior-art.md`, `LICENSE.md`, `AGENTS.md`, `docs/voice-and-tone.md` | docs |

---

### Task 1: The model table and the run coder (pure)

**Files:**
- Create: `firmware/components/stylewriter/{CMakeLists.txt,include/sw_model.h,include/sw_code.h,sw_model.c,sw_code.c}`, `tools/rip/test/test_sw_model.c`, `tools/rip/test/test_sw_code.c`
- Modify: `tools/rip/Makefile` (`INCS`, `SRCS`)

**Interfaces:**
- Produces:
  ```c
  typedef enum { SW_NONE, SW_ORIGINAL, SW_II, SW_1200, SW_C1500, SW_C2200, SW_C2400, SW_C2500, SW_MODELS } sw_model_t;
  typedef struct {
      const char *name;          // "StyleWriter II", "Color StyleWriter 2500"
      const char *make_model;    // "Apple StyleWriter II"
      uint16_t dots_across;      // 2880 / 2919
      uint16_t margin_l, margin_r, margin_t, margin_b;  // dots at 360 dpi from the sheet's edges
      uint16_t band_rows;        // 800 / 400
      uint16_t rect_dx;          // 0 or 16
      uint8_t  colour;           // takes a colour cartridge
      uint8_t  delta;            // rows XOR the row before (0 on the original)
      uint8_t  cleans;           // has `P` (0 on the original)
  } sw_spec_t;
  const sw_spec_t *sw_spec(sw_model_t m);                 // NULL for SW_NONE
  sw_model_t sw_model_from_id(const uint8_t *r, size_t n, int *is_cs);  // the `?` reply
  sw_model_t sw_model_from_p(uint8_t p);
  int      sw_cart_colour(sw_model_t m, uint8_t h);        // 1 colour, 0 black, -1 unreadable
  int32_t  sw_free_bytes(sw_model_t m, uint8_t b);         // never below 0
  int      sw_b_empty(sw_model_t m, uint8_t b, int colour_cart);
  uint32_t sw_block_cap(sw_model_t m, int planes);         // data bytes in one G block
  uint32_t sw_printable_w(sw_model_t m, uint32_t paper_w); // min(dots_across, paper_w - 144)
  size_t   sw_code_max(size_t rowbytes);                   // worst case for one plane row
  size_t   sw_code_row(const uint8_t *row, const uint8_t *prev, size_t rowbytes, uint8_t *out);  // prev NULL: raw
  size_t   sw_decode_row(const uint8_t *in, size_t n, const uint8_t *prev, size_t rowbytes, uint8_t *row);  // bytes used, 0 bad
  ```

- [ ] **Step 1: Failing tests.** `test_sw_model.c`: `sw_model_from_id("SW\r")` is `SW_II` with `is_cs` 0, `"SW3\r"` `SW_1200`, `"IJ10\r"` `SW_ORIGINAL`, `"CS"` (no CR) `SW_NONE` with `is_cs` 1, `"XYZ\r"` and an empty reply `SW_NONE`; `sw_model_from_p` 01/02/04/05/09 → 2400/2200/1500/2500/2200; `sw_cart_colour(SW_C2200, 0x81)` 1, `0x01` 0, `0x40` -1, `sw_cart_colour(SW_C1500, 0x80)` 1; `sw_free_bytes(SW_II, 0xF8)` 61440, `(SW_ORIGINAL, 0xA0)` 8192, `(SW_II, 0x00)` 0; `sw_b_empty(SW_C2200, 0xA3, 1)` 1, `(SW_C2500, 0x80, 1)` 1, `(SW_C1500, 0x9F, 0)` 1; `sw_block_cap(SW_C2500, 4)` 0x79FA, `(SW_C2200, 1)` 0x7E77, `(SW_II, 1)` 0x76ED − 360, `(SW_ORIGINAL, 1)` 0x2000; `sw_printable_w(SW_II, 3060)` 2880, `(SW_C2400, 3060)` 2916, `(SW_C2400, 2976)` 2832; every model's spec matches the Global Constraints' table.
  `test_sw_code.c`: a 365-byte row of zeros against NULL codes to exactly `80`; a row equal to `prev` codes to `80`; a row of 365 × FF codes to runs of `FF`-class codes (11xxxxxx) summing 365; a random row round-trips through `sw_code_row`/`sw_decode_row` with and without `prev` (1000 seeded rows); no code count is 0 except the lone `80`, none above 62; output never exceeds `sw_code_max`; `sw_decode_row` returns 0 on a truncated input and on codes that overrun the row.
- [ ] **Step 2:** `make -C tools/rip test` → the new tests fail to build.
- [ ] **Step 3: Implement.** The table is `static const` (the Makefile rejects writable statics in pure cores). `sw_code_row`: XOR against `prev` into a scratch row first (caller-sized, no allocation: take a `uint8_t *scratch` if needed and say so in the header), then emit white runs for pairs of 00, black runs for pairs of FF, repeats for 3 or more equal bytes with 5 or more left, literals otherwise, and a lone `80` when the delta row is all zero. Port lpstyl's `encodescanline` only if it is simpler, with its BSD notice in the file and `LICENSE.md`.
- [ ] **Step 4:** `make -C tools/rip test` → all pass, the writable-statics check included.
- [ ] **Step 5:** Commit `stylewriter: model table and row run coder, host-tested (W-1103)`.

### Task 2: A software StyleWriter, and its decoder

**Files:**
- Create: `tools/stylewriter/swfake.py`, `tools/stylewriter/test_swfake.py`, `tools/stylewriter/test_plain_question.py`, `tools/stylewriter/README.md`

**Interfaces:**
- Produces (CLI, documented in the module docstring like `etpeer.py`'s):
  - `swfake.py serve MODEL [--pty | --serial DEV | --tcp HOST:PORT | --listen PORT] [--cart black|colour] [--out DIR] [--log FILE] [--drain BYTES_PER_S]` answers as `MODEL` (`original`, `ii`, `1200`, `1500`, `2200`, `2400`, `2500`); `--pty` prints the slave path on its first stdout line; each page goes to `DIR/page-N.png` (ink on paper) and `DIR/page-N-{c,m,y,k}.pbm` (1 = ink); `--log` writes both directions as `> hex` / `< hex` lines with millisecond stamps.
  - Stdin commands, one per line, as `etpeer.LaserWriter.command`: `paper`, `jam`, `cart`, `ink`, `door`, `waste`, `silent`, `ok`, `busy N`, `overflow` (accept more than the buffer and then fault as a real one would).
  - `swfake.py decode MODEL STREAM.bin --out DIR` decodes a captured or ripped stream into the same files.
  - Module API used by the tests: `class StyleWriter: feed(data: bytes) -> bytes` (what the printer answers), `pages: list`, `command(line)`.

- [ ] **Step 1: Failing tests** (`python3.14 -m unittest tools/stylewriter/test_swfake.py`): `?` gets `SW\r` as `ii` and `CS\r` as `2500`; `FF FF FF p` after `CS` gets `05`, and `p` sent to `ii` makes it eject and reset (it answers nothing until `00 FF FF FF I`, then 2 s settle in fake time); `original` ignores `?` until reset; `B` starts at the model's empty value and falls by the bytes received, refilling at `--drain`; a hand-made page (`L`, `c` rect, one `G` block of four coded planes, `0C`) decodes to the planes it was made from; a block longer than its length, a rect past the printable width, or an unknown letter ejects and resets, logged as `bad`; `paper` makes `2` read 04 until `ok`; `silent` answers nothing.
  `test_plain_question.py`: a model of a 9600 8N1 UART with 16× oversampling and mid-bit start validation (as the 8251 family and the ESP32 do), fed the 57600 waveform of one `?` at 64 phase offsets, receives no byte at any offset; fed `00 FF FF FF 49` it prints which bytes it receives (recorded in the test's output, not asserted: that is the cost of a wrong pick).
- [ ] **Step 2:** Run → fails.
- [ ] **Step 3: Implement `swfake.py`.** Transports are byte streams (`os.openpty` with the slave in raw mode, a serial device via `termios` raw 57600 8N1, TCP), not `etpeer`'s frame transports. The decoder uses the same rules as `sw_decode_row`. The drawing reuses `tools/iwview/iwview.py`'s ink and paper model with a dots-per-inch parameter (360) rather than its 72-dpi dot radius. The reset and the `p`-to-non-CS rule follow the spec's facts.
- [ ] **Step 4:** Tests pass. Write `tools/stylewriter/README.md`: what swfake is, how to run it against `swhost` and a board, and (for Task 7) `swprint` for owners.
- [ ] **Step 5:** Commit `tools/stylewriter: a software StyleWriter that answers, decodes and draws pages; the plain question modelled at 9600 (W-1103)`.

### Task 3: Apple's tables and icon, the Apple screen, the tone curve

**Files:**
- Create: `tools/stylewriter/mktables.py`, `firmware/components/halftone/include/apple_tables.h` (generated), `tools/icon/stylewriter.png` source and `firmware/components/ipp_server/icons/stylewriter-{48,128,512}.png` (generated)
- Modify: `firmware/components/halftone/{halftone.c,include/halftone.h}`, `firmware/components/ripjob/{ink.c,include/ink.h}`, `tools/rip/test/test_ink.c`, `tools/rip/test/test_halftone.c` (or the file that tests screens today), `tools/rip/main.c` (`--dither apple`), `tools/icon/mkicons.py`, `firmware/components/ipp_server/CMakeLists.txt`, `LICENSE.md`

**Interfaces:**
- Produces:
  ```c
  // apple_tables.h (generated; names the driver file and resource of each table)
  extern const uint8_t apple_screen16[256];       // HTSC 1, row-major 16 x 16
  extern const uint8_t apple_tone_cs[2][256];     // GAMA 1, 2 of the Color SW 2200 driver (2200/2400/2500)
  extern const uint8_t apple_tone_sw[2][256];     // GAMA 1, 2 of the Color SW 1500 driver (original/II/1200/1500)
  // halftone.h
  typedef enum { HT_THRESHOLD, HT_ATKINSON, HT_FLOYD, HT_BAYER, HT_BLUENOISE, HT_APPLE } ht_algo_t;  // HT_APPLE last, name "apple"
  void ht_set_tone(ht_t *h, const uint8_t curve[256]);   // NULL: identity; ht_set_gain composes on top
  // ink.h
  void ink_screens_set_tone(ink_screens_t *s, const uint8_t curve[256]);  // applied to R, G, B before ink_split
  ```
  Which of GAMA 1 and 2 applies to which job is whatever TailTalk's `raster.rs:34-182` does; record its line in a comment where the curve is chosen (Task 5).

- [ ] **Step 1: Wells's yes, then the drivers.** Ask Wells to approve downloading Apple's Color StyleWriter 2200 and Color StyleWriter 1500 (and 2500, for the icon) drivers: name each file, its source (Apple's archived download, or an archive mirror of it) and size. On a yes, put them in `~/.config/airwriter/sw-drivers/`, unpack with `unar`/`hdiutil` as their format needs.
- [ ] **Step 2: Failing tests.** `test_halftone`: `ht_algo_from_name("apple")` is `HT_APPLE`; with a flat 50 % row the Apple screen inks exactly the cells whose `apple_screen16` value is under 128, tiled every 16 pixels and 16 rows; `ht_set_tone` with a curve mapping everything to 0 inks every pixel, with NULL is identity, and `ht_set_gain(h, 20)` after `ht_set_tone` equals the gain curve applied to the tone curve's output. `test_ink`: the loop over screens includes `HT_APPLE`; with `HT_APPLE`, C, M and Y use the screen unturned and K the screen turned over (TailTalk's placement, `raster.rs:173-182`), and `ink_screens_set_tone` changes the planes as a curve on R, G, B before `ink_split` would.
- [ ] **Step 3: `mktables.py`.** Reads a resource fork (macOS `file/..namedfork/rsrc`, a raw `.rsrc`, or MacBinary) with a small built-in resource-map parser, takes `GAMA` 1 and 2 and `HTSC` 1 from each driver and `icl8` (plus `clut` 8) from the 2500's, and writes `apple_tables.h` (each table's driver file, version and resource id in a comment) and `tools/icon/stylewriter.png`. Checks before writing: each GAMA is 256 bytes and monotonic, HTSC holds 256 values, both drivers' HTSC agree. `--compare FILE` reads TailTalk's `raster.rs` and reports any value that differs (a check on our extraction, nothing copied).
- [ ] **Step 4: Implement** `HT_APPLE` (a case beside blue noise, `& 15`), `ht_set_tone`, `ink_screens_set_tone`, `ink_screen_phase` for the 16 tile, `"apple"` in `ht_algo_from_name`, `rip --dither apple`. Add `stylewriter` to `mkicons.py`'s pairs without growing the palette, and embed the three PNGs.
- [ ] **Step 5:** `make -C tools/rip test regress` → pass, the four fixtures unchanged. `LICENSE.md` gets a row: the tables and icon, "© Apple; extracted from Apple's Color StyleWriter 1500, 2200 and 2500 drivers by `tools/stylewriter/mktables.py`; not licensed under this repository's licences", as the ImageWriter icon's row reads.
- [ ] **Step 6:** Commit `halftone: Apple's StyleWriter screen and tone curves, read out of Apple's drivers by our own script; the StyleWriter's icon (W-1103)`.

### Task 4: The page: per-media geometry, four planes to a stage, `sw_page`

**Files:**
- Create: `firmware/components/stylewriter/{include/sw_page.h,sw_page.c}`, `tools/rip/test/test_sw_page.c`, `tools/rip/test/lpstyl_encode.{c,h}`, `tools/rip/test/swcheck.py`
- Modify: `firmware/components/printer_caps/{printer_caps.c,include/printer_caps.h}`, `firmware/components/stylewriter/sw_model.c` (`sw_caps`), `firmware/components/ripjob/{ripjob.c,include/ripjob.h}`, `firmware/components/resample/resample.c`, `firmware/components/ipp_attrs/ipp_attrs.c`, `tools/ipp/test/` (caps tests), `tools/rip/main.c` (`--printer stylewriter --model M --cart black|colour --out-sw F`), `tools/rip/Makefile` (`swcheck` target)

**Interfaces:**
- Consumes: Task 1, Task 3.
- Produces:
  ```c
  // printer_caps.h
  typedef struct {
      const char *media;                      // "na_letter_8.5x11in", "iso_a4_210x297mm", "na_legal_8.5x14in"
      int32_t     x, y;                       // 1/100 mm
      int32_t     margin_l, margin_r, margin_t, margin_b;   // 1/100 mm, rounded outward
      uint16_t    page_w;                     // the page's width at rip dpi: 3060, 2976, 3060
      uint16_t    rip_left, rip_w;            // dots kept across
  } printer_media_t;
  // printer_caps_t gains, after crop_margin:
  //   const printer_media_t *media_list; uint8_t n_media;   // 0: the scalar media fields, as the ImageWriter's
  const printer_caps_t *sw_caps(sw_model_t m);              // sw_model.h; static tables per family
  // ripjob.h: rip_stage_t gains, after ctx:
  //   void (*ink_row)(void *ctx, const uint8_t *const bits[IW_INKS]);   // set: RGB pages come as four planes
  // ripjob_cfg_t gains: const uint8_t *tone;   // the stage printer's curve (Task 3), NULL none
  // sw_page.h
  enum { SWF_PAGE = 1, SWF_RECT = 2, SWF_BLOCK = 3, SWF_PAGE_END = 4 };   // frame = [type][u16 LE len][payload]
  typedef struct {
      sw_model_t model;
      int        colour_cart;                 // 0: four-plane rows print as K only
      void     (*out)(void *ctx, const uint8_t *frame, size_t n);   // one whole frame per call
      void      *ctx;
  } sw_page_cfg_t;
  typedef struct sw_page sw_page_t;
  sw_page_t *sw_page_new(const sw_page_cfg_t *cfg);    // allocates its band (PSRAM on the board)
  void       sw_page_stage(sw_page_t *p, rip_stage_t *st);
  void       sw_page_free(sw_page_t *p);
  ```
  Payloads: `SWF_PAGE` one byte (1 colour, 0 black); `SWF_RECT` the wire bytes (`R` or `c`, l, t, r, b); `SWF_BLOCK` the coded data only (the session adds `G`, the length and `00`); `SWF_PAGE_END` empty.

- [ ] **Step 1: Failing tests.**
  - `tools/ipp/test`: the ImageWriter's caps still produce the golden bytes (`n_media` 0); `sw_caps(SW_II)` lists three media; Letter's margins are L 508, R 762, T 508, B 805 (1/100 mm, outward) and `rip_left` 72, `rip_w` 2880; `sw_caps(SW_C2500)` Letter R 508, T 318, B 699, `rip_w` 2916; A4 on both `rip_w` 2832; `media-col-database` has three entries; the answer to a full Get-Printer-Attributes stays under `RESP_MAX` (5120) with three media (print its size).
  - `test_sw_page.c` (rows from a synthetic stage feed, no URF): a Letter page of 2880 × 3960 for `SW_II` drops the first 72 and last 114 rows; bands are 800 rows with rects `R`, l = 16, r = 16 + 2879, t and b relative to the printable area; an all-white band emits no rect; a page with one black dot emits one `SWF_RECT` and one `SWF_BLOCK`; a band whose coded size passes `sw_block_cap` splits into blocks with the delta carried across them; a colour page for `SW_C2500` with `colour_cart` 1 emits `c` rects and four planes per row in C, M, Y, K order from `iw_ink_t`'s Y, M, C, K; with `colour_cart` 0 it emits `R` and the K plane only; `SW_ORIGINAL` codes every row raw. `odd_sizes` (Review Focus 2): 2976-, 1440- and 3060 × 5040-dot pages keep `rip_left` .. `rip_left + rip_w` (or the page's own width if narrower) and never emit a coordinate past the printable area. Every emitted stream decodes, through `sw_decode_row`, to exactly the rows fed in.
  - `swcheck.py` (target `make -C tools/rip swcheck`): for each of the seven models and for `fixtures/checker.urf` plus a synthetic 360 dpi gray and colour page (generated as `make_300dpi_fixture.py` does), `rip --printer stylewriter --model M --out-sw s.bin --out-pbm p.pbm` then `swfake.py decode` must give the rip's own bits per ink; and `lpstyl_encode` (lpstyl's `encodescanline`, BSD-2, its notice kept) coding the same rows must decode to the same bits.
- [ ] **Step 2:** Run → fails.
- [ ] **Step 3: Implement.** `ripjob`: `colour = rgb && cfg->colour && (!stage || stage->ink_row)`; with a stage and colour, `on_rgb_row` calls `stage->ink_row` and `iwc` is never allocated; with `caps->n_media`, the media entry is the one whose `page_w` is within 2 dots of the page's width (else the first), the crop is `rip_left`/`rip_w` instead of the symmetric `crop_margin`; `cfg->tone` goes to `ht_set_tone` and `ink_screens_set_tone`; switch the plain `calloc`/`malloc` in `ripjob.c` and `ink.c` to `bigmem.h`. `resample`: when source and destination widths are equal, copy rows (no per-pixel divides). `ipp_attrs`: loop the media keywords, `media-col-*` and `media-size-supported` over the list; the four `media-*-margin-supported` are de-duplicated lists. `sw_page`: vertical margins are its own (rows before `margin_t` and from `h - margin_b` are dropped); band rows go to a PSRAM band of `band_rows × planes × row bytes`; a band is coded when full or at page end.
- [ ] **Step 4:** `make -C tools/rip test regress pscheck swcheck` and `make -C tools/ipp test` → pass, goldens unchanged.
- [ ] **Step 5 (optional oracle):** `swcheck.py --tailtalk DIR` builds a small Rust example against a local TailTalk checkout (never vendored) that codes the same rows with its encoder, and requires them to decode to the same bits. Skipped without `cargo` or `DIR`.
- [ ] **Step 6:** Commit `stylewriter: the page stage, per-media geometry and four-plane rows to a stage, checked against swfake and lpstyl's coder (W-1103)`.

### Task 5: The job's options: print quality from IPP, the target's colour

**Files:**
- Modify: `firmware/components/ipp_server/{ipp_req.c,ipp_server.c,include/ipp_server.h}`, `tools/ipp/test/` (request parse tests), `firmware/main/main.c` (the three sinks' `begin`), `firmware/components/laser/laser.c` (its sink), `firmware/components/job_pipe/{job_pipe.c,job_pipe_rip.c,include/job_pipe.h}`, `tools/rip/test/test_job_pipe_rip.c`

**Interfaces:**
- Produces:
  ```c
  typedef struct { int quality; } ipp_job_opts_t;     // print-quality 3/4/5; 0 when not sent
  // ipp_doc_sink_t.begin becomes:
  //   int (*begin)(void *ctx, const char *format, const char *job_name, const ipp_job_opts_t *opts);
  // job_pipe_target_t gains:
  //   int colour;              // RGB pages to the stage as four planes (the target's stage has ink_row)
  //   int buffered;            // use the URF/printer-bytes split, as the ImageWriter does
  //   void (*prn)(void *ctx, const uint8_t *buf, size_t n);   // printer-bytes ring consumer
  //   void *prn_ctx;
  //   const uint8_t *tone;
  void job_pipe_out(const uint8_t *buf, size_t n);   // a stage's write into the printer-bytes ring (blocks while full)
  ```

- [ ] **Step 1: Failing tests.** `tools/ipp`: a Print-Job whose Job group carries `print-quality` 3 reaches the sink's `begin` with `opts->quality` 3; none gives 0; a Job group with unknown attributes and a `media-col` collection is skipped without upsetting the parse; Operation-group values still parse as before. `test_job_pipe_rip`: a target with `colour` 1 and a stage with `ink_row` gets four-plane rows for an RGB page; with `colour` 0 gray rows, as now.
- [ ] **Step 2:** Run → fails.
- [ ] **Step 3: Implement.** `ipp_req.c` reads the Job group for `print-quality` only. Every existing sink takes the new argument and ignores it. `job_pipe_rip.c` stops forcing `base.colour = 0` and takes the target's. `job_pipe.c`: `s_buffered` and the ring follow the target's `buffered`; the `prn` task calls `target->prn` when set, else `s_cfg.out` as today.
- [ ] **Step 4:** `make -C tools/ipp test`, `make -C tools/rip test regress interop`, `idf.py build` → pass.
- [ ] **Step 5:** Commit `ipp_server: print-quality from the Job group to the sink; job_pipe targets take colour and a printer-bytes consumer (W-1103)`.

### Task 6: The session (pure)

**Files:**
- Create: `firmware/components/stylewriter/{include/sw_session.h,sw_session.c}`, `tools/rip/test/test_sw_session.c`

**Interfaces:**
- Consumes: Task 1, Task 4's frames.
- Produces:
  ```c
  typedef enum { SWS_READY, SWS_PRINTING, SWS_NO_PAPER, SWS_JAM, SWS_CART_MISSING, SWS_CART_JAM,
                 SWS_INK_OUT, SWS_DOOR, SWS_WASTE, SWS_OFFLINE, SWS_FAULT } sw_state_t;
  enum { SW_OK = 0, SW_E_SILENT = -1, SW_E_MODEL = -2, SW_E_STALL = -3, SW_E_FAULT = -4, SW_E_CANCEL = -5, SW_E_CART = -6 };
  typedef struct {
      int      (*write)(void *ctx, const uint8_t *b, size_t n);    // 0 ok
      int      (*read)(void *ctx, uint8_t *b, size_t cap, uint32_t first_ms, uint32_t gap_ms);  // bytes, 0 none
      void     (*drain)(void *ctx);
      void     (*sleep_ms)(void *ctx, uint32_t ms);
      uint32_t (*now_ms)(void *ctx);
      int      (*cancelled)(void *ctx);
      void     (*state)(void *ctx, sw_state_t st);
      void      *ctx;
  } sw_ops_t;
  sw_model_t sw_ask_plain(const sw_ops_t *o);                 // `?` alone; never SW_ORIGINAL
  sw_model_t sw_ask_full(const sw_ops_t *o);                  // the full sequence, 2 s then 8 s
  int        sw_ask_status(const sw_ops_t *o, sw_model_t m, sw_state_t *st);   // `1` and `2`; SW_E_SILENT
  int        sw_ask_cart(const sw_ops_t *o, sw_model_t m);    // 1 colour, 0 black, SW_E_*
  int        sw_clean(const sw_ops_t *o, sw_model_t m);       // one `P`; waits for busy to clear, 600 s
  typedef struct { sw_model_t model; int quality; int colour_job; uint32_t stall_ms; } sw_job_cfg_t;  // stall_ms 600000
  typedef struct sw_session sw_session_t;
  sw_session_t *sw_session_new(const sw_ops_t *o, const sw_job_cfg_t *c);
  int  sw_session_begin(sw_session_t *s);                    // reset, settle, status, `?` = model, cart, `B`, setup
  int  sw_session_feed(sw_session_t *s, const uint8_t *buf, size_t n);   // frames, any split
  int  sw_session_end(sw_session_t *s);                      // wait 08, `E` (`Y` on the 1200), `l`
  void sw_session_abort(sw_session_t *s);                    // `E` on black-only models, then the reset
  int  sw_session_cart(const sw_session_t *s);               // what `H` said
  void sw_session_free(sw_session_t *s);
  ```
  The per-model setup and page letters are TailTalk's reading of Apple's drivers (`tailtalk/src/stylewriter/job.rs:34-59, 155-198`, `session.rs:585-722`), plain paper only, with the `x`/`h` fast-clock requests left out; put the table in `sw_session.c` as `static const` data with those line references.

- [ ] **Step 1: Failing tests** (a scripted fake in C behind `sw_ops_t`, fake clock, no sleeping):
  - `begin_order`: a 2200 colour job at normal sends `00 FF FF FF 49`, waits 2 s, asks `1` and `2`, asks `?`, `D`, `B`, `H`, then `m2`, `n`, `Z`, `A`; per page `H` once then `L`; an SW II job at normal sends `u`, its normal letter, `D`, `A`. The sequences equal TailTalk's `stylewriter_cross.rs` orders without `x`/`h` (copied into the test as expected bytes, with the reference).
  - `quality`: draft and best change only the quality letters, per model.
  - `pacing`: blocks are written as `G` + length + data + `00`; never more than the tracked free bytes; when short, `B` is re-read every 100 ms; before each rect, status is asked until clear.
  - `paper_out`: `2` reading 04 mid-page reports `SWS_NO_PAPER`, sends `S` every 5 s, resumes when clear; after `stall_ms` returns `SW_E_STALL`.
  - Each fault bit reports its state; `2` = 80 on a 2400 or 2500 is ignored.
  - `silent_mid_page` (Review Focus 1): no answer for 30 s reports `SWS_OFFLINE` and returns `SW_E_SILENT`.
  - `cart_swapped` (Review Focus 3): `colour_job` 1 with `H` = 01 prints `R` rects of K only and `sw_session_cart` is 0.
  - `garbled_answers` (Review Focus 4): a stray byte before the `?` reply is drained; `SW` without CR is read; `B` = 00 is no room.
  - `cancel_mid_band` (Review Focus 5): `cancelled` turning true mid-block lets that block finish, then `E` (black-only) and the reset, `SW_E_CANCEL`.
  - `wrong_model`: `?` answering `SW3` for a job made for `SW_II` returns `SW_E_MODEL` before any setup letter.
  - `plain`: `sw_ask_plain` writes exactly one byte `?`; `sw_ask_full` sends `p` only after `CS`, and tries 8 s when 2 s got nothing.
  - `clean`: `sw_clean(SW_ORIGINAL)` sends nothing and returns `SW_E_FAULT`; others send the setup with one `P`.
- [ ] **Step 2:** Run → fails.
- [ ] **Step 3: Implement** to the tests. The frame reader keeps a partial frame across `feed` calls in a buffer sized to the largest block (PSRAM on the board, `bigmem`).
- [ ] **Step 4:** `make -C tools/rip test` → pass.
- [ ] **Step 5:** Commit `stylewriter: the session, recognition, pacing, faults and cancel, host-tested (W-1103)`.

### Task 7: `swhost`, interop, and `swprint` for owners

**Files:**
- Create: `tools/rip/swhost.c`, `tools/stylewriter/test_swinterop.py`
- Modify: `tools/rip/Makefile` (`build/swhost`, `swinterop` target), `tools/stylewriter/README.md`

**Interfaces:**
- Consumes: Tasks 1, 4, 6.
- Produces: `swhost --dev PATH [--model M | --ask] [--quality 3|4|5] [--log FILE] PAGE.urf|PAGE.png...` opens `PATH` (a pty or a serial device; termios raw, 57600 8N1, no flow control, DTR and RTS asserted), finds or takes the model, rips each page through `ripjob` + `sw_page`, and runs the session with `sw_ops_t` over the device, logging both directions. Exit 0 printed, 1 with the session's error. `swprint` is `swhost` under that name in `tools/stylewriter/README.md`.

- [ ] **Step 1: Failing interop** (`make -C tools/rip swinterop`, Python spawning `swfake.py serve M --pty` then `swhost --dev <slave>`): for all seven models, `--ask` finds the model (the original only via `--model original`, since the plain question cannot find it), a gray and a colour page print and the fake's pages equal the rip's bits; with `paper` sent mid-page then `ok`, the page completes; with `silent` mid-page, `swhost` exits 1 after 30 s with "printer stopped answering"; `cancel` (SIGINT to `swhost` mid-band) leaves the fake reset and the next run prints.
- [ ] **Step 2:** Run → fails.
- [ ] **Step 3: Implement `swhost.c`** after `lwhost.c`'s shape.
- [ ] **Step 4:** `make -C tools/rip swinterop` → pass. README: how an owner with a USB serial adapter and Apple's printer cable runs `swprint --dev /dev/cu.usbserial-* --ask page.png` and sends the log back.
- [ ] **Step 5:** Commit `tools: swhost prints through the firmware's StyleWriter code to swfake or a real printer (swprint) (W-1103)`.

### Task 8: The port's link, and the port policy (pure)

**Files:**
- Create: `firmware/components/printers/{port_policy.c,include/port_policy.h}`, `tools/rip/test/test_port_policy.c`
- Modify: `firmware/components/serial_out/{serial_out.c,include/serial_out.h}`, `tools/rip/Makefile`

**Interfaces:**
- Produces:
  ```c
  // serial_out.h (board only; every call a no-op returning -1 in loopback)
  void serial_out_set_link(int baud, int cts);       // waits for TX, sets baud and CTS, drops RX
  int  serial_out_read(uint8_t *b, size_t cap, uint32_t first_ms, uint32_t gap_ms);
  int  serial_out_write_raw(const uint8_t *b, size_t n, uint32_t timeout_ms);   // no CTS, 1-tick waits
  // send_unflowed and lend(0) restore the current link's CTS rather than turning it on.
  // port_policy.h
  typedef enum { PA_NONE, PA_ESCQ, PA_PLAIN, PA_STATUS, PA_KICK } port_act_t;
  typedef struct {
      int      kind;            // pr_kind_t of the port record, -1 none
      int      said_other;      // "Something else"
      int      answering;       // the port's printer answered its last question
      int      line_ready, line_up_edge;   // HSKo, and a ready edge after >= 1 s down
      int      look;            // boot, setup or the Printers section asked
      int      want_kick;       // printer_state_want_kick()
      uint32_t now_ms, last_plain_ms, last_status_ms;
      int      plain_unanswered;  // the last plain question got nothing
  } port_in_t;
  port_act_t port_policy(const port_in_t *in);
  ```

- [ ] **Step 1: Failing tests** (`test_port_policy.c`): a StyleWriter record that answers gets `PA_STATUS` every 10 s and never `PA_ESCQ`, `PA_PLAIN` or `PA_KICK`; one that stops answering gets `PA_PLAIN` every 60 s and `PA_ESCQ` on a line-up edge; an ImageWriter record that answers gets `PA_ESCQ` on line-up only, never `PA_PLAIN`; one that does not answer also gets `PA_PLAIN` every 60 s; no record: `look` gives `PA_ESCQ` (line ready) then `PA_PLAIN`, and `PA_PLAIN` every 60 s; `PA_KICK` only with `want_kick` and `plain_unanswered` and never for a StyleWriter record; `said_other` gives `PA_NONE` always.
- [ ] **Step 2:** Run → fails. **Step 3:** Implement both. **Step 4:** `make -C tools/rip test`, `idf.py build` → pass.
- [ ] **Step 5:** Commit `serial_out: a runtime link for 57600 without flow control; the port policy, host-tested (W-1103)`.

### Task 9: Printer records for StyleWriters

**Files:**
- Modify: `firmware/components/printers/{printers_core.c,include/printers_core.h,printers.c,include/printers.h}`, `tools/rip/test/test_printers.c`, `docs/traps.md` (the blob section)

**Interfaces:**
- Produces: `PR_KIND_STYLEWRITER` appended to `pr_kind_t`; `PR_MODEL_SW1, PR_MODEL_SW2, PR_MODEL_SW1200, PR_MODEL_SWC1500, PR_MODEL_SWC2200, PR_MODEL_SWC2400, PR_MODEL_SWC2500` appended to `pr_model_t` (`pr_model_to_sw()` maps them to `sw_model_t`); `PR_BLOB_VER` 2 with a v1 reader; `void pr_port_seen_sw(pr_list_t *l, pr_model_t m)`; `pr_post_t` gains `int clean_id`; `pr_live_t` gains `uint8_t cart` (0 unknown, 1 black, 2 colour); `printers_cfg_t` gains `int (*clean)(int id)` (0, or 409 when the job lock is held), `void (*confirm_sw)(int id)` (run the full sequence for a model the person picked); `printers_test_kind()` returns 3 for a port StyleWriter; `port_mode_t` gains `PORT_STYLEWRITER`.

- [ ] **Step 1: Failing tests** in `test_printers.c`: a StyleWriter seen on an empty port makes a shared record named "StyleWriter II", kind `stylewriter` in JSON with `model` and `cartridge`; seen where an ImageWriter record is raises the changed notice ("The printer port now has a StyleWriter II. Use it instead of ImageWriter II?") and accepting it makes the record a StyleWriter with a new uuid; the reverse (an ImageWriter answering on a StyleWriter record) raises it too; `{"add_port":"Color StyleWriter 2500"}` and `{"model":"StyleWriter"}` on an unknown port are accepted (the existing test at `test_printers.c:551` that refused `"StyleWriter"` changes) and call `confirm_sw`; `{"id":N,"clean":true}` sets `clean_id`; `"apple"` passes the screen whitelist for a StyleWriter record only; a v1 blob loads (old kinds and models, nothing new), a v2 blob round-trips, a v3 blob is newer and kept; `pr_test_target` picks a port StyleWriter.
- [ ] **Step 2:** Run → fails. **Step 3:** Implement; tables stay `static const`; add `_Static_assert` on `sizeof(pr_rec_t)`; update `docs/traps.md`'s blob note (new kinds need a version bump, not only a new key). **Step 4:** `make -C tools/rip test` → pass.
- [ ] **Step 5:** Commit `printers: StyleWriter records, models and notices; list blob v2 with a v1 reader (W-1103)`.

### Task 10: The board: port, jobs, status

**Files:**
- Create: `firmware/components/stylewriter/{stylewriter.c,include/stylewriter.h}`
- Modify: `firmware/main/main.c`, `firmware/components/printer_state/{printer_state_core.c,include/printer_state_core.h}`, `tools/rip/test/test_printer_state.c`, `firmware/components/job_pipe/job_pipe.c` (finisher hook if needed)

**Interfaces:**
- Consumes: Tasks 4–9.
- Produces:
  ```c
  // stylewriter.h: J2 under the caller's job lock
  sw_model_t stylewriter_ask_plain(void);          // link to 57600, one `?`, then the link the record wants
                                                   // (57600 for a StyleWriter record, else 9600 + CTS)
  sw_model_t stylewriter_ask_full(void);
  int        stylewriter_status(sw_model_t m, sw_state_t *st);
  int        stylewriter_cart(sw_model_t m);
  int        stylewriter_job_begin(sw_model_t m, int quality, int colour_job, void (*state)(sw_state_t));
  void       stylewriter_prn(void *ctx, const uint8_t *buf, size_t n);   // job_pipe_target_t.prn
  int        stylewriter_job_end(int aborted);     // the link stays 57600 while the record is a StyleWriter
  int        stylewriter_clean(sw_model_t m);
  // printer_state_core.h: PS_EV_FAULT_ON / PS_EV_FAULT_OFF make derive() give PS_ATTENTION at once
  ```

- [ ] **Step 1: Failing test** in `test_printer_state.c`: `PS_EV_FAULT_ON` during a job is `PS_ATTENTION` with no 30 s wait; `PS_EV_FAULT_OFF` returns to printing.
- [ ] **Step 2:** Run → fails; implement the reducer change; pass.
- [ ] **Step 3: Wire main.c.**
  - `kick_task` runs `port_policy` every 500 ms with `xSemaphoreTake(s_job_lock, 0)` as today; `PA_PLAIN`/`PA_STATUS` go through `stylewriter.c`, answers feed `printers_port_seen` / `pr_port_seen_sw`, `s_iw_ready` (the port's ready for `port_live`, `port_state`, `box_ready`) and `printer_state_port_line`; HSKo edges are ignored while the record is a StyleWriter.
  - The idle CTRL-X kick follows `PA_KICK` only.
  - `colour_ok` for a StyleWriter record runs `stylewriter_cart` (not `ribbon_check`) and sets `s_ribbon`, `settings_save_ribbon` and `ipp_server_set_colour` as the ribbon does.
  - The port sink, for a StyleWriter record, builds a `job_pipe_target_t` with `sw_caps`, `sw_page`'s stage writing through `job_pipe_out`, `colour`, `buffered` 1, `prn` = `stylewriter_prn`, `tone` per Task 3's choice, and the job's quality; a StyleWriter finisher beside `laser_finished` calls `stylewriter_job_end`, frees the lock, and on `SW_E_SILENT`/`SW_E_STALL` fails the job as `printer_fail` does.
  - `port_push` maps `sw_state_t` to `ipp_server_set_state_for(0, …)`: no paper `media-empty-error` "Needs attention: check paper"; jam `media-jam-error`; cartridge missing `marker-supply-missing-error`; ink out `marker-supply-empty-error`; door `door-open-error`; waste `marker-waste-full-error`; offline `other` "Offline: check power and cable"; any change of fault calls it, not only a change of display state.
  - `/api/status` `model` is the port record's `make_model`; `printer` follows `s_iw_ready`; `STATUS_JSON` headroom checked.
  - `confirm_sw` runs `stylewriter_ask_full` under the lock and records the model found (or keeps the person's pick, offline, if nothing answers).
- [ ] **Step 4:** `idf.py build` clean; `make -C tools/rip test regress interop pscheck swinterop` and `make -C tools/ipp test` → pass.
- [ ] **Step 5:** Commit `firmware: a StyleWriter on J2: found, shared, printed through the session, status on the phone (W-1103)`.

### Task 11: The StyleWriter test page, and Clean Print Head

**Files:**
- Create: `firmware/components/stylewriter/{sw_testpage.c,include/sw_testpage.h,sw_font.h,sw_art.h}`, `tools/rip/test/test_sw_testpage.c`
- Modify: `firmware/components/testpage/tools/mkart.py` (`--grid 360x360 --out sw_art.h`), `firmware/components/testpage/tp_scene.c` (an aspect argument; the ImageWriter's call unchanged), `tools/rip/main.c` (`--sw-testpage black|colour --model M --out-sw F`), `firmware/main/main.c` (`test_page_to`, `test_button`, `web_test_page`, the second-tap cancel, `clean`)

**Interfaces:**
- Produces:
  ```c
  typedef struct {
      const char *printer, *board, *host;   // its name; "AirWriter v0.3.0 at 10.0.1.115"; "airwriter-2434.local"
      sw_model_t  model;
      int         colour_cart;
      int         gain;
  } sw_tp_info_t;
  int sw_testpage(const sw_tp_info_t *i, const rip_stage_t *st);   // one Letter page of rows to the stage; 0, or -1 short of memory
  ```
  `sw_font.h`: a 5 × 7 face for 0x20–0x7E drawn for this repo, drawn at 4 × 4 dots a pixel.

- [ ] **Step 1: Failing test** `test_sw_testpage.c`: decoded through `sw_page` and `sw_decode_row`, the page has the letterhead in its top inch, the ruler's ticks every 36 dots (0.1 in), four squares of exactly 360 × 360 dots, six screen swatches (the Apple screen among them), four ink swatches with a colour cartridge and a gray ramp with black, nothing outside the printable area.
- [ ] **Step 2:** Run → fails. **Step 3:** Implement the page (sections as the ImageWriter's: letterhead, details, inks or ramp, the still life via `tp_scene_row` at square pixels, screens, ruler and squares; no printer fonts section). Wire the button, DIP4 and `POST /api/test` to it for a port StyleWriter (the cable-test branch skipped), a second tap cancelling it, and `POST /api/printers {"id":N,"clean":true}` to `stylewriter_clean` under the job lock (409 when held).
- [ ] **Step 4:** `make -C tools/rip test`; preview with `tools/rip/build/rip --sw-testpage colour --model 2500 --out-sw tp.bin` and `swfake.py decode 2500 tp.bin --out <scratch>`, and look at the PNG; `idf.py build` clean.
- [ ] **Step 5:** Commit `stylewriter: a raster test page in a face of our own; Clean Print Head (W-1103)`.

### Task 12: The settings page

**Files:**
- Modify: `tools/webui/{body.html,app.js,style.css,states.json}`, regenerate `firmware/components/webui/webui_page.h` with `mkheader.py`

- [ ] **Step 1:** A `g-style` symbol (a StyleWriter in the page's 16-unit single-path style) and `stylewriter` in `KIND_IC`; the Model picker and Add a Printer list the seven StyleWriters after the ImageWriters; the detail's Status box gains a Cartridge row (Black or Color, the `rib-colour` swatch pattern); the button row gains Clean Print Head with the line "Cleaning uses ink." (not on the original); the Halftone list offers Apple on StyleWriters only and hides the ImageWriter's 144 × 72 sample for them; the paper message is "check paper" for a StyleWriter, "check paper and SELECT" for an ImageWriter; the offline message "check power and cable"; the unknown-port notice's picker includes StyleWriters. Copy per `docs/voice-and-tone.md`.
- [ ] **Step 2:** States in `states.json`: `stylewriter-colour`, `stylewriter-black`, `stylewriter-original` (no Clean), `stylewriter-paper`, `stylewriter-offline`, `stylewriter-changed` (the notice), `unknown-port-stylewriter` (the picker). `python3 tools/webui/preview.py`, then every state at desktop and phone width, light and dark, at http://localhost:8079/_states.
- [ ] **Step 3:** `mkheader.py`, `idf.py build` clean.
- [ ] **Step 4:** Commit `webui: StyleWriters in Printers: row, detail, cartridge, Clean Print Head, the Apple screen (W-1103)`.

### Task 13: Apple's drivers, captured (time-boxed; not a merge gate)

**Files:**
- Create: `tools/stylewriter/capture.md` (how the rig is set up), `tools/rip/test/swcaptures.py`
- Modify: `tools/rip/Makefile` (`swcaptures` target, skips without `~/.config/airwriter/sw-captures/`)

- [ ] **Step 1:** Basilisk II (`~/Projects/macemu`, or the Pi's) with System 7.5 and Apple's StyleWriter II, 1200, Color SW 1500, 2200 and 2400/2500 drivers (the Task 3 downloads plus the mono ones, same yes from Wells), its printer port redirected to a pty that `swfake.py serve M --pty --log` answers on. If a driver bypasses the serial driver and nothing reaches the pty, try Snow; if neither works in a day, write down what happened in `capture.md` and stop: this task does not gate the merge.
- [ ] **Step 2:** For each driver print blank, the checker, a text page and a colour photo (colour drivers); keep the logs and decoded pages in `~/.config/airwriter/sw-captures/<model>/`.
- [ ] **Step 3:** `swcaptures.py`: every capture decodes; each model's command order (minus page data) equals what `sw_session` sends for the same quality, or the difference is listed; AirWriter's checker page with `--dither apple` and Apple's decode to dots that agree within a stated tolerance (record the figure). Fix `sw_session`'s tables where Apple's driver disagrees with TailTalk's reading, citing the capture.
- [ ] **Step 4:** Commit `tools: the Apple-driver capture rig and checks, captures kept local (W-1103)`.

### Task 14: Docs

**Files:**
- Modify: `docs/SPEC.md` (§3.16 StyleWriters; §3.15's port paragraph), `README.md` (printers, credits), `docs/prior-art.md` (lpstyl, InkTalk; TailTalk extended), `LICENSE.md` (lpstyl if ported), `docs/voice-and-tone.md` (its "StyleWriters aren't supported" example), `AGENTS.md` (State, the Printers paragraph, Not built, Verification commands), the wiki (separate repo `wr/airwriter.wiki`)

- [ ] **Step 1:** Ask Wells how to credit Bolacore (handle, link, and whether it is the same person as FeralFirmware's Arctessa). Write the repo docs with that credit beside Monroe Williams and FeralFirmware.
- [ ] **Step 2:** Draft the wiki: Supported Printers flips the seven to `Yes*` (Portable StyleWriter, 4100, 4500, 6500 and any other stay `No`); a StyleWriter page (plugging it in, the cable, cartridges, Clean Print Head, and for rev A boards fitting R9 with a photo of where). Show Wells the draft; publish only on his yes.
- [ ] **Step 3:** Rev B fits R9 (the spec's call, approved with it): add "Fit R9 (0 Ω) so J2 pin 1 holds HSKo asserted for StyleWriters" to the rev B list in `docs/hardware-rev-a.md` and to `W-1091 Rev B: the main board's next revision` in Linear.
- [ ] **Step 4:** Commit `docs: StyleWriters (W-1103)`.

### Task 15: Bench, then ask to merge

No code. Each step that touches board 1 waits for the main session's go; each ImageWriter print waits for Wells's yes.

- [ ] **Step 1:** Record board 1's ImageWriter `printer-uuid` and a Get-Printer-Attributes dump. With prints held and `printing=false`, OTA the branch build; wait for `"trial":false`.
- [ ] **Step 2:** The ImageWriter unchanged: same uuid and attributes (save `printer-up-time`), `ipptool` suite passes; with Wells's yes, top margin 0, the checker gives crc32 `8ee84844`, then top margin back to −126.
- [ ] **Step 3:** The plain question on the ImageWriter: with Wells's yes and paper loaded, a debug build sends it 300 times over 5 minutes, then a CR LF; Wells checks the paper for stray marks. Then the full sequence once, and what it printed goes in `docs/traps.md`.
- [ ] **Step 4:** With the printer port empty: 10 minutes of the 60 s looks, the panel and page unchanged, `/api/mem` steady.
- [ ] **Step 5:** If a USB serial adapter and a Mac-style cable are to hand (ask Wells): `swfake.py serve 2500 --serial /dev/cu.usbserial-* --cart colour` on J2; found within seconds; an AirPrint page from a phone at each quality decoded by swfake as the rip's dots; paper out and back, cancel, a cleaning, the test page; `/api/mem` across a colour photo page.
- [ ] **Step 6:** With every host test, `swinterop` and steps 2–4 passing, update `W-1103 StyleWriters on the printer port` in Linear (what passed, what an owner must still confirm, the R9 question) and ask Wells to merge. On his word, merge to main and push.
