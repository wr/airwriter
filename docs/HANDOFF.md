# AirWriter handoff

Written 2026-08-04. Everything below reflects the repo at that date.

## Where the project stands

**Hardware rev A is finished and not yet ordered.** The board routes clean, the fab package is
generated, and the mini-DIN is settled — Kycon KMDGX-8S-BS from Mouser. Nothing blocks the order.

**Six of the nine firmware components exist and are tested.** `urf_decode`, `resample`,
`halftone` and `imagewriter` do the raster path; `ipp_attrs` and `ipp_server` do AirPrint.
All six build for the ESP32 and run there — ESP-IDF v5.5.1 is installed and the project
flashes. 569 checks pass on the host. Still to write: `serial_out`, `printer_state`,
`net_provision` and `webui`, all of which want hardware or are M6.

| | State |
|---|---|
| Schematic | Complete, ERC clean, zero errors |
| PCB | Routed, 52 x 78mm, DRC clean, schematic parity clean |
| Fab package | Gerbers, drill, BOM, pick-and-place in `hardware/airwriter/fab/` |
| Firmware | M2 and M4 done: raster pipeline and AirPrint, 569 checks green |
| Devkit | ESP32-C6 stands in for the board; it discovers, accepts jobs and prints from an iPhone |
| Boards ordered | No |

Linear project **AirWriter** on the Personal team tracks this. W-562 through W-567 are milestones
M1–M6 from the spec; **W-568** is the board and carries the full decision log in its comments.

## What the board is

An ESP32-S3 dongle with two mini-DIN-8 ports and a USB-C power inlet.

- **Printer port** drives an ImageWriter II over RS-422 at 9600 baud with DTR handshaking. This is
  the reason the project exists.
- **LocalTalk port** puts the board on an AppleTalk network as a node, using a PIC12F1840 running
  Tashtari's TashTalk firmware as the LocalTalk data-link engine.
- **AirPrint** runs over WiFi, concurrently with either.

Three modes were scoped. Mode 1 is AirPrint only. Mode 2 adds EtherTalk over WiFi and needs no
hardware, only firmware. Mode 3 adds the physical LocalTalk port. **The board is built for mode 3**,
so all three are reachable — but only one AppleTalk transport can be live at a time unless someone
writes an AppleTalk router. AirPrint is IP and runs alongside any of them.

## Deviations from the spec, and why

`docs/SPEC.md` has been amended in place where these apply. Read it as the current design, not the
original draft.

**The receiver polarity in spec §2.4 was wrong.** The draft mapped transceiver A to the printer's
TxD- and B to TxD+. Apple's convention is that a binary 1 has TxD- more negative than TxD+, so the
`+` line is non-inverting. With A low at mark, the receiver would idle low and every byte would
arrive inverted. Corrected to **A ← pin 6 (TxD+), B ← pin 3 (TxD-)**. Confidence is good but not
certain: both sources document the Macintosh serial port rather than the printer, and they are
secondary. The ImageWriter II Technical Reference would settle it.

**The polarity jumpers are gone.** The spec wanted solder jumpers so the differential pairs could be
swapped without a respin. The ESP32 UART does the same job in firmware — `uart_set_line_inverse()`
with `UART_SIGNAL_TXD_INV` / `UART_SIGNAL_RXD_INV`. Inverting DI flips Y and Z together, which is
electrically identical to swapping the pair, and the same holds for RO. Four parts and eight pads
saved, insurance kept.

**U2 is a MAX3490, not a MAX3491.** The spec tied DE high and RE low permanently, so the enable pins
were dead weight on the printer port.

**SW1 is an Omron B3U-1000P.** The closest KiCad stock footprint to the spec's "4.5mm tactile" was
the B3S-1100, whose courtyard is 10 x 8.9mm. That had become the binding constraint on the pocket
beside the mini-DIN.

**R9 added**, a DNP 0R in series between the MAX3232's T1OUT and the printer's HSKi. The spec wanted
that path wired but not populated; a DNP series resistor expresses that without U3 driving HSKi by
default.

