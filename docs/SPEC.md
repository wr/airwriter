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
| `net_provision` | Setup network (open AP, every DNS answer pointing at the setup page), NVS credential storage |
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
- Each printer answers at its own resource (3.15), `ipp/p<id>`, port records included; the
  oldest port record also at `ipp/print`, and any other path goes to it while it is shared
  (W-1116; before, `ipp/print` was the port's one record and `ipp/p<id>` AppleTalk's).
- `Print-Job` (0x0002) — body arrives chunked, into a 1 MB PSRAM buffer as fast as Wi-Fi
  delivers it. An ImageWriter job holds 512 KB of URF in it, and the rip runs as the URF
  arrives into the other 512 KB, which holds the printer's bytes and drains to the port at
  9600 baud (W-1066): an iPhone Letter page is ~140 KB of URF and a full-page colour photo
  ~5.7 MB, but either rips to under 460 KB of ImageWriter bytes, while pixels would be 1.9 MB
  (gray) or 5.8 MB (colour). A LaserWriter job holds URF in all of it and rips at the
  printer's pace. The response goes back as soon as the upload is complete,
  with the job `processing`; the job ends `completed`, `aborted` or `canceled` when the
  paper stops, and holds the printer until then (W-1042). Reading the socket to the end is
  also what makes a dropped upload visible at once: without the buffer a paused printer
  stalls the upload behind a zero window, and the client's FIN waits behind its unsent
  data until paper returns. A document whose half outruns its 512 KB (a LaserWriter's URF
  past 1 MB, an ImageWriter job's printer bytes past 512 KB), or a board with no PSRAM (8 KB
  internal ring), falls back to streaming with TCP backpressure.
  A Print-Job that finds the printer taken (another phone's job, a Mac's on AppleTalk, the
  test page, an update) is answered `server-error-busy` at once, its body read to the end,
  and the client asks again (RFC 8011). It never waits with its document unread: received
  frames left unread hold internal RAM, and on the W-1076 bench a job that waited behind a
  Mac's page could not start its rip.
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
own job holds the printer. An AirPrint job that finds it taken is told busy too (3.4). Jobs are named by link ("LocalTalk job", "Basilisk II job",
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

### 3.14 LaserWriters on AppleTalk (W-1082)

Design: `docs/superpowers/specs/2026-10-21-laser-printers-design.md` (phase 1). **Untested on a
real LaserWriter:** built and proven against a fake one (`tools/ethertalk/etpeer.py
laserwriter`); the design's "What only a printer settles" is the bench list. Since W-1088 a
LaserWriter is a record in the printer list (3.15), found and shared like any printer; the
ImageWriter on the port keeps its identity, UUID and bytes (`tools/ipp/test/golden/`).

- **Finding it.** NBP `=:AirWriter@*`, `=:LaserWriter@*` and `=:ImageWriter@*` on every running
  link (`lt_node_lookup`), 300 ms apart and the three again 1.5 s later: 10 s after start (at
  once in setup mode), every 30 s for 5 minutes, then every 5 minutes, and again on "Look for
  Printers". SendStatus to each printer in the list in turn, every 5 s (2 s printing). The
  policy is `laser/lw_core.c`. Heard on two links, the cable's wins, then the network, the
  tunnel, the zone. A printer answering from another AirWriter's node is skipped. No answer for
  30 s is `stopped` + `offline-report`.
- **Switched on** (W-1098). A printer coming up checks no one has its name before it takes it,
  an NBP LkUp of that one name a few times about 2 s apart (Inside AppleTalk 7-9; a Personal
  LaserWriter NTR: five, after 8 s of LLAP ENQs reusing its node ID). Every link's node passes a
  LkUp of one name, no wildcard, to `lwc_name_check`; for a LaserWriter or ImageWriter not heard
  in the last 30 s on that link it puts off one look until 3 s after the last check. A printer
  switched on is found in seconds, new or renamed; nothing is sent while it is off (the NTR on
  board 1: ready 3 s after its last check). A check of a new name from the address another name
  was heard at ends that name there at once: a renamed printer answers the old name's SendStatus,
  which names nothing. An old Mac's driver looks up its printer by name before printing, and a
  look follows that too unless the name answered lately on that link. `/api/laser` counts looks
  and checks.
- **Roles.** A link runs when its way in is switched on or a printer in the list lives there;
  it answers old Macs only where its way in is on (3.15's offers).
- **The query job** (`laser/lw_query.c`): a Level 1 PAP job that prints nothing and asks
  product, version, language level, resolution, imageable area, page size, page count and
  whether it prints a start page (W-1099), each inside `stopped`, plus a 256-byte binary
  probe. Binary goes to the printer only if the probe came
  back intact; otherwise hex. Answers in its record; run again at the first answer after boot
  and when the printer comes back, tried again after 60 s twice, then every 5 minutes. Three
  unanswered make the "didn't answer" notice (Share Anyway takes its defaults).
- **The AirPrint printer** (`ipp_server` printer 1-5, `laser/lw_caps.c`): `ipp/p<id>` (a board's
  W-1082 LaserWriter advertises `ipp/laser` and answers at both), its own Bonjour instance (its name in the list),
  `printer-uuid` `41575254-4c57-<CONFIG_AIRWRITER_LASER_UUID_EPOCH>-
  <hash of the NBP name>-<MAC>`, URF `RS300`, `W8`, only the paper it reported, margins from its
  imageable area rounded outward, `laser-*.png` icons. One job at a time across every printer.
- **The page** (`ps_page`): the rip at 300 dpi (`printer_caps`, `ripjob`'s output stage, the
  printer's own screen and gain) into Level 1 `imagemask` bands of at most 64 rows, ended by a
  blank row, trimmed to their ink; one LF before binary data; hex lines of 64 digits. A text
  page is about 40 % of its bitmap. `make -C tools/rip pscheck` holds it to Ghostscript dot for
  dot.
- **PAP client** (`localtalk/pap_client.c`, beside the server on the same node): OpenConn from
  one socket that is also the responding socket, busy retried every 2 s up to the stall limit
  (600 s), the printer pulls 8 Data packets of at most 512 bytes per SendData, the last 8 answers
  kept for retransmits, the printer's output read back from the start, tickles every 30 s,
  dead after 120 s silent, CloseConn after the printer's EOF (or 30 s without it).
- **Status.** `PrinterError:` texts map to `media-empty-error`, `media-jam-error`,
  `door-open-error`, `toner-empty-error`, `other-error` (stopped, the printer's words as the
  message); `toner-low-warning` does not stop it; busy and warming up are `processing`. A
  PostScript error in its output becomes the job's `job-state-message`. The texts are guesses
  until a LaserWriter is benched.
- **Test page** (`ps_page/ps_testpage.c`): one Letter page of Level 1 PostScript, hex only, in
  the printer's own Times and Helvetica: the mark, its name and product, the board, a type
  sample, its gray ramp, AirWriter's five screens as 300 dpi swatches, a ruler and four
  one-inch squares; sent as it is (`laser_test_page`).
- **Start page** (W-1099). The printer's detail has a Start page switch where the query read
  `statusdict /dostartpage`. Flipping it sends LaserWriter Utility's recipe on the laser task as
  a query goes (`lw_start_page_job`: `serverdict begin 0 exitserver statusdict begin <bool>
  setdostartpage end`, then the setting read back), which the printer keeps in non-volatile
  memory; `POST /api/printers {"id":N,"start_page":bool}` waits for the answer (up to 25 s, 8 s
  of it a busy printer): saved, busy (409), a password other than 0 (`invalidaccess`), or no
  answer (502). The record follows what the printer says (`lw_info_t.start_page`, in what was
  its tail padding: its stored size is pinned).
- **Panel.** A LaserWriter job is an AirPrint job (3.15's box state).
- **Web.** `GET /api/laser` is diagnostics (heard, targets, the last job's figures); the page
  reads `/api/printers`. `/api/status` `laser`: `{"targets":N,"online":M,"job":bool}`.

### 3.15 Printers (W-1088)

Design: `docs/superpowers/specs/2026-10-23-printers-design.md`. AirWriter keeps a list of the
printers it has, found or told (`components/printers`: `printers_core.c` pure and host-tested,
`printers.c` the board's), in NVS `printers`/`list` (`pr_save`, versioned; a blob that does not
read back is a list made again). At most 8 records.

- **Records.** id (stable), where (port, or an AppleTalk link), kind (ImageWriter with model II,
  LQ or original; StyleWriter with one of its seven models, 3.16; LaserWriter; AppleTalk
  ImageWriter; unknown), name (from the printer: the
  port's model, an AppleTalk printer's NBP name; two the same get " 2"; renamed freely, UTF-8,
  MacRoman for NBP), uuid (made once and never changed, whatever the record's model becomes:
  a port record `41575254-4c57-<epoch>-<model><family>-<MAC>`, the model's number and `01`
  for an ImageWriter or `02` for a StyleWriter, since W-1116, each model's record its own
  printer to phones; before W-1116 the family tag alone, `0001` or `0002`, which records from
  then keep; a form a record since corrected to another model still holds sets the model
  byte's top bit; an AppleTalk printer W-1082's form), shared, found (auto or person),
  halftone and gain, and a LaserWriter's query answers.
- **Renamed** (W-1098). A LaserWriter's record also keeps where it last answered its query (the
  link and its address there) and its page count, in NVS `printers`/`seen` beside the list
  (`pr_seen_save`; the list's blob is unchanged, so an older firmware reads it). A printer new to
  the list whose query is answered at the address a silent LaserWriter of the same product had,
  with as many pages or more, is that one renamed: the old record takes the new NBP name, its
  own name following unless the person gave it one, and keeps its id, uuid, AirPrint printer and
  settings; the new record goes, with no notice. Otherwise a new name is a new printer.
- **The port.** At line-up after 1 s or more down (and at the first report) the board sends
  `ESC ?` and `CR`: `IW10`… is an ImageWriter II, `LQ`… an LQ, no answer with the line ready an
  unknown device, named "Unknown printer", never shared (made only with no port record; with
  an original ImageWriter record it is that record's answer, with any other port record a lost
  answer, nothing). A StyleWriter may never raise the line
  and speaks at 57600, so a port whose printer does not answer is also asked the StyleWriter's
  plain question, one `?` byte that an ImageWriter at 9600 does not hear; a StyleWriter that
  answers it is a port record of its own (3.16 has the rules and what each question costs).
  ImageWriter commands (the CTRL-X kick, resync, the job's self-ID, the test page) go to J2 only
  for an ImageWriter (the kick also with nothing found yet), and never to a StyleWriter.
  "Something else" stops even the self-ID. W-1116 (3.16): the port keeps a record for every
  model that answers on it, one per model, and the one that answered last is the printer on
  the port. An answer naming a recorded model is that record's, at once. One naming a model
  with no record changes the list only when it comes twice in a row (any other model's answer,
  or nothing said, in between starts it over; the line down does not), so a garbled reply never
  adds a printer: it makes the model's record, shared if there is room; a found "Unknown
  printer" record becomes it (uuid kept), or goes with its notice when the model has a record
  already; a "Something else" record is replaced. "Add a Printer…" adds a record for a model
  not in the list (original ImageWriter, one that is off, a StyleWriter by model), beside the
  records there, or converts an "Unknown printer" or "Something else" record; a model already
  listed is refused, and so is a record's Model set to one another record has, or to
  Something else while another port record is one (one per port).
- **Sharing.** Identified printers are shared at once, up to six (`PR_SHARE_MAX`, W-1116; any
  mix of the port's records and AppleTalk printers, where before it was the port's and three
  on AppleTalk), each its own AirPrint printer in one of `IPP_PRINTERS` (6) slots, Bonjour by
  name (`CONFIG_MDNS_MAX_SERVICES` 10 in `sdkconfig.defaults`: six `_ipp`, the `_printer` stub
  and `_http`). Every record answers at `ipp/p<id>` for as long as it exists, port records
  included; the oldest port record is also advertised at `ipp/print`, at printer 0, with the
  legacy routing of any other path, and W-1082's LaserWriter also answers at `ipp/laser`.
  Removing the oldest port record moves `ipp/print` to the next one, whose `ipp/p<id>` keeps
  working (`/api/printers` `rp` names the advertised path; the slot's `legacy_resource` is the
  other, and `ipp_route` tries every slot's two paths, slot 0's included, before any other
  path falls to slot 0). The seventh identified printer is listed, not shared, with the
  "too many" notice, and can be shared once another stops; a shared record keeps its place
  through a model change. A change reaches
  Bonjour at once, after the job holding its slot. The W-1079 AirPrint switch is the whole
  box's. AirPrint to an AppleTalk ImageWriter is not built: it is old Macs' only. The slots
  (`printers.c`): printer 0 is the oldest port record; every other shared record, port or
  AppleTalk, takes the lowest free slot of 1-5 and keeps it for its life there; a record that
  becomes the oldest moves to printer 0 under the same Bonjour name, so printer 0 is cleared
  first and the record leaves its old slot before it takes it, and while a job holds either
  slot nothing moves until the job ends. A port record's slot carries the record's own sink, with
  its slot and id in the ctx as a LaserWriter's does, so a job's end goes back to the slot it
  came in on. What a port record's slot says (`pr_port_say`, `main.c`'s `port_push`, on every
  change of the port's state or the list): the printer on the port its own state, in its
  family's words (3.9's for an ImageWriter, 3.16's for a StyleWriter), and the colour the
  ribbon or cartridge last read offers (a black-only StyleWriter none); every other port
  record `stopped`, `offline-report`, "Offline: *name* is connected instead." while that
  printer is present, "Offline: the printer port is set to Something else." while a Something
  else record is listed, else its family's offline words with nothing answering; so phones see
  every shared port printer all the time, each offline while it isn't the one answering. The
  same printer put in its slot again (a rename, a model change: its uuid) keeps its state and
  colour; another one starts with none until its owner pushes them.
- **Notices.** Only what asks the person something: unknown port, no answer (Share Anyway),
  too many; at most 8, each with a caution mark, and one beside the sidebar's Printers while
  any waits; each is dismissed. A newly found printer just appears in the list, and so does a
  different printer answering on the port (W-1116): a "found" notice stored by an older build,
  or a "changed" one (accept or dismiss) from before W-1116, is dropped on load.
- **Old Macs.** On each way in that is on, the node's own name is the port's one ImageWriter
  (if any): the oldest ImageWriter record's name, fixed across swaps so a Chooser selection
  keeps working (W-1116; `pr_offers` lists that record alone for the port, and
  `printers_offers` names it from `pr_chooser_name`, where before the last port ImageWriter
  overwrote the name). `pr_chooser_name` is also the queue name at boot (mDNS's default
  instance, the node's NBP object): the oldest ImageWriter record's name, else the oldest port
  record's, else "AirWriter" (before W-1116, the one port record's). The test page names the
  printer on the port (`printers_port_name`, `pr_port_current`).
  A Mac's ImageWriter job goes to whichever ImageWriter answers on the port (the II, LQ and
  original take the same stream). Registrations 1-3 are the AppleTalk printers from other
  links under their own NBP names and types (relayed), and `<host name>:AirWriter` is at
  registration 4 on every running link (`lt_node_offer`, sockets 0x85-0x88). A Mac's
  connection on a relayed name goes through `localtalk/pap_relay.c`, run by `laser.c` with the
  one PAP client under the job lock: busy, in the printer's own status words, until the
  printer's connection is open; then both ways as they are, the printer's output 256 bytes a
  read.
- **Panel.** Idle, the box: ready while any shared printer is; while the port prints, its own
  line (a StyleWriter has none: its answers to `1` and `2`); during a LaserWriter job, the
  LaserWriter's. The port's own state belongs to one record (`pr_port_live_id`, W-1116): the
  printer on the port, or the Something else record while one is listed; every other port
  record's live state is offline, on the page, to phones and in the box's state alike.
- **Test page.** The button, DIP4 and the page's Print Test Page (without an id) go to the
  printer on the port (`pr_port_current`, an ImageWriter or StyleWriter record) while it is
  present, else the first shared printer that is ready, else that port record (the button then
  runs the cable test, as before), else the first shared printer (`pr_test_target`, W-1116);
  `POST /api/test {"id":N}` names one.
- **Setup.** In setup mode the port is asked and the cable looked on at once, and the page is
  three steps, sized for a phone: the printers found, Wi-Fi, then "You're All Set" (how to print
  from a phone, and the test button for a test page; the settings' address for later, nothing
  to copy). `/api/status` `fresh` (NVS, set by the captive page's Wi-Fi save, cleared by
  `POST /api/general {"finish":true}`) opens the "AirWriter is ready" sheet on the network.
- **A board from before.** At its first boot on W-1088 (Wi-Fi saved, no list) its records are
  made from what it had: the port's ImageWriter with its old uuid and the AirPrint switch as its
  Share (the switch then on), a W-1082 LaserWriter at `ipp/laser`. Its host name is stored, so
  `imagewriter-xxxx` stays; a new board is `airwriter-xxxx` with the AppleTalk port on.
- **Web.** `GET /api/printers`: the records with live state and `rp` (above), the notices,
  then (W-1116) `port_on` (the id of the port record whose printer is on the port: the one
  that answered last, while it is present; 0 for none), `said_other` (a Something else record
  is in the list), `chooser` (the names vintage Macs see: the port ImageWriter's as on the
  page, an AppleTalk printer's own NBP name whatever the page calls it) and `max_shared` (6);
  `swapped` is gone. `POST /api/printers` with `{"look":true}`, `{"add_port":"<model>"}`,
  `{"note":N, "dismiss":true}` or `{"id":N, "name", "shared", "model" ("none" for Something
  else), "remove", "use_anyway", "dither", "gain", "clean"}`; 400 with a sentence for the page
  ("<model> is already in Printers." for `add_port` or `model` naming a model another port
  record has, "Something else is already in Printers." for `model` `none` while another port
  record is one, "You can share up to 6 printers." at the cap, W-1116). A StyleWriter's record
  also carries `cartridge` and `fault` (3.16).

### 3.16 StyleWriters (W-1103)

Design: `docs/superpowers/specs/2026-10-25-stylewriters-design.md`; plan
`docs/superpowers/plans/2026-10-26-stylewriters.md`. Phase 1 is AirPrint to a StyleWriter on the
printer port: `components/stylewriter` (`sw_model`, `sw_code`, `sw_page`, `sw_session`,
`sw_status` and `sw_testpage` are pure and host-tested in `tools/rip`; `stylewriter.c` is the
board's), `components/printers/port_policy.c`, and `tools/stylewriter` (the software printer
and the owner's tool). **Built and host-tested against a software StyleWriter
(`tools/stylewriter/swfake.py`) and a second coder (lpstyl's); no StyleWriter has printed from
AirWriter, and Apple's drivers have not been captured** (the rig stops at a 68k Mac ROM,
`tools/stylewriter/capture.md`). The command letters, orders, geometry and block caps are
TailTalk's reading of Apple's drivers (`tailtalk/src/stylewriter/*`, FeralFirmware; read for
facts, never copied) and lpstyl's `README.protocol`; only the Color StyleWriter 2200 has
printed from TailTalk on a real printer. Phase 2, vintage Macs printing through AirWriter as
Apple's StyleWriter adapter (ADSP, not PAP), is not built.

- **Models.** `sw_model_t`: the original StyleWriter, StyleWriter II, StyleWriter 1200, Color
  StyleWriter 1500, 2200, 2400 and 2500 (the names the page and Bonjour use). The Portable
  StyleWriter and the Color StyleWriter 4100, 4500 and 6500 are not supported. A port record
  of kind StyleWriter keeps its model (`pr_model_t`, appended after the ImageWriters' so stored
  numbers keep theirs). The list blob is version 2 for it, with a reader for version 1, and 3
  for a second port record (W-1116; the layout is the same throughout); `pr_save` writes the
  lowest version that holds the list, 1 until a record or notice names a StyleWriter and 2
  until a second port record, so an OTA rollback does not touch a one-printer list, and a
  rolled-back firmware keeps a newer stamp unread for the next update (`docs/traps.md`).
- **Two languages on J2.** An ImageWriter is 9600 baud with CTS on HSKi (3.9); a StyleWriter is
  57600 baud, 8N1, RS-422, no flow control (`serial_out_set_link`; its handshake line is not a
  pacing signal and says nothing, so a low line is no sign of a cable and the test button's
  cable-test branch is skipped for it). The link follows the port's record. Every question puts
  it back, and ImageWriter-only traffic (`ESC ?`, the CTRL-X kick, the resync, the job's
  self-ID, the ImageWriter test page) goes after `stylewriter_link(0)` and never to a
  StyleWriter record. The cable is expected to be the ImageWriter II's (Apple's
  System/Peripheral-8 crossover), as a StyleWriter plugs into a Mac's printer port the same
  way: not yet confirmed on a StyleWriter.
- **Recognition.** Two questions. The *plain question* is the one byte `?` at 57600. At 9600 its
  frame is a 17 µs low pulse, high past the receiver's mid-bit check, then a 35 µs low that
  ends in the stop bit before the next check: an ImageWriter's UART takes both for false start
  bits and hears nothing (`tools/stylewriter/test_plain_question.py` runs a 16× receiver model
  at 64 phases). The II, 1200 and colour models answer it, to CR at 250 ms a byte: `SW` the II,
  `SW3` the 1200, `CS` a colour model, whose member is the byte after `FF FF FF 70` (`p`): 01
  the 2400, 02 the 2200, 04 the 1500, 05 the 2500, anything else the 2200; a `CS` that does
  not answer `p` names no model, and the next look asks again. `p` sent to any other model
  ejects and resets it, so only a `CS` is sent it. The *full sequence* is the reset
  `00 FF FF FF 49` (eject and reset), a 2 s settle (8 s on a second try), a drain, then `?`
  again; it also finds the original (`IJ10`), which ignores `?` until reset (InkTalk's notes,
  from its author's own original; not yet seen here). Its `00` and `49` can read as stray
  characters at 9600, so it goes only to a printer a record or the person has named: before
  every job to a StyleWriter record, once when the person picks a StyleWriter (Add a Printer…,
  the unknown printer's notice, the detail's Model), to confirm it and read its model, and to
  an original StyleWriter record once at start-up and on the person's Look for Printers when
  nothing else answered (Wells, 2026-10-27). So the original is found only when named.
- **What the idle port is asked** (`port_policy`, pure: one act a call, every 500 ms, under the
  job lock and never while a job runs on the port; `port_tick`, `port_after` and
  `port_line_down` keep its books between calls, so the host tests run the whole loop against
  scripted printers for ten minutes, every record against every printer, and through scripts
  of swaps with the list kept as the board keeps it). Its inputs are the current port record's
  kind (`pr_port_current`, W-1116), what the list holds (any StyleWriter record, an original
  ImageWriter record, an original StyleWriter record: the order of the questions is the
  list's, not the current record's alone), the family that answered last, the line, and the
  events. In this order: an act the board wants repeated (`confirm`: a seen call answered
  `PR_SEEN_AGAIN`, one sighting of a model with no record, and the same answer twice makes the
  record), at once, before anything else; the person said Something else, nothing of
  AirWriter's own, and the person's own Look for Printers asks once, as any look does, with
  nothing following it (not the plain question that follows an unanswered `ESC ?` elsewhere,
  not the cadence, and a full sequence owed waits until the Something else record goes); a
  StyleWriter record that answers, `1` and `2` every 10 s, on a look and on a line-up edge
  (the line may be another printer's: a StyleWriter that is gone turns into the stop's look at
  once, and the check's bytes at 57600 are harmless to an ImageWriter, `docs/traps.md`
  2026-10-27), and nothing else (no `ESC ?`, no plain question, no kick); on a line-up edge or
  a look with the line ready, for every other port, the
  plain question first where the port is StyleWriter-first (any StyleWriter record listed, a
  port that last answered as a StyleWriter, or an original ImageWriter record listed, which
  gives no self-ID), then `ESC ?` once that got nothing, and `ESC ?` alone everywhere else (an
  empty port, ImageWriter II or LQ records only), so an ImageWriter swapped in is noticed and
  a StyleWriter that would answer never hears `ESC ?` (`swfake.py` takes it for a letter it
  does not know and stops listening until a reset; one that raises its line on a port with no
  StyleWriter record hears it first, the design's cost); the plain question on a look with the
  line down; the full sequence once while an original StyleWriter record is listed and
  nothing answers, at start-up (the board sets `full_due`; a look of the board's own goes
  first and leaves it owed) and when the person's Look for Printers got nothing from the plain
  question or `ESC ?` (or from the status, for one that was answering), since the original
  ignores the plain question until a reset and nothing else finds it; never on a cadence, and
  not once anything answers (an ImageWriter's `ESC ?` there is the ImageWriter's answer, 3.15);
  the plain question while the port's printer does not answer, every 5 s (W-1116; 60 s on the
  StyleWriter branch), whatever the list holds and whatever answered last, so a StyleWriter
  plugged in is found in seconds by someone who never added it, at boot after the quiet
  power-on, when `ESC ?` went unanswered, and once per busy spell when a kick is wanted and
  none has been asked since the line went down; the CTRL-X kick last, only after a plain
  question since the line went down that got nothing, and never while the current record is a
  StyleWriter, so a StyleWriter that answers the plain question is never kicked (the
  StyleWriter branch's quiet ImageWriter record, nothing of its own and one kick a look while a
  StyleWriter answered on its port, went with the "changed" notice it waited for). An
  ESC ? that gets nothing with the line up is an original ImageWriter record's answer while
  one is listed, whatever record is current. The plain question asked for a line-up or a look
  goes with it: once the event ends any other way, the next line-up asks the plain question
  first again. A StyleWriter that answers is its record's, or makes one once it has answered
  twice (3.15), shared at once, named by its model; one that stops answering stays in the list,
  offline (but the original, below). A StyleWriter record that was answering and stops (its
  `1` and `2`, or a job's question, get nothing) is looked at once, once per stop: the plain
  question, then `ESC ?` if that got nothing, so an ImageWriter put in its place with its line
  already up (behind an A/B switch) is found within one status interval, and one whose line
  comes up while the StyleWriter record still answers is found by the status its line-up asks
  at once and the stop's look, in about 1.5 s. So the design's timings hold: a StyleWriter
  within 5 s, an ImageWriter at its line-up's `ESC ?` after the 2 s settle, whatever answered
  before; only a StyleWriter that raises no line, put in place of one that answers, waits for
  the next status check, within 10 s, whose `?` names it.
- **Several printers on the port** (`W-1116 Printer port: several printers, the one plugged in
  shows online`; design `docs/superpowers/specs/2026-10-30-port-printers-design.md`, which
  replaces the StyleWriter branch's swapped-port refusal and W-1088's "changed" notice). The
  printer list keeps a port record per model (3.15), the family that last answered on the port
  and the model it named (`pr_port_heard`, `heard_model`: `ESC ?` naming an ImageWriter, an
  original ImageWriter record's line up with nothing said, the plain question, the full
  sequence, and a record's own `1` and `2` or a job that printed, which keep the model of the
  printer on the port when it is of that family), and from them the printer on the port
  (`pr_port_current`: the record whose model answered last, else the oldest; `pr_port` is
  always the oldest, the record a board had before, which keeps `ipp/print`).
  `pr_port_other_family` says the port last answered as the other family, so one family's
  bytes never reach the other; `pr_job_gate` is what an ImageWriter job does after its own
  `ESC ?` and plain question: go when its own model answered (an original ImageWriter: its
  line up with nothing said), wait while nothing answers, stop when another model or a
  StyleWriter did. None of it is stored; a "changed" notice stored before is dropped on load.
  Built so far and host-tested: in `printers_core` the records, the twice-seen rule, the
  per-model uuid, the version 3 blob, `/api/printers` without `swapped`, what each port
  record's AirPrint printer says of itself (`pr_port_say`), the record the port's state
  belongs to (`pr_port_live_id`) and the Chooser name (`pr_chooser_name`); in `port_policy`
  the list-level inputs, the 5 s plain question, the person's look on a Something else port
  and the repeat of a first sighting (above); in `ipp_server` a slot per shared record, any
  mix, slot 0's legacy path. On the board: every shared port record is its own AirPrint
  printer in its slot (3.15 Sharing), the one on the port with the port's state and colour,
  the others offline with why; only that record carries the port's live state on the page;
  the Chooser name and the queue name are the oldest ImageWriter record's; a port job's end
  goes to the slot it came in on; the StyleWriter glue's `swapped` is
  `pr_port_other_family(PR_HEARD_STYLEWRITER)` (the port last answered as an ImageWriter),
  whichever record. The board fills the list-level inputs (`printers_port_listed`) and so
  asks every 5 s and in the list's order, but still gives the policy the oldest port record's
  kind, never sets `confirm`, keeps the oldest port record's kind as the port's mode
  (`printers_port_mode`, `s_port_sw`: the words and live state of the printer on the port are
  read as that family's until the mode follows it), and refuses the oldest record's AirPrint
  jobs (the job aborts), its test page (the button blips Busy and runs no cable test, the
  page's Print Test Page answers 409), a cleaning (409) and a Mac's job (busy) while the other
  family answered last (`printers_port_conflict`, on `pr_port_other_family`), as the
  StyleWriter branch did; that ends when the record's own family answers again, or when the
  person names the record a StyleWriter (an ImageWriter's answer before it is forgotten). The
  busy answer for a port printer that is not the one answering, the current record as the
  port's mode and the page follow in W-1116's later commits.
- **AirPrint.** 360 × 360 dpi, URF `RS360`; Letter, A4 and Legal, the first the default
  (`printer_media_t` in `printer_caps`: the page's width at 360 dpi names the entry, and the
  rip keeps the engine's printable area from `rip_left` for `rip_w` dots). Margins in whole
  1/100 mm rounded outward, on Letter 5.08 left, 7.62 right, 5.08 top and 8.05 bottom mm for
  the original, II, 1200 and 1500, and 5.08, 5.08, 3.18 and 6.99 for the 2200, 2400 and 2500.
  `printer-make-and-model` and the Bonjour `ty` are "Apple StyleWriter II" and so on, and
  `/api/status` `model` is that while the port's printer is a StyleWriter (else "Apple
  ImageWriter II"). `print-quality` 3, 4 and 5 are draft, normal and best, read from the Job
  group into the sink's options (`ipp_job_opts_t`). The icon is Apple's StyleWriter Chooser
  icon (LICENSE.md). Colour is advertised only while a colour cartridge was read in a colour
  model, under the same UUID, as the colour ribbon is (3.8): the cartridge lives where the
  ribbon does (`s_ribbon`, `ipp_server_set_colour`, `/api/status` `ribbon`), and a black-only
  model never offers colour. `/api/printers` shows it as `cartridge` (`black`, `colour`) once
  read, and `pr_live_t.cart` holds it, not the record.
- **Geometry.** Dots at 360 dpi; margins are from the sheet's edges. Rect x values are relative
  to the printable area's top-left corner, inclusive, little-endian u16, and on the II, 1200 and
  1500 both are 16 more for the same page. The printable width is the lesser of dots across and
  the paper's width less both side margins (Letter 3060: 2880 on the first family, 2916 on the
  second; A4 2976: 2832 on both).

  | | Dots across | Margins L/R, top, bottom | Band | Rect x offset | Block cap (data bytes) | `B` reads empty |
  |---|---|---|---|---|---|---|
  | Original | 2880 | 72/72, 72, 114 | 800 rows | 0 | 0x2000 | A0 |
  | II | 2880 | 72/72, 72, 114 | 800 | +16 | 0x76ED − planes × row bytes | F8 |
  | 1200 | 2880 | 72/72, 72, 114 | 800 | +16 | as the II | F8 |
  | Color 1500 | 2880 | 72/72, 72, 114 | 800 | +16 | as the II | 87 with a colour cartridge, 9F black |
  | Color 2200 | 2919 | 72/72, 45, 99 | 400 | 0 | 0x7E77 black, 0x79FA colour | `B & A0` = A0 |
  | Color 2400 | 2919 | 72/72, 45, 99 | 400 | 0 | as the 2200 | F8 |
  | Color 2500 | 2919 | 72/72, 45, 99 | 400 | 0 | as the 2200 | 80 |

  Free bytes are `B` × 256 − 2048 (the original `B` × 64). A block never exceeds the least an
  empty buffer can take less its 4 bytes of framing and one step of `B` (256 bytes, 64 on the
  original), so a printer whose `B` reads one step short of empty still takes a whole block: 30460
  data bytes on the 2500 and 8124 on the original, the table's cap on the rest. A block of
  0x8000 stalls a CS printer.
- **The page** (`sw_page`, the rip's last stage in the place the LaserWriter's PostScript stage
  has). `ripjob` gives it 360 dpi rows, one K plane or, for an sRGB page on a colour
  cartridge, four (C, M, Y, K); the stage drops the engine's top and bottom margins and codes
  rows as they arrive, holding no page and no band: the block being filled and the row before,
  a plane each (about 33 KB at most, the Color 1500's, through `bigmem.h`). A band starts at
  the first row with ink after the last band ended and runs the model's band height (less at
  the printable area's last row); blank rows between bands send nothing and the next rect's
  top moves on, while blank rows inside a band are coded. A rect is `R` (black) or `c` (four
  inks) and left, top, right, bottom; blocks are `G`, a little-endian u16 length, the data and
  `00`; `0C` ends the page. A row is run-coded per plane: the low 6 bits of a code are a count
  1–62 and the top bits 00 literal (that many bytes follow), 01 repeat the next byte, 10 white
  bytes, 11 black bytes; `80` alone is a white plane row. Each plane is XORed with the same
  plane of the row before within a band, the band's first row raw, the delta carrying across
  blocks within a band (TailTalk's reading; lpstyl gives each block its own rect, so this is not
  yet confirmed on a printer), and no delta at all on the original. Colour rows go C, M, Y, K. The stage
  writes frames, `[type][u16 LE length][payload]` (page start, rect, block, page end), into the
  printer-bytes half of the job buffer (the same 512 KB / 512 KB split as the ImageWriter's,
  3.8); the `prn` task hands them to `stylewriter_prn`, which reassembles frames across the
  ring's reads and runs the session, so the upload waits on the printer once the 512 KB fills.
  Nothing is ripped before the cartridge is known: the printer is reset and `H` read as the
  rip begins.
- **Screens and Apple's tables.** The record's screen at 360 dpi after the tone curve. A gray
  page or a black cartridge screens one plane; a colour page goes through `ink_split` and
  `ink_screens` into C, M, Y, K as 3.7 says for the ribbon. Apple's drivers carry the
  calibration of StyleWriter ink on paper: two tone curves (`GAMA` 1 with the ordered and mask
  screens, `GAMA` 2 with error diffusion, a pairing not yet confirmed by a print; the 2200
  driver's for the 2200, 2400 and 2500, the
  1500 driver's for the other four) and a 16 × 16 threshold screen (`HTSC` 1). The curve is the
  StyleWriter's default tone correction, in place of the dot-gain curve's 0, and `gain` still
  adjusts on top; the screen is a sixth beside the five, offered on StyleWriters only (`apple`,
  "Apple" in the Screen list). They are read out of Apple's driver files by our own script
  (`tools/stylewriter/mktables.py`; `untome.py` takes the files out of Apple's Installer
  packages) into `halftone/apple_tables.{h,c}`, whose header says where each table came from.
  The drivers are never committed, and nothing is copied from TailTalk's source.
- **The session** (`sw_session`, pure behind an ops table so the host tests run it against a
  scripted printer on a fake clock). A job: the reset, the settle, `1` and `2` clear, `?`
  (which must name the record's model, else the job fails with nothing further sent), then `D`
  (the colour models), or `N` (the 1200), `B`, `H` (all but the original: CS 0x81 colour and
  0x01 black, the 1500 bit 0x80), and the model's setup string for the quality and the
  cartridge. The Color 2200, 2400 and 2500 take `m2`, the quality (`d`; `b`, or `nZ` with a
  colour cartridge; `s`) and `A`, the 1500 `m2`, `d`/`Z`/`s`, `A`; the II and 1200 `u`,
  `d`/`n`/`b`, `D`, `A`; the original `nuA`. Plain paper only. Per page, the 2200, 2400 and 2500
  take their mode letter, `H` for a colour page on a colour cartridge and `B` otherwise, when
  it changes, and `L`; the 1200 and 1500 take `L`. Each rect goes out after `1` and `2` show no
  fault, each block when the tracked free space fits it (re-read every 100 ms while it does
  not; the setup and page letters, rects and `0C` are counted against it), a page's end after
  the last block; after the last page, wait for `1` to say the page is
  printed (not the original), then `E` (the 1200 adds `Y`) and `l`. Cancel stops at the next
  step (a block in flight goes out whole; a cut `G` block wedges the printer), then `E` (the
  black-only models) and the reset, and the next job starts with the reset anyway. A cartridge
  read as black for a job made for colour prints the page in gray (the stage is built from
  what `H` said) and the board turns IPP colour off before the next job.
- **Faults and silence.** `1` and `2` are read as `sw_state_t`. Named faults hold the job and
  are shown: no paper (`2` 04; "Needs attention: check paper", `media-empty-error`), paper jam
  (`2` 08, `media-jam-error`), cartridge missing (`2` 20, `marker-supply-missing-error`),
  cartridge jammed (`2` 01), ink out (`2` 02 on the 2200, 2400 and 2500,
  `marker-supply-empty-error`), door (`1` 40 on the same three, `door-open-error`) and waste ink
  (`1` 80 on them and the 1500, `marker-waste-full-error`); a jammed cartridge and a fault the
  design does not name report `other`. `S` goes every 5 s while one
  holds, and the job waits `CONFIG_AIRWRITER_STALL_ABORT` (600 s) as the ImageWriter's
  paper-out does. A fault the design does not name ends the job after 10 s. Bit 02 on the
  original, II, 1200 and 1500 is the printer holding the job and clears itself; bit 80 of `2` is
  spurious on the 2400 and 2500. Silence is 30 s with nothing the printer says heard (counted
  from its last answer, or from when the caller last gave the session something to do): the job
  fails, the state is offline ("check power and cable"), and the next job resets the printer. A
  printer switched off mid-page has lost the page.
- **Idle and status.** Idle, `1` and `2` every 10 s feed the page, the panel and IPP as the
  ImageWriter's line does: an answer is ready (or the fault it holds), none is offline. The
  original StyleWriter ignores the plain question until a reset and is switched off between
  uses, so its silence while idle says nothing (`sw_idle_silent`): once it has answered it
  reads as ready while quiet, takes jobs, and is asked no `1` and `2` while it is not
  answering; a session that gets nothing from it (the start-up reset, the person's look or
  pick, a job's full sequence, a job it goes silent during) shows it offline until it answers
  or the next session. Switched on, it answers only Look for Printers' reset or a job's, so the
  wiki's Offline row ends with pressing Look for Printers; the page and phone keep the one
  line "Offline: check power and cable", as for every StyleWriter. A StyleWriter that starts answering again is asked `D`
  and `H` for its cartridge, as a record that changes family is asked for its ribbon or
  cartridge. The panel
  shows the box's state, and the port's own while it prints.
- **Test page.** The button, DIP4 and the page's Print Test Page reach a port StyleWriter as
  they reach an ImageWriter; a second tap cancels. A StyleWriter has no fonts, so the page is a
  raster drawn on the board at 360 dpi, one Letter page, rows made as they are sent through the
  same stage: the letterhead (the icon and Apple Garamond as dots, `sw_art.h`), the board's
  details in a 5 × 7 face (the HD44780 shapes, `sw_font.h`, LICENSE.md), the inks (four solid
  swatches, the still life and a ramp of each ink with a colour cartridge; the still life in
  gray, a gray scale and a black ramp without), the six screens side by side with the one in use
  ticked, a ruler with a tick every 0.1 in and four one-inch squares. 88 KB of working memory
  in gray and 141 KB in colour, in PSRAM. `rip --sw-testpage colour|black --model M --out-sw
  F` makes it on the host; `swfake.py decode` draws it.
- **Clean Print Head.** One press is one cleaning: the job's reset and setup with a single `P`
  in it, wait for the printer to stop being busy (up to 600 s), then `E` (`Y` on the 1200) and
  `l`. The original has none. `POST /api/printers {"id":N,"clean":true}` answers 409 when a
  job holds the lock or another printer answered on the port, 502 when the model has no
  cleaning or the printer is off or holds a
  fault (it is asked `1` and `2` first, as a cleaning would wait on a fault with the lock held),
  and otherwise starts it on a task of its own under the job lock. Nothing cleans on its own.
- **Web.** The settings page draws a StyleWriter's row and detail (Cartridge, Black or Color;
  Print Head with Clean and the line "Cleaning uses ink."; the Halftone list with Apple, without
  the ImageWriter's 144 × 72 sample; "check paper" without SELECT). `/api/printers` adds
  `cartridge` and, while the printer holds one, `fault`: `paper`, `jam`, `cartridge`,
  `cartridge-jam`, `ink`, `cover`, `waste` or `other`. The model names in `add_port` and `model`
  are the seven.
- **The handshake pin.** Rev A's R9 (0 Ω, not fitted) joins U3's T1OUT to J2 pin 1 (HSKo); with
  it fitted the `HSKI_DRV` clamp (IO35 low) holds the line asserted at RS-232 levels, as a Mac's
  port does, and InkTalk holds its StyleWriter's input high for the same reason ("a StyleWriter
  won't send anything back otherwise"). Whether a StyleWriter answers with the pin floating is
  unknown; a rev A board that is to run one gets R9 fitted until an owner shows it is not
  needed (the wiki's StyleWriters page says how), and rev B fits it (`W-1091`; an ImageWriter
  ignores that pin, 2.4).
- **Not built.** Vintage Macs through AirWriter (phase 2: ADSP in `components/localtalk`, the
  adapter as `<name>:StyleWriter1200AT@*` or `<name>:ColorStyleWriter2400AT@*` on socket 129,
  busy as `0001`, `GET /api/sw/job`, a fast mode the port cannot follow refused or the Mac
  switches the printer over and the adapter drops off); the printers' fast modes (a II or 1200
  at 230,400 baud after `h`, likely within rev A's reach; a Colour StyleWriter's clock on HSKi
  after `x`, about 885 kHz on a 2200, which rev A's U3 cannot pass, 2.4, and the ESP32's UART
  cannot follow: `W-1102`'s to work out), so every page goes at 57600; 720 dpi on the 2500, other media and transparencies;
  StyleWriters behind Apple's network adapter found over AppleTalk.
- **Checks.** `make -C tools/rip test` (model table, run coder, page stage, session, status,
  test page, the board code with fakes, the port policy) and `make -C tools/ipp test`
  (the media list); `make -C tools/rip swcheck` (the stage's pages decoded by `swfake.py`, an
  independent decoder, and the same dots coded with lpstyl's coder, held to the dots the rip
  made); `make -C tools/rip swinterop` (the firmware's session built for the host,
  `build/swhost`, printing to `swfake.py` over a pty: all seven models, gray and colour, with
  each fault and a cancel); `make -C tools/rip swcaptures` (Apple's drivers' captures held to
  the session, skipped without them) with `swcaptures-selftest`. `build/swprint` is `swhost`
  for an owner to print a page to a real StyleWriter through a USB serial adapter and send
  the log back (`tools/stylewriter/README.md`).

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

**M6 — Manners.** Paper-out detection, job cancel, web config UI, Wi-Fi setup through the
board's own network, OTA. (Improv over BLE was dropped: it needs Web Bluetooth, which Safari
on iOS lacks.)

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
