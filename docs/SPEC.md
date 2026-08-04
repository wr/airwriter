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
blind vias. Target outline 55 x 35mm, rectangular with 3mm corner radii and four M2
mounting holes inset 3mm from the corners.

**As built: 32 x 54mm** (1728mm² against the target's 1925), rectangular, four M2 holes.
The aspect ratio flipped to portrait because the WROOM-1 wants its antenna overhanging a
short edge while the mini-DIN and USB-C both need the opposite short edge. Corner radii
are not cut yet. See `hardware/airwriter/README.md` for the size derivation.

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
| J2 | Kycon KMDGX-8S or CUI MD-80S | THT | Mini-DIN-8 female. Footprint may need to be drawn — verify against KiCad stock libs first |
| D1 | WS2812B-2020 | 2020 | Status |
| SW1 | Omron B3U-1000P | SMD | Config / factory reset, wired to GPIO0. **Changed from a generic 4.5mm tactile** — the closest KiCad stock footprint (B3S-1100) has a 10 x 8.9mm courtyard, which became the binding constraint on the pocket beside the mini-DIN. The B3U-1000P is 4.8 x 3.4mm |
| — | 22uF + 10uF + 0.1uF | 0805/0603 | Bulk on 3.3V rail, one 0.1uF per IC pin pair |

### 2.4 Connector wiring

Mini-DIN-8 pinout on the printer (female on the board, so a straight DIN8-to-DIN8 cable
works — these are cheap and plentiful on eBay):

| Pin | Signal | Direction (printer's POV) | Goes to |
|---|---|---|---|
| 1 | HSKo | out | U3 R1IN → ESP32 UART1 CTS |
| 2 | HSKi | in | U3 T1OUT (DNP) |
| 3 | TxD- | out | U2 A (receiver) |
| 4 | GND | — | GND |
| 5 | RxD- | in | U2 Z (driver) |
| 6 | TxD+ | out | U2 B (receiver) |
| 7 | GPi | in | not connected |
| 8 | RxD+ | in | U2 Y (driver) |

**Polarity solder jumpers.** Put a two-way solder jumper on each differential pair (JP1 on
the driver pair, JP2 on the receiver pair) so A/B and Y/Z can be swapped without cutting
traces. RS-422 polarity conventions between Apple's implementation and the transceiver
datasheet are not something to bet a board spin on.

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
- USB-C is power only. Flashing and console go over the S3's native USB peripheral on
  GPIO19/20 — expose those on a 4-pin 1.27mm header for bring-up.

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
pdl=image/urf,image/pwg-raster
adminurl=http://<host>.local/
priority=10
UUID=<stable, derived from eFuse MAC>
Color=F
Duplex=F
URF=<see 3.3>
```

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

### 3.4 IPP operations to implement

Minimum viable set for iOS:

- `Get-Printer-Attributes` (0x000B) — must respond in under two seconds or the print sheet
  gives up
- `Validate-Job` (0x0004)
- `Print-Job` (0x0002) — body arrives chunked; decode as it streams, do not buffer the page
- `Get-Job-Attributes` (0x0009) — iOS polls this for progress
- `Get-Jobs` (0x000A)
- `Cancel-Job` (0x0008)

Return `server-error-operation-not-supported` for everything else rather than dropping the
connection.

### 3.5 Printer attributes worth getting right

**Margins.** The tractor feed means this printer physically cannot print to the edges.
Declare honest values or iOS will lay out edge-to-edge content and silently clip it.
Start with 6.35mm (0.25in) left and right, 12.7mm top and bottom, in hundredths of a
millimetre as IPP requires. Refine after test prints.

**Media.** `na_letter_8.5x11in` plus the continuous fanfold sizes. Not borderless.

**`printer-icons`.** An array of URLs served by the device's own HTTP server, at 48, 128
and 512 pixels. iOS renders these in the print sheet. Serve them from SPIFFS/LittleFS as
PNG. Placeholder assets are fine for now; final artwork comes from the silkscreen design.

**`printer-uuid`.** Derive from eFuse MAC so it survives reflashing. Add a `DEV_MODE`
build flag that regenerates it on every boot — iOS caches capabilities against the UUID
aggressively, and during development you will otherwise spend an afternoon staring at a
print sheet showing yesterday's media list with no way to flush it.

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

### 3.8 The ImageWriter output stage

Target for v1: **144 dpi horizontal, 72 dpi vertical, single pass.** Printable width 8
inches = 1152 dots.

Per eight raster rows:

1. Accumulate 8 rows of 1bpp data into a strip buffer (1152 bytes worst case — under 2KB,
   so nothing ever needs a full page in RAM)
2. Transpose to column-major: each output byte is one vertical column of 8 dots
3. Emit `ESC G nnnn` where `nnnn` is a four-digit ASCII decimal byte count
4. Emit the column bytes
5. Emit carriage return and a line feed of 8/72 inch

Set line spacing once per page with `ESC T nn` (nn in 144ths of an inch, ASCII digits).
End every page with a form feed — the widely reported "you have to press Select to make it
print" behaviour is almost always a missing FF, not a hardware fault.

Defer 144x144 double-pass mode to v2. It needs half-line feeds and two passes per strip,
and it doubles the time on a printer that is already slow.

### 3.9 Flow control and state

The printer has a 2KB input buffer. Wire HSKo to the ESP32's UART CTS pin and let the
peripheral stall the FIFO — no polling loop, no software watermark.

Then use that same line for status. If DTR stays deasserted past a timeout (start at 30
seconds, make it configurable), set `printer-state-reasons` to `media-empty` and
`printer-stopped`. That surfaces as a real alert on the phone. A 1985 dot matrix telling an
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

---

## 4. Unverified assumptions

These are marked because they must be checked against hardware or Apple's *ImageWriter II
Technical Reference*, not inferred. Any of them being wrong will produce output that looks
almost right, which is the worst failure mode.

1. **Bit order within a graphics byte.** Whether bit 0 is the top pin or the bottom pin is
   not settled here. Bring-up task: print a strip where only the top pin fires, observe,
   and encode the answer in a single `#define`.
2. **Density escape codes.** The ImageWriter II supports roughly eight horizontal
   densities selected by single-character escapes. The letter-to-dpi mapping in circulating
   documentation is inconsistent. Verify the code for 144 dpi before building anything on
   top of it.
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
   hold, but JP3/JP4 remain the cheap insurance.

   Impact is limited: the ImageWriter talks back over HSKo hardware handshaking rather than
   the serial receive path, so a wrong receiver polarity does not block M1 or M5. Bias
   resistors R6/R7 already pull A high and B low, which matches the corrected mapping.
4. **HSKo electrical levels.** Assumed RS-232-ish single-ended against ground, which the
   MAX3232 receiver handles. Scope it before trusting it.
5. **URF 32-byte page header field offsets.** Take these from cups-filters source, not from
   memory.

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