**Four parts are fitted only if the printer port misbehaves.** They are the bare pad pairs in the row
under U3 and U2, and they are marked DNP in both the schematic and the board.

| | Value | What it does | Fit it when |
|---|---|---|---|
| R5 | 120R | Differential termination across the printer's TxD pair, at U2's receiver | The cable is long enough to ring. At 9600 baud on a printer cable it only loads the line |
| R6 | 4.7k | Failsafe bias, +3V3 to TxD+ | The receiver reads garbage with the printer off or unplugged |
| R7 | 4.7k | Failsafe bias, TxD- to GND | Same — R6 and R7 are a pair, fit both or neither |
| R9 | 0R | Series jumper from the MAX3232's T1OUT to the printer's HSKi pin | You decide the board should drive HSKi rather than only listen on HSKo |

**The BOM had three parts wrongly marked DNP.** It grouped rows by value and footprint but not by the
DNP flag, so R8 landed in the same row as R9 and R18/R19 landed in the same row as R6/R7 — and the
row took the DNP label. An assembler working from it would have left the printer port's shield
ungrounded and the I2C bus without pullups. Regenerated with DNP in the group-by. The pick-and-place
file is now exported with `--exclude-dnp`, so it lists only parts that should actually be placed: 53
of the 57.

**Nine board footprints carried placeholder values** — R17 read "R17", C19 read "C19", J6 read "J6" —
because the board was never fully re-synced from the schematic. Those show up in the pick-and-place
file's value column, so an assembler would have seen "R18" where "4.7k" belongs. Synced against the
BOM.

**Board is 52 x 78mm, not 55 x 35.** The aspect flipped to portrait because the WROOM-1 wants its
antenna overhanging a short edge while both mini-DINs and the USB-C need the opposite one. It grew
from 32 x 54 when the LocalTalk port was added, to 66 tall when the indicators, the buttons and
the I2C header needed a band of their own, and to 78 for the artwork band below. Growing it sideways
was tried first and undone: the ports live on one edge, so the room has to come from the axis they
sit on.

**A blank band runs across the board at y 86 to 98, for a logo and whatever else wants silkscreen.**
Roughly 50 x 12mm, clear of copper on both layers apart from stitching vias, sitting between the
ESP32 row and the transceiver row. The AirWriter wordmark sits just above it at y 82 to 86. Adding
the band is what took the board from 66 to 78mm tall: every footprint below y=87 moved down 12mm and
nothing above it moved, so the antenna keepout, the front connectors and their 2mm proud stand are
all untouched.

C9 and C14 came down with the band. They bypass U2 and U3, and leaving them in the top row would
have put them 22mm from the chips they serve. They now sit at the top edge of the lower block,
about 5mm from their pins — closer than before the board grew.

**The three internal connectors carry function labels, not pinouts.** J3 is `USB`, J5 is `ICSP`,
J6 is `I2C`. Per-pin labels do not fit: J3 has 1.35mm between the board edge and U1's pads, and J6's
1mm pitch cannot hold text at the 0.8mm minimum height with legal spacing. J6 is wired in Qwiic pin
order — GND, 3V3, SDA, SCL — so the word is the pinout for anyone with a Qwiic cable.

**The mounting holes are not grounded, deliberately.** A ground ring buys nothing in a printed
plastic case, and it would create a second path to ground that cannot be broken. The mini-DIN shells
already reach GND through R8, which is a 0R precisely so that tie can be lifted; the ImageWriter II
is mains-powered with a metal chassis, so a second, unbreakable ground path is exactly what puts
noise on the serial link. The holes stay non-plated.

**All three front connectors stand 2mm proud of the board edge**, so their faces sit flush with the
outside of a 3mm-wall printed case rather than with the PCB. Move them together or not at all. They were not coplanar with each other
before either — the mini-DINs stood 1.25mm proud while the USB-C sat 0.55mm back. The mini-DIN
origin sits 2.75mm past the edge; the USB-C origin 2.15mm short of it.

