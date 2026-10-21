# AirWriter — AirPrint bridge for Apple serial printers

*ImageWriter → AirWriter. Sits alongside AirTalk in the same ecosystem, which is the point.*

A WiFi dongle that plugs into the serial port of an Apple ImageWriter II and makes it
appear as a native AirPrint printer to iOS, macOS, and any IPP Everywhere client. No host
computer, no CUPS, no drivers.

This document is the build spec. It covers a KiCad hardware project and an ESP-IDF
firmware project. Read the "Unverified assumptions" section before writing code that
depends on printer escape codes or wire polarity — several details in here are marked for
empirical verification and must not be guessed at.

---

## 1. Why this exists

Apple's ImageWriter family speaks RS-422 async serial at 9600 baud with DTR hardware
handshaking. It has no network interface. Existing solutions require either a host running
CUPS with a legacy Ghostscript driver (the `iwhi` device was removed from Ghostscript after
9.20 and no longer exists in any current distribution) or a LocalTalk bridge plus the rare
AppleTalk option card plus a netatalk 2.x stack that hasn't been maintained since 2012.

AirWriter does the rasterization on-device. iOS sends URF raster over IPP; the ESP32 dithers
it, transposes it into the printer's column-oriented graphics format, and streams it out
the UART with hardware flow control.

---

## 2. Hardware

### 2.1 Deliverables

A KiCad 10 project at `hardware/airwriter/` producing:

- `airwriter.kicad_sch`, `airwriter.kicad_pcb`, `airwriter.kicad_pro`
- Passing ERC and DRC, verified via `kicad-cli sch erc` and `kicad-cli pcb drc`

**Before running any `kicad-cli` command, run `kicad-cli --version` and `kicad-cli <sub>
--help` and work from that output.** Subcommand names, flags, and output formats have moved
between major releases, and training data for KiCad 10 is thin. Assume anything you
remember about the CLI is KiCad 8 or 9 era and verify it. The same caution applies to
file-format details in `.kicad_sch` and `.kicad_pcb` — generate them with the tool rather
than hand-writing s-expressions from memory.
- Gerbers + drill files in `hardware/airwriter/fab/` via `kicad-cli pcb export gerbers`
- BOM CSV with LCSC/Digi-Key part numbers
- Pick-and-place CSV
- A 3D render PNG for the README

Two-layer, 1.6mm FR4, 1oz copper, HASL. Standard process — no controlled impedance, no
blind vias, matte black soldermask, ENIG finish. Outline 52 x 78mm, rectangular with 3mm corner radii and four M2
mounting holes inset 3mm from the corners.

**As built (rev A, first powered 2026-10-14):** a 62 x 104.27mm main board plus a
snap-apart passive art panel, matte black, ENIG, four M2.5 holes. Rev A departs from this
section in many places — parts, LEDs, controls, outline. `docs/hardware-rev-a.md` lists every
deviation and why; the current BOM is `hardware/combined/fab/combined-assembly-bom.csv`.

### 2.2 Block diagram

```
USB-C (5V) ─┬─ ESD ─ LDO 3.3V ─┬─ ESP32-S3-WROOM-1
            │                  │
            └─ (power only)    ├─ UART1 TX/RX/CTS
                               │
                               ├─ MAX3491 (RS-422 full duplex) ─┬─ DIN8 pin 3/6 (TxD-/TxD+, printer out)
                               │                                └─ DIN8 pin 5/8 (RxD-/RxD+, printer in)
                               │
                               ├─ MAX3232 (1 rx channel) ──────── DIN8 pin 1 (HSKo, printer DTR)
                               │
                               ├─ WS2812B status LED
                               └─ config button
```

### 2.3 Parts

