# AirWriter hardware

KiCad 10 project. Two-layer, 1.6mm FR4, 1oz copper, HASL.

**Board is 32 x 54mm.** The spec targeted 55 x 35mm (1925mm²); this is 1728mm².
Three parts set the floor and none of them can shrink:

- the ESP32-S3-WROOM-1 needs 18 x 19.5mm of board with its antenna overhanging the top edge
- the mini-DIN body reserves the entire bottom 13.5mm
- the USB-C receptacle sits beside it on the same edge

## Status

| Stage | State |
|---|---|
| Schematic | Complete. ERC clean, zero errors. 52 footprints, both ports. |
| Routing | Complete. 434 segments, 34 vias. **Every component pad connected.** |
| DRC | 5, all cosmetic. 15 unconnected, all pour-fragment to pour-fragment, none involving a pad or track. |
| Fab outputs | Gerbers, Excellon drill + map, BOM, pick-and-place, render, in `fab/`. |

Board is **52 x 54mm**, two layers. Bottom edge carries the printer mini-DIN, the
LocalTalk mini-DIN and USB-C. BOM is 44 populated parts plus 4 DNP.

### What DRC still reports, and why it is fine

- 2 silkscreen-over-edge on U1. The module deliberately overhangs the top edge.
- 3 library metadata mismatches. J2 and J4 because the mini-DIN footprint was injected
  into the board by hand; U1 because its thermal-via drills were deliberately enlarged.
- 15 "unconnected" between GND pour fragments. Every fragment is anchored to GND through
  an adjacent pad or via, so this is zone contiguity rather than floating copper. **No pad
  and no track appears in the unconnected list.**

## Modes

The board supports three, and only the third needed hardware:

1. **AirPrint only** — IPP over WiFi to the printer's serial port.
2. **AirPrint + EtherTalk** — pure firmware, no hardware change. Not yet written.
3. **AirPrint + LocalTalk** — the second mini-DIN, the PIC running TashTalk, and the split
   transceiver. This is AirWriter acting as the AppleTalk option card the ImageWriter never
   had. Firmware not yet written.

Only one AppleTalk transport can be active at a time unless you write an AppleTalk router.
AirPrint runs concurrently with any of them because it is IP.

## Routing

Routed with Freerouting 2.2.4. It cannot be automated: KiCad 10 dropped Specctra DSN export and
SES import from `kicad-cli`, so the round trip is GUI-only. Freerouting lives at
`~/.local/share/freerouting/freerouting.jar` and Java 25 is present. To re-route:

1. Open the PCB, File > Export > Specctra DSN
2. `java -jar ~/.local/share/freerouting/freerouting.jar -de board.dsn -do board.ses`
3. File > Import > Specctra Session

**After any re-route, always run DRC with `--refill-zones`.** Freerouting does not repour the GND
zone, and an unfilled zone produces hundreds of phantom clearance errors — 278 violations that
drop to 3 once refilled.

## Fab: PCBWay

Order as: **32 x 54mm, 2 layer, 1.6mm FR4, 1oz copper, HASL**. Nothing here needs a premium
process.

The design is held to PCBWay's cost-effective tier on purpose:

| Parameter | This board | PCBWay |
|---|---|---|
| Min drill | 0.30mm | 0.30mm standard. **0.20mm is an upgraded spec and costs a lot more** |
| Min track / space | 0.20mm actual, 0.15mm rule | 0.076mm possible, 0.15mm recommended to save cost |
| Copper to board edge | 0.2665mm worst case | under 0.20mm gets flagged |
| Via | 0.6mm pad / 0.3mm drill | standard |

**The 0.30mm drill floor is the one to protect.** KiCad's stock ESP32-S3-WROOM-1 footprint puts
twelve 0.20mm thermal vias in its ground pad, which would drop the whole board into PCBWay's
advanced tier — a documented case went from $5 to $65 for ten boards. Those twelve drills were
enlarged to 0.30mm in the board file. The 0.6mm pad still leaves a 0.15mm annular ring.

That edit lives in the board, not in KiCad's library, so **re-importing U1's footprint from the
library will silently put the 0.20mm drills back**. It is also why U1 shows a library mismatch in
DRC. Check the drill table before every order:

```
kicad-cli pcb export drill --output fab/ --format excellon airwriter.kicad_pcb
grep -E '^T[0-9]+C' fab/airwriter.drl     # nothing below 0.300
```

BOM carries manufacturer part numbers for the actives and connectors. Passives are specified by
value, package, dielectric and voltage with "or equivalent", which PCBWay sources normally.