**Resistor and capacitor designators live on F.Fab; everything else is on the silkscreen.** Those
two classes are the dense ones, and their silk text was the source of 61 of the 75 DRC violations
the board carried. They still print on the assembly drawing, so pick-and-place and hand assembly
lose nothing. Chips, connectors, LEDs, buttons and mounting holes keep silk designators, and the
parts a user touches carry a function label above and the designator below: PWR, NET, JOB over the
LEDs, BOOT and MODE over the buttons.

**The placement is block-per-function, not row-per-part-type.** The first layout put every passive
into three long rows across the middle of the board regardless of what it served — USB-C CC
pulldowns 40mm from the connector, RS-232 charge-pump caps far from the MAX3232 — while the whole
bottom-right quadrant sat empty. Each block now sits with its chip:

| Block | Where | Parts |
|---|---|---|
| ESP32 rail and EN | directly under U1 | C4 C5 R3 C6 |
| LocalTalk digital | top right | U7 PIC, C17, R14, R15, R16, J5 ICSP |
| I2C expansion | right edge, top | J6, R18, R19 |
| Printer analog | left, above J2 | U3, U2, C9–C15, R5–R9 |
| LocalTalk line drivers | centre, above J4 | U6, U8, C16, C18, R10–R13 |
| Power chain | right, above J1 | C2, C3, U4, U5, C1, R1, R2 |
| UI strip | band above the ports | D3 D1 D2, SW1 SW2, R17, C7, C8, C19, C20 |

Moving U6 and U8 down beside J4 cut the four LocalTalk differential runs from about 30mm to about
14mm. Nothing enclosure-critical moved: U1, J1, J2 and J4 are exactly where they were.

**The four mounting holes are a symmetric 45 x 50mm rectangle.** H2 used to sit at x=161.5, which is
9.5mm off the right edge of a board that ends at 152 — it was outside the outline entirely, so rev A
had three usable holes and one that would have been drilled into nothing.

**The ground pour is stitched, with 44 vias, not the 82 a blanket grid wanted.** Freerouting never
stitches, so a fresh route leaves the front pour cut into 17 fragments. `stitch.py` places one via
per fragment first, at the roomiest point inside it, then fills in a 5mm lattice. The counts, all
measured on this board:

| stitching vias | fragments left unjoined |
|---|---|
| 0 | 14 |
| 8, one per fragment | 8 |
| 22, blanket 5mm grid | 12 |
| 44, one per fragment plus a 5mm grid | 8 |
| 82, blanket 2mm grid | 9 |

Targeted placement is what does the work; a blanket grid mostly drops vias into fragments that were
already stitched and misses the small ones. The 8 that remain are slivers too small to take a 0.6mm
via with clearance — each is tied to GND through its own pads, so they are harmless. The grid pass
is there for return-path continuity under the 2.4GHz module, not for connectivity, and it is free:
0.3mm is the same drill tool the board already uses.

`stitch.py` has no undo — pcbnew's `Remove()` on a via does not survive a `Save`, so running it
twice just accumulates vias. To re-stitch, strip every segment, via and arc from the `.kicad_pcb`,
run `route.py`, then run `stitch.py`.

## The unverified assumptions, current status

Spec §4 lists five. Three are now resolved, all from primary sources.

1. **Bit order within a graphics byte** — **resolved.** Bit 0 is the top pin, bit 7 the bottom,
   read off the ImageWriter II Technical Reference p. 107 and independently confirmed against
   Ghostscript's `gdevadmp.c`. A reviewer re-checked it against the scan. Implemented in
   `imagewriter.c`.
2. **Density escape codes** — **resolved.** `ESC p` is 144 dpi at 1152 dots; `ESC P` is 160 dpi.
   Table 8-2, p. 106, cross-checked against `gdevadmp.c` and re-verified against the scan by a
   reviewer. The full table is in `SPEC.md` §4.