| Ref | Part | Package | Notes |
|---|---|---|---|
| U1 | ESP32-S3-WROOM-1-N8R2 | module | 8MB flash, 2MB PSRAM. KiCad `RF_Module:ESP32-S3-WROOM-1` |
| U2 | MAX3490ESA+ | SOIC-8 | 3.3V full-duplex RS-422, always enabled. **Changed from MAX3491 during the library audit** — the spec tied DE high and RE low permanently, so the MAX3491's enable pins were dead weight. The MAX3490 is the same transceiver without them, in a smaller package, and ships with a verified symbol in KiCad's stock `Interface_UART` library |
| U3 | MAX3232ESE+ | SOIC-16 | Only R1 used; T1 wired to HSKi but DNP by default |
| U4 | AP7361C-33E | SOT-223 | 1A LDO. Do not use a 600mA part — S3 WiFi TX peaks near 500mA and margin matters |
| U5 | USBLC6-2SC6 | SOT-23-6 | USB ESD |
| J1 | USB-C receptacle, 16-pin | SMD | 5.1k CC1/CC2 pulldowns |
| J2, J4 | **Kycon KMDGX-8S-BS** | THT | Mini-DIN-8 female, right-angle, fully shielded. Buy from Mouser, not Digi-Key, which is backordered. Footprint is custom — KiCad ships none — and was drawn from Kycon's drawing and verified against their STEP model. **Do not substitute Same Sky MD-80S/MD-80SM**, listed here in the original draft: their mounting is a rectangular-slot and earth-blade arrangement that does not fit this footprint. Solder by hand; see `docs/sourcing.md` |
| D1, D2 | WS2812B-2020 | 2020 | Status. D2 chains off D1's DOUT, so the second costs no GPIO. D1 = network, D2 = printer/job |
| D3 | green LED + 1k | 0603 | Power. Straight across the 3V3 rail, no GPIO. Separates "board is dead" from "firmware is dead" during bring-up |
| SW2 | Omron B3U-1000P | SMD | BOOT, on IO0: a strapping pin, so holding it at reset means USB download mode. Component side, not reachable through the case |
| SW3 | 6 mm tactile, 16 mm tall | THT | The `test` button, under the panel's test dot, on IO14. A tap prints a test page; held, it is the factory reset (§3.11) |
| J6 | JST-SH 4-pin, Qwiic pin order | SMD | I2C on IO36/IO37 with 4.7k pullups. A display module plugs in if one is ever wanted; the board does not commit 27 x 27mm to a screen that may never be fitted |
| SW1 | Panasonic EVQP44B3M | SMD | EN reset, side-press, east edge (reached through the case's pinhole). No code runs while it is held, so it cannot be the factory reset; that is SW3 (§3.11) |
| — | 22uF + 10uF + 0.1uF | 0805/0603 | Bulk on 3.3V rail, one 0.1uF per IC pin pair |

### 2.4 Connector wiring

**J2 is wired as a Macintosh serial port (flipped 2026-09-02, review #4), so the cable is
Apple's own crossover — 590-0552-A / M0197 "System/Peripheral-8", the cable every
ImageWriter II owner already has (1↔2, 3↔5, 6↔8, 4↔4, 7↔7). A pin-to-pin straight cable
does NOT work: the drivers fight (harmlessly, ~75 mA through the printer's 54 Ω) and the
handshake lands on the printer's DSR input, so the board reports "out of paper". J4
(LocalTalk) uses the same convention, so both sockets read the same way.**

**Cable:** use Apple's own Mac-to-printer cable (590-0552-A / M0197, null-modem: 1↔2,
3↔5, 6↔8) or any cable that works between a real Mac and the printer; many reproduction
"ImageWriter II" cables are wired straight-through and will not work with either (buzz
pin 1 on one end to pin 2 on the other before trusting one). Avoid mini-DIN-8 cables sold
for S-Video or Roland gear — they may drop conductors. (Research 2026-09-02: pinouts.ru,
colino.net's bench trace, VCF forum thread; colino.net found Pro Signal and CableLeader
"ImageWriter" cables wired straight.)

| J2 pin | Board signal (Mac convention) | Goes to | Reaches printer pin (via the crossover) |
|---|---|---|---|
| 1 | HSKo, out | U3 T1OUT via R9 (DNP) | 2 DSR (ignored by the printer) |
| 2 | HSKi, in | U3 R1IN → ESP32 UART1 CTS | 1 DTR / HSKo (the flow-control line) |
| 3 | TxD−, out | U2 Z (driver) | 5 RxD− |
| 4 | GND | GND | 4 GND |
| 5 | RxD−, in | U2 B (receiver) | 3 TxD− |
| 6 | TxD+, out | U2 Y (driver) | 8 RxD+ |
| 7 | GPi | not connected | 7 |
| 8 | RxD+, in | U2 A (receiver) | 6 TxD+ |

Printer-side polarity is unchanged from the corrected mapping below: the printer's TxD+
still reaches U2's non-inverting A input and TxD− reaches B; U2's Y drives the printer's
RxD+ and Z its RxD−. Net names stay printer-centric (`PR_TXD_*` = the printer's transmit
pair, on J2's RxD pins).

*(Before 2026-09-02 J2 was the mirror image of the printer's socket and needed a straight
cable, which nobody has; the original draft's receiver mapping was also inverted — see
assumption 3 in section 4.)*

**No polarity jumpers.** The draft called for solder jumpers on each pair so A/B and Y/Z
could be swapped without cutting traces. They were built and then removed, because the
ESP32 UART can inverse TXD and RXD in firmware — `uart_set_line_inverse()` with
`UART_SIGNAL_TXD_INV` / `UART_SIGNAL_RXD_INV`. Inverting DI flips Y and Z together, which
is electrically identical to swapping the pair, and the same holds for RO. That makes
polarity a one-line config change instead of four extra parts, so the insurance is kept
and the components are not.

**Termination and bias.** Footprint a 120R across the receiver pair and 4.7k bias
resistors to 3.3V and GND on it. All three DNP. Cable runs here are under two metres and
Apple's own cables were unterminated, but the pads cost nothing.

### 2.5 Layout notes

- Keep the differential pairs short and routed together. No length matching needed at
  9600 baud, but keep them away from the antenna.
- The ESP32-S3-WROOM-1 antenna keepout must overhang the board edge. No copper, no ground
  pour, no traces under the antenna region on either layer.
- Star-ground the DIN8 shell to signal ground through a single 0R jumper plus a footprint
  for a 1nF capacitor in parallel, so the shield strategy can be changed after testing.
- **USB-C carries data as well as power.** D+ and D- go to the S3's native USB peripheral
  on GPIO19/20 through U5, so the same cable that powers the board also flashes and
  consoles it. Both flip orientations are tied (A6+B6, A7+B7). J3, the 4-pin 1.27mm
  header, stays as a parallel tap on the same two nets for bring-up.
  *Amended: the original draft made the USB-C power-only and put USB on the header alone.*

---

## 3. Firmware

### 3.1 Stack

ESP-IDF 5.x, C. Project at `firmware/`. Components under `firmware/components/`:

| Component | Responsibility |
|---|---|
| `net_provision` | Improv-over-BLE provisioning, NVS credential storage |
| `ipp_server` | IPP/1.1 over HTTP on port 631 |
| `ipp_attrs` | Printer attribute set, job attributes, state reasons |
| `urf_decode` | URF and PWG raster stream decoder |
| `halftone` | Grayscale → 1bpp dithering |
| `imagewriter` | Column transpose, escape code emission |
| `serial_out` | UART driver with hardware CTS, DTR watchdog |
| `webui` | Config page on port 80 |
| `localtalk` | AppleTalk: the cable's LocalTalk node, EtherTalk for Basilisk II and Infinite Mac, the printer they share (§3.13) |
| `printer_state` | Job queue and state machine |

### 3.2 Discovery

mDNS advertisement:

- Service `_ipp._tcp` on port 631, with subtype `_universal._sub._ipp._tcp` (the subtype is
  what makes iOS list it — without it the printer is invisible to the print sheet)
- Also advertise `_printer._tcp` on 515 as a stub for older clients that probe it

TXT record keys:

```
txtvers=1
rp=ipp/print
ty=Apple ImageWriter II
product=(AirWriter)
pdl=image/urf
adminurl=http://<host>.local/
priority=10
UUID=<stable, derived from eFuse MAC>
Color=F
Duplex=F
URF=<see 3.3>
```

*Amended (`W-1064 Colour printing over AirPrint with the ImageWriter II colour ribbon`):
`Color` is `T`, and `URF` the colour string of §3.3, while the printer's self-ID last reported
a colour ribbon. Both are re-announced when that changes, under the same UUID (§3.5).*

Only `image/urf` is advertised for now — `urf_decode` recognises the PWG Raster magic
but rejects it (`URF_ERR_UNSUPPORTED`), and advertising a PDL the firmware can't decode
fails every job a client sends in that format. Add `image/pwg-raster` back to `pdl=` once
the 1796-byte PWG header parser exists.

Default hostname `imagewriter-XXXX.local` where XXXX is the last two MAC bytes in hex,
overridable from the web UI.

### 3.3 The URF string

Start with:

```
URF=V1.4,CP1,RS72-144,W8,DM1,IS1,MT1,FN3
```

`W8` declares 8-bit grayscale, `DM1` no duplex, `RS72-144` the two resolutions the printer
can actually hit. Some clients ignore anything but `RS300`; if iOS refuses the job or sends
300dpi anyway, keep a downsample path in `urf_decode` and fall back to advertising
`RS300`. Make this a compile-time flag, not a rewrite.

*Settled 2026-08-04: iOS honours `RS72-144` and renders at exactly 144 dpi
(`fixtures/ios-20260804-232652.urf`, asserted in `test/test_ios_capture.c`), so no
downsample path or `RS300` is needed.*

*Amended (W-1064): with the colour ribbon in, `SRGB24` follows `W8`
(`V1.4,CP1,RS72-144,W8,SRGB24,DM1,IS1,MT1,FN3`), in the TXT record and `urf-supported`.*

### 3.4 IPP operations to implement

Minimum viable set for iOS:

- `Get-Printer-Attributes` (0x000B) — must respond in under two seconds or the print sheet
  gives up
- `Validate-Job` (0x0004)
- `Print-Job` (0x0002) — body arrives chunked. The whole document goes into a ~1 MB PSRAM
  buffer as fast as Wi-Fi delivers it (an iPhone Letter page is ~140 KB of URF; decoded it
  would be 1.9 MB, which is why the buffer holds URF, not pixels), and the rip decodes from
  there at the printer's pace. The response goes back as soon as the upload is complete,
  with the job `processing`; the job ends `completed`, `aborted` or `canceled` when the
  paper stops, and holds the printer until then (W-1042). Reading the socket to the end is
  also what makes a dropped upload visible at once: without the buffer a paused printer
  stalls the upload behind a zero window, and the client's FIN waits behind its unsent
  data until paper returns. A document bigger than the buffer, or a board with no PSRAM
  (8 KB internal ring), falls back to streaming with TCP backpressure.
- `Get-Job-Attributes` (0x0009) — iOS polls this for progress
- `Get-Jobs` (0x000A)
- `Cancel-Job` (0x0008)

Return `server-error-operation-not-supported` for everything else rather than dropping the
connection.

**Not `Create-Job` / `Send-Document` (W-1040).** They were tried so the phone would hold a
job-id before the upload. With them in `operations-supported` the iPhone held every job at
"Waiting" and never submitted it, re-polling Get-Printer-Attributes. They are not needed: while
a Print-Job body is still uploading, iOS and macOS find the job with Get-Jobs and poll
Get-Job-Attributes, so they see a paused job anyway (3.9); with the job buffered they have
its job-id from the early reply as well.

### 3.5 Printer attributes worth getting right

**requested-attributes.** Get-Printer-Attributes returns what was asked for: the groups `all`,
`printer-description` and `job-template`, explicit names, or `none` (RFC 8011 4.2.5.1), and
`media-col-database` only when named. macOS marks a printer that ignores this with
`cups-ipp-conformance-failure-report`. Check with `tools/ipp/build/gpa_server` and CUPS's
`get-printer-attributes-suite.test`; macOS's copy of that suite has a broken fifth test (it
requests `all` but expects only `media-col-database`), which no printer can pass.

**Margins.** The tractor feed means this printer physically cannot print to the edges.
Declare honest values or iOS will lay out edge-to-edge content and silently clip it.
Start with 6.35mm (0.25in) left and right, 12.7mm top and bottom, in hundredths of a
millimetre as IPP requires. Refine after test prints.

*Amended (2026-10-20): the top margin is 25.4mm (1in), bottom still 12.7mm. Paper loaded as
the ImageWriter II Owner's Guide says, its leading edge under the paper bail, prints from about
an inch below its edge, and Apple left software to allow for it. A page arrives as the whole
sheet, so the board drops that inch of every page (the Top margin setting, default -1in, §3.8)
and iOS, told the inch is margin, draws nothing there: every row prints where iOS put it. The
printable area is 8 x 9.5in. On board 1 the calibration page's lines landed exactly 1in from
the top and 0.5in from the bottom at -7/8in, which is where its Top margin is set.*

**Media.** `na_letter_8.5x11in` plus the continuous fanfold sizes. Not borderless.

**`printer-icons`.** Three PNGs served on the IPP port itself,
`http://<host>.local:631/icon-{48,128,512}.png`, smallest first as IPP Everywhere orders
them. A Mac fetches them once, when the printer is added, and keeps an `.icns` in
`/Library/Printers/Icons/` that the queue's PPD names (`*APPrinterIconPath`); a new icon
shows only after the printer is removed and added again. The PNGs are built into the
firmware by `tools/icon/mkicons.py` from a 32 × 32 pixel grid: 128 and 512 are exact 4× and
16× enlargements, and 48 is the grid at 1× with a margin, since 1.5× would draw uneven
pixels. The artwork is Apple's colour ImageWriter icon, redrawn pixel for pixel
(`LICENSE.md`).

**`printer-uuid`.** Derive from eFuse MAC so it survives reflashing. Add a `DEV_MODE`
build flag that regenerates it on every boot — iOS caches capabilities against the UUID
aggressively, and during development you will otherwise spend an afternoon staring at a
print sheet showing yesterday's media list with no way to flush it.

*Amended (`W-1037 Status language v2: discrete states, idle sleep, Spin boot`): dev mode is off by default — a UUID that moves every boot left the iPhone
with stale twins and stuck jobs. Instead the UUID is MAC plus `CONFIG_AIRWRITER_UUID_EPOCH`,
bumped only when advertised capabilities change (a new UUID strands the Mac's CUPS queue).*

**Colour (W-1064).** The board asks the printer which ribbon is in with `ESC ?` (self-ID,
Technical Reference Table 6-7: `IW10CF`, `C` with a colour ribbon in) when the line comes
ready while idle and before each AirPrint job, and saves the answer. While it is colour:
`color-supported` true, `print-color-mode-supported` `monochrome, color`, default `color`.
Otherwise exactly the black-only set. This changes advertised values under one UUID, which
`docs/traps.md` warns against; it is a deliberate first try, and the fallback (a settings switch
with a second UUID) is in `docs/superpowers/specs/2026-10-17-colour-printing-design.md`.

### 3.6 Raster decode

iOS sends `image/urf`. Support `image/pwg-raster` too — same pixel data, different
container.

- URF: 8-byte `UNIRAST\0` magic, 4-byte big-endian page count, then a 32-byte header per
  page followed by encoded lines.
- PWG: 4-byte `RaS2` sync word, then a 1796-byte header per page.

Both use the same PackBits-style per-line RLE. **Do not derive the encoding from first
principles.** Port it from the reference implementations in cups-filters (`rastertopwg.c`,
`urftopdf.cpp`) and cite the source file in a comment. The line-repeat byte and the
literal/repeat run semantics have edge cases that are easy to get subtly wrong and hard to
debug on target.

### 3.7 Halftoning

Use Atkinson dithering. It's the right choice technically for a high-contrast bilevel
device with no ink density control, and Bill Atkinson wrote it for the Mac in the same
years this printer shipped, which is a detail worth having in the README.

Make the algorithm selectable in NVS — Atkinson, Floyd-Steinberg, ordered Bayer 8x8, and
plain threshold. Ordered dithering will look better than error diffusion on the small text
that dominates real print jobs, and you won't know until you've printed both.

*Amended (W-1068): a pin's dot is about as wide as its row (0.35 mm against a 0.18 mm step
across and 0.35 mm down), so neighbours overlap and a tint prints darker than its bits. A tone
curve can run ahead of every screen, the inverse of `1 - (1 - a)^k` for the dot gain at a half
tint (NVS `gain`, seeded by `CONFIG_AIRWRITER_DOT_GAIN`), so pure black and white are unchanged
and the midtones ask for fewer dots. On paper the uncorrected page read best against the
screen, so the default is 0 and the curve is there to tune. The error-diffusion screens scan serpentine, each row the other
way with the kernel mirrored, which removes their diagonal worms. A fifth screen, `bluenoise`,
is a 64x64 void-and-cluster tile (`halftone/tools/make_bluenoise.py`) grown with rows
counting double, 16 cells per level so a flat tint prints exactly its share. On a colour page
the inks are screened black first, cyan, magenta, yellow: the diffusion screens print on
another ink's dot only when a quarter darker and carry the rest on (dot-off-dot), and the mask
screens put black and cyan on one tile upside down to each other and magenta and yellow on a
tile far away, so the common two-ink tints never share a dot until they have to.*

### 3.8 The ImageWriter output stage

Target for v1: **144 dpi horizontal, 72 dpi vertical, single pass.** Printable width 8
inches = 1152 dots. Select the density once per page with **`ESC p`** — see assumption 2 in
section 4 for the full table and the `ESC p` / `ESC P` trap.

Per eight raster rows:

1. Accumulate 8 rows of 1bpp data into a strip buffer (1152 bytes worst case — under 2KB,
   so nothing ever needs a full page in RAM)
2. Transpose to column-major: each output byte is one vertical column of 8 dots, **bit 0
   at the top** (assumption 1, resolved) — so the transpose reverses bit order
3. Emit `ESC G nnnn` where `nnnn` is a four-digit ASCII decimal byte count
4. Emit the column bytes
5. Emit carriage return and a line feed of 8/72 inch

Set line spacing once per page with `ESC T nn` (nn in 144ths of an inch, ASCII digits).
End every page with a form feed — the widely reported "you have to press Select to make it
print" behaviour is almost always a missing FF, not a hardware fault.

*Amended (2026-10-20): every job starts at top of form. `ESC c` leaves top of form where it is
(Table A-2), so after a cut job, or a Mac's that stopped mid-page, the next job sends a form feed
first. And a Top margin setting, in 1/144 in, signed, moves AirPrint pages and the test page:
positive adds paper ahead of a page, negative drops its first rows. Paper loaded as the Owner's
Guide says, its leading edge under the paper bail, prints from about 1 in below its edge, so the
default is -1 in: a page, which arrives as the whole sheet, then lands where iOS laid it out.*

*Amended (2026-10-20): a strip with no dots sends no `ESC G`, only the line feed it owes, held
like any other until a strip below it prints. Board 1 fed a white page at a line a second when
each blank strip was 1152 zeros at 9600 baud.*

*Amended (W-1064): an sRGB page with the colour ribbon in prints in four inks. Each pixel splits
into yellow, magenta, cyan and black (grey components go to black), each ink is dithered on
its own, and a strip prints one pass per ink it uses, in that order (yellow first, Technical
Reference p. 123): `ESC K n` (1 yellow, 2 magenta, 3 cyan, 0 black) when the ink changes,
`ESC G` with that pass's count trimmed to its last dot, the columns, CR with no LF. A strip
with no ink only adds its LF. The page ends on black. With a black ribbon an sRGB page is
turned to gray (Rec. 601 luma) and printed as above.*

Defer 144x144 double-pass mode to v2. It needs half-line feeds and two passes per strip,
and it doubles the time on a printer that is already slow.

### 3.9 Flow control and state

The printer has a 2KB input buffer. Wire HSKo to the ESP32's UART CTS pin and let the
peripheral stall the FIFO — no polling loop, no software watermark.

Then use that same line for status. If DTR stays deasserted past a timeout (start at 30
seconds, make it configurable), set `printer-state` to `stopped` and `printer-state-reasons`
to `media-empty-error` (the `-error` suffix of RFC 8011 5.4.12; a stopped printer must carry an
error reason), with a `printer-state-message`. The printing job then reads `job-state`
`processing-stopped` with `job-state-reasons` `printer-stopped` (RFC 8011 5.3.7), which iOS
and macOS poll for with Get-Jobs and Get-Job-Attributes, during the Print-Job upload and,
with the job buffered, after the early reply while it prints (3.4). That surfaces as a
real alert on the phone. A 1985 dot matrix telling an
iPhone it's out of paper is the demo shot for the video.

### 3.10 Host-side test harness

Build `urf_decode`, `halftone`, and `imagewriter` as a plain host binary under
`tools/rip/`:

```
rip --in sample.urf --out-preview preview.png --out-stream stream.bin --dither atkinson
```

This is the most important item in this document for development velocity. It means the
raster pipeline can be developed and regression-tested without an ESP32 or a printer in the
loop, and `stream.bin` can be diffed byte-for-byte between changes. Capture a real URF
sample from iOS early (point the print sheet at a stub IPP server that dumps the body to
flash) and commit it as a fixture.

### 3.11 Recovery: partitions, rollback, factory reset

An owner must always be able to get back to a working printer without a computer
(`W-1046 Factory reset must always work, even with broken firmware`,
`W-1045 Long-press factory reset with live feedback on the LED panel`,
`W-1059 Known-good slot: factory reset boots the last version that worked`; design in
`docs/superpowers/specs/2026-10-16-known-good-slot-design.md`).

**Partitions** (`firmware/partitions.csv`, 8 MB): nvs 24K at 0x9000, otadata at 0xf000,
phy_init, **known_good** 8K at 0x12000, ota_0 2M at 0x20000, ota_1 2M at 0x220000, ota_2 2M at
0x420000, storage 1.9M. Three equal app slots, no factory slot.

**The known-good slot.** `known_good` holds a 32-byte record (magic, sequence number, slot, the
first 16 bytes of the image's `app_elf_sha256`, CRC-32) twice, one per 4 KB sector; the valid
copy with the higher number names the slot and the image, and a blank or damaged record means
ota_0. The record vouches for that image only: if the slot now holds another one, it vouches
for nothing. A new copy goes into the sector that does not hold the
current one, so a power cut leaves the old one in charge (`components/ota_health/
known_good_core.c`, which the bootloader hook builds too). The running image points the record
at itself once it is kept (it booted valid, or its trial's mark-valid succeeded) and has
finished a real print: an IPP or
LocalTalk job that ran to its end with every byte taken by the printer, not the test page or a
cancelled, failed or cut-short job. That is one small write per update, made under the job
lock. Nothing is ever copied between slots. An update goes into the first slot after the
running one that is neither the running slot nor the known-good one, so the known-good image is
never overwritten. `idf.py flash` writes a blank record beside the blank otadata, so a USB
flash starts with the app it wrote (ota_0) as known-good.

**Rollback** (`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`). A new OTA image boots on trial and keeps
itself only after 15 s of unbroken health: with Wi-Fi credentials, joined and with IPP and the
web UI listening; without them, the setup AP and web UI up (`components/ota_health`). Not
healthy 120 s after boot, it marks itself invalid and reboots into the image before it; a
crash or reboot during the trial is rolled back by the bootloader itself. While an image is
on trial `/update` refuses another. A failed trial goes back to the image that was running
before the update (otadata's other entry), not to the known-good one. `/api/status` carries
`slot`, `trial`, `rolled_back` (the version last rolled back from, or empty), `known_good` (the
slot the power-up reset boots) and `known_good_version`.

**Factory reset, two levels, both on the test button** (SW3, IO14):

- *Settings*, in the app (W-1045). A tap (under 1 s) prints a test page, on release. Held
  past 1 s the panel counts down; letting go cancels. At 8 s the whole nvs partition is erased
  (Wi-Fi, queue name, hostname, dither, the LocalTalk node ID) and the board restarts once the
  button is let go. The firmware stays.
- *Settings and firmware*, in the bootloader (W-1046, W-1059). Hold the button while powering
  up (plug in, or the reset pinhole) and keep holding for 5 s: the bootloader erases nvs and
  boots the known-good slot, whatever state the app is in. A hook does it
  (`bootloader_components/airwriter_boot`; IDF's `CONFIG_BOOTLOADER_FACTORY_RESET` is off,
  because it would boot ota_0): it tries the known-good image if the record still vouches for
  it, then the newest image otadata says was kept, then the rest from ota_0 up, and last any
  image otadata says is new, on trial or failed, checking each the way the bootloader checks
  any image. It writes an otadata entry for the first that checks out (state valid,
  over the sector that does not hold the newest kept entry, so a cut leaves that entry in
  charge rather than an update on trial), erases
  nvs, and leaves the RTC flag the app reads for the panel cue; the bootloader's own slot
  choice then boots it. The bootloader never writes the record: a fallback image becomes
  known-good after its first real print. The panel stays dark while the button is held (the
  bootloader has no LED driver; the hook first clamps IO48, IO35 and IO4 so the hold does not
  leave them floating), then spins, then shows the reset's confirmation.

IO14 is not a strapping pin (the S3's are IO0, IO3, IO45, IO46), and SW3 shorts it straight to
GND with no external pull-up or capacitor; the bootloader enables the internal pull-up before
it reads. A press already down when the app starts is the tail of the bootloader's hold, and
the app ignores it until it is let go. A build with Wi-Fi credentials in its `sdkconfig` rejoins
them after either reset; one without (board 1's, and every release) comes back in setup mode.
The bootloader cannot be updated over the air, so the hook is the one piece a mistake in which
needs a USB flash on every board.

**Moving a board from an older table takes one USB flash**, because an OTA cannot rewrite the
bootloader or the partition table. `idf.py -p PORT flash` writes the bootloader, the table, a
blank otadata, a blank record and the app into ota_0 (where the factory slot was); nvs, otadata
and phy_init kept their offsets, so Wi-Fi credentials and settings survive it. Whatever the old
slots held would now sit in ota_1 and ota_2, where a fallback could boot it, so the move also
erases 0x220000–0x61ffff. Any USB flash writes all five images of the build's `flash_args`
(`idf.py flash`, or `esptool write_flash @flash_args`); the app alone leaves otadata and the
record naming other slots. An existing `sdkconfig`
needs the options from `sdkconfig.defaults` and `sdkconfig.defaults.esp32s3` (main.c refuses to
build without them). This firmware OTA'd onto a board still on the W-1046 table runs, logs that
it has no `known_good` partition, updates into IDF's next slot and never promotes; that board's
power-up reset still boots its factory slot.

Over the air, only signed images install (§3.12), and an update waits for, and gives way to,
a print job. A USB flash must write a signed build, or the board takes any image once.

### 3.12 Updates: signed images, GitHub releases, auto-update

`W-1048 OTA updates from GitHub releases: signed, verified, optional auto-update`; design in
`docs/superpowers/specs/2026-10-16-signed-github-updates-design.md`.

**Signing.** IDF's signed apps without secure boot (`CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT`,
RSA-3072, `SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT`, `SECURE_BOOT_BUILD_SIGNED_BINARIES`; main.c
refuses to build without them). `esp_ota_end` checks the new image's signature block against the
key that signed the *running* image, so `/update` and the GitHub path share one check. No eFuse
is burned and the bootloader is unchanged. A board running an unsigned image accepts any valid
image once; after that only images signed with the same key. Changing the key takes a USB
flash. The release key is `~/.config/airwriter/firmware-signing-key.pem` on Wells's Mac (backed
up in 1Password), never in the repo; its public half is `firmware/keys/airwriter-release.pub.pem`,
digest `6f11af9cd8eec9923985957400aa753ba034e7e057e18ebf61033cb4d327ad89`
(`CONFIG_AIRWRITER_UPDATE_KEY_DIGEST`). A build from someone else's key makes a board that takes
only their builds, and its web page says it can't install AirWriter releases.

**Releases.** A tag `vX.Y.Z` with two assets, `airwriter-esp32s3.bin` (the signed app image) and
`airwriter.json` (`{"version":"vX.Y.Z","esp32s3":"airwriter-esp32s3.bin"}`), made only by
`tools/release/release.sh`: it builds from `sdkconfig.defaults` alone in a throwaway worktree,
checks the version, the bench options, the signature and that no local Wi-Fi credential or
purge-list string is in the image, then tags and publishes. The board reads
`github.com/<repo>/releases/latest/download/airwriter.json` (no API, no rate limit;
`CONFIG_AIRWRITER_UPDATE_REPO`, default `wr/airwriter`) and installs with `esp_https_ota`.
The manifest is a hint: the image's own version and project name are checked as it starts to
download, and its signature at the end. A release build's version is exactly `vX.Y.Z`;
anything else (`v0.1.0-3-gabc1234-dirty`) is a dev build, and a release is newer only if its
`vX.Y.Z` is past the running one's.

**When.** An update takes the job lock without waiting and holds it to the restart. A job that
finds the lock taken raises a flag, and the update aborts at its next chunk and lets the job
print; `/update` answers 409. Not while the last update is on trial, never in setup mode
(GitHub path), never into the running or the known-good slot (§3.11; `esp_https_ota` gets the
slot as `partition.staging`).

**Auto-update** (`airwriter`/`autoupd` in NVS, off by default, the web page's auto key). Off,
the board never contacts GitHub by itself; opening the web page checks at most hourly. On, it
checks 10 minutes after the first Wi-Fi join and every 24 hours of uptime (a failed check
retries after an hour), and installs a newer release once the board has had no job for
10 minutes. It never replaces a dev build, never installs the version the bootloader last
rolled back, and tries each release once per check. The panel shows progress through W-1049's
`LG_ACT_FW_*` overlays.

**Web.** `GET /api/update`; `POST /api/update/check`, `/api/update/install` (optional
`{"tag":"vX.Y.Z"}`, which may be older or a prerelease), `/api/update/auto` (`{"auto":true}`).

### 3.13 AppleTalk: the cable, Basilisk II and Infinite Mac

To a Mac, AirWriter is Apple's ImageWriter LocalTalk Option Card: the Chooser's AppleTalk
ImageWriter finds `<queue>:ImageWriter@*` by NBP and prints over PAP, and the job is the Mac
driver's own ImageWriter stream, which goes to the printer as it is. Four links reach that
printer (`components/localtalk`):

- **The cable** (`W-1051 LocalTalk node: show up in the Chooser as an ImageWriter`, `W-1053
  LocalTalk: print PAP jobs from the Mac's AppleTalk ImageWriter driver`): LocalTalk through
  TashTalk in U7 on J4, in DIP mode 2. LLAP node acquisition by ENQ (TashTalk answers ENQ and
  RTS for the claimed ID), short DDP headers.
- **Basilisk II's UDP tunnel** (W-1076, design `docs/superpowers/specs/2026-10-19-ethertalk-design.md`):
  EtherTalk frames as UDP datagrams to port 6066 on the home network (`udptunnel true` in
  Basilisk II's prefs, and `udpappletalkonly true` to keep the Mac's internet through its
  `ether`; SheepShaver has no tunnel). The board's MAC there is `42 32` and its IPv4
  address. On by default; NVS `et`/`tunnel`.
- **The board's own Wi-Fi** (`W-1078 EtherTalk over Wi-Fi: print from real Macs (BlueSCSI,
  Ethernet Macs)`): EtherTalk frames to and from the station interface itself, for Macs on the
  home network (Ethernet, or Wi-Fi through a BlueSCSI's DaynaPORT). lwIP's
  `LWIP_HOOK_UNKNOWN_ETH_PROTOCOL` hands over the frames it does not know; they go out through
  `esp_wifi_internal_tx`. The Wi-Fi delivers 802.3 frames with a length of 0, rebuilt from the
  DDP header (or AARP's 28). On by default; NVS `et`/`radio`.
- **Infinite Mac's zone relay** (W-1076): EtherTalk frames as JSON over a WebSocket to
  `wss://infinitemac.org/zone/<zone>/websocket`, as the Basilisk II and SheepShaver Macs in an
  infinitemac.org tab started with `?appleTalk=<zone>` use. Off until NVS `et`/`zone_on` is set
  (W-1079), and then needs `et`/`zone`: 1 to 32 of `A-Z a-z 0-9 - _ .`. The name works like a
  password (anyone who opens Infinite Mac with it can print) and appears only in
  `/api/et/settings`; the page makes one up rather than offering a guessable default. The board's MAC there is `B2` and the rest of its station MAC: Infinite Mac's emulators send
  unicast only to `B2…` MACs, their own kind, and drop the rest. It answers
  Infinite Mac's ping frames, and reconnects after 1 s, doubling to 60 s.

All three EtherTalk links are AppleTalk Phase 2 with no router: an address in the startup range
(network FF00-FFFE, node 1-253) taken by 10 AARP probes 200 ms apart and kept in NVS for the
next boot, AARP answered for it, MACs learned from AARP and every datagram received, long DDP
headers, broadcasts to `09:00:07:FF:FF:FF`. A network with a router is noted, not joined.

**One printer.** Every link prints through one spool and print task (`at_printer.c`): one job
at a time under the job lock, so a Mac whose OpenConn finds the printer taken (an IPP job, the
test page, another link's job) is told busy, and each node's status word says busy unless its
own job holds the printer. Jobs are named by link ("LocalTalk job", "Basilisk II job",
"Infinite Mac job"); the last one's bytes are at `GET /api/lt/job`. An EtherTalk connection
that sends nothing for 60 s while the printer is ready is closed, so an idle relay peer cannot
hold the printer from AirPrint; the cable keeps its own timers. A link that drops (the relay
closing, Wi-Fi going, a new IP) ends its job abandoned at once.

**Memory.** mbedTLS allocates from PSRAM (`CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC`): a held TLS
session in internal RAM left Wi-Fi and lwIP starving.

**Web.** `GET /api/lt` (the cable node), `GET /api/et` (both EtherTalk links, the shared
printer, internal RAM, the log), `GET`/`POST /api/et/settings`
(`{"radio":true,"tunnel":true,"infinite":true,"zone":"…"}`; 400 for a zone name outside the rule
above).

---

## 4. Unverified assumptions

These are marked because they must be checked against hardware or Apple's *ImageWriter II
Technical Reference*, not inferred. Any of them being wrong will produce output that looks
almost right, which is the worst failure mode.

**Status 2026-10-15: all five are resolved, and all five are confirmed by correct prints on
rev A** (iPhone, Mac and `ipptool` jobs through J2 and Apple's crossover cable; the board's
stream for `fixtures/checker.urf` is byte-identical to `checker.expected.bin`). HSKo has
never been scoped; see 4.

1. ~~**Bit order within a graphics byte.**~~ **RESOLVED FROM PRIMARY SOURCE.**

   *ImageWriter II Technical Reference*, p. 107, verbatim: "Bit 7 causes the dot at the
   bottom of the column to be printed and bit 0 causes the dot at the top to be printed."
   So **bit 0 is the TOP pin**, bit 7 the bottom — reversed from the MSB-is-top convention
   most raster code assumes. Ghostscript's `gdevadmp.c` does an explicit 8x8 transpose plus
   a bit reversal for exactly this reason, which independently confirms it.

   Scan: the ImageWriter II Technical Reference Manual (see `docs/reference/README.md`). PDF page = book page + 20 for
   chapters 1-7; the offset drifts to +19 partway through the appendices, so Table A-15 on book
   p. 144 is PDF page 163, not 164.

2. ~~**Density escape codes.**~~ **RESOLVED FROM PRIMARY SOURCE.**

   Table 8-2, p. 106. Every byte cross-checked against `gdevadmp.c` and they agree exactly.

   | Command | Hex | Pitch | dpi | Max dots/line |
   |---|---|---|---|---|
   | `ESC n` | 1B 6E | Extended 9 cpi | 72 | 0576 |
   | `ESC N` | 1B 4E | Pica 10 cpi | 80 | 0640 |
   | `ESC E` | 1B 45 | Elite 12 cpi | 96 | 0768 |
   | `ESC e` | 1B 65 | Semicondensed 13.4 cpi | 107 | 0856 |
   | `ESC q` | 1B 71 | Condensed 15 cpi | 120 | 0960 |
   | `ESC Q` | 1B 51 | Ultracondensed 17 cpi | 136 | 1068 |
   | **`ESC p`** | **1B 70** | **Pica proportional** | **144** | **1152** |
   | `ESC P` | 1B 50 | Elite proportional | 160 | 1280 |

   **`ESC p` is 144 dpi and `ESC P` is 160 dpi.** Case-sensitive, adjacent in the table, and
   the most commonly mis-transcribed pair in circulating documentation.

   Two further corrections to watch, both verified against the manual:

   - **`ESC G nnnn` takes four ASCII decimal digits**, not a 16-bit binary count. Summaries
     describing it as `n1 + n2 * 256` are quoting Epson ESC/P's `ESC K`, which is a different
     printer family. `ESC S nnnn` is an exact alias for `ESC G`. `ESC g nnn` takes three
     digits and prints `nnn * 8` bytes.
   - **`ESC T nn` is nn/144 inch, exactly two ASCII digits, 01-99.** `ESC T16` (16/144 =
     8/72) exactly abuts successive 8-dot bands, which is the 72 dpi vertical case in §3.8.
     144 dpi vertical needs two interleaved passes with `ESC T01` between them.

   One more, not an assumption but a trap: the printer's DIP default is **7 data bits**.
   Graphics commands force 8-bit for the duration of their data, but text will not behave
   until the mode is set.
3. ~~**Differential polarity** on both pairs. Hence the solder jumpers.~~
   **RESOLVED BY RESEARCH — and §2.4's receiver mapping is wrong.**

   Apple's convention: *"a binary 1 is represented when TxD- is more negative than TxD+.
   Binary 0 is indicated by TxD- more positive than TxD+"*, and the RxD pair reads the same
   way. So the `+` line is non-inverting: at mark/idle, TxD+ is high and TxD- is low.

   The MAX3490 has RO high when A > B, and DI high drives Y high and Z low.

   - **Driver (us → printer): §2.4 is correct.** Idle TX is high → Y high, Z low → RxD+ high,
     RxD- low = mark. Y→pin 8 (RxD+), Z→pin 5 (RxD-).
   - **Receiver (printer → us): §2.4 is backwards.** It maps A←TxD- and B←TxD+, which puts
     A low at mark, so RO idles low and every byte arrives inverted. It should be
     **A ← pin 6 (TxD+), B ← pin 3 (TxD-)**.

   Confidence: good, not certain. Both sources are secondary (Mac serial references, not the
   ImageWriter II Technical Reference), and they document the Macintosh port rather than the
   printer specifically. The two are the same Apple serial family so the convention should
   hold; the firmware invert switches (`uart_set_line_inverse`) remain the cheap insurance
   now that the solder jumpers are gone.

   **2026-08-28: corroborated by the primary source.** The ImageWriter II Technical
   Reference Manual (`docs/reference/README.md`) Appendix E,
   "Transmitted Data" / "Received Data" (PDF pp. 206-207): "When no data is being
   transmitted, the TxD- line is held in the logic 1 state", and TxD-/RxD- are "of the
   proper polarity to serve as the single ... data line for an RS423 or an RS232
   interface". Under the RS-232/RS-423 mark-negative convention that fixes TxD+ as the
   non-inverting line — exactly the corrected mapping above. Status: resolved from the
   printer's own manual; the bench check on board 1 is confirmation, not discovery.

   Impact is limited: the ImageWriter talks back over HSKo hardware handshaking rather than
   the serial receive path, so a wrong receiver polarity does not block M1 or M5. Bias
   resistors R6/R7 already pull A high and B low, which matches the corrected mapping.

   **Confirmed on rev A (2026-10-14):** prints come out right through J2 and the Apple
   crossover cable.
4. ~~**HSKo electrical levels.** Still open, but narrowed.~~ **RESOLVED on paper
   (2026-08-28), from the primary source.** Appendix E Figure E-3 ("Output Circuits",
   PDF pp. 204-209) draws DTR/HSKo driven by **one half of a 26LS30 in single-ended
   mode off ±5 V rails** shared with the TxD driver half. A ±5 V RS-423-style swing is
   well inside the SP3232 receiver's ±25 V absolute maximum and comfortably past its
   ~1.2-1.5 V threshold, and Appendix E's "Data Terminal Ready" text ("true (logic 0)
   when the printer is on and able to receive data") matches the design's
   true = logic 0 = ready sense exactly. The SP3232 receiver choice is sound; scoping
   HSKo on board 1 is a confirmation step. (`CONFIG_AIRWRITER_INVERT_CTS` stays as the
   insurance either way.)

   **The handshake timing is a hard firmware constraint.** DTR goes false when the buffer
   has room for fewer than 30 characters, and the host must stop within 27 more characters
   or data is lost. At 9600 baud 8N1 that is a 28 ms window — comfortable for the hardware
   CTS path, which is another reason to let the peripheral stall the FIFO rather than
   watermarking in software. DTR returns true at 100 characters free.
   **Polarity: true = logic 0 = ready.**

   XON/XOFF is the forgiving alternative if the hardware path disappoints: XOFF at fewer
   than 266 free, with 256 characters of slack, XON at 337 free. Appendix E, pp. 187-193.

   **Confirmed in practice on rev A, never scoped.** IO8 reads LOW with the printer ready
   (read live over JTAG, `GPIO_IN` bit 8), so `CONFIG_AIRWRITER_INVERT_CTS` stays off, and CTS
   pauses and resumes correctly across SELECT off/on (2026-10-15). Still worth a scope trace
   during a long job before rev B: if the swing is modest, a divider and clamp into a GPIO
   could delete U3.
5. ~~**URF 32-byte page header field offsets.** Take these from cups-filters source, not from
   memory.~~ **RESOLVED.** Taken from `cups/raster-stream.c` in OpenPrinting/cups and
   implemented in `urf_decode.c` with the offsets cited in comments. Confirmed against real iOS
   captures (`fixtures/ios-*.urf`) and iPhone prints on rev A.

---

## 5. Milestones

**M1 — Wire.** Board assembled, `serial_out` sends a literal ASCII string, printer prints
it. Resolves assumptions 3 and 4.

**M2 — Ink.** `rip` host tool produces a correct preview PNG from a captured URF fixture.
No hardware involved. Resolves the raster decode and dithering.

**M3 — Dots.** `imagewriter` streams a known bitmap to the printer via M1. Resolves
assumptions 1 and 2.

**M4 — Discovery.** Device appears in the iOS print sheet as AirWriter, with correct icon
and media list. No printing yet — `Print-Job` returns success and discards.

**M5 — Print.** End to end from an iPhone.

**M6 — Manners.** Paper-out detection, job cancel, web config UI, Improv provisioning,
OTA.

Ship M5 as the first video. M6 is the second one.

---

## 6. Repository layout

```
airwriter/
├── hardware/airwriter/          KiCad 10 project
│   ├── airwriter.kicad_sch
│   ├── airwriter.kicad_pcb
│   ├── lib/                  custom symbols and footprints
│   └── fab/                  gerbers, BOM, PnP
├── firmware/
│   ├── main/
│   ├── components/           per 3.1
│   └── sdkconfig.defaults
├── tools/rip/                host build of the raster pipeline
├── fixtures/                 captured URF samples
└── enclosure/                 3D printed case, later
```

Licence: hardware CERN-OHL-S, firmware GPLv3, to match the rest of the retro ecosystem.
