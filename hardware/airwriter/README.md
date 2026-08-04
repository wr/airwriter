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
| Schematic | Complete. ERC clean, zero errors. 37 components, 33 signal nets. |
| Footprints | All 41 placed. |
| Placement | Complete. DRC 6 violations, all benign (see below). |
| Routing | **Not started.** 96 unconnected items. |
| Fab outputs | BOM and pick-and-place done. Gerbers deliberately not generated — they would be meaningless before routing. |

### The 6 accepted DRC violations

- 2 courtyard overlaps, H1/H2 against U1. The WROOM-1's courtyard is 48 x 41mm because it
  includes the antenna keepout. The mounting holes are NPTH with no copper, so this is fine.
- 2 silkscreen-over-edge, U1. The module deliberately overhangs the top edge.
- 2 library metadata mismatches, U1 and J2.

## Routing

Autorouting could not be automated. KiCad 10 dropped Specctra DSN export and SES import from
`kicad-cli`, so the Freerouting round trip is GUI-only. Freerouting 2.2.4 is installed at
`~/.local/share/freerouting/freerouting.jar` and Java 25 is present, so the manual path works:

1. Open the PCB, File > Export > Specctra DSN
2. `java -jar ~/.local/share/freerouting/freerouting.jar -de board.dsn -do board.ses`
3. File > Import > Specctra Session

A GND zone is already laid on B.Cu. It will not connect much until stitching vias exist, which
the router adds.

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
- **Four polarity jumpers, not two.** A 3-pad solder jumper switches one line, not a pair, so
  swapping A/B and Y/Z takes four. Same rework story the spec wanted.
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

1. Route the board.
2. Regenerate DRC, confirm zero unconnected.
3. Export gerbers and drill files.
4. Add LCSC/Digi-Key ordering codes to the BOM — currently it carries values and footprints only.
5. Push the DNP flags through to the board footprints. They are set in the schematic (R5, R6, R7,
   R9) but the pick-and-place still lists them, so an assembler would populate them.