3. **Differential polarity** — **resolved by research**, and it found the spec error described above.
   Not yet confirmed against hardware.
4. **HSKo electrical levels** — open. Scope it before trusting the MAX3232 receiver assumption. If the
   swing turns out modest, a divider and clamp into a GPIO could delete U3 entirely.
5. **URF page header field offsets** — **resolved.** Taken from `cups/raster-stream.c` in
   OpenPrinting/cups, not from memory, and implemented in `urf_decode.c` with the offsets cited
   in comments. Still unconfirmed against a real iOS capture — see below.

## Traps that cost real time

These all bit at least once. Several bit twice.

**KiCad silently reverts file edits.** If a KiCad session was open before an external edit, saving
from KiCad writes its stale copy back. This ate the board outline once and the design rules twice.
Always close KiCad before editing files, and re-verify afterwards.

**Two konnect MCP tools corrupt this project.** `set_design_rules` writes constraints into the
`.kicad_pcb` setup block, but KiCad 10 reads them from `.kicad_pro` — the result is duplicate keys
and an unloadable board. `set_board_size` appends an outline instead of replacing it, leaving two
overlapping rectangles on Edge.Cuts. Edit `airwriter.kicad_pro` directly for rules; rebuild the
outline by hand.

**Freerouting hangs on any DSN KiCad exports, and the cause is fractional coordinates.** KiCad
writes coordinates with decimals while declaring `(resolution um 10)`; Specctra wants whole units.
Freerouting turns them into exact rationals, the numerators explode in `Line.intersection`, and it
spins at 200% CPU inside `BasicBoard.normalize_traces` forever, still printing "Opening ...".
Rounding every number in the `.dsn` to an integer drops the load from never to 0.9 seconds.
`route.py` does this for you. This is also why `check_freerouting` appeared to wedge the MCP
server — same root cause, not a tooling quirk. Recovery is `pkill -f freerouting.jar`.

**Freerouting also blows up re-reading its own previous output.** Same arithmetic, so `route.py`
drops the existing wiring and routes the whole board from scratch each time; a full route takes
about 15 seconds. `--keep-wiring` routes only what is missing but will hang on a board Freerouting
has already routed.

**Freerouting never repours zones.** DRC without `--refill-zones` showed 278 violations that dropped
to 3 once refilled. Always pass it.

**`add_zone` created the ground pour on net `"GND"` while every pad is on `"/GND"`.** KiCad uses the
hierarchical name with a leading slash. The pour was floating copper for its entire life and DRC
reported it as 28 isolated-copper islands rather than as a disconnected zone. Check zone nets by
string, not by eye.

**Footprint zones store absolute coordinates.** Moving a footprint by editing `(at ...)` in a script
moves the pads but strands any keepout polygon. The WROOM-1's antenna keepout sat 46mm off the board
until corrected, so DRC had never been enforcing it.

**konnect's placement tools resolve `lib_id` only against KiCad's install directory**, ignoring both
project and global library tables. The project libraries are symlinked into the app bundle as a
workaround. Those symlinks vanish on a KiCad upgrade.

**A generated `.kicad_mod` contained unescaped double quotes** inside its `(descr ...)` string, which
makes the s-expression malformed. KiCad failed to load the footprint and dropped the part from the
board without complaint. Balance-check any generated library file.

**KiCad 10 removed Specctra DSN export and SES import from `kicad-cli`** — but not from the
`pcbnew` Python module, which still has `ExportSpecctraDSN` and `ImportSpecctraSES`. Routing is
therefore fully automatable; run `hardware/airwriter/route.py` with the interpreter inside the app
bundle and it does export, round, route, import, and DRC in one go, no GUI:

```
/Applications/KiCad/KiCad.app/Contents/Frameworks/Python.framework/Versions/3.9/bin/python3 \
    hardware/airwriter/route.py --passes 30
```

