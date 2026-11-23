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
| J2, J4 | **Kycon KMDGX-8S-BS** | THT | Mini-DIN-8 female, right-angle, fully shielded. Buy from Mouser, not Digi-Key, which is backordered. Footprint is custom — KiCad ships none — and was drawn from Kycon's drawing and verified against their STEP model. **Do not substitute Same Sky MD-80S/MD-80SM**, listed here in the original draft: their mounting is a rectangular-slot and earth-blade arrangement that does not fit this footprint. The maintainer solders them after delivery (J2 on every rev 2026.11 board, J4 on the full AirWriter); PCBWay does not. See `docs/sourcing.md` |
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

**Rev 2026.11 (W-1176)** gives both sockets the same circuit, so either is the printer port
or LocalTalk: the pins and the cable above hold for both, and the transceiver
behind each is a THVD1452 whose driver a GPIO or that socket's TashTalk enables
(`docs/hardware-rev-2026-11.md`, "Either socket, either job"; 3.19 for the firmware).

**HSKi carries a clock from a QuickDraw LaserWriter** (W-1102, §3.18). A Personal LaserWriter
300 drives 533 kHz on its handshake line whenever it is on, and takes the board's TxD one bit
per clock. Rev A's U3 (the original SP3232EEN, R9 open) delivers it to IO8 at a 49 to 51% duty
(board 1). TxD is clocked out by GP-SPI2 through the GPIO matrix, so the pin choices above stand.

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
  for a 1nF capacitor in parallel, so the shield strategy can be changed after testing (rev A,
  J2 only). Rev 2026.11 ties both sockets' shells straight to signal ground (W-1176): rev A's
  link was never lifted, and the two sockets are made the same.
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
| `net_provision` | Setup network (WPA2 with AirWriter's password, open without one; every DNS answer pointing at the setup page), NVS credential storage |
| `ipp_server` | IPP/1.1 over HTTP on port 631 |
| `ipp_attrs` | Printer attribute set, job attributes, state reasons |
| `urf_decode` | URF and PWG raster stream decoder |
| `halftone` | Grayscale → 1bpp dithering |
| `imagewriter` | Column transpose, escape code emission |
| `serial_out` | UART driver with hardware CTS, DTR watchdog |
| `webui` | Config page on port 80, and its sign-in (§3.20) |
| `auth` | The password: the chip's, the build's or one set on the page, and the sign-in's state (§3.20) |
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
- `Print-Job` (0x0002) — body arrives chunked, into a PSRAM buffer as fast as Wi-Fi
  delivers it: 1 MB on rev A, 6.5 MB on rev 2026.11 (3.19). An ImageWriter job holds 512 KB of URF in it, and the rip runs as the URF
  arrives into the other 512 KB, which holds the printer's bytes and drains to the port at
  9600 baud (W-1066): an iPhone Letter page is ~140 KB of URF and a full-page colour photo
  ~5.7 MB, but either rips to under 460 KB of ImageWriter bytes, while pixels would be 1.9 MB
  (gray) or 5.8 MB (colour). A LaserWriter job holds URF in all of it and rips at the
  printer's pace. The response goes back as soon as the upload is complete,
  with the job `processing`; the job ends `completed`, `aborted` or `canceled` when the
  paper stops, and holds the printer until then (W-1042). Reading the socket to the end is
  also what makes a dropped upload visible at once: without the buffer a paused printer
  stalls the upload behind a zero window, and the client's FIN waits behind its unsent
  data until paper returns. A document whose half outruns it (a LaserWriter's URF
  past the buffer, an ImageWriter job's printer bytes past 512 KB), or a board with no PSRAM (8 KB
  internal ring), falls back to streaming with TCP backpressure.
  A Print-Job that finds the printer taken (another phone's job, a Mac's on AppleTalk, the
  test page, an update) is answered `server-error-busy` at once, its body read to the end,
  and the client asks again (RFC 8011). It never waits with its document unread: received
  frames left unread hold internal RAM, and on the W-1076 bench a job that waited behind a
  Mac's page could not start its rip.
  A job to a port printer that is not the one on the port -- another port record's printer is
  (`pr_port_absent`, the port sink's `port_claim`, W-1116) -- is answered `server-error-busy`
  the same way, nothing held, so the printer that is there keeps printing and the phone keeps
  the job and asks again; it prints once the person swaps the cable (3.16).
- `Get-Job-Attributes` (0x0009) — iOS polls this for progress: the job's state, and the
  pages printed so far as `job-impressions-completed` and `job-media-sheets-completed` (3.5,
  W-1141)
- `Get-Jobs` (0x000A)
- `Cancel-Job` (0x0008). The test button stops a printing job too (W-1165,
  `ipp_server_cancel_printing`): the job ends `canceled` with `job-canceled-at-device`. macOS
  sends Cancel-Job when it reads that reason, and shows a refusal under the printer's name as
  "Unable to cancel print job.", so a Cancel-Job for a job stopped at the printer is answered
  `successful-ok` (`IPP_CANCEL_ALREADY`), not RFC 8011's `client-error-not-possible`. A second
  Cancel-Job for a job its client canceled is still refused.

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
printable area is 8 x 9.5in. On a rev A board the calibration page's lines landed exactly 1in
from the top and 0.5in from the bottom at -7/8in.*

**Media.** `na_letter_8.5x11in`, the default, and on an ImageWriter continuous fanfold. Not
borderless.

*Amended (`W-1173 Continuous paper on ImageWriters: a banner prints as one unbroken strip`):
the three ImageWriters list a roll after Letter (`printer_caps_t.roll`): `media-supported`
`roll_min_8.5x1in` and `roll_max_8.5x120in`, `media-ready` `roll_current_8.5x0in`, and in
`media-col-ready`, `media-col-database` and `media-size-supported` a `media-size` of
`x-dimension` 21590 and `y-dimension` the range 2540–304800 (1 in to 10 ft, Wells's limit),
margins 635 at the sides and 0 at the top and bottom. Letter stays first and the default. It
has to be in `media-col-ready`: iOS 26.5 (simulator, 2026-11-20) asked a roll-aware app for a
cut length (`printInteractionController(_:cutLengthFor:)`, handed 612 × 8640 pt, the range's
maximum) only then, and ignored a roll in `media-col-database` alone. Safari and Photos, which
don't ask, listed only US Letter and printed Letter pages; the print sheet reads "US Letter"
for a banner too, with the strip in its preview. A Mac turns the range into custom page sizes
(`*CustomPageSize`, CUPS `ppd-cache.c`) with each margin list's largest value as their margins,
so 1 in top and 0.5 in bottom; a queue added before keeps its PPD until removed and added again.
No UUID epoch bump: iOS asked for `media-col-ready` at every print sheet, and a new UUID would
orphan every Mac's queue. A job asks for the roll by its media-col's `y-dimension` (more than
1/8 in from Letter's), by a media-col `media-top-margin` of 0 (only the roll has none, so a
banner cut within 1/8 in of 11 in still asks), or by `media`/`PageSize` naming a roll or custom
size (`ipp_req`, `printer_caps_wants_roll`); how a roll page prints is §3.8.*

**`printer-icons`.** Three PNGs served on the IPP port itself,
`http://<host>.local:631/<set>-{48,128,512}.png`, smallest first as IPP Everywhere orders
them. A Mac fetches them once, when the printer is added, and keeps an `.icns` in
`/Library/Printers/Icons/` that the queue's PPD names (`*APPrinterIconPath`); a new icon
shows only after the printer is removed and added again. The PNGs are built into the
firmware by `tools/icon/mkicons.py` from 32 × 32 pixel grids: 128 and 512 are exact 4× and
16× enlargements, and 48 is the grid at 1× with a margin, since 1.5× would draw uneven
pixels. The artwork is Apple's, pixel for pixel from its drivers and LaserWriter 8 printer
descriptions (`LICENSE.md`), one set per printer body; models that share a body share a set.
`pr_icon` (`printers_core.c`) picks a port printer's set by model and a LaserWriter's by the
start of its PostScript product name, as Apple's printer descriptions spell them, with the
LaserWriter II's for any other. `ipp_icons.h` lists the sets. `/api/printers` names each
printer's set as `icon`, and the settings page draws the same grids at 32 px, inlined by
`tools/webui/mkheader.py`. A printer of no known kind has an empty `icon` and shows the page's
gray tile.

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

**Supply Levels (W-1137).** What a Mac shows under Options & Supplies, Supply Levels. When
the tab opens, macOS asks the printer by name for `marker-names`, `marker-colors`,
`marker-types`, `marker-levels`, `marker-low-levels` and `marker-high-levels`, and draws one row
per name. None of these printers measures its ribbon or ink, so a row says what is installed and
shows full: Color Ribbon (four stripes in one bar, the `#RRGGBB` values run together) or Black
Ribbon from the ImageWriter's self-ID; Black Ink, or Black, Cyan, Magenta and Yellow Ink, from a
StyleWriter's cartridge; Toner on a LaserWriter, its name carrying the `pagecount` the printer
last gave ("Toner, 10,291 Pages Printed"). A row at 10 (`toner-low-warning`) or 0
(`toner-empty-error`, `marker-supply-empty-error`) is only ever the printer's own report, so the
panel's warning triangle is true. Nothing known, no cartridge (`marker-supply-missing-error`) or
a printer showing as Offline sends no rows, and macOS says "Information Not Available". An
original ImageWriter never says its ribbon, and a Personal LaserWriter 300 its toner (§3.18), so
neither has a row.

The six lists go only when named, like `media-col-database`: in a full answer they would take a
Color StyleWriter past `RESP_MAX`. (`all`, `media-col-database` and all six in one request still
would, by a few bytes; no client asks for that.) `printer-supply-info-uri`, sent with at least one
row, is the settings page. On the bench
HP, macOS copied that attribute into the queue's PPD as `*APSupplies`, and a PPD with that line
shows the panel's More Info link, so a queue added before this has no link. Not yet seen on an
AirWriter queue added since.

Wi-Fi, firmware and page-count rows of their own were tried on board 1 and dropped: any bar in
that panel reads as how much is left, and a row can't leave its right side blank (`docs/traps.md`).

**Pages printed (W-1141).** `job-impressions-completed` and `job-media-sheets-completed`, the
same number since every printer here prints one side of a sheet, count the job's pages whose
last byte has gone to the printer. Until W-1141 both stayed 0 for the whole job, and an iPhone
showed "Printing 1 of 19" from start to end on every printer. A page counts when it leaves for
the printer, not when it is ripped: the rip runs up to the ring's 512 KB ahead of an ImageWriter
or a StyleWriter (3.4), many pages on a light job. So the rip notes how many of the job's
printer bytes it had written when each page ended (`jp_pages_t`, `job_pipe_rip.c`; 16 marks,
and a rip further ahead than that folds the newest pages into one mark, reported late, never
early), and the printer task reports a page once it has handed that many bytes on. An
ImageWriter's page is then in the UART's 128-byte FIFO and the printer's own buffer; a
StyleWriter's session has had the page's last band taken. A LaserWriter's stage sends its own
PostScript through `laser.c`'s 16 KB spool, so its page counts as the stage ends it, at most
16 KB before the spool has sent it and before the printer has imaged it: a job whose
connection dies with its last page still in the spool can end with that page counted. Once
the printer sends a PostScript error the count stops, since the printer reads the rest of the
job and throws it away (`laser_job_ps_error`). Any job on a board with no PSRAM counts as the
LaserWriter's does, its bytes going to the printer as they are ripped. A Personal LaserWriter
300's page counts once its session has printed it, in the stage's page end (3.18, W-1102); a
page too big for its memory, which the job skips and goes on past, does not count, and the
job's message names it. A job that prints ends with its whole page count; a failed or
canceled one keeps the pages that came out. Only the
printing job's count moves, and only up (`ipp_job_pages`), set by slot through
`ipp_server_job_pages_for`. Whether iOS's "Printing N of M" follows it is still to be seen on
a phone.

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

*Amended (W-1143): ripping a page was the slowest step of printing it (board 1, a Personal
LaserWriter 300's blank page: about 21 s, against the printer's 17). The board runs at 240 MHz
(`sdkconfig.defaults.esp32s3`; ESP-IDF's default is 160) and the rip's per-row code
(`urf_decode`, `resample`, `halftone`, `ripjob`, `imagewriter`, the StyleWriter's and the 300's
coders, `ps_page`) builds at `-O2`, the rest at ESP-IDF's `-Og`. The diffusion screens set up
once a row, carry two columns of error slack a side so no tap is checked, and skip a white
pixel with no error carried; the resampler passes a row through untouched when nothing scales.
None of it changes a dot. Each page logs where its time went (`page N: T ms: read, scale,
screen, second screen, out, end, lines`, ripjob.h; lines since W-1174) and each job how long
the rip waited for the upload; `rip --times` prints the same on the host. On board 1 with a Personal LaserWriter 300,
three 300 dpi text pages (251,477 bytes of codes each) went from 40 s a page to 24.7 s: the
rip's own work from about 22.6 s a page to 2.6 s (Atkinson 12.5 s to 1.3 s), leaving the line
at 533 kHz (about 7.5 s for such a page) and the printer's 17 s. The board draws at most 0.45 A
at 240 MHz (traps.md).*

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
like any other until a strip below it prints. A rev A board fed a white page at a line a
second when each blank strip was 1152 zeros at 9600 baud.*

*Amended (W-1064): an sRGB page with the colour ribbon in prints in four inks. Each pixel splits
into yellow, magenta, cyan and black (grey components go to black), each ink is dithered on
its own, and a strip prints one pass per ink it uses, in that order (yellow first, Technical
Reference p. 123): `ESC K n` (1 yellow, 2 magenta, 3 cyan, 0 black) when the ink changes,
`ESC G` with that pass's count trimmed to its last dot, the columns, CR with no LF. A strip
with no ink only adds its LF. The page ends on black. With a black ribbon an sRGB page is
turned to gray (Rec. 601 luma) and printed as above.*

*Amended (2026-11-20, `W-1178 ImageWriter: text and thin lines print crisp, faint hairlines
included`): an ImageWriter's page, gray or colour, goes through W-1174's text and lines
(`halftone/lineart.c`, §3.18) ahead of its screen, Atkinson still the default. On a colour page
the gray decides and only a gray pixel (R, G and B within 48) becomes black or paper, so
coloured text and icons keep their inks (`ripjob.c` `la_apply_rgb`, the RGB rows held ten rows
behind). A thin light line may be as light as 247 (`ripjob_cfg_t.lineart_light`, the 300 keeps
231): a row is 1/72 in, so a hairline averages to about 245 before it is seen. Chosen on board
1's ImageWriter II with the colour ribbon, against a control printed as exact dots through the
same ribbon and pins: an order slip's card outlines and dividers print, its gray text black and
sharp. Blue noise, which the 300 took, was rejected on screen: a 5% tint becomes a scatter of
big coloured dots and small text goes ragged, so a pale tint stays blank. `rip --no-lineart`
prints as before; the iOS and colour-as-gray fixtures moved, the checker didn't. The test page
draws its own rows (`testpage`) and is unchanged. A review on a different model found two
faults, fixed with tests: a colour page under ten rows paired its rows with colour rows before
the ring (now counted as they come back), and a pale line had to go on straight, so a slanted
one printed as nothing or dashes (now it may go on through a corner, on either side). The 300's
order slip prints the same dots as before W-1178.*

*Amended (2026-11-14, W-1140): the test page is the page every printer prints (`testpage`, the
test-page design's amendment block), drawn as rows at 144 × 72 and given to `iw_stage`, a rip
stage over these same strip writers, which writes byte for byte what ripjob writes for the same
rows and top margin. The Top margin setting moves the page as it moves a job's, but drops no
more than the sheet's blank first inch.*

*Amended (W-1173): a page on the continuous roll (§3.5) prints straight through. Its job
asked for the roll, or it is more than 1/8 in longer than Letter (a Mac's custom size names no
roll); either way only on an ImageWriter. The Top margin setting neither drops nor adds rows,
since a roll page has no margin: its first row prints where the head stands, about 1 in below
a fold after the job's opening form feed. Rows past 10 ft are dropped and logged ("page N is
longer than the roll's 8640 rows"). The page ends with its form feed as every page does, which
on fanfold feeds to the next fold past the strip, so a banner tears off at folds. A page the
same length as Letter or shorter, with no ask, prints byte for byte as before.*

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
0x420000, storage 1.9M (its first 256 KB is the log, 3.17). Three equal app slots, no factory
slot. Rev 2026.11 (`partitions-16mb.csv`, 16 MB): the same up to 0x20000, then ota_0, ota_1 and
ota_2 3M each at 0x20000, 0x320000 and 0x620000, storage 6.9M at 0x920000.

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

- *Settings*, in the app (W-1045). A tap (under 1 s) prints a test page, on release; while
  anything prints, a tap stops it (W-1165; the status-language design's test button). Held
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
  bootloader has no LED driver; the hook first clamps IO48, IO4 and J2's handshake drive, IO35
  on rev A, with J4's on rev 2026.11 (IO47 and IO13, told by its 16 MB flash, 3.19), so the hold
  does not leave them floating), then shows the boot wave, then the reset's confirmation.

IO14 is not a strapping pin (the S3's are IO0, IO3, IO45, IO46), and SW3 shorts it straight to
GND with no external pull-up or capacitor; the bootloader enables the internal pull-up before
it reads. A press already down when the app starts is the tail of the bootloader's hold, and
the app ignores it until it is let go. A build with Wi-Fi credentials in its `sdkconfig` rejoins
them after either reset; one without (every release) comes back in setup mode.
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
key that signed the *running* image, so `/update` and the GitHub path share one check. Signing
burns no eFuse and leaves the bootloader unchanged (a sold board's user block holds its password,
§3.20, which signing never reads). A board running an unsigned image accepts any valid
image once; after that only images signed with the same key. Changing the key takes a USB
flash. The release signing key is kept outside the repo, by the maintainer, with a backup;
its public half is `firmware/keys/airwriter-release.pub.pem`,
digest `6f11af9cd8eec9923985957400aa753ba034e7e057e18ebf61033cb4d327ad89`
(`CONFIG_AIRWRITER_UPDATE_KEY_DIGEST`). A build from someone else's key makes a board that takes
only their builds, and its web page says it can't install AirWriter releases.

**Releases.** A tag `vX.Y.Z` with an image per board (W-1109, 3.19): `airwriter-esp32s3.bin`
(rev A) and `airwriter-2026.11.bin`, each signed, and `airwriter.json`
(`{"version":"vX.Y.Z","esp32s3":"airwriter-esp32s3.bin","2026.11":"airwriter-2026.11.bin"}`;
a release made before W-1109 has rev A's alone). A board reads the key its build names
(`CONFIG_AIRWRITER_UPDATE_TARGET`: rev A keeps `esp32s3`, the key every earlier release used),
and an install by tag fetches `airwriter-<key>.bin`. Made only by `tools/release/release.sh`: it
builds both from the defaults alone in a throwaway worktree, checks each one's version, project,
key, bench options and signature and that no local Wi-Fi credential or purge-list string is in
it, then tags and publishes. The board reads
`github.com/<repo>/releases/latest/download/airwriter.json` (no API, no rate limit;
`CONFIG_AIRWRITER_UPDATE_REPO`, default `wr/airwriter`) and installs with `esp_https_ota`.
The manifest is a hint: the image's own version and project name are checked as it starts to
download, and its signature at the end. The project name is the board's (`airwriter`,
`airwriter-2026.11`), and a board refuses the other board's image, on download and on `/update`
("that firmware is for a different AirWriter board"): its PSRAM mode and pins would be wrong. An
upload refused that way has its slot's first sector erased, so nothing boots it. A release build's version is exactly `vX.Y.Z`;
anything else (`v0.1.0-3-gabc1234-dirty`) is a dev build, and a release is newer only if its
`vX.Y.Z` is past the running one's.

**No going back before sign-in** (W-1156, §3.20). A board with a password burned in (or a test
one) that runs `FWU_SIGNIN_FLOOR` (`fw_update_core.h`, the first release with sign-in) or later
refuses an older image, by tag and on `/update` ("This AirWriter can’t go back to a version
before vX.Y.Z"): older firmware would come up with an open setup network. A refused upload's slot
has its first sector erased, as the other board's does. A dev build numbered below the floor has
none, so a bench board moves between builds; a later version, and a USB flash, are never
limited. The release that first ships sign-in sets the floor to its own tag; `release.sh` runs
`tools/release/floor_check.py`, which refuses a tag below the floor and a floor at or below any
earlier tag without sign-in. A sold board is flashed only with a release at or past the floor,
never a dev build (a dev build below it would leave the board without one).

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
`{"tag":"vX.Y.Z"}`, which may be older, down to the floor above, or a prerelease),
`/api/update/auto` (`{"auto":true}`).

### 3.13 AppleTalk: the cable, Basilisk II and Infinite Mac

To a Mac, AirWriter is Apple's ImageWriter LocalTalk Option Card: the Chooser's AppleTalk
ImageWriter finds `<queue>:ImageWriter@*` by NBP and prints over PAP, and the job is the Mac
driver's own ImageWriter stream, which goes to the printer as it is. Four links reach that
printer (`components/localtalk`):

- **The cable** (`W-1051 LocalTalk node: show up in the Chooser as an ImageWriter`, `W-1053
  LocalTalk: print PAP jobs from the Mac's AppleTalk ImageWriter driver`): LocalTalk through
  TashTalk in U7 on J4, while the AppleTalk port is on. LLAP node acquisition by ENQ (TashTalk answers ENQ and
  RTS for the claimed ID), short DDP headers. On rev 2026.11 a cable is a socket doing
  LocalTalk (3.19): U7 on J4, U10 on J2, or both; `/api/lt` has `port` (1 or 2, 0 for none;
  the lower with both), which the page's LocalTalk card shows.
  `W-1186 Rev 2026.11: LocalTalk on both ports at once, so two LaserWriters each get a port`:
  two LocalTalk sockets are two networks, and nothing crosses between them but a job to a
  printer on the other (3.15's relay). Each socket has a node of its own (`localtalk.c`'s sides, as `ethertalk.c` keeps one per
  link), with its own node ID, kept in NVS `lt`/`node` for J4's and `lt`/`node_p1` for J2's, its
  own offers and its own link in the printer list (`AT_LINK_CABLE` J4's, `AT_LINK_CABLE_P1`
  J2's). One task and one lock serve both: the task waits a tick at most for a byte on either
  UART (each driver's receive interrupt wakes it) and never blocks inside a driver. A socket
  that takes the printer job parks its node: quiet, its UART driver deleted and the pins given
  back; `localtalk_start` brings it back on its TashTalk as it was. Each side's UART rings are
  4 KB in and 2 KB out, internal RAM (8 and 4 KB before): a frame is at most 605 bytes and
  arrives at the cable's 230.4 kbit/s, so 4 KB is 140 ms of it, and a PAP burst of 8 frames
  waits about 100 ms on a 2 KB ring that TashTalk's CTS drains at the same rate.
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

**Web.** `GET /api/lt` (the cable node: the lowest-numbered LocalTalk port's, as it was with
one; W-1186 adds `ports`, a row for each port doing LocalTalk, lowest first, with `port`,
`chip` (`lt_chip`'s words), `phase` (`online`, `acquiring`, `listening`, or `off` for a node
quiet or not running), `node`, `on`, `laser` and that network's `macs`; rev A has one row and
the LC none; the page's one LocalTalk switch is every port's, and with two rows the LocalTalk
card draws one per port: its lamp and Port N, then its chip's trouble, Joining, or its Macs), `GET /api/et` (both EtherTalk links, the shared
printer, internal RAM), `GET`/`POST /api/et/settings`
(`{"radio":true,"tunnel":true,"infinite":true,"zone":"…"}`; 400 for a zone name outside the rule
above). The page's EtherTalk card (W-1189) sets `radio` and `tunnel` together under one switch
and lists both links' Macs; Infinite Mac has a card and switch of its own.

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
  `W-1186 Rev 2026.11: LocalTalk on both ports at once, so two LaserWriters each get a port`:
  the lookups go out on both cables, and each is a link of its own, so the one PAP client
  rides whichever cable a job's printer is on (jobs stay one at a time under the job lock).
  Two LaserWriters often keep the name they shipped with: a target whose record names its
  cable is reached on that cable alone, never by the other cable's printer of its name. A name
  told from one cable while the same name answered on the other within 12 s (`LW_TWIN_MS`,
  two status rounds) is told as a twin, a second printer; one that turns up on the other
  cable after the first went quiet is the same printer, its cable moved. Not covered: both
  ports on one LocalTalk network, where a LaserWriter answers on both and is listed twice.
- **Switched on** (W-1098). A printer coming up checks no one has its name before it takes it,
  an NBP LkUp of that one name a few times about 2 s apart (Inside AppleTalk 7-9; a Personal
  LaserWriter NTR: five, after 8 s of LLAP ENQs reusing its node ID). Every link's node passes a
  LkUp of one name, no wildcard, to `lwc_name_check`; for a LaserWriter or ImageWriter not heard
  in the last 30 s on that link it puts off one look until 3 s after the last check. A printer
  switched on is found in seconds, new or renamed; nothing is sent while it is off (the NTR on
  the bench: ready 3 s after its last check). A check of a new name from the address another name
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
- **The AirPrint printer** (`ipp_server` printer 1-6, `laser/lw_caps.c`): `ipp/p<id>` (a board's
  W-1082 LaserWriter advertises `ipp/laser` and answers at both), its own Bonjour instance (its name in the list),
  `printer-uuid` `41575254-4c57-<CONFIG_AIRWRITER_LASER_UUID_EPOCH>-
  <hash of the NBP name>-<MAC>`, URF `RS300`, `W8`, only the paper it reported, margins from its
  imageable area rounded outward, the icon set for its product name (3.5). One job at a time across every printer.
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
- **Test page** (W-1140): the page every printer prints (`testpage`), drawn at 300 dpi on the
  printer's paper with its 8 in centred on the imageable area it reported (`lw_tp_geom`), and
  given to the PostScript stage a phone's page goes to (`laser_test_page`), in the record's
  screen and gain: about 375 KB binary, 760 KB hex. Under Printer, its product and PostScript
  version. It goes to any LaserWriter in the list, shared or not.
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

- **Two printer sockets** (W-1179, rev 2026.11; 3.19). Every rule below holds per socket: each
  has its own last answer, pending sighting, current record, Unknown printer and Something
  else (`pr_list_t.ports[]`, `pr_port_*(l, s, …)`). A record is a printer, not a socket: it
  carries `port`, the socket it was last seen on (the byte `pr_rec_t` always padded at offset
  171, stored as 0 by every firmware before it, so the stored form and `PR_BLOB_VER` are
  unchanged), read only where `n_ports` is 2. An answer naming a model whose record was last
  seen on the other socket is that record's when nothing shows itself on the other socket now
  (`pr_port_up`, from main's live state): the printer was moved, and keeps its record, name,
  uuid and AirPrint slot. When the other socket's printer is there and is that record, the
  answer is a second printer of the model, which gets a record of its own ("ImageWriter II 2")
  by R16. Add a Printer… makes a record on port 1 until its printer answers on one. The test
  page goes to port 1's printer, then port 2's, then as below. `/api/printers` gives each port
  record's `port` (1 or 2) and a printer heard on LocalTalk the LocalTalk socket's, on any
  board whose case names its ports (the LC too); with two sockets also `port2_on` and
  `port_unsupported_port`. Rev A's list and JSON are as before.
- **Records.** id (stable), where (port, or an AppleTalk link), kind (ImageWriter with model II,
  LQ or original; StyleWriter with one of its seven models, 3.16; Personal LaserWriter 300,
  3.18; LaserWriter; AppleTalk ImageWriter; unknown), name (from the printer: the
  port's model, an AppleTalk printer's NBP name; two the same get " 2"; renamed freely, UTF-8,
  MacRoman for NBP), uuid (made once and never changed, whatever the record's model becomes:
  a port record `41575254-4c57-<epoch>-<model><family>-<MAC>`, the model's number and `01`
  for an ImageWriter, `02` for a StyleWriter or `03` for a QuickDraw LaserWriter, since W-1116, each model's record its own
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
- **Two cables** (`W-1186 Rev 2026.11: LocalTalk on both ports at once, so two LaserWriters each
  get a port`). An AppleTalk record matches a printer heard by kind and NBP name. A record heard
  on a LocalTalk cable also keeps the port its cable is on, 1 or 2, in the record's `port` byte
  (the one a port record keeps its socket in; 0 until it is next heard, for a record from
  before, which stays online as it learns its port, rev A's too). `where` stays the cable for both ports and the blob's version does not change, so a
  firmware from before reads the list after a rollback and ignores the byte. A name heard on
  one cable that the list has on the other is a second printer when the first still answers
  (3.14's twin): a record of its own, with its own UUID (the name's hash with one byte more)
  and its own name on phones (" 2"). When the first has gone quiet it is that printer, its
  cable moved: the record takes the new port and keeps its id, UUID and settings. The Renamed
  match never crosses cables. The page's `port` for a cable record is its own (the lowest
  LocalTalk port's until it is heard); rev A's JSON has none, as before.
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
  A clock on HSKi is a QuickDraw LaserWriter (3.18): it is asked only its own questions, and
  nothing for an ImageWriter or a StyleWriter goes to it. "Something else" stops even the
  self-ID. W-1116 (3.16): the port keeps a record for every
  model that answers on it, one per model, and the one that answered last is the printer on
  the port. An answer naming a recorded model, where nothing else would change, is that
  record's, at once. One that would change the list (a model with no record, or one that
  would replace a "Something else" record or drop a found "Unknown printer" record, whether
  or not the model has a record, ruling R16) changes it only when it comes twice in a row
  (any other model's answer, or nothing said, in between starts it over; the line down does
  not), so a garbled reply never changes the list: it makes the model's record, shared if
  there is room; a found "Unknown
  printer" record becomes it (uuid kept), or goes with its notice when the model has a record
  already; a "Something else" record is replaced. "Add a Printer…" adds a record for a model
  not in the list (original ImageWriter, one that is off, a StyleWriter by model), beside the
  records there, or converts an "Unknown printer" or "Something else" record; a model already
  listed is refused, and so is a record's Model set to one another record has, or to
  Something else while another port record is one (one per port). The one on the port (the
  record that answered last) shows its state; every other port record is Offline. An
  ImageWriter record reads Ready only once `ESC ?` has named its model since the line came up
  (never from the line alone), so the page never shows the wrong ImageWriter for a moment. A
  line-up mid-job (a paper reload drops the line) keeps the name, and so does one while the
  link is dirty after a job cut short, when the idle `ESC ?` waits for a job's resync (3.8)
  (R17; the plain question still runs while the printer isn't answering). A line whose
  `ESC ?` answer was lost gets the 5 s plain question and then `ESC ?` when it got nothing,
  while it is up, whatever printer's record is on the port: after a restart that is the
  oldest port record until a printer answers, so an ImageWriter that misses its start-up `ESC ?` is still
  found (R20, W-1175, 3.16). Removing the record on the port forgets
  the port's last answer,
  and the port is asked again at once (R19). `/api/status` `model` is the record
  on the port's make and model, an ImageWriter's by its model. How long a change takes to
  show (a job or test page waiting for its ImageWriter takes the same 2 s settle after the
  line comes up, then the plain question and `ESC ?`, 3.16):

  | Printer | Plugged in or switched on, to Ready | Unplugged or switched off, to Offline |
  |---|---|---|
  | ImageWriter II or LQ | 2-3 s (line up, the 2 s settle, `ESC ?`) | 3 s |
  | Original ImageWriter | About 3 s (line up, the settle, `ESC ?` and the plain question unanswered) | 3 s |
  | StyleWriter II, 1200, Color 1500-2500 | Within 5 s | Within 10 s |
  | Original StyleWriter | On Look for Printers or at start-up | Reads Ready while quiet once found |
  | Personal LaserWriter 300 | Under 1 s from its clock | About 2 s |
  | LaserWriter on AppleTalk | In seconds | 30 s |

- **Sharing.** Identified printers are shared at once, up to six (`PR_SHARE_MAX`, W-1116; any
  mix of the port's records and AppleTalk printers, where before it was the port's and three
  on AppleTalk), each its own AirPrint printer in one of `IPP_PRINTERS` (7) slots: printer 0
  is the oldest port record's alone and stays empty while that record is not shared or there
  is none, printers 1-6 take every other shared record, so six fit either way (R14). Bonjour
  by name (`CONFIG_MDNS_MAX_SERVICES` 10 in `sdkconfig.defaults`: only a slot with a record
  registers, so at most six `_ipp`, printer 0's `_printer` stub and `_http`, 8 of the 10).
  Every record answers at `ipp/p<id>` for as long as it exists, port records
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
  AppleTalk, takes the lowest free slot of 1-6 and keeps it for its life there; a record that
  becomes the oldest moves to printer 0 under the same Bonjour name: printer 0 is cleared
  first (while a job holds printer 0 nothing moves until it ends), then the record leaves its
  old slot and takes printer 0; while a job holds its old slot it stays there and `ipp/print`
  answers nobody until that job ends. After any such move, and after one that waited for a
  job, every port slot is told its state again (`printers_cfg_t.placed`: a slot starts with
  none). A port record's slot carries the record's own sink, with
  its slot and id in the ctx as a LaserWriter's does, so a job's end goes back to the slot it
  came in on. What a port record's slot says (`pr_port_say`, `main.c`'s `port_push`, on every
  change of the port's state or the list): the printer on the port its own state, in its
  family's words (3.9's for an ImageWriter, 3.16's for a StyleWriter, 3.18's for a 300), and the colour the
  ribbon or cartridge last read offers (a black-only StyleWriter and an original ImageWriter
  none; a record that is not on the port keeps the colour it last offered, unless its model
  has none, until its slot is put again for another printer, which starts with none until it
  answers); every other port
  record `stopped` with the message "Offline", the one word the page shows too for every
  offline printer of any kind, whether nothing answers, another port record's printer is
  present or a Something else record is listed (2026-10-31: Offline is a status name
  alone; what to check, and why, are the wiki's Offline troubleshooting row's); only the reason keyword differs, `offline-report` while the port is
  another's (`PR_PORT_SAY_TAKEN`), the family's own with nothing answering; so phones see
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
  record's, else "AirWriter" (before W-1116, the one port record's). The test page names its
  own record (`printers_describe`), the one it was sent to.
  A Mac's ImageWriter job goes to whichever ImageWriter answers on the port (the II, LQ and
  original take the same stream). Registrations 1-3 are the AppleTalk printers from other
  links under their own NBP names and types (relayed), and `<host name>:AirWriter` is at
  registration 4 on every running link (`lt_node_offer`, sockets 0x85-0x88). Rev 2026.11's two
  cables are two links (W-1186), so a Mac on one is offered the LaserWriter on the other as an
  EtherTalk Mac is offered a cable's, and nothing else crosses between the two networks. A
  link takes a name once (`offer_names`, one pass in list order; `pr_offer_as`). A printer
  whose NBP name no other record of its kind has is offered under it. One that shares its
  NBP name is offered under its record's name, so Macs see the page's names wherever they are
  free, whatever order the list is in and whatever the person renamed. A name the link has
  already (a printer of that kind on the link itself, or an earlier offer) gets " 2" to " 9"
  after it, cut to fit as MacRoman characters, and never one that is another record's page
  name or NBP name. Two LaserWriters both named LaserWriter, one on each cable, are
  "LaserWriter" and "LaserWriter 2" on the page, on phones (Bonjour and IPP take the record's
  name, which the list keeps unique) and to EtherTalk Macs. A Mac on port 1 sees its own
  "LaserWriter" and port 2's as "LaserWriter 2". A Mac on port 2 sees its own as "LaserWriter"
  (the page's "LaserWriter 2", which answers there itself) and port 1's as "LaserWriter 3":
  on a cable, the plain name is always that cable's own printer. The offers carry each name's
  record id (`lt_offers_t.ids`), which the relay goes by, not the name. A Mac's
  connection on a relayed name goes through `localtalk/pap_relay.c`, run by `laser.c` with the
  one PAP client under the job lock: busy, in the printer's own status words, until the
  printer's connection is open; then both ways as they are, the printer's output 256 bytes a
  read.
- **Panel.** Idle, the box: ready while the printer on the port is, if its record is shared
  (`box_ready`: an ImageWriter once `ESC ?` named it), or any shared LaserWriter is; while the
  port prints, its own
  line (a StyleWriter has none: its answers to `1` and `2`); during a LaserWriter job, the
  LaserWriter's. The port's own state belongs to one record (`pr_port_live_id`, W-1116): the
  printer on the port, or the Something else record while one is listed; every other port
  record's live state is offline, on the page, to phones and in the box's state alike.
- **Test page.** The button and the page's Print Test Page (without an id) go to the
  printer on the port (`pr_port_current`, an ImageWriter, StyleWriter or Personal LaserWriter 300 record) while it is
  present, else the first shared printer that is ready, else that port record (the button then
  runs the cable test, as before), else the first shared printer (`pr_test_target`, W-1116);
  while a Something else record is listed no port record gets it. `POST /api/test {"id":N}`
  names one; for a port record that isn't on the port, or any port record while a Something
  else record is listed, it answers 409 "Connect <name> to the printer port first."
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
- **Reporting a problem** (`W-1154 Settings page: Report a Problem on every printer, and a line
  for printers not yet confirmed`). Every printer's detail has **Report a Problem…** beside Test
  Cable… and Print Test Page. Its sheet downloads the log (3.17) and opens
  `github.com/wr/airwriter/issues/new` with `title` and `body` filled in from what the page
  holds: the printer's model (`model` for a port printer, `product` for an AppleTalk one; never
  `name`, which the owner may have typed), its connection, `version` and the board, with blanks
  for what happened, what was expected and what printed it, and the log's file name. About 400
  characters of address. GitHub takes no file by address, so the person drags the log in. The
  page sends nothing itself. The log screen's Report an issue fills in the same way, without a
  printer. A printer the wiki's Supported Printers page marks **Yes\*** shows "Expected to work,
  but not yet confirmed on this model." under its status: every port ImageWriter, StyleWriter or
  QuickDraw LaserWriter and every AppleTalk LaserWriter except those whose model is in
  `CONFIRMED` in `tools/webui/app.js` (ImageWriter II, Color StyleWriter 2500, Personal
  LaserWriter 300, and the product "LaserWriter Personal NTR"; the 2500 since
  `W-1168 The Color StyleWriter 2500 is confirmed on paper`). Unknown printers and AppleTalk
  ImageWriters show no line.

### 3.16 StyleWriters (W-1103)

Design: `docs/superpowers/specs/2026-10-25-stylewriters-design.md`. Phase 1 is AirPrint to a
StyleWriter on the printer port: `components/stylewriter` (`sw_model`, `sw_code`, `sw_page`, `sw_session`,
`sw_status` and `sw_testpage` are pure and host-tested in `tools/rip`; `stylewriter.c` is the
board's), `components/printers/port_policy.c`, and `tools/stylewriter` (the software printer
and the owner's tool). **Built and host-tested against a software StyleWriter
(`tools/stylewriter/swfake.py`) and a second coder (lpstyl's); a Color StyleWriter 2500 prints on a
rev A board, in colour since 2026-11-07 with a new old stock colour cartridge (its black
cartridge is dry: on 2026-11-02 its first test page ran to its end blank), no other model has
printed from AirWriter, and Apple's drivers have not been captured** (the rig stops at a 68k Mac ROM,
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
  `00 FF FF FF 49` (eject and reset), a 2 s settle, a drain, then `?` again, and again a
  second at a time for 8 s before a second try with an 8 s settle (a 2500 waking from its
  sleep stayed dark when the second reset came 2 s after the first); it also finds the original (`IJ10`), which ignores `?` until reset (InkTalk's notes,
  from its author's own original; not yet seen here). Its `00` and `49` can read as stray
  characters at 9600, so it goes only to a printer a record or the person has named: before
  every job to a StyleWriter record, once when the person picks a StyleWriter (Add a Printer…,
  the unknown printer's notice, the detail's Model), to confirm it and read its model, and to
  any StyleWriter record once at start-up and on the person's Look for Printers when
  nothing else answered (2026-10-27, for the original; every model since 2026-11-02, when
  a Color StyleWriter 2500 answered nothing but a reset after a page cut short and while asleep). So the
  original is found only when named.
- **What the idle port is asked** (`port_policy`, pure: one act a call, every 500 ms, under the
  job lock and never while a job runs on the port; `port_tick`, `port_after` and
  `port_line_down` keep its books between calls, so the host tests run the whole loop against
  scripted printers for ten minutes, every record against every printer, and through scripts
  of swaps with the list kept as the board keeps it). Its inputs are the current port record's
  kind (`pr_port_current`, W-1116), what the list holds (any StyleWriter record, an original
  ImageWriter record, an original StyleWriter record: the order of the questions is the
  list's, not the current record's alone), the family that answered last, the line, and the
  events. In this order: an act the board wants repeated (`confirm`: a seen call answered
  `PR_SEEN_AGAIN`, one sighting of an answer that would change the list, and the same answer
  twice makes the change, R16), at once, before anything else; the person said Something
  else, nothing of
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
  line down; the full sequence once while a StyleWriter record is listed and nothing
  answers, at start-up (the board sets `full_due`; a look of the board's own goes first and
  leaves it owed) and when the person's Look for Printers got nothing from the plain question
  or `ESC ?` (or from the status, for one that was answering), since the original ignores the
  plain question until a reset and nothing else finds it, and a 2500 asleep or left with a page
  cut short answers nothing else either; to an original's record at once, to any other only
  once a plain question got nothing, so a StyleWriter that answers is never reset idle; never
  on a cadence, and
  not once anything answers (an ImageWriter's `ESC ?` there is the ImageWriter's answer, 3.15);
  the plain question while the port's printer does not answer, every 5 s (W-1116; 60 s on the
  StyleWriter branch), whatever the list holds and whatever answered last, so a StyleWriter
  plugged in is found in seconds by someone who never added it, at boot after the quiet
  power-on, when `ESC ?` went unanswered, and once per busy spell when a kick is wanted and
  none has been asked since the line went down; a line that `ESC ?` has not named since it
  came up, whatever printer's record is on the port (a lost answer, `iw_unnamed`: not on an
  Unknown printer or Something else record, not for an original ImageWriter record, not on a
  dirty link, not before the
  line-up's own question has run, and not while a StyleWriter answered last) gets the 5 s
  plain question and then `ESC ?` when it got nothing, while it is up and the record on the
  port does not answer (R20, widened by W-1175: a restart makes the oldest port record the
  current one, which may be a QuickDraw LaserWriter switched off); an idle self-ID that gets
  nothing is logged once until something answers or the line comes up again; the CTRL-X kick
  last, only after a plain
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
  comes up while the StyleWriter record still answers is found by the status its line-up edge
  asks at once (R11) and the stop's look, in about 1.5 s from the edge. The board raises that
  edge 2 s after the line itself comes up (the settle), so about 3 to 3.5 s from the line
  coming up. So the design's timings hold: a StyleWriter within 5 s, an ImageWriter 2 to 3 s
  after its line comes up (the settle, then `ESC ?`), whatever answered before; only a
  StyleWriter that raises no line, put in place of one that answers, waits for the next status
  check, within 10 s, whose `?` names it.
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
  One family's bytes never reach the other by what is on the port now and what a job's own
  questions got, never the list's last answer: a job, test page or cleaning for a record that
  is not the one on the port is refused (`pr_port_absent`), an ImageWriter job's own `ESC ?`
  and plain question gate its page (`pr_job_gate`: go when its own model answered -- an
  original ImageWriter: its line up with nothing said -- wait while nothing answers, stop when
  another model or a StyleWriter did), and the StyleWriter glue sends nothing while an
  ImageWriter is the printer on the port (`printers_port_on_is_imagewriter`).
  `pr_port_other_family` (the port last answered as the other family) stays in the core with
  its host tests, but nothing on the board reads it. None of it is stored; a "changed" notice
  stored before is dropped on load.
  Built so far and host-tested: in `printers_core` the records, the twice-seen rule, the
  per-model uuid, the version 3 blob, `/api/printers` without `swapped`, what each port
  record's AirPrint printer says of itself (`pr_port_say`), the record the port's state
  belongs to (`pr_port_live_id`) and the Chooser name (`pr_chooser_name`); in `port_policy`
  the list-level inputs, the 5 s plain question, the person's look on a Something else port
  and the repeat of a first sighting (above); in `ipp_server` a slot per shared record, any
  mix, slot 0's legacy path. On the board: every shared port record is its own AirPrint
  printer in its slot (3.15 Sharing), the one on the port with the port's state and colour,
  the others "Offline", never why; only that record carries the port's live state on the page;
  the Chooser name and the queue name are the oldest ImageWriter record's; a port job's end
  goes to the slot it came in on; the StyleWriter glue's `swapped` is an ImageWriter being the
  printer on the port now (`printers_port_on_is_imagewriter`: its record current and present),
  whichever record. The current record is the port's mode and state: `printers_port_mode`,
  `s_port_sw` and the live state, model, screen, sharing and `/api/status` `model` follow
  `pr_port_current`, so the page reads the printer that answered last (a job or test page
  takes its family, model and screen from its own record), and a Something else record in the
  list silences the port whatever else is there. An
  ImageWriter record reads Ready only once `ESC ?` has named its model since the line came up
  (`s_iw_named`), never from the line alone, so the page never shows the wrong ImageWriter for
  a moment, and Ready appears with the answer, about 2 to 3 s after the line comes up. The
  board sets `confirm` to repeat a first sighting, asks every 5 s and in the list's order, and
  gives the policy the current record's kind; a person's model change on the port forgets the
  port's last answer (R10) and the port is asked again at once; the person's pick of a
  StyleWriter is confirmed on the record they named (`pr_port_sw_found` by id). Jobs, the test
  page and cleaning follow the record they are for, not the oldest and not the printer on the
  port (W-1116's job commit): a job for a port record while another port record's printer is
  on the port is answered busy (`port_claim` on `pr_port_absent`, 3.4), and the page's Print
  Test Page and Clean Print Head for it answer 409 "Connect <name> to the printer port first."
  (only the page can name such a record: the button's target, `pr_test_target`, is
  the printer on the port, a ready shared printer, or the port's record for the cable test, as
  before). A job or test page takes its family, model and screen from its own record
  (`printers_record_is_sw`, `printers_record_model`, `printers_record_screen`), not the port's.
  An ImageWriter job that starts on a quiet port sends no page byte until its own model has
  answered `ESC ?` (`colour_ok` on `pr_job_gate`), and the gate reads only what this job's own
  `ESC ?` and plain questions got (`s_job_heard`, reset as its wait begins), never the list's
  last answer: a StyleWriter that answered before the job and was unplugged, or an LQ that
  answered before a II job, neither ends the job nor passes it. The wait is for the job's
  record's line whatever record is on the port now, to one stall limit in all
  (`CONFIG_AIRWRITER_STALL_ABORT`, the questions' time counted), asking the plain question every
  5 s with the line down. A line that comes up while the job waits gets the idle line-up's
  2 s settle (W-1064) before any question, so a II switched on mid-wait answers its first
  `ESC ?` and is not taken for an original ImageWriter's silence: 2 s from when the job's own
  wait saw the line up (the wait and serial_out's line task each poll the pin, and either may
  see the edge first), or to the idle settle's end if that is later (a line-up just before
  the job), never more than 2.5 s; a cancel or a cut upload ends it, and a line that drops
  inside it is waited for again. Whenever the line is up the plain question goes first, and a
  StyleWriter answering it is the job's answer with no ImageWriter byte sent (not the resync,
  not `ESC ?`: one that raised its line and heard `ESC ?` first would stop listening until a
  reset); only after it got nothing do the resync and `ESC ?` go, again after a 500 ms rest
  while the model is not named yet. A cancel or an upload cut short ends it as that, never as
  "another printer". An original ImageWriter record's answer is its line up with `ESC ?` and
  the plain question unanswered: the core names that record, and the job takes it as its own
  answer, so an original's job goes and a II's or LQ's on such a port ends as another
  printer's. Another ImageWriter model or a StyleWriter answering the job's own questions ends
  it aborted ("Another printer was connected. Print again.") before any byte, since its page
  was ripped for the model it was sent to. The test page (`test_page_task`) waits and re-asks
  the same way (`job_gate_wait`) and prints only on GO; at the deadline or a second tap it
  stops without a byte. A Mac's ImageWriter stream goes to whichever ImageWriter answers, and
  hears busy while the printer on the port is a StyleWriter now (`s_port_sw`), a flip is
  pending, or a StyleWriter's first answer awaits its repeat (`s_port_confirm` with the list's
  last answer a StyleWriter's: the port task asks again within 500 ms and has let the lock go
  meanwhile), not on the list's last answer alone, so the driver's bytes never reach a
  StyleWriter. The StyleWriter glue's `swapped` is an ImageWriter being the printer on the
  port now (`printers_port_on_is_imagewriter`: its record current and present), not the list's
  last answer, so a StyleWriter's job, test page or cleaning on a quiet port runs after an
  ImageWriter was unplugged; the session's own `?` still refuses the wrong printer.
- **AirPrint.** 360 × 360 dpi, URF `RS360`; Letter, A4 and Legal, the first the default
  (`printer_media_t` in `printer_caps`: the page's width at 360 dpi names the entry, and the
  rip keeps the engine's printable area from `rip_left` for `rip_w` dots). Margins in whole
  1/100 mm rounded outward, on Letter 5.08 left, 7.62 right, 5.08 top and 8.05 bottom mm for
  the original, II, 1200 and 1500, and 5.08, 5.08, 3.18 and 6.99 for the 2200, 2400 and 2500.
  `printer-make-and-model` and the Bonjour `ty` are "Apple StyleWriter II" and so on, and
  `/api/status` `model` is that while the port's printer is a StyleWriter (else the
  ImageWriter record's own, "Apple ImageWriter II", "Apple ImageWriter LQ" or "Apple
  ImageWriter", W-1116; the II with no ImageWriter record). `print-quality` 3, 4 and 5 are
  draft, normal and best, read from the Job
  group into the sink's options (`ipp_job_opts_t`). The icon is Apple's for its model (3.5,
  LICENSE.md). Colour is advertised only while a colour cartridge was read in a colour
  model, under the same UUID, as the colour ribbon is (3.8): the cartridge lives where the
  ribbon does (`s_ribbon`, `ipp_server_set_colour`, `/api/status` `ribbon`), and a black-only
  model never offers colour. `/api/printers` shows it as `cartridge` (`black`, `colour`) once
  read, and `pr_live_t.cart` holds it, not the record.
- **Geometry.** Dots at 360 dpi; margins are from the sheet's edges. Rect x values are relative
  to the printable area's top-left corner, inclusive, little-endian u16, and on the II, 1200 and
  1500 both are 16 more for the same page. A rect's right edge is the last dot of its rows'
  whole bytes, into the printable width's last byte where the width is not whole bytes (Letter
  and Legal on the 2200, 2400 and 2500: 2920 dots over 2916, the extra dots white): the 2500
  counts a row's bytes from the rect's width rounded down, and a 2916-dot rect over 365-byte
  rows stopped it on the first row with `2` 80 (a Color StyleWriter 2500, 2026-11-02; `docs/traps.md`). The
  printable width is the lesser of dots across and
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
- **Screens and Apple's tables.** The record's screen at 360 dpi after the tone curve, blue
  noise by default (W-1177, below). A gray page or a black cartridge screens one plane; a
  colour page goes through `ink_screens` into C, M, Y, K with the inkjet's own split
  (`ink_screens_set_inkjet`, `ripjob_cfg_t.inkjet`), not the ribbon's (3.7): black takes the
  gray the three colour inks share where the colour is near gray, all of it with the three equal
  and none once they differ by 96 of 255 (`INKJET_CHROMA`), and more again in a dark colour, none
  with the least of the three at 96 (`INKJET_DEEP`, 38% dark) and all of it at 255; the colour
  inks print what lies above that gray at its full strength, as if the colour were no darker
  than its hue, less so the nearer gray. So a strong colour lighter than 38% has no black dots
  in it, and a navy prints in cyan and magenta with a little black. Apple's drivers carry the
  calibration of StyleWriter ink on paper: two tone curves (`GAMA` 1 with the ordered screens,
  `GAMA` 2 with error diffusion and blue noise; the 2200
  driver's for the 2200, 2400 and 2500, the
  1500 driver's for the other four) and a 16 × 16 threshold screen (`HTSC` 1). The curve is the
  StyleWriter's default tone correction, in place of the dot-gain curve's 0, and `gain` still
  adjusts on top; the screen is a sixth beside the five, offered on StyleWriters only (`apple`,
  "Apple" in the Screen list). They are read out of Apple's driver files by our own script
  (`tools/stylewriter/mktables.py`; `untome.py` takes the files out of Apple's Installer
  packages) into `halftone/apple_tables.{h,c}`, whose header says where each table came from.
  The drivers are never committed, and nothing is copied from TailTalk's source.
- **Pale tints, thin lines and clean colours** (`W-1177 Color StyleWriter: colours print clean
  and bright, with black only for grays`, 2026-11-20). An order slip from a Mac, on board 1's
  Color StyleWriter 2500 in Atkinson, lost its 5% panel, its gray tile, its 16% card outlines and
  dividers, and printed its gray text faint: Atkinson prints no dot under about 12% ink. Blue
  noise brought them back but printed the photo and logo too dark on `GAMA` 1, made for Apple's
  pattern screen; on `GAMA` 2, Apple's curve for scattered dots, it printed about as light as
  Atkinson. Blue noise also showed what Atkinson had hidden: the ribbon's split, which puts
  black under every colour short of full strength, spotted the logo's red and blue with black
  and turned the photo's royal blue to a muted navy. Wells chose on paper from four splits
  printed side by side as exact dots (the ribbon's; black for grays only; that deepened;
  deepened further) and took the third. A review then found it printed a dark colour light: a
  navy came out mid-blue. The split above puts the colour at full strength and adds black from
  38% dark. Wells chose it on paper from three starting points for that black (50%, 38% and 25%
  dark), printed as exact dots on a colour chart, the slip, three photos and six very dark
  colours: on the photos the three looked the same, and only very dark, saturated colours told
  them apart, the earliest black the deepest and the most spotted with black. A fourth, the
  colour inks keeping the gray black leaves them, kept those colours dark but turned a dark blue
  teal and puts more ink on the page; it was not chosen. A very dark colour still prints lighter
  than it is: a chocolate brown as a deep orange, Oxford blue as a strong blue. Blue noise
  is the StyleWriter's default: a record that becomes a StyleWriter on Atkinson takes it, one
  that stops being a StyleWriter or QuickDraw LaserWriter goes back from it to Atkinson
  (`default_screen`), and a record still on Atkinson moved to it once at start-up (`w1177` in NVS
  `printers`), so a screen picked since stays, as the 300's did (3.18). The test page's blue
  noise patch takes `GAMA` 2 with the jobs; the rest of the page and the ImageWriter's split are
  unchanged. Wells approved the slip printed through board 1 with this split (2026-11-21). Not
  yet printed from a Mac: the slip was converted to a page on the host and sent over IPP.
- **The session** (`sw_session`, pure behind an ops table so the host tests run it against a
  scripted printer on a fake clock). A job: the reset, the settle, `1` and `2` clear (up to
  three resets, each given `1` twice, 5 s each, the second and third with the long settle, so a
  printer that answers none fails the job after about 53 s), `?`
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
  spurious on the 2400 and 2500 (lpstyl's testers), but it is also how a 2500 stops on a page it
  cannot read: with `1` not busy and the page not draining (waiting for room, or for the page
  to finish with the buffer still holding it), 80 on the 2400 and 2500 ends the page after the
  same 10 s, and the abort resets the printer (`docs/traps.md`). Silence is 30 s with nothing the printer says heard (counted
  from its last answer, or from when the caller last gave the session something to do): the job
  fails, the state is offline, and the next job resets the printer. A
  printer switched off mid-page has lost the page.
- **Idle and status.** Idle, `1` and `2` every 10 s feed the page, the panel and IPP as the
  ImageWriter's line does: an answer is ready (or the fault it holds), none is offline. Each
  check also asks the plain question (`?`, and `p` after `CS`), so a different StyleWriter put
  in its place shows within one check; a StyleWriter plugged in on a quiet port is found within
  5 s by the plain question's cadence, and reads Offline within 10 s of being switched off (the
  timings table in 3.15). The
  original StyleWriter ignores the plain question until a reset and is switched off between
  uses, so its silence while idle says nothing (`sw_idle_silent`): once it has answered it
  reads as ready while quiet, takes jobs, and is asked no `1` and `2` while it is not
  answering; a session that gets nothing from it (the start-up reset, the person's look or
  pick, a job's full sequence, a job it goes silent during) shows it offline until it answers
  or the next session. Switched on, it answers only Look for Printers' reset or a job's, so the
  wiki's Offline row ends with pressing Look for Printers; the page and phone read "Offline",
  as for every printer. The one Color StyleWriter 2500 benched goes dark on its own about 5 minutes
  after a job's end (305, 317 and about 315 s after its `E l`) and then answers nothing, not
  `?` nor `1`, until a reset wakes it; left alone after a reset with no job it stayed awake past
  11 minutes, and a job's reset woke it dark and printed. So a 2200, 2400 or 2500 that was sent
  a job's end (the session showing ready, `s_sw_asleep`) and has not been reset since reads as
  ready while it is silent (`sw_idle_silent`), as the original does, and the next job wakes it;
  a start-up or Look for Printers reset that gets an answer ends the sleep (it stays awake
  after), and one that gets nothing, or a job that gets nothing, shows it offline
  (2026-11-02: ready and woken, as Apple's driver leaves it, over dropping `l` to keep it
  awake). One switched off at its button reads ready too, until a job finds it off. The 2200
  and 2400 are taken to sleep as the 2500 does, by family. After the reset that ends a failed
  page it has gone dark at once, two resets 2 s or 7 s apart have left it dark where a single
  one woke it, and once a job's reset darkened it while it was awake and a later reset brought
  it straight back (`docs/traps.md`). A StyleWriter that starts answering again is asked `D`
  and `H` for its cartridge, as a record that changes family is asked for its ribbon or
  cartridge. The panel
  shows the box's state, and the port's own while it prints.
- **Test page.** The button and the page's Print Test Page reach a port StyleWriter as
  they reach an ImageWriter; a second tap cancels. The page is the one every printer prints
  (`testpage`, W-1140; the test-page design's amendment block), drawn on the board at 360 dpi
  for the model's printable width and the cartridge the session reads (`sw_tp_geom`), its rows
  made as they are sent through the same stage a job's go through, each screen with the model's
  tone curve and ink placement: the model's icon and the letterhead, the board's details in a
  5 × 7 face (the HD44780 shapes, `tp_face.h`, LICENSE.md), the type tester, the inks (with a
  colour cartridge, seven solid swatches across the page, the four inks and the three they make
  over each other (W-1134), then a ramp of each ink, two by two; without, eleven gray steps and
  a black ramp), the six screens with the one in use ticked, a ruler with a tick every 0.1 in and
  four one-inch squares. Nothing on it is traced or set on the board: the page is drawn and sent
  from one task with nothing between them, so tracing there kept the printer waiting partway down
  the page (W-1133). `tools/rip/mkscene.c` traces the screens' still life once with `tp_scene.c`
  (400 × 300 gray through `ink_gray`) into `tp_pictures.h`, 12 KB; the type tester is 43 KB
  (`tp_type.h`); `tp_unpack.c` reads both a row at a time, with the ROM's tinfl on the board (its
  state and 32 KB window, 44 KB, held only while a picture's rows are made) and zlib on the host.
  Over a page it asks PSRAM for about 125 KB in gray and 154 KB in colour (`test_testpage.c`'s
  count), each part's freed after its last row.
  `rip --sw-testpage colour|black --model M --out-sw F` makes it on the host; `swfake.py decode`
  draws it.
- **Clean Print Head.** One press is one cleaning: the job's reset and setup with a single `P` in
  it, wait for `1` to say the cleaning is over (up to 600 s), then `E` (`Y` on the 1200) and `l`.
  Over is busy (01) gone off after reading on, or page done (08) come on after reading off,
  whichever comes first: the 2500 reads busy throughout and its 08 is off from the reset until the
  cleaning ends; a printer that reads busy in none of the first 5 s has cleaned. The original has
  none. `POST /api/printers {"id":N,"clean":true}` answers 409 "Connect <name> to the printer port
  first." when this StyleWriter is not the printer on the port -- another port record's printer is
  (`pr_port_absent`, W-1116) -- 409 when a job holds the lock, 502 when the model has no cleaning
  or the printer is off or holds a fault (it is asked `1` and `2` first, as a cleaning would wait
  on a fault with the lock held), and otherwise starts it on a task of its own under the job lock.
  Nothing cleans on its own.
- **Web.** The settings page draws a StyleWriter's row and detail (Cartridge, Black or Color;
  Print Head with Clean and the line "Cleaning uses ink."; the Halftone list with Apple, without
  the ImageWriter's 144 × 72 sample; "check paper" without SELECT). `/api/printers` adds
  `cartridge` and, while the printer holds one, `fault`: `paper`, `jam`, `cartridge`,
  `cartridge-jam`, `ink`, `cover`, `waste` or `other`. Its `state` is `cleaning` from the press
  to the cleaning's end (W-1132), where it would be `printing`, and the page says Cleaning with
  Printing's lamp; a fault meanwhile is still `attention`. The panel shows the printing light
  and the phone sees no change. The model names in `add_port` and `model` are the seven.
- **The handshake pin.** Rev A's R9 (0 Ω, not fitted) joins U3's T1OUT to J2 pin 1 (HSKo); with
  it fitted the `HSKI_DRV` clamp (IO35 low) holds the line asserted at RS-232 levels, as a Mac's
  port does, and InkTalk holds its StyleWriter's input high for the same reason ("a StyleWriter
  won't send anything back otherwise"). A Color StyleWriter 2500 answers and prints with the
  pin floating: on a rev A board with R9 taken off again (2026-11-03) it woke at the start-up reset
  and ran the test page to its end, as fast as with R9 fitted. Whether the other models need it
  is not known (InkTalk's remark is about some StyleWriter, not named), so a rev A board that is
  to run one other than a 2500 gets R9 fitted until an owner shows it is not needed (the wiki's
  StyleWriters page says how), and rev B fits it (`W-1091`; an ImageWriter
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

### 3.17 The log (W-1136)

`W-1136 Settings page: a technical log that survives crashes and being unplugged`. Design:
`docs/superpowers/specs/2026-11-08-log-design.md`. `logbook_core.c` is the pure core, host-tested
in `tools/rip/test/test_logbook.c`; `logbook.c` is the ESP-IDF side.

**What goes in.** Every `ESP_LOG` line at info and above, through `esp_log_set_vprintf` and on to
the console unchanged. The hook runs on whichever task logged. It formats into PSRAM under a lock
it waits one tick for; if it can't take the lock or can't yield, it drops the line and counts it.
The Wi-Fi library sends its prefix, message and newline as separate calls, so `lb_feed` joins them
per task. Each start writes a first line at level `S`, `reset=<reason> version=<v>`, with a run
number. A panic goes through a wrap of `esp_panic_handler` (`-Wl,--wrap`): the reason, task, core
and backtrace addresses wait in RTC memory and become one `E sys` line on the next start.

**What's left out.** A name somebody typed or chose, or anything that could locate them, so that a
log can be posted. No log statement carries a document, person or custom printer name, and a check
in `make -C tools/rip test` holds the firmware to that. `lb_scrub` replaces the saved network and
host names in every line with `<network>` and `<host>`, and anything shaped like a MAC address in
lines tagged `wifi` with `<mac>`, before the line is stored.

**Two layers.** A 40 KB ring in PSRAM, in the noinit segment with a magic number and a check on
each boot, survives a panic and a software restart. The first 256 KB of `storage` (64 sectors of
4 KB, about 3,000 lines), raw `esp_partition`, survives a power cut. `logbook_tick` runs once a
second on the main task. With no job it keeps two sectors erased ahead and saves what's new. During
a job (`job_busy`: the job lock held, or a job within the last 5 s) it saves only when a warning or
error is waiting, a page write with no erase. Flash calls come only from tasks with internal
stacks. Erase All Settings and the power-up reset clear both layers.

**Routine status polls** (`lb_is_chatter`: `ipp`'s `op … asks` and `attributes of …` lines for
Get-Printer-Attributes, Get-Job-Attributes and Get-Jobs) stay in RAM and never reach flash. The ring
keeps them in a part of their own: 32 KB (about 400 lines) for every other line, 8 KB for the polls,
so polls push out only older polls. 8 KB is about 34 polls, half a minute of a Mac that asks every
second while it has a job queued. In one shared ring that asking pushed a job's first lines out
before the job ended and they could be saved (`W-1142 Log: a Mac's status polls push a job's own
lines out before they're saved`; board 1, 2026-11-13: a Personal LaserWriter 300 job with its cover
opened mid-page left its end in the log and not its start). Both parts number their lines from one
sequence, and a reader merges them with flash.

**Reading.** `GET /api/log`: `?n=N` for the newest N, `?after=S` for the lines after sequence
number S, neither for everything. Plain text: a header `#\t<newest>\t<errors this start>\t<dropped>\n`,
then `<seq>\t<run>\t<ms>\t<level>\t<tag>\t<text>\n`. A request decides once, when it begins, whether
it may read flash: not during a job. The job lock is held for moments with nothing printing, so
asking again partway changes the answer. Flash also stays out of it when the ring holds every line
wanted, which is every poll of an open screen. `/api/status` carries `log_errors`, the count for
this start.

**The screen.** **Log** on About, at `/about/log`. Newest line first, each start under a divider,
at most 500 lines, asking for new lines every second, or every 3 s during a print: a held-open
response would tie up the web server, which answers one request at a time. Times are `+MM:SS` or
`+H:MM:SS` since that start, because the board has no clock. The current start's divider shows when
it began, from the viewer's clock, on screen only. **Copy** (the newest 300 under the filter) and
**Download** (everything) are oldest first, with a header. The header lists each printer by model
(never its name), connection and status, and an AppleTalk printer's own status line when it says
more than the status (W-1154; `lwStatus` turns a busy line naming a job into "Busy"). A printer's
Report a Problem… downloads the same file (3.15).

**Bench (rev A, board 1).** A NULL dereference and an abort each left their line with earlier lines
intact. RAM wiped and restarted lost no lines, and neither did unplugging an idle board. A
StyleWriter page printed normally with the screen open and a forced flash save each second, with
`ipp0` at 340 bytes of stack spare, about 80 of them the hook's. Not run: a LaserWriter or LocalTalk
job with the screen open, or the ImageWriter checker on this build.

---

### 3.18 QuickDraw LaserWriters (W-1102, W-1144, W-1145, W-1146)

`W-1102 QuickDraw LaserWriters on the printer port (Personal LaserWriter LS/300, Select 300)`;
`W-1144 Personal LaserWriter 300: load the next page while one prints, so multi-page jobs finish sooner`;
`W-1145 Personal LaserWriter LS on the printer port`;
`W-1146 LaserWriter Select 300 on the printer port`.
Design: `docs/superpowers/specs/2026-11-12-quickdraw-laserwriters-design.md`, which holds the
command table and the measurements this section stands on. `components/qdlaser`: `qd_model`,
`qd_wire`, `qd_status`, `qd_page`, `qd_session` and `qd_testpage` are pure and host-tested in
`tools/rip`; `qd_link.c` (the SPI slave) and `qdlaser.c` (the port's questions and the job
pipeline's hooks) are the board's. `tools/qdlaser` holds `qdfake.py`, a software Personal
LaserWriter 300 (and LS, and Select 300) and page decoder, and `mkqdtables.py`, which reads
Apple's 300 screen, curve and the PGRD table out of Apple's LaserWriter 300 driver 1.1 into
`apple_tables_lw.{h,c}`, and the LS's icon out of its own driver. The 300 shows its body's
icon, the LaserWriter 4/600 PS's (`lw4600`, W-1151), not its driver's, which the driver also
gives the Select 300.
**A Personal LaserWriter 300 prints from AirWriter on a rev A board (board 1, 2026-11-12 and
13). A Personal LaserWriter LS and a LaserWriter Select 300 are found and shared, each built
from its own driver, but neither has been tried (W-1145 and W-1146, below).**

- **Finding it.** The 300 drives a 533 kHz clock on J2 pin 2 (HSKi; the port's CTS pin, IO8 on
  rev A) the whole time it is on. `serial_out` counts its edges (`docs/traps.md`): clocked after
  two 50 ms windows at 100 kHz or more, gone after 20 below (`serial_clock.c`, host-tested), so a
  printer switched off reads gone in about 2 s and a sub-second stop, which board 1's printer
  made on its own, only logs a warning. Clocked is first in `port_policy`'s order: no line-up, line-down or settle reaches
  the ImageWriter logic, and the plain question, `ESC ?` and the StyleWriter's status never go to
  a clocked port. With a clock and nothing named, the board asks `81 00`, `83 40`, `83 3E`,
  `82 00`; an `83 40` refused or unanswered does not end it (an LS's own driver never sends
  it). Engine identity `29` makes an auto-found record (`PR_KIND_QDLASER`, `PR_MODEL_PLW300`),
  `0D` an LS's (`PR_MODEL_PLWLS`), `23` none (a Japanese model Apple's driver half covers),
  any other a Select 300's (`PR_MODEL_SEL300`: Apple's drivers have no number for it), shared
  if there is room; the same printer back is the same record. A `23` is a QuickDraw
  LaserWriter AirWriter doesn't support: `/api/printers` carries it as `port_unsupported` and
  the page shows a grayed row at the end of the Printers list (`Printer port · Not supported
  yet`, nothing to open) until the clock goes. Each find logs the clock's rate and duty. The
  port's family is `pr_family_t` (ImageWriter, StyleWriter, QuickDraw LaserWriter), the one
  switch the port policy, the job sink, the link and the test page branch on. A 300 record
  makes the stored list version 4, an LS record version 5, a Select record version 6
  (`docs/traps.md`, stored forms).
- **The link** (`qd_link.c`). GP-SPI2 as a slave, mode 0, LSB first, clocked from HSKi through
  the GPIO matrix, its MISO on TxD through the matrix and its MOSI fed from the TxD pad, so each
  chunk comes back as the line carried it and is compared bit for bit. Chip select uses no pad
  (`docs/traps.md`). A chunk is 64 lead marks, packets as line images (`qd_wire`: a 100-bit
  BREAK, 120 marks, the bytes 8N1 LSB first, 180 marks), then a tail of 4,096 marks; a timer
  armed as the chunk loads raises chip select at the tail's middle. A question uses two 1 KB
  chunks, a job two 8 KB chunks (a whole row packet fits one), in internal DMA-capable RAM freed
  when the link closes. Replies are UART1 at the measured clock, flow control off; the printer's
  BREAK is UART1's break event (`serial_out_break_take`). A chunk that ends more than a second
  after it should, or a packet put with no clock, ends the job as Offline.
- **Read-back.** A chunk whose bits differ from those sent fails the job (`bad_bits`), as a
  broken cable would. On board 1 about one chunk in 20 to 30, never the first after the link
  opens, comes back with no more than its lead recorded (0 to 7 bits) though it reached the
  printer whole: sending it again printed its rows twice (2026-11-13). Such a chunk counts as
  `unchecked` and the link goes on if the clock is toggling at that moment; a chunk that ended in
  one of the printer's own clock pauses never left and ends the job Offline. The cause is not
  known (§4).
- **A page** (`qd_page`, `qdlaser.c`). The shared 300 dpi rip (`ripjob`), the sheet found by
  width and height, gray, cropped to Apple's image area for that sheet. First, text and thin
  lines (W-1174, below). Each row is screened in
  the record's screen and, through `rip_stage_t.gray_row`, in Apple's 300 screen (`HT_APPLE_LW`:
  `GAMA` 3's curve, then `HTSC` 2's 8 × 8 clustered thresholds), and coded both ways with
  `sw_code_row(row, NULL, …)`. The record's rows go to the printer as they are made; the
  Apple-screen rows are kept in the job buffer's second 512 KB (the URF ring has the first). The
  page budget is 500,000 bytes of codes (a page in the printer's memory measured full at
  513,287). A page that would pass it stops there: the sink is opened again for the same page,
  which waits for the page printing before it to be done; then `81 00` and the whole setup drop
  the rows already in, and the kept page goes instead (log: `page N: Apple's screen, B bytes of
  codes`). If that passes the
  budget too, the page is skipped and the job goes on to the next, as Apple's alert's Continue
  did; the job then ends failed, with "Page N didn’t fit in the printer’s memory." as its
  `job-state-message` and in `/api/status`'s `job_error`. A record set to Apple's screen keeps
  nothing. After the last row, once the page before it is done: `8E 00`, `82 00`, `83 2A` (paper
  at the chosen source), `84` with the Paper light off, `8B 8B` twice, `8A 01`. Page done is
  `8E 00` bit 2, asked every 500 ms and at once after the printer's BREAK; 85 s from `8A 01`
  with none fails the page (Apple's limit). On board 1 the printer's BREAK came with every page
  done, and a row with a bad check (a test build flipped one) set `8E 00` bit 1 (`FA`) with
  `82 00` as for any page (`A0`): the page fails there, unprinted (`8A 00`, `81 00`, `84 01`),
  as `qdinterop` checks against the software 300.
- **The next page loads while one prints** (W-1144, `qd_session`). `qd_session_print` sends a
  page's print sequence and returns; the wait for its done is the next page's print, or the
  job's end (`qd_session_end`, from `qdlaser_job_end`). Each page after the first follows a
  print's time behind the one before, not a load and a print. One task still: the rip's rows go
  straight to the printer and the session runs the conversation between them. What board 1's
  printer did on the bench (2026-11-13, runs A1 to A5 below), and what the session does about
  each:
  - *The next page's rows go in while a page prints*, with no `81 00` and only the paper
    source (`83 4C` or `83 4F`) before them; its own print sequence, sent at the printing
    page's done, prints it. A job's first page has the whole setup; each page after it on the
    same sheet sends the source alone.
  - *The offsets and the width (`85` to `89`) are read as a page prints*: changed 3 s into a
    print, both sheets moved. A page on another sheet waits for the printing page's done, then
    has the whole setup, `81 00` first.
  - *The memory holds both pages.* 513,287 bytes of codes fit; 533,655 was full. Past it while
    a page prints: `8E 00` bit 0 with a BREAK, the page printing done early (6 s after its
    `8A 01`), and both sheets blank. A page's room is free once it is done: three 221 KB pages,
    each loaded while the last printed, never filled it. `82 00` bit 1 comes past about 262 KB
    and bit 2 says nothing waits; bit 0 was never set. The session keeps the page printing and
    the page loading inside 500,000 bytes together (`qd_job_cfg_t.room`): a row that would pass
    it waits for the printing page's done (log: `page N waited T ms for room in the printer`).
    A page alone keeps its 500,000 budget and its fallback.
  - *`81 00` while a page prints blanks the sheet*, and no page done comes for it. `81 00` goes
    back to the start of the memory and clears nothing. The session sends none between a
    page's `8A 01` and its done except to stop it.
  - *Between rows*, at the printer's BREAK or every 500 ms, one `8E 00`, then `83 01` and
    `83 02` when that is not done. `8E`'s page done is cleared by the read, so every `8E` read
    that says done is the printing page's, wherever it was asked, and its bits 0 and 1 are
    kept from every read for the loading page's check. An `8E` unanswered after the printer's
    BREAK is asked again at once, and a reply that comes just after its 1 s wait is read
    before the next question's drain can drop it. With nothing printing, `83 01` and `83 02`
    alone. What they find wrong is seen to before another row goes (the printer was never
    tried taking rows with a jam or its cover open): no paper for the page printing waits
    for paper and prints it again; a jam or the cover with nothing printing holds the rows
    until it is gone; with a page printing, see jams below. An engine fault ends the job
    there. Two polls in a row with no answer are Offline; any answer ends the run.
  - *Cancel* sends no more rows. A sheet printing with nothing wrong comes out (to its done,
    inside the 85 s), then `8A 00`, `81 00`, `84 01`; with nothing printing, or the page
    printing stopped for paper or a jam, at once. A Cancel that comes as the last page prints
    ends the job cancelled once that sheet is out. An abort with no error of the session's own
    (the upload cut off, a page size refused, no memory), or with one that was the line's (a
    chunk read back wrong, the paper source unanswered as a page opened), is the same; after
    a printer failure (no clock, an engine fault) it stops at once.
  - *While `83 02` says jam or cover, only questions, `8A 00` and the lights go*: no row, no
    `81 00`, no `84 08`, no `8A 01`, from the recovery, a Cancel or an abort alike. The
    printer keeps an `8A 01` sent into a jam and feeds by itself when the jam clears, and
    `81 00` and `84 08` sent into that feed jammed it again. A jam counts as gone once `83 02`
    has stayed clear on reads 3 s apart and `83 01` bit 5 (moving) is clear: with page 1's
    sheet still in the path, a jam on page 2 read clear once after the cover closed, then jam
    again until the second sheet was out. The recovery asks `83 02` once more before its
    `8A 01`, and waits again if the jam is back.
  - *A jam or the cover: the pages go again from the board's copy.* The printer's own
    recovery prints again the page at the start of its memory, the first since the last
    `81 00`: after a jam on page 2 it printed page 1 (bench), and a jam as page 2 prints
    usually has page 1's sheet in the path too. So the board never relies on it for a job.
    `qdlaser_job_begin` takes 512 KB of PSRAM (`QD_COPY_CAP`; the log gives PSRAM free before
    and after) and the session keeps in it, as a ring, every row it puts into the printer
    (`[u16 n][codes]`), with a record a page: its number, sheet, place, bytes, rows, whether
    it is loading, printing or done, and when its done came. Room comes back from the oldest
    page that is done, never from one still in the printer; two pages at the budget with two
    bytes a row are 515,000 bytes. A row that could only go in over a page still in the
    printer is left out and its page's copy marked not whole. After a jam: `8A 00`; the wait
    until `83 02` has stayed clear; Apple's recovery up to but not its last `81 00`, `8A 01`;
    then, by the path they first went by (the whole setup for the first, rows, the check, the
    print sequence, each waiting for the done before it, inside the room), the page whose done
    came less than 20 s before the jam (its sheet may be the one that jammed on its way out;
    it may come out twice), the page that was printing, and the rows so far of the page
    loading, which the rip then goes on with. Log: `page N sent again after a jam`. A jam
    while they go again starts over once it has cleared.
  - *With no copy* (no PSRAM for it at the job's start, logged; a copy not whole; the test
    page, which is one page at the start of the memory): the printer's own recovery when the
    page stopped is the first since the last `81 00`, and a page loaded whole behind it prints
    after (bench A4c). Any other jam stops the job (`QD_E_FAULT`, log: `page N: lost to a jam
    or the cover…`), the state showing the jam until it clears; so does a jam on the first
    page with the next part loaded, whose other rows the recovery's `81 00` leaves nowhere to
    go. `jam_reprints` in `qd_session.c` holds that rule.
  - *The log*, per page: `page N: printing` (its `8A 01`), `page N: done, read T ms after its
    print`, and any wait for room. The rip's per-page line's `end` is now the wait for the page
    before and this page's print sequence. The job's `all out in N s` counts the end's wait for
    the last page.
- **Sizes and paper.** Letter, Legal, Executive, #10 and Monarch envelopes and A4, each with the
  image area Apple's driver gives it (`qd_model.c`); only Letter has printed. `media-source` is
  `main` (the cassette, `83 4C`, the default) or `manual` (the manual slot, `83 4F`), read from
  the job's `media-col`, or from `InputSlot` "Manual", which is how a Mac sends its Paper Feed
  choice: macOS's print dialog sends no `media-col` at all, only `media` "Letter" and
  `InputSlot` beside about 50 settings of its own. AirWriter
  never moves to the slot by itself.
- **Status** (`qd_status`, `qd_session`). At rest the board asks `83 01`, `83 02`, `83 2A` and
  `82 00` every 3 s under the job lock, and at once after the printer's BREAK. The first match
  wins:

  | Printer | `printer-state-message` | `printer-state-reasons` | Front lights (`84`) |
  |---|---|---|---|
  | Jam (`83 02` bit 3) | Needs attention: clear the paper jam | `media-jam-error` | Jam |
  | Cover open or no cartridge (`83 02` bit 2) | Needs attention: close the cover | `door-open-error` | Jam |
  | Cassette not in properly (`83 02` `20`, from Apple's driver) | Needs attention: check the paper cassette | `input-tray-missing-error` | Paper |
  | Engine fault (`83 01` bit 1 without `83 02`'s `10`; `83 04`, `83 07` name it in the log) | Needs attention: check the printer | `other-error` | |
  | No paper in the cassette, or the cassette pulled out (`83 2A`; the printer also sets `83 02` `10` and `83 01`'s fault bit) | Needs attention: check paper | `media-empty-error` | Paper |
  | The manual slot asked for and empty | Needs attention: load the manual feed slot | `media-needed-report` | Paper |
  | Paper moving (`83 01` bit 5), or a page loading | | | Ready, blinking once a second |
  | Otherwise | | | Ready |
  | No clock | Offline | `other` | (none: the printer is off) |

  Jam and cover go through Apple's recovery once `83 02` clears: `81 00`, `84 08` for 280 ms,
  `84 01`, `83 40`, `83 3E`, `82 00`, `83 5D`, the source, `83 02`, `81 00`, `8A 01`, which
  prints again the page at the start of the printer's memory. A job uses it only up to that
  last `81 00`, `8A 01`, and then sends its pages again from its copy (above); the test page,
  one page at the start of the memory, uses it whole. The cover opened mid-page reads as a
  jam (the sheet is caught: `83 01` `13`, `83 02` `0D`), and the printer wants the cover
  opened and closed again once the sheet is pulled; for a few seconds after it closes the `83`
  questions go unanswered, which the five tries of each ride out.
  Paper out mid-job stops the page (`8A 00`) and prints it (`8A 01`) once `83 2A` has paper. A
  job ends with `83 01`, `81 00`, `84 01`. A job that used or asked for the manual slot also
  sends `83 4C` however it ends, done, cancelled or failed: the printer otherwise keeps the
  slot, says it has no paper and pulls in any sheet put there; while `83 01`
  bit 5 says the sheet is still coming out (about 15 s past page done) the state stays Printing.
  A status question the printer leaves unanswered logs which one and the link's last failure.
- **Settings.** The usual five screens plus Apple, which for a 300 is Apple's 300 screen.
  Default blue noise (W-1174): a record found is made with it, and a record still on Atkinson,
  the default before, moved to it once at start-up (`w1174` in NVS `printers`), so a screen
  picked since stays.
- **Light tints, thin lines and pictures** (`W-1174 Personal LaserWriter 300: light tints, thin
  lines and dark pictures print as they look`; design amendment). An order slip from a Mac lost
  its 5% panel and its 16% outlines and dividers, and printed its navy photo near-black.
  Atkinson prints no dot under about 12% ink and nothing but ink over about 88%, and the 300's
  toner spreads: from a photo of a printed strip of blue noise against 12-dot squares, 10% of
  dots printed about 26% dark, 30% about 58%, 50% about 79% (its blank paper streaky that day).
  Every page now has, ahead of both screens:
  - *Crisp text and lines* (`halftone/lineart.c`, `ripjob_cfg_t.lineart`): a thin dark feature
    on paper or a light tint prints solid with no soft edge, gray text black; a thin light one
    (192 to 231) with white paper (250 up) within 4 dots on both sides, and going on along
    itself, straight or through a corner, an unbroken line one dot wide, so a photo's pale grain
    and a line on a tint are left to the screen and a lone pale dot on white is paper (W-1178);
    the soft edge of
    any dark shape on paper (a QR code's squares, a headline) crisp; a gray area 9 dots wide and
    the 2 dots around it are the screen's, so a filled icon is screened whole. Rows come out ten
    rows behind, in about 59 KB (`la_bytes`) taken last of a page's buffers and from PSRAM only
    (`big_malloc_only`); a page with no room for it prints without, logged (board 1's first
    build asked 81 KB and failed a job: once the 512 KB copy is made about 115 KB of PSRAM is
    left).
    The first build took 4.2 s of the order slip's 10.7 s rip on board 1 (`lines` in the
    page's time line); skipping rows with only paper within ten and merging two planes halved
    it on the host (17 ms to 8, the same dots), not yet timed on the board.
  - *The toner curve* (`ht_toner_curve`, `qd_toner_curve`): the gain law at `QD_TONER_GAIN` 13,
    24 and under solid, eased over 25 to 39, 253 and over paper, on every screen but Apple's
    (its own curve) and the threshold (`qd_tone_for`). Wells chose 13 on paper over 25 and none:
    a Mac's grays are a screen's, lighter in the middle than the ink a page needs, and the
    spread puts some of that back.
  A mask screen (blue noise, Bayer, threshold, Apple's) allocates no error rows (`ht_init`),
  about 14 KB a halftoner, two of them on a 300's page.
- **Test page** (`qd_testpage`). The page every printer prints (`testpage`, W-1140) drawn at
  300 dpi in the 300's image area, with its icon and Apple's 300 screen as the Apple panel,
  its screens through the toner curve as a job's are (W-1174),
  loaded into the printer as it is made through the same page path: 171,067 bytes of codes in
  blue noise, 160,330 in Atkinson. The test button prints it, never the cable test, which reads
  HSKi's level.
- **The Personal LaserWriter LS** (W-1145). Built from the LS's own driver (Personal LW LS 7.1)
  and the LaserWriter 300 driver's LS branch, read for facts; no LS has been tried. Wells:
  shared like the 300 and called expected to work, untested, as the ImageWriter LQ is. Where
  the two Apple drivers differ, the LS gets what its own driver sends (`qd_spec_t.driver`
  `QD_DRV_LS`), plus what the board can't work without: `83 3E` (the 300's driver asks an LS
  it), `8E 00` every 500 ms while a page prints, and `83 01`, `83 02` and `82 00` at rest.
  - *One page at a time.* Each page waits for the done of the one before, then has `81 00` and
    its whole setup; no row goes while a page prints. The LS's own driver loads the next page
    while one prints; the 300's driver 1.2 added a wait for exactly that on an LS. One at a time
    also means only one page has to fit in a memory nobody has measured. About 8 s more a page
    after the first than the 300.
  - *Setup*, in its driver's order: `85`/`86`, `87`, `88`/`89`, the source, then `83 9D` with
    the sheet's paper number as it is, not shifted (Letter `04`, A4 `01`, Legal `0C`, B5 `09`,
    else `21`), each sent once and its reply taken whatever it is, as its driver reads
    neither. No `83 40` in a job, no density, no `84 00`.
  - *The source* is always `83 52`, the multipurpose tray: IPP `main` alone, and a job asking
    for `manual` gets the tray too. Every LS has the tray, which feeds a stack of up to 70
    sheets or a sheet by hand, and its Print dialog chooses it; the paper cassette (`83 51`)
    was an optional feeder (Personal LaserWriter LS Owner's Guide, 1991). Its driver's
    `83 25` bit 0x20 may say a cassette is fitted, which is inferred, so the cassette is
    never used. A build that sent `83 51` had every page of an LS without the cassette
    wait for paper; `qdfake.py`'s LS has no cassette, so that fails `qdinterop`.
  - *Sheets:* Letter, A4, Legal, B5 (`iso_b5_176x250mm`) and #10, each with its driver's setup:
    top (paper h − image h) / 2 rows, left (paper w − image w + 0xB7) / 16 bytes, the row
    ((image w + 24) / 8) & ~3 bytes, so Legal's and B5's 2,000 dots go in 252-byte rows and
    #10's 1,133 in 144, white past the image (`qd_row_bytes`; the rip keeps #10's as 1,136, a
    whole number of bytes). Letter is `85 C1 86 FF`, `87 EC`, `88 D4 89 FE`.
  - *Rows* carry its driver's 9-bit length, `0x4000 | (0x200 − n)` (`41 FF` for one byte of
    codes; the 300's driver sends every model 12 bits). The run codes are the 300's.
  - *Paper* is `83 02` bit 4, never `83 2A`; `83 02` bit 5 is not read as the cassette.
    Nothing asks about paper before a page's `8A 01`, as its driver asks nothing: paper out is
    known after. Paper out as a page prints: `8A 00`, then `8A 01` once bit 4 clears or every
    15 s, as a person pressing its driver's Continue would, in case the bit stays set until
    the printer next tries to feed; the state stays out of paper until a try finds paper, and
    the stall limit counts from the first.
  - *A jam or the cover:* `8A 00`, the wait until it has stayed clear, `81 00` alone, then the
    page again from the job's copy with its whole setup; with no copy (the test page), `81 00`
    and `8A 01` print the page in the printer again. No `84 08`, `83 40` or `83 5D`.
  - *Memory:* a page budget of 480,000 bytes of codes, a little under the 300's 500,000 (512 KB
    as Apple shipped both; the 300 took 513,287). The page timeout stays 85 s (its driver's is
    45 s).
  - `qdfake.py --engine 0d` is an LS as its driver has it, and logs anything its driver never
    sends (`83 2A`, `83 A7`, `83 5D`, `83 4C`, `83 4F`, `84 08`) or a row while a page prints;
    `qdcheck` and `qdinterop` print to it. Its icon is its driver's (`plwls`).
- **The LaserWriter Select 300** (W-1146). Built from its own driver (LW Select 300 1.0) and
  LaserWriter 300 1.2's Select branch, read for facts (the notes are kept outside this repo);
  no Select has been tried. Wells: found and shared like the LS, called untested in the docs and
  wiki. Where the drivers differ, the Select's own wins, but for 1.2's `83 68` and `83 67`,
  which 1.2 sends only to a Select. `qd_spec_t.driver` `QD_DRV_SEL`; the icon is Apple's for
  the LaserWriter Select's body (`lwselect`), since its driver only reuses the 300's.
  - *Two page paths, by `82 00`.* At each job's start, bits 7-6 of `82 00` `01` take page
    memory, anything else the 300's path, its driver's one test. Every clue in the driver
    (PhotoGrade, its curve, its plain-path margin left alone) says a real Select answers
    `01`. One page at a time on both: each waits for the done of the one before.
  - *Job.* `81 00`, `83 3E`, `82 00` (no `83 40`), `83 68` (one try, its reply dropped). On
    page memory then the driver's probe: `92 00`; `5A` written at 0 and `A5` at 0x0FFFFF with
    `E0-E2` and `8B`, read back at 0x0FFFFF with `E3-E5` and `CF 00`: not `A5` is under 1 MB
    (7); else `5A` at 0x3FFFFF and read back at 0x0FFFFF: `5A` is 1 MB (5), else more (0); `92`
    with the result, `84` with bit 4 once, and the identity again. The result changes nothing
    else: the page budget is 480,000 bytes of codes, as the LS's.
  - *Setup*, in its driver's order: the left margin, top (`85`/`86`, (paper h − image h) / 2
    + 0x27 rows), `83 5B` and its value byte `C2`, density `83 58` and Apple's default as
    `E9` (no value when `83 58`'s answer has bits 7-6 `10`), the row width, the source (replies
    dropped), Ready. The 300's path: left `87` (2550 − image w + 0x16A) / 16 bytes, the width
    in bytes. Page memory: left (2550 − image w + 0x16A) / 2 dots as `87` and `8C` (its high
    byte), `A2 19`/`A3` and `A2 18`/`A3` with 11 less it (high, low), the width `88`/`89` in
    dots; once a job the `PGRD` table (`apple_lw_pgrd`: entry i's first word's low byte at
    `A0` i, high byte at `A0` 20+i, the second word's low byte at `A0` 40+i, each with `A1`),
    `A2 00`/`A3 C0`, `A0 60`/`A1 00`. On page memory the setup goes with the first page,
    straight after the probe's identity, and again only for another sheet, with no `81 00`; on
    the 300's path, `81 00` and the setup before every page.
    Letter: `85 9B 86 FF`; plain `87 E0`, `88 D4 89 FE`; page memory `87 00 8C FF`,
    `A3 FF`/`A3 0B`, `88 A0 89 F6`.
  - *A page on page memory:* the source, `8B 8B` twice, then `C0-C2`, the write address, is
    the page's start; its rows (the 300's packets and codes) go where the printer writes;
    `83 02` for paper (and the jam and cover); `8B 8B` twice, `E3-E5` the page's start,
    `8A 01`. Done is the read address (`C3-C5`) come to the write address, asked every 500 ms
    with `83 01` and `83 02`. `8E` goes only after the printer's BREAK, as its driver asks it,
    its reply one byte: its memory-full and error bits stop the page from printing, and its
    done bit only brings the addresses on at once, since a done read late would end the next
    page early. Opened again before it
    printed (Apple's screen after a page too big), `E0-E2` go back to its start and the new
    rows go over the old. No `8A 02` (it readies the engine early; every reprint and copy
    prints without it), no size check (`83 0B`/`0D`: a phone can't change a cassette).
  - *Sheets:* Letter, A4 and Legal, the 300's image areas (its driver shares the code). Its
    envelopes would need the margin it can't carry moved into every row, and its driver's
    Executive is drawn 5.5 × 8.5 in.
  - *Paper* from `83 01`/`83 02` as the LS: `83 02` bit 4 no paper, bit 5 the cassette, bit 6
    its front cover or toner cartridge, shown as the cover. The cassette is `83 51`, IPP
    `main`; the multipurpose tray `83 4F`, `manual`; the optional lower cassette (`83 52`,
    fitted when `83 25` bit 0x20) is never used. Never `83 2A`.
  - *A jam or the cover:* `8A 00` and `83 5D`, the wait until it has stayed clear, and on page
    memory Apple's reprint from the printer's memory, where the page is still whole: `C0-C2`
    read, `81 00`, `E0-E2` put back, `E3-E5` the page's start, `8A 01`. Nothing is sent again.
    On the 300's path: `81 00` and the page again from the job's copy.
  - *Job end:* Ready, `83 67` (one try, its reply dropped), no `81 00`, as both its drivers end.
  - `qdfake.py --engine 31` is a Select on page memory (`--plain`: on the 300's path), with
    addressable memory, the probe, the addresses and the PGRD table, and logs anything
    AirWriter never sends a Select; `qdcheck` and `qdinterop` print to it on both paths.
- **Bench (rev A, board 1, with Wells's Personal LaserWriter 300, 2026-11-12 and 13).** Found
  by itself at 533.5 to 533.7 kHz, duty 49 to 51%; switched off, Offline in about 2 s; switched
  on, the same record. Mac pages and an iPhone page; a manual-slot page chosen from a Mac; a
  three-page job, in order; a full-page photo that passed the budget in Atkinson and printed in
  Apple's screen (63,625 bytes of codes, 300 chunks, 0 bad bits, 8 unchecked); the test page,
  approved on paper by Wells. Every job: 0 bad bits, 0 stalls; internal RAM never under 42,967
  bytes, 0 failed allocations; worst chunk end 1,877 µs late against the tail's 3.8 ms half.
  The cassette pulled out and the cover opened at rest and mid-job, paper run out mid-job, a
  jam pulled: each shown and recovered as above. Cancelled from a phone while waiting for paper
  and from CUPS while a page loaded: nothing printed, Ready in 4 s. A test page cut by switching
  the printer off failed with the clock gone. A Color StyleWriter 2500 and the ImageWriter II
  were found on J2 afterwards, the StyleWriter's test page printed. Legal, A4 and the envelopes
  have not been printed.
- **Bench for W-1144 (the same board and printer, 2026-11-13 evening, a throwaway build that
  ran scripted packets).** A1: two and three 153 KB pages, each loaded while the last printed
  with only `83 4C` between: page done to page done 17.0 to 17.4 s, against 24.2 to 25.2 s for
  the same pages a page at a time; a page's rows took 5.6 s. A2: one 364 KB page printed; two
  filled the memory 158 KB into the second, both sheets blank; three 221 KB pages printed. A3:
  `81 00` 5 s into a print, a blank sheet and no page done. A4c: the cover opened 7.2 s into
  page 1 with page 2 loaded, then Apple's recovery: page 1 whole 20.6 s later, page 2 whole
  17.3 s after its own print sequence. A5: the offsets changed 3 s into page 1's print, both
  sheets 4.5 mm down and 5.5 mm left. Free PSRAM during a three-page job never under 597 KB.
  A4d, A4e (later that evening): the
  cover opened 8.5 s into page 2's print, page 1's sheet still on its way out. After it
  closed, `83 02` read clear once, then jam for 15 minutes, until a second sheet was found
  near the exit and taken out; the recovery's `8A 01`, sent while the printer said jam,
  printed nothing then, and ran by itself when the jam cleared (`83 01` `20` within 9 s);
  `81 00` and `84 08` sent into that feed jammed it again. Not known: whether the printer
  takes rows while a jam or the open cover holds it (the session sends none). Asked again
  after A4e: the printer's own recovery after a jam on page 2 printed page 1.
- **Bench for W-1144's firmware (board 1 and the same printer, 51e4243, 2026-11-13 night,
  `ipptool` from a Mac).** Three pages all out in 63 s (75 s a page at a time), prints 17.2 and
  17.4 s apart; the copy took its 524,288 bytes of PSRAM with 648,988 free. Ten pages of 118 to
  123 KB of codes in 191 s (about 250 s before), each 17.05 to 17.22 s after the last. Two
  364 KB pages: page 2 waited 9.65 s for room and printed 10.9 s after page 1's done. A Cancel
  during page 2 of three: page 2 came out, page 3 never went, the job ended canceled. Wells
  checked every sheet: whole and in order. The cover opened during the ten pages, as sheet 3
  landed 16.0 s after its done, with page 4 printing and page 5 loading: the board read jam at
  once and, once it cleared, sent page 3 again from the copy (its done inside the 20 s), then
  page 4 and page 5's rows. `83 2A` then read no paper for 2.8 s (the cassette was out to clear
  the jam), the page waited and printed, and pages 5 to 10 followed 16.8 to 16.9 s apart:
  all ten, page 3 twice, in 270 s. 0 bad bits in every job.

### 3.19 Two boards: rev A and rev 2026.11 (W-1109)

`W-1109 Firmware for rev 2026.11 boards: N16R8 build, straps, own handshake pins, no DIPs, LC`.
Rev 2026.11 (`docs/hardware-rev-2026-11.md`) has an ESP32-S3-WROOM-1-N16R8, 16 MB flash and
8 MB octal PSRAM on IO35-IO37, where rev A's N8R2 has 8 MB and 2 MB quad. ESP-IDF fixes the
PSRAM mode at build time, so each board has its own build from the same source:
`idf.py build` is rev A as before; `idf.py -B build-2026.11 -DAIRWRITER_BOARD=2026.11 build`
layers `sdkconfig.defaults.2026.11` on the defaults, keeps `sdkconfig.2026.11`, and makes
`airwriter-2026.11.bin`. `CMakeLists.txt` and main.c stop a build whose sdkconfig is the other
board's. Neither image runs on the other board, and each board refuses the other's (3.12).

Espressif rates the N16R8 to 65 °C around it, and to 85 °C, rev A's N8R2's rating, with the
PSRAM's error correction on, so rev 2026.11's build turns it on (`CONFIG_SPIRAM_ECC_ENABLE` in
`sdkconfig.defaults.2026.11`; main.c stops a 2026.11 build without it). ESP-IDF then keeps 1/16
of the PSRAM for the check bits and maps the other 7.5 MB. Built, never run: no rev 2026.11
board exists.

- **Pins.** Kconfig's board choice (`AIRWRITER_BOARD_REV_A`, `AIRWRITER_BOARD_2026_11`) sets the
  defaults. J2's handshake drive (`HSKI_DRV`) is IO47 on rev 2026.11, not IO35; J4's (IO13) and
  J4's handshake in (IO21) exist only there. The bootloader hook drives IO47 and IO13 low and
  never touches IO35-IO37; it tells the board by the flash size, the one part of the board
  choice its build sees. App_main holds both low (asserted at the port) from its first lines.
- **Either socket, either job** (`W-1176 Rev 2026.11: either socket takes a serial printer or
  AppleTalk`, `W-1179 Rev 2026.11: plug anything into either socket and AirWriter works out
  what it is`). Rev 2026.11's J2 and J4 have the same circuit (`docs/hardware-rev-2026-11.md`):
  a THVD1452 whose receiver is always on, a TashTalk of its own (U10 for J2, U7 for J4), and
  GPIOs that take the line over when that TashTalk is held in reset. The case calls J2 port 1
  (socket 0 in the code; the LC's only socket) and J4 port 2 (socket 1), and so does the page.
  The bootloader hook drives IO42 low (U10 in reset); app_main then gives each socket its job.
  - **The jobs** (`components/ports`). A socket doing the printer job holds its PIC in reset
    (CLK, DAT and CTS pulled down), drives its DE high and has a printer line of its own in
    `serial_out` (J2's on UART1, J4's on UART0, each with its own clock counter and line task);
    one doing LocalTalk parks that line (TX an input with no pull), lets DE go and releases its
    PIC. The S3 has three UARTs, so a socket has one for whichever job it does (W-1186):
    parking deletes the printer line's driver, the socket's LocalTalk node installs its own on
    the same UART (1 Mbaud, the PIC's pins through the GPIO matrix, no inversion), and
    unparking puts the printer line's back once the node has let go (`localtalk_park` first).
    UART2 is spare on rev 2026.11; rev A's LocalTalk keeps it. `ports_hw` is the one place the
    pins change hands, under the job lock. Each socket has its job by itself (W-1186): both
    may print, both may do LocalTalk, or one each. With both on LocalTalk no printer line is
    selected (`s_psock` -1): a job, test page or cleaning for a port record is told busy or
    refused, a vintage Mac's ImageWriter job is told busy and the nodes' status word says
    offline, the port task asks nothing, and the panel follows the LaserWriters, or with none
    shared reads the line as not ready (`box_ready`). A node starts only between job changes
    (`s_lt_mx`, taken by `sock_job_set` after the job lock), so a socket's UART never has two
    drivers asked for at once and a node never starts before its socket is told to listen.
    Rev A and the LC keep one job per socket, set up as before.
  - **What's plugged in** (`port_detect_core.c`, pure and host-tested; `sock_detect` in main,
    between the port task's questions). Nothing is sent to find out. A printer's handshake held
    ready for 2 s, or a clock on HSKi, on the LocalTalk socket with no LocalTalk frame heard
    for 30 s moves that socket to the printer job, unless a frame was heard since the handshake
    went ready: then what holds it is a Mac on a direct cable, and LocalTalk stays until the
    line drops. Framing errors and breaks on a printer socket with no printer showing (its line
    down, or up with nothing answering there) put it on LocalTalk on trial, kept only if
    TashTalk hears a frame there within 20 s; else it goes back to the printer job and waits
    10 min before another. A printer that shows on it mid-trial (a clock, or its handshake
    coming up after the trial began) ends the trial at that check, with no wait after: noise
    from a printer switched off and on may be what began it. Each socket is worked out by itself, whatever the other does or
    hears (W-1186). Quiet sockets keep their last job; with nothing plugged in, J2
    prints and J4 does LocalTalk. Every change is a log line naming the port, its old and new
    job and the signal.
  - **Listening first.** AppleTalk frames sent into an ImageWriter II print as junk, selected or
    not: a deselected one keeps them and prints them when SELECT is pressed (board 1,
    2026-11-21: about 3 sheets, line feeds and form feeds included). So the node sends nothing
    from a socket until LocalTalk has been heard there (`localtalk_set_hold`, `pd_may_send`):
    it starts listening, holds no node ID in TashTalk, and its ticks, NBP lookups, the PAP
    client and echoes send nothing, while TashTalk's frames are still counted. A good-FCS frame
    on that socket lets it send, and it acquires a node as at start; a socket fixed to
    LocalTalk (`port1`, `port2`) sends at once. What was heard is dropped when the socket
    changes job, and at the first check a printer shows there (a handshake ready with no frame
    since it went ready, or a clock), so the node goes quiet before the socket moves; it must
    be heard again after. Each socket's node has its own hold, and its own count of frames
    (W-1186), so one network heard lets only its own socket send. Heard is saved
    (`port1heard`, `port2heard`): after a restart the socket sends once its line has shown no
    printer for 1 s (`PD_BOOT_MS`). A cable test's own frames don't count. The cost: a
    LaserWriter already switched on when its cable goes into a socket that hasn't heard
    LocalTalk is found only when it, or a Mac on that network, next sends a frame (switched off
    and on, it does). `W-1185 Rev 2026.11: read each port's handshake voltage, so a switched-on
    printer that isn't ready is seen` would let a socket with nothing driving its handshake
    send at once. While listening, `/api/lt` gives `"phase":"listening"` with `on` as its
    setting, and the LocalTalk card reads On port N with no Macs. Rev A and the LC send as
    before. Not covered: a printer deselected before its cable goes into a socket that has
    heard LocalTalk shows nothing there, so that socket still sends (W-1185 again), and so does
    the test button's cable test. A Mac on a direct cable whose handshake drops for one check
    makes its socket listen until the Mac next sends.
  - **Saved.** Each socket's last job (NVS `general` `port1job`, `port2job`), so a restart
    keeps it. `/api/general` `{"port1":"auto"|"printer"|"localtalk","port2":…}` fixes one
    (`port1set`, `port2set`; auto by default); the page has no control for it. `/api/status`
    `ports` lists each socket's `job` and `set` (`fixed` on rev A and the LC), and its
    `lt_chip` (W-1186: the top-level `lt_chip` and `lt` are the lowest LocalTalk port's chip,
    and any port's node answering as the ImageWriter).
  - **Two printers at once.** Both printer sockets are live and shared: the port task visits
    each in turn (`port_visit`) and asks it its own questions (3.16's policy, with books per
    socket, `psock_t`), selecting it (`serial_out_select`, `qd_link_init`) under the job lock.
    Jobs stay one at a time under that lock: a job selects its record's socket
    (`job_sock_take`), a vintage Mac's ImageWriter stream the first socket with an ImageWriter
    there (`lt_sock`), and a job for the other socket's printer is told busy until the first
    ends. A job for a record whose socket now does LocalTalk is told busy too (`port_claim`),
    never sent to the other socket's printer. Each socket keeps its own line, ribbon (port 1's is the one saved), StyleWriter and
    QuickDraw LaserWriter state, and CTRL-X kick. The panel follows the printing socket's line;
    idle, it is Ready while any shared printer is. 3.15 has the records.
  - **TashTalk on U10 and U7.** Both ship blank (W-1176; `pic_prog` takes either socket's
    pins). At power-up, before LocalTalk starts, each PIC that answers is read back
    (`pic_check`: about a third of a second with TashTalk on it, milliseconds blank), and one
    without this build's TashTalk gets it then, whatever its socket's job, so both are ready
    before any cable is; a printer socket's PIC stays in reset after. A failed install shows on
    the LocalTalk card, and the page's button puts it on, as on rev A's U7. `/api/localtalk/flash` takes `{"port":1|2}` (default:
    the LocalTalk socket).
  - The cable test needs a socket on each job; its frames leave the printer socket's driver.
    With both sockets on LocalTalk, the page's **Test Cable…** gives port 1 the printer job
    first (port 2 if port 1 is set to LocalTalk).
    A test cable from port 1 to port 2 shows each socket the other's handshake, so both may
    be on the printer job by the time the page's **Test Cable…** runs: the socket that isn't
    the printer line's then takes LocalTalk for the test, unless it is set to the printer job.
    The test button's cable test does not (it runs with the job lock already held).
  - With no socket on LocalTalk every node is parked: `/api/lt` gives `"parked":true`, `port` 0
    and `on` as its setting, so the LocalTalk card's switch keeps its place.
- **Straps**, rev 2026.11 only, read once at boot with the pull-ups on, which stay on only where
  a strap reads high: IO16 (`BOARD_REV`, low on every 2026.11 board) and IO15 (`BOARD_LC`, low on
  the LC). `/api/status` has `board` (`rev-a` or `2026.11`), `lc`, and `board_ok`, false when a
  2026.11 build reads IO16 high (R32 missing, or another board), which the log says too.
- **No DIP switches.** Rev 2026.11 has none, and since `W-1162 Remove the DIP switch code: rev A
  boards ignore S1, the page's switches decide` no board reads any: rev A's S1 is still fitted
  and does nothing. The test button is `test_button`. What the switches set: the AppleTalk port
  follows the page's switch alone, on unless switched off (NVS `lt_port`, stored at first boot;
  a rev A board that never used the switch reads it as on); no test page at power-on (the test
  button prints one); Rainbow status lights is a switch on the page's About on every board
  (`{"rainbow":true}` to `/api/general`, NVS `rainbow`). On rev A the app parks S1's pins (IO13,
  21, 38-42, 47) in their GPIO function with the pad off: IO39-42 come out of reset as JTAG,
  where MTDO is a driven output that a switch left on would short. About's line under the name
  reads `2026.11 · Wells Workshop` or `Rev A · Wells Workshop`.
- **Cable test.** After both data legs pass, rev 2026.11 checks each handshake wire: it drives
  J4's handshake out high then low and reads J2's in (IO8, J4.1 to J2.2), then J2's out against
  J4's in (IO21, J2.1 to J4.2). A level comes back as itself through U3; an open wire leaves the
  receiver's input on its pull-down, which reads high, so the low level finds it. Verdicts
  `no_busy` (J4.1 to J2.2: the printer can't tell AirWriter to wait), `no_hsk_out` (J2.1 to
  J4.2) and `no_hsk` (both); the reply's `hsk` names the leg.
- **Job buffer** (3.4): 6.5 MB of the 7.5 MB the error correction leaves
  (`AIRWRITER_JOB_POOL_KB`), so a full-page colour photo's ~5.7 MB of URF fits ahead of the
  rip; 1 MB if that much can't be had. That leaves the rest of the firmware 1 MB of PSRAM, as
  rev A's 2 MB does.
- **U7 is left off an LC**: every start-up check of U7 (`lt_go("boot", 1)`) leaves its pins on
  the chip's pull-ups, so they never float.
- **The LC's light.** When `BOARD_LC` reads low, `led_grid` draws D26, the chain's first LED
  there, with `lg_render_one16`: the status language in time and colour alone (the status
  language design's LC section), and the rest of the chain dark. `/api/panel` sends that frame,
  and the page's mark beside the name draws the one dot when `/api/status` has `lc`, and
  the page is named AirWriter LC.
- **Chip temperature** (`chip_temp`). A module's rating is for the air around it, and
  neither board has been measured in its closed case. Both builds read the
  ESP32-S3's own sensor (ESP-IDF's `temperature_sensor` driver, started in its -10 to 80 °C
  range, where it is within 1 °C; the driver moves to 20 to 100 °C, within 2 °C, by itself)
  once a second from the main task's loop, and `/api/status` has `temp`, that reading in °C to
  a tenth, and `temp_max`, the highest since boot; each is `null` on a board whose sensor did
  not start or whose last reading failed. The sensor is on the die, so it reads above the air
  in the case, by how much depends on what the chip is doing. Nothing acts on the number and
  the settings page does not show it. Not yet read on a board.
- **Tested:** both builds, the stack check on both, and the host tests (the manifest with both
  keys, the handshake verdicts, the LC light's caps, colours, endings and priorities; W-1179's
  socket detector, the printer list with two sockets, the port policy's books per socket and
  the kick per socket). Nothing has run on a rev 2026.11 board: none exists yet. On
  the first, check the straps, the handshake legs through Apple's crossover, a StyleWriter
  (J2's handshake held asserted by IO47) and the Personal LaserWriter 300's clock through
  rev 2026.11's U3; then each socket's jobs: a printer on each, LocalTalk on each, a printer
  moved from one to the other, and U10's and U7's first TashTalk flash.

### 3.20 Password and sign-in (W-1156)

`W-1156 Setup and the settings page are locked by a password`; design in
`docs/superpowers/specs/2026-11-15-setup-password-design.md`. It is what lets the EU paperwork
self-declare EN 18031: a sold board has a password the person can't skip.

**The password**, in the `auth` component (`auth_core.c`, pure and host-tested, and `auth.c`).
It is the first of: one set or changed on the page (NVS `auth/pw`, 8 to 63 printable ASCII, WPA2's
rules); the one burned into the chip at the bench; or, on a blank chip, the build's
`CONFIG_AIRWRITER_TEST_PASSWORD` (empty in every release, so a bench board is tested without
being burned). A sold board's password and serial are burned together into the user eFuse block
(`EFUSE_BLK_USER_DATA`, BLOCK3, written once and whole):

| Bytes | Holds |
|---|---|
| 0 | Layout version, `1` |
| 1–16 | Serial, ASCII, zero-padded (`2652AWF2611-002`) |
| 17–24 | Password: 8 of `abcdefghjkmnpqrstuvwxyz23456789` |
| 25–31 | Zero (the S3's custom-MAC field, never used) |

All zero is no password; anything else malformed is logged and counts as none. The serial stays
out of the log and the page. `/api/status` carries the kind, `"pw":"label"`, `"changed"`, `"set"`
or `"none"`, never the password. Erase All Settings and both test-button erases wipe NVS, so a
burned board goes back to its label's password and any other board to none.

**The setup network** is WPA2-PSK (CCMP) with the current password, open on a board with none.
Its captive page never asks: joining took the password.

**Signing in.** On the home network, on a board with a password, every route on port 80 but the
page (`/` and its addresses) and `/api/auth/*` answers 401 `{"signin":true,"label":bool}` to a
browser without a known token (`label`: the password on the label still works, unchanged). The
page shows its sign-in alert on the first 401, over its plain background, and stills its other
asks until signing in reloads it. Its Forgot password? answers where the label's password is
while it works, and how to erase the settings once it doesn't.

1. `GET /api/auth/nonce` → `{"nonce","salt","iter"}`: a challenge good once, for 60 s (4 kept),
   the board's salt (16 random bytes in NVS `auth/salt`, new after an erase) and 10000 rounds
   (each guess at a recorded sign-in costs more than one at WPA2's 4096).
2. The page derives `K = PBKDF2-HMAC-SHA256(password, salt, iter, 32)` and sends
   `POST /api/auth/signin {"nonce","proof"}`, `proof = HMAC-SHA256(K, "signin" ‖ nonce)`. Plain
   http offers no `crypto.subtle`, so the page carries its own (`tools/webui/crypto.js`).
3. The board holds `K` (derived when the password or salt changes, its time logged, and kept in
   NVS `auth/key` beside a check of both, so a start-up doesn't derive it) and compares in
   constant time. A right proof gets `Set-Cookie: aw=<32 hex>; Max-Age=15552000; Path=/;
   HttpOnly; SameSite=Strict`; 8 tokens are kept in NVS `auth/tok`, the oldest pushed out.

400 "Try again." is a challenge the board doesn't have (it restarted): the page tries once more
with a fresh one. 403 "Wrong password."; after 5 in a row, 429 "Too many tries. Try again in a
minute." with `Retry-After` for 60 s (kept in RAM). `POST /api/auth/signout` forgets the
caller's token. `POST /api/auth/password {"nonce","proof","enc"}` changes it: the proof is made
with `"change"` and the new password comes XORed with `HMAC-SHA256(K, "change" ‖ nonce ‖ i)` for
i = 1, 2, so neither crosses the network; every other browser is signed out. On a board with
none, `{"new"}` sets the first one in the clear (there is no key yet) and signs that browser in.
Nothing is guarded in captive mode or on a board with no password; IPP on 631 and AppleTalk never
are. `tools/webui/signin.py` signs in for `curl`.

**Older firmware**: a board with a burned or test password refuses images before the first
release with sign-in once it runs one (§3.12).

**The bench** (`tools/bench/bench.py`, on a Python with Pillow) prints a whole sheet of labels
through an AirWriter sharing the Personal LaserWriter 300: drawn at 300 dpi in pure black and
white (nothing for the printer to screen), sent as one URF page in an IPP Print-Job. Every label
gets a serial and a password, recorded in a private book (`labels.csv`, never in a repo). Then
`burn` reads the board's user block (`espefuse dump --format split`), and on a blank one burns the
next label of the kind asked for (`burn_block_data BLOCK_USR_DATA`), reads it back and records
the MAC. Serials are one count across full and LC boards from 001, given out as a sheet prints,
the week being the sheet's ISO week; a void label's number is never reused. The 300's image area
is 8 in, centred on a letter page: a label within 1/4 in of the sheet's left or right edge loses
its edge.

- **Tested:** the core on the host (hashes against Python's hashlib, the block, challenges, the
  lockout, tokens, the cookie), the page's script against the same values, the page against
  `preview.py`'s sign-in (right and wrong, a stale challenge, the wait, change, set, signing out
  in one tab), `signin.py` against the preview, and the bench tool (the book, the block, a sheet
  through the rip and `qdfake.py` dot for dot, a Print-Job to `ippcap.py`, a burn against a
  stand-in espefuse). Both builds and both stack checks. Not yet on a board; no chip is burned.

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
   printer's own manual; a bench check on a built board is confirmation, not discovery.

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
   HSKo on a built board is a confirmation step. (`CONFIG_AIRWRITER_INVERT_CTS` stays as the
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
6. **QuickDraw LaserWriters (W-1102, §3.18). Open.**
   - **Why some chunks come back empty.** On board 1 about one chunk in 20 to 30 is read back
     with no more than its lead though it reached the printer whole (rows sent again printed
     twice). It is counted as unchecked; a chunk that truly never went out would leave rows
     missing with nothing in the log but the count. Every page printed since has been whole.
   - **The LS** (W-1145). Built from its own driver and never tried. Only an LS can settle: its
     clock rate and whether rev A's U3 and the SPI slave follow it; whether it minds the `83 40`
     the probe sends every printer; whether it wants `83 9D`'s value as it is (its driver) or
     shifted with parity (the 300's driver); how much of a page fits; its real status bytes,
     above all whether `83 02` bit 4 says no paper at rest and clears when paper goes in, and
     whether `83 01` bit 1 without it is always an engine fault (the 300's rule, which ends the
     job; its driver fails only on particular `83 04` and `83 07` bits); which source it
     takes at power-up (the tray, as its guide's lights suggest, is what `qdfake.py` does);
     whether `83 25` bit 0x20 says the cassette is fitted, to offer it.
   - **The Select 300** (W-1146). Built from its own driver and never tried. Only a Select can
     settle: its `82 00` answer, on which the page path hangs; whether the probe, the
     addresses and `8B 8B` behave as read, and what `81 00` does to the write address (the
     reprint puts it back either way); what happens when the write address reaches the end of
     its memory (the driver never handles it; AirWriter takes it to wrap); its memory as
     shipped; the left margin on page memory (0x16A in its own driver, 0x1A0 in LaserWriter 300
     1.1 and 1.2, 27 dots apart); its Fuji Xerox engine's real status bytes; its clock rate and
     whether rev A's U3 follows it; whether it wants `83 68`/`83 67`, minds the probe's
     `83 40`, and needs the PGRD table for a page of one bit a dot.
   - **Sizes other than Letter.** Legal, Executive, #10, Monarch and A4 use the image areas read
     out of Apple's driver and have not been printed.
   - **`81 00` while a page prints.** Whether it cuts the page is not known, which is why one
     page at a time is in the printer.
   - **A board with `CONFIG_AIRWRITER_INVERT_TX`.** The link's chip select assumes TxD idles high
     (`docs/traps.md`); untested inverted.

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