## Toolchain notes

`kicad-cli` is not on PATH: `/Applications/KiCad/KiCad.app/Contents/MacOS/kicad-cli` (10.0.2).

Three traps hit while building this, all worth remembering:

- **`set_design_rules` (konnect MCP) corrupts this board.** KiCad 10 keeps DRC constraints in
  `.kicad_pro`, but the tool writes them into the `.kicad_pcb` setup block, duplicating keys and
  making the board unloadable. Edit `airwriter.kicad_pro` instead.
- **`set_board_size` appends an outline rather than replacing it.** Calling it twice leaves two
  overlapping rectangles on Edge.Cuts.
- **The konnect placement tools resolve `lib_id` only against KiCad's install directory**, ignoring
  project and global library tables. The project libraries are symlinked into the app bundle as a
  workaround; those symlinks vanish on a KiCad upgrade and need recreating.

`min_through_hole_diameter` is set to 0.2mm because the WROOM-1 footprint uses 0.2mm thermal vias
in its ground pad. That is within JLCPCB capability.

## Deviations from the spec

- **U2 is a MAX3490ESA+ (SOIC-8), not a MAX3491ESD+ (SOIC-14).** The spec tied DE high and RE low
  permanently, so the MAX3491's enable pins were dead weight. The MAX3490 is the same full-duplex
  RS-422 transceiver without them, and it ships with a verified pinout in KiCad's stock library.
- **No polarity jumpers at all.** The spec wanted them so the RS-422 pairs could be swapped
  without a respin. The ESP32 UART does the same job in firmware — `uart_set_line_inverse()`
  with `UART_SIGNAL_TXD_INV` / `UART_SIGNAL_RXD_INV`. Inverting DI flips Y and Z together,
  which is electrically identical to swapping the pair. Four parts saved, insurance kept.
- **Receiver polarity corrected.** Spec 2.4 mapped A<-TxD- and B<-TxD+, which inverts every
  received byte. Apple's convention is that a binary 1 has TxD- more negative than TxD+, so
  the + line is non-inverting: A<-TxD+ (pin 6), B<-TxD- (pin 3).
- **R9 added**, a DNP 0R in series with the MAX3232 T1OUT to HSKi path. The spec wanted T1 wired
  but not populated; a DNP series resistor expresses that without U3 driving HSKi by default.
- **SW1 is a B3U-1000P**, not the 4.5mm tactile the spec assumed. The B3S-1100 KiCad offers has a
  10 x 8.9mm courtyard which had become the binding constraint on the pocket beside the mini-DIN.

## Custom library

`lib/airwriter.kicad_sym` and `lib/airwriter.pretty` hold the two parts KiCad does not ship:

- `MiniDIN-8_Kycon_KMDGX-8S-BS_RightAngle` — KiCad has no mini-DIN footprint at all. Geometry taken
  from Kycon engineering drawing KMDGX-xS-BS rev C6 (ECO#18-013), 8-position "Recommended PCB
  Layout (Top View)". Note pin 7 sits at +0.90mm from the centreline while pin 2 directly above it
  sits at +1.30mm — the lower row is deliberately asymmetric.
- `Mini-DIN-8_Shielded` — the stock symbol has no shield pin, so there is no way to net the shell
  for the 0R + 1nF star ground. Pin 9 is SHIELD, tied to all three mounting posts.

## Still to do

1. Add LCSC/Digi-Key ordering codes to the BOM. It currently carries values and footprints only.
2. Optional: 3mm corner radii, which the spec asked for and the outline does not yet have.

## Traps that cost real time here

- **The GND zone was on net `"GND"` while every pad is on `"/GND"`.** KiCad uses the hierarchical
  name with a leading slash. The pour was floating for its entire life and DRC reported it as 28
  isolated-copper islands rather than as a disconnected zone. Check zone nets by string.
- **Footprint zones store absolute coordinates.** Moving a footprint by editing `(at ...)` in a
  script moves the pads but strands any keepout polygon. The WROOM-1 antenna keepout sat 46mm off
  the board until corrected, so DRC was never enforcing it.
- **Freerouting does not repour zones.** Always DRC with `--refill-zones`.

## Trademarks

AirPrint, AppleTalk, LocalTalk, EtherTalk and ImageWriter are trademarks of Apple Inc.
AirWriter is an independent project and is not affiliated with, authorised, sponsored or
endorsed by Apple Inc. These marks are used only descriptively, to state what the board
interoperates with.
