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
| Placement | Complete. |
| Routing | **Ripped up.** The jumper removal changed the RS-422 nets, so the board needs one more Freerouting pass. |
| DRC | 3 violations, both categories cosmetic (see below). Schematic parity clean. |
| Fab outputs | Stale — regenerate after the re-route. |

### The 3 remaining DRC violations

- 2 silkscreen-over-edge on U1. The module deliberately overhangs the top edge.
- 1 library metadata mismatch on J2, from hand-injecting the footprint into the board.

Everything else is clean: zero unconnected, zero clearance, zero shorts, zero drill-range.

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