**A footprint's rotation does not rotate its pad shapes.** Each pad carries its own angle in
`(at x y ANGLE)`, and KiCad writes the footprint rotation into it. A generated footprint that omits
the angle places its pads correctly but leaves every pad shape unrotated — on J6 that turned a
1mm-pitch connector into four shorted pads. Set the angle on every pad of a rotated footprint.

## Custom library

`hardware/airwriter/lib/` holds three things KiCad does not ship.

- **`MiniDIN-8_Kycon_KMDGX-8S-BS_RightAngle`** — KiCad has no mini-DIN footprint at all. Geometry
  read off Kycon engineering drawing KMDGX-xS-BS rev C6 (ECO#18-013), 8-position "Recommended PCB
  Layout (Top View)", cross-checked against three dimension chains. **Pin 7 sits at +0.90mm from the
  centreline while pin 2 directly above it sits at +1.30mm** — the lower row is deliberately
  asymmetric and easy to get wrong by eye. Kycon's own STEP model, fetched later, has the same
  asymmetry, which is independent confirmation the footprint is right.
- **`lib/airwriter.3dshapes/MiniDIN-8_Kycon_KMDGX-8S-BS_RightAngle.step`** — Kycon's official model,
  from `kycon.com/website/Extranet/3D/KMDGX-8S-BS_rC6.STEP`. That Extranet path is not linked from
  the product page; it was found through a search index. Attached to the footprint at
  `rotate -90 0 180`, offset zero. Verified by rendering the bottom of the board: all eight pins and
  all three posts land centred in their holes. Without it the board's STEP export had two holes
  where the ports should be.
- **`lib/airwriter.3dshapes/USB_C_Receptacle_HRO_TYPE-C-31-M-12.step`** — KiCad 10 ships this
  footprint but not its model, and the stock footprint points at a `${KICAD10_3DMODEL_DIR}` path
  that does not exist, so J1 vanished from the 3D export without any warning. Upstream
  `kicad-packages3D` does not have it either; KiCad issue #22139 tracks it. Taken from
  NabuCasa/yellow-3dshapes, which processed the vendor model through KiCad StepUp. That repo
  carries no licence file — check provenance before publishing the hardware.
  The model's origin is a corner, not the footprint centre, hence `rotate -90 0 0` with
  `offset -4.47 -3.65 0`. Derived by matching the four shield tabs (8.64mm apart, rows 4.18mm
  apart) and the two 0.65mm legs (5.78mm apart) to the pads, then confirmed by render: all six
  through-board features seat in their holes, and the asymmetric shield-tab pattern rules out a
  180-degree error.

**J1's model lives on the board, not in a library.** The footprint comes from KiCad's stock
`Connector_USB`, so re-importing it from the library will restore the dead `${KICAD10_3DMODEL_DIR}`
path and J1 will silently disappear from the STEP export again. Same class of trap as U1's vias.

**D1 has no 3D model.** `LED_WS2812B-2020_PLCC4_2.0x2.0mm.step` does not exist in KiCad 10 or
upstream, so the WS2812B is absent from the export. It is a 2.0 x 2.0 x 0.8mm block; it matters only
if the enclosure needs a light pipe over it.

Before trusting a STEP export, check every model path resolves — a missing one is silent:

```
python3 - <<'EOF'
import re, os
b = open('hardware/airwriter/airwriter.kicad_pcb').read()
base = '/Applications/KiCad/KiCad.app/Contents/SharedSupport/3dmodels'
proj = 'hardware/airwriter'
for p in re.findall(r'\(model "([^"]+)"', b):
    real = p.replace('${KICAD10_3DMODEL_DIR}', base).replace('${KIPRJMOD}', proj)
    if not os.path.exists(real): print('MISSING', p)
EOF
```
- **`Mini-DIN-8_Shielded`** — the stock symbol has no shield pin, so there is no way to net the shell.
  Pin 9 is SHIELD, tied to all three mounting posts in the footprint.
- **`SN65HVD1473DGSR`** — reused with credit from FeralFirmware/TailTalk. No longer placed on the
  board after the transceiver swap, but kept because it is the part TashTalkUSB ships.

## Sourcing

**Fab is PCBWay, parts are China-sourced.** Price against LCSC, not Digi-Key — the gap is large
enough to change design decisions. MAX3232 is $9.98 at Mouser and $0.91 at LCSC. MAX3490 is $6.61 at
Digi-Key and $0.687 at LCSC.

Order the board as **52 x 78mm, 2 layer, 1.6mm FR4, 1oz copper, HASL**. Nothing needs a premium
process.

**Protect the 0.30mm drill floor.** PCBWay's standard minimum is 0.30mm and 0.20mm is an upgraded
spec — a documented case went from $5 to $65 for ten boards. KiCad's stock ESP32-S3-WROOM-1 footprint
puts twelve 0.20mm thermal vias in its ground pad, and they were enlarged to 0.30mm **in the board
file, not the library**. Re-importing U1's footprint will silently put them back. That is also why U1
shows a library mismatch in DRC. Before every order:

```
kicad-cli pcb export drill --output fab/ --format excellon airwriter.kicad_pcb
grep -E '^T[0-9]+C' fab/airwriter.drl        # nothing below 0.300
```

**Every named part sources. Prices and stock, LCSC unless noted, verified 2026-08-05:**

| Ref | Part | Price | Stock |
|---|---|---|---|
| U1 | ESP32-S3-WROOM-1-N8R2 | $3.75 | 9,842 |
| U2 | MAX3490ESA+T (printer port) | $0.687 @10 | 3,788 |
| U3 | MAX3232ESE+T | $0.91 | 7,956 for the plain +, the +T line is its own SKU |
| U4 | AP7361C-33E-13 | $0.52 @10, $0.40 @100 | 1,526 |
| U5 | USBLC6-2SC6 | $0.099 ST, $0.018 UMW clone | good |
| U6, U8 | SP3485EN-L/TR | $0.21 each | 37,970 |
| U7 | PIC12F1840-I/SN | $0.70 | good |
| D1, D2 | WS2812B-2020 | $0.092 @10 | 35,115 |
| J1 | TYPE-C-31-M-12 | $0.096 | good |
| J2, J4 | Kycon KMDGX-8S-BS mini-DIN | ~$2.50 (Mouser) | 1,032 — Digi-Key is the backordered one |
| J6 | SM04B-SRSS-TB(LF)(SN) | $0.13 | 60,960 |
| SW1, SW2 | Omron B3U-1000P | $0.128 | 112,590 |

That is about **$13 of parts per board**, and $5 of it is the two mini-DINs. Passives are generic
0603 and 0805 with no MPN in the BOM — buy whatever is cheap and in stock.

**J3 and J5 are the only awkward line.** LCSC does not stock 1.27mm single-row male headers as 1x4
or 1x5; it stocks strips. Buy `C28952` (1x10P, $0.025) or `C3408` (1x50P, $0.123) and cut. Neither
header has to be fitted for the board to work: J3 is bring-up only, and the PIC can be programmed
through J5's pads with pogo pins instead of a soldered header.

**U2 and U3 carry the `+T` tape MPNs, not the plain `+`.** Same silicon; the plain MAX3232ESE+ is
$1.70 at LCSC against $0.91 for the +T. Cut tape is what a small order gets anyway.

**The WS2812B-2020 footprint is right despite its name.** `LED_WS2812B-2020_PLCC4_2.0x2.0mm` draws a
2.2 x 2.0mm body, which is the real package — LCSC lists it as 2.2x2mm too. The pin order matches the
chain as wired: 1 DOUT, 2 GND, 3 DIN, 4 VDD, with D1's DOUT feeding D2's DIN.

**The mini-DIN is settled: Kycon KMDGX-8S-BS, bought from Mouser.** The part was only ever a
problem because Digi-Key is backordered on it; Mouser holds 1,032 and ships immediately. It is the
exact part the footprint was drawn from and cross-checked against Kycon's own STEP model, so the
geometry risk is zero. Buy six — you need two, it is a through-hole part you can damage, and the
spares cost about $2.50 each.

**Solder them by hand rather than having PCBWay fit them.** PCBWay keeps no parts library and buys
every component per order, so a through-hole connector becomes a manually quoted line item with a
5-10 day sourcing delay. Twenty-two joints across two large, well-anchored parts is a few minutes
with an iron, it keeps the connector off the critical path of the board order, and a wrong part
then costs $5 instead of a scrapped assembly run.

The alternatives were checked and rejected, so nobody needs to re-do this:

- **Kycon KMDGX-8S-AS** — in stock at Digi-Key, but it is a "Board Guide, Board Lock" push-pull
  variant and Digi-Key does not list it as a substitute for the -BS. Different mounting scheme.
- **Adam Tech MDJ-008-FS** — a genuine drop-in; its drawing matches all eight signal positions and
  the ±6.75 posts. Out of stock at Digi-Key. Keep as the fallback if Kycon ever dries up.
- **Same Sky (ex-CUI) MD-80SM** — signal pads match, mounting does not. The shield tabs are
  rectangular slots and there is a 2.5 x 1.0mm earth blade where the footprint has a ø2.3 round
  hole; a 2.5mm blade has a 2.69mm diagonal and will not fit. MD-80S is worse — unshielded, no
  side posts. The spec's old suggestion of MD-80S was wrong.
- **HOOYA / Legion DIN-802 (LCSC C20611694 / C41409494)** — cheapest by far and 780 in stock at
  HOOYA, but the pads sit 0.15-0.20mm out almost everywhere and the centre feature is a 1.5 x 3.0
  oval slot rather than a round hole. Not usable without redrawing the footprint.

**The pin-numbering discrepancy is resolved: our footprint is correct and the DIN-802 one is
mirrored.** Derived from the socket-face view (the mirror of the plug convention) combined with
KiCad's +Y-downward frame, it predicts all eight pad positions including the 0.90/1.30 asymmetry.
The DIN-802 footprint transposes pins 1 and 2, puts 3 on the wrong side, and places centre pin 4
in the front row with outer pin 5 behind it, which is backwards for a right-angle fan-out.

Worth knowing: **the 0.90mm asymmetry is an industry pattern, not a Kycon quirk.** Adam Tech and
Same Sky both show 0.90 against 1.30 on their 7- and 8-pin layouts and neither shows it on their
6-pin ones. It is the extra lower-row pin offset to clear the one above it.

Note that full-duplex RS-422 transceivers are a niche part and keep going out of stock on LCSC —
ISL3177EIBZ and MAX3491ESD+ were both zero. Half-duplex RS-485 is commodity. That is why the
LocalTalk transceiver is two half-duplex parts rather than one full-duplex one.

## Projects this builds on

**TashTalk** (lampmerchant) — a LocalTalk data-link engine that fits in a PIC12F1840, bit-banging
SDLC and FM0 framing and presenting it over a 1 Mbps UART with CTS flow control. The board carries
one running his firmware. It needs flashing; that is what J5 is for.

**TailTalk / TashTalkUSB** (FeralFirmware) — the LocalTalk section's wiring was copied from
TashTalkUSB's KiCad netlist node for node rather than derived from prose. TailTalk itself is a
userspace AppleTalk toolkit for desktop OSes and is the best reference for the protocol work ahead.

**AirTalk** — LocalTalk over WiFi dongles, ESP32-based. Relevant if mode 2 or a WiFi tunnel is ever
built; you would need to match its wire protocol to interoperate.

**ImageScribbler** (polpo) — a 1:1 clone of Apple's ImageWriter II/LQ LocalTalk Option Card, running
Apple's original ROM on a 65C02 and Z8530. CC0, and JCM-1 sells it assembled. It does what mode 3
does, better and with zero protocol risk, but needs vintage silicon and goes inside the printer.
**Its ROM image (341-0034-B) is publicly archived and is the authoritative answer to what PAP
actually delivers to an ImageWriter** — the question gating all the AppleTalk work. Disassembling it
serves mode 2 and mode 3 equally.

## What to do next

**The iPhone capture is done.** `fixtures/ios-20260804-232652.urf` came off a real phone through
`tools/ippcap`, and iOS honoured `RS72-144` — it rendered at exactly 144 dpi rather than sending
300, so no downsample fallback is needed and `RS300` need not be advertised. The capture is in
`make regress` and `test/test_ios_capture.c` asserts its header fields, so assumption 5 now has a
real-world check. 249 checks green.

**M4 is done and was verified from a real iPhone.** The phone found the printer in its print
sheet and sent a job, which arrived complete. CUPS `ipptool` passes `get-printer-attributes`
conformance, and a Print-Job carrying the captured fixture moved all 143,470 bytes in 240ms.

**M5 is next and needs no board.** Swap the byte-counting sink in `main.c` for the real chain —
`urf_decode` → `resample` → `halftone` → `imagewriter` — and write the printer stream to flash
or the console instead of a UART. That makes the whole pipeline verifiable on the devkit, and
what remains for the real board is only `serial_out`: the UART, the RS-422 levels and the DTR
handshake. `printer_state` and the M1/M3 work are the parts genuinely gated on boards, which
are not ordered.

**Printer scope is settled.** See `docs/superpowers/specs/2026-08-04-printer-support-scope-design.md`.
Short version: v1 is the ImageWriter II over serial and nothing else. StyleWriter and
ImageWriter LQ are cut on consumables. LocalTalk is built second, because its job goes out
through the serial port and that has to work first. Two of the five unverified assumptions
in `SPEC.md` §4 are now resolved from Apple's technical reference.

**Settle the mini-DIN.** It blocks ordering and you need two.

**Then M2, which needs no hardware and no ESP-IDF.** The spec calls `tools/rip` the highest-value
item for development speed, and it is a prerequisite for every mode:

1. Stand up a stub IPP server that answers `Get-Printer-Attributes` and dumps a `Print-Job` body to
   disk. Point an iPhone at it and commit the result under `fixtures/`.
2. Port the PackBits-style RLE from cups-filters (`rastertopwg.c`, `urftopdf.cpp`) and cite the source
   file in a comment. Do not derive it from first principles.
3. Build `urf_decode`, `halftone` and `imagewriter` as a host binary. `stream.bin` diffs
   byte-for-byte between changes, which is the whole point.

**In parallel:** install ESP-IDF 5.x, which is not on this machine and is needed for everything after
M2; and pull the option-card ROM to start reading the PAP side.

**Then the milestones in order** — M1 wire, M3 dots, M4 discovery, M5 print. M5 is the first video.
Mode 3's AppleTalk stack comes after that; it is weeks of work and gated on the PAP question.

## Toolchain

`kicad-cli` is not on PATH: `/Applications/KiCad/KiCad.app/Contents/MacOS/kicad-cli`, version 10.0.2.

Freerouting 2.2.4 is at `~/.local/share/freerouting/freerouting.jar`, Java 25 is present. To route:
export Specctra DSN from the PCB editor, run
`java -jar ~/.local/share/freerouting/freerouting.jar -de board.dsn -do board.ses`, import the
session. Then DRC with `--refill-zones`.

ESP-IDF **v5.5.1 is installed** at `~/esp/esp-idf`, with tools for both esp32s3 and esp32c6.
Source `. ~/esp/esp-idf/export.sh` before any `idf.py`. `sdkconfig.defaults` is split per
target because the devkit on hand is a C6 and the board is an S3; the C6 has no PSRAM, which
makes it the stricter of the two for memory.

WiFi credentials live in the gitignored `sdkconfig` (`idf.py menuconfig`, AirWriter menu) and
are overridden by anything in the `wifi` NVS namespace.
