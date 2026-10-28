# Rev 2026.11 hardware, as drawn

The main board after rev A, named for what prints on the top case. Designed and routed;
not yet ordered or built. Why each change exists is in the design,
`docs/superpowers/specs/2026-10-28-rev-2026-11-board-design.md`. This file is the board as
drawn: what moved from rev A, where things are, and what to leave alone. Edit it in place.

## How the board file is made

`hardware/airwriter/airwriter.kicad_pcb` is generated from rev A (the `hw-rev-a` tag) by
scripts, so every change can be read and re-run:

1. `tools_pcb/r13_build.sh`: rev A's board, then `r13_sync.py` (follow the schematic),
   `r13_place.py` (new parts, D3, TP9, silk), `r13_clean.py` (remove rev A copper the
   changes made wrong; it stops rather than cut a net the design didn't change),
   `r13_place.py` again, and `r13_reroute.py` (rev A copper and parts that move).
2. `tools_pcb/r13_route.py`: the new traces. The long runs are drawn point by point; the
   short stubs round a part's own pins are found by a small grid search in a corridor.

Once Wells has edited the board in KiCad, don't re-run `r13_build.sh`: it starts again
from rev A. Close KiCad before any of these scripts write the board.

## U3's two RS-232 channels

Rev A used channel 1 for the printer port and tied channel 2's inputs to GND. Rev 2026.11
swaps them, so J4's lines leave U3 on the side facing J4:

| U3 pin | Function | Net | Goes to |
|---|---|---|---|
| 7 | T2OUT | `HSKI_RS232` | R9 (0 Ω), J2.1 |
| 8 | R2IN | `PR_HSKO` | J2.2 |
| 9 | R2OUT | `UART_CTS` | IO8 |
| 10 | T2IN | `HSKI_DRV` | IO35, R24 to GND |
| 11 | T1IN | `HSKI_DRV` | IO35 |
| 12 | R1OUT | `J4_HSKI_RX` | IO37 |
| 13 | R1IN | `J4_HSKI` | J4.2 |
| 14 | T1OUT | `J4_HSKO` | J4.1 |

J4's HSKo follows J2's: IO35 drives both drivers' inputs. No free GPIO reaches U3 (IO36
sits under UART_TX's via, IO7 is walled off north of U7, IO9–12, IO45 and IO46 are boxed
in), so the design's own GPIO and pull-down for it (R31) went. The cable test still sees each
handshake wire on its own: it drives IO35, and through a crossover IO8 reads J2.2 (from
J4.1) and IO37 reads J4.2 (from J2.1).

## GPIOs that changed

| GPIO | Net | Notes |
|---|---|---|
| IO15 | `BOARD_LITE` | R33 to GND, fitted on Lite only |
| IO16 | `BOARD_REV` | R32 to GND, fitted on every 2026.11 board; rev A floats high |
| IO35 | `HSKI_DRV` | both of U3's drivers; R24 holds them low until the bootloader hook |
| IO37 | `J4_HSKI_RX` | J4.2 through U3's receiver 1 |
| IO40 | `DIP3_GPIO` | through R28 (1 k) to DIP3: the ROM drives IO40 high in USB-OTG download |

Free: IO1, IO3, IO7, IO9–IO12, IO36, IO45, IO46. Only IO7 can be reached, from the north.

## Parts that moved from rev A

| Part | Rev A | Now | Why |
|---|---|---|---|
| C18 | (124.20, 152.40) | (121.55, 152.40) | beside U8's VCC pin, west of J4's handshake runs |
| R10 | (121.20, 152.40) | (120.00, 152.40) | `LT_RXD_N` straight down from U8 |
| R11 | (122.70, 152.40) | (118.45, 152.40) | `LT_RXD_P` straight down from U8 |
| R14 | (125.70, 152.40) | (131.30, 144.60) | on `LT_DATA` between U7 and U6, below U7's TASHTALK label |
| SW2 | (144.50, 151.20) | (140.00, 152.70) | so R28 sits under IO40; BOOT label to its right |

The new resistors sit beside what they serve: R24 below U3, R25 east of U6 (off UART_TX's
back run by a via), R27 east of U7 in LT_RESET's back run, R28 under IO40, R21 above SW2,
R32 and R33 above IO16 and IO15.

## Rev A copper that changed

- U3's 3.3 V feed runs from pin 16 under U3's body (x 117.3) to the branch at y 144.24,
  not down U3's right side.
- The printer port's handshakes rejoin rev A's routes by short steps from pins 7–10; IO35
  drops to rev A's back run through a via below U3.
- `LT_RESET` meets R27 east of U7; U7.4 is fed back along rev A's run as `LT_MCLR`.
- DIP1 comes up into IO38 from below; its rev A loop under the module is gone. DIP3 runs
  down from its rev A lane to R28.
- `BOOT_CFG` goes straight down from IO0 to R21 and SW2.
- `LED_DATA`'s run north of the module is 0.37 mm higher, which makes room for R32 and R33's
  ground vias.
- The LocalTalk receive pair from R10/R11 to J4: `LT_PORT_RXD_P` on the back.
- The VBUS via at the H4 crossing is 0.5 mm drill (rev A 0.4), not doubled: its plated wall
  now holds more copper than the 0.5 mm track that feeds it, so that track, unchanged from
  rev A, is the limit.
- D− reaches the ESP32 through U5's pad 3, as D+ goes through U5; U5.2 has a ground via
  under U5.

## New runs

- J4.1 and J4.2 from U3.14 and U3.13: down beside U3, under U8, a via each just below U8,
  then the back layer to J4.
- IO37: a via under U3, the back layer along the gap between `UART_CTS` and `DIP5`, up east
  of UART_TX's diagonal to U1.30.
- Ground vias for R24, R32, R33, TP9's patch of the back pour and U5.2.

## Deliberate, don't "fix"

- J4's HSKo shares IO35 with J2's (above).
- TP9 and the old test pads sit on the back under U1's courtyard.
- Vias under U5's and U8's bodies are tented.
- R29 and R30, in series with J2's receive pair, are 15 k (the design said 10 k). U2's input
  resistance is 12 k minimum and 15 k typical, so with 10 k a −15 V RS-232 line reached
  about −9 V, past the SP3490's −7 V common-mode limit. With 15 k, −12 V lands at −5.3 to
  −6.0 V and an RS-422 printer's 2 V still arrives as at least 0.89 V.
- D3's cathode is marked K on the silk beside pad 1: the spacer hides the square pad.
- R17's pads sit 0.58 mm in from the east edge: PCBWay's minimum is 0.30, with ±0.2 mm on a
  routed edge.

## Checks, 2026-10-28

- ERC: 0 errors, 44 known warnings.
- DRC with `--severity-all --schematic-parity --refill-zones`: 0 errors, 0 unconnected, 5
  `lib_footprint_mismatch` warnings, 16 field-parity warnings.
- `r12_gnd.py`: one GND component, no stranded pads.
- `r7_verify.py`: rev A's known FAIL lines, plus TP9 under U1.
- `r13_check.py`: every pad of a part inside the board 0.30 mm or more from the edge, no
  front silk off the board, D3's K in place.
- `enclosure/build.sh`: 129 tests pass.
- `make_fab.sh`: 138 placements on AirWriter, 61 on Lite, drill floor 0.30 mm.
