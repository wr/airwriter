# Rev 2026.11 hardware, as drawn

The main board after rev A, named for what prints on the top case. Designed and routed, the
routing approved by Wells (2026-10-29); not yet ordered or built. Why each change exists is in the design,
`docs/superpowers/specs/2026-10-28-rev-2026-11-board-design.md`. This file is the board as
drawn: what moved from rev A, where things are, and what to leave alone. Edit it in place.

## How the board file is made

`hardware/airwriter/airwriter.kicad_pcb` is generated from rev A (the `hw-rev-a` tag) by
scripts, so every change can be read and re-run:

1. `tools_pcb/r13_build.sh`: rev A's board, then `r13_sync.py` (follow the schematic),
   `r13_place.py` (new parts, D3, TP9, silk), `r13_clean.py` (remove rev A copper the
   changes made wrong; it stops rather than cut a net the design didn't change),
   `r13_place.py` again, and `r13_reroute.py` (rev A copper and parts that move). It stops
   at the first step that fails.
2. `tools_pcb/r13_route.py`: the new traces. The long runs are drawn point by point; the
   short stubs round a part's own pins are found by a small grid search in a corridor.
3. `tools_pcb/r14_lc_light.py`, after `r13_sync.py` has brought JP1 in from the
   schematic: JP1 placed, its two runs drawn by the same grid search, two ground vias (below).
4. W-1176's two-way sockets, after `r13_sync.py` has brought the new parts in:
   `tools_pcb/r15_clean.py` (copper to pads that changed nets, on the port nets only),
   `r15_place.py` (positions, set outright) and `r15_route.py` (every new run drawn point by
   point; a second run reports it is already drawn).

D3's and SW1's footprints are `r13_mkfp_d3.py` and `r13_mkfp_sw1.py`, their 3D models
`lib/make_led_spacer_model.py` and `lib/make_sw1_model.py`.

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
| 10 | T2IN | `HSKI_DRV` | IO47, R24 to GND |
| 11 | T1IN | `J4_HSKO_DRV` | IO13 |
| 12 | R1OUT | `J4_HSKI_RX` | IO21 |
| 13 | R1IN | `J4_HSKI` | J4.2 |
| 14 | T1OUT | `J4_HSKO` | J4.1 |

IO8 also carries a QuickDraw LaserWriter's clock: a Personal LaserWriter 300 drives 533 kHz
on J2.2 whenever it is on, and the firmware clocks TxD out to it with GP-SPI2 through the GPIO
matrix, so the link needs no pin of its own (SPEC §2.4, §3.18; measured on rev A only).

Each port's HSKo has its own GPIO, on pins the DIP switches freed. The cable test drives
each, and through a crossover IO8 reads J2.2 (from J4.1) and IO21 reads J4.2 (from J2.1).
U1 is the ESP32-S3-WROOM-1-N16R8 (16 MB flash, 8 MB octal PSRAM; rev A's is the N8R2): its
PSRAM takes IO35–IO37, so those stay off every net. It needs its own firmware build (SPEC §3.19:
`idf.py -B build-2026.11 -DAIRWRITER_BOARD=2026.11 build`). Espressif
rates it to 65 °C around it, and to 85 °C with the PSRAM's error correction on, which that
build turns on at the cost of 1/16 of the PSRAM (7.5 MB usable).

## Either socket, either job (W-1176)

J2 and J4 have the same circuit, but for the last three rows below, so each runs a serial
printer or LocalTalk. Per socket:

| | J2 | J4 |
|---|---|---|
| Transceiver: THVD1452DGSR, VSSOP-10, ~RE to GND (receiver always on) | U2 | U6 |
| TashTalk PIC12F1840 | U10, SOIC, placed by PCBWay | U7, DIP-8 socket |
| PIC data net (RA5, the transceiver's DI) | `LT2_DATA` | `LT_DATA` |
| PIC DE net (RA4, the transceiver's DE), 10 k to GND | `LT2_DE`, R36 | `LT_DE`, R22 |
| Receiver output: straight to a GPIO, to the data net through 2.2 k | `UART_RX` (IO18), R37 | `J4_RX` (IO10), R42 |
| GPIO to the data net through 220 Ω | `UART_TX` (IO17), R38 | `J4_TX` (IO9), R43 |
| GPIO to DE through 220 Ω | `J2_DE` (IO7), R39 | `J4_DE` (IO3), R44 |
| TX pair biased to mark, 4.7 k to 3V3 and to GND | R40, R41 | R45, R46 |
| Receive pair, 15 k in each leg | R29, R30 | R10, R11 |
| PIC MCLR / ICSPCLK / ICSPDAT / CTS | IO42 / IO11 via R34 / IO12 via R35 / IO41 | IO2 via R27 / IO4 via R15 / IO5 via R16 / IO6 |
| 10 Ω in each TX leg | none | R12, R13 |
| Shell | to GND through R8 and C15 | straight to GND |
| 1 k in MCLR, against a programmer's 9 V | none: U10 is soldered | R27 |

- **LocalTalk:** the socket's GPIOs are inputs and its PIC runs TashTalk: RA5 drives the data
  net while it sends (overriding the receiver through the 2.2 k) and reads it otherwise.
- **Printer port:** the PIC is held in reset, which leaves its pins as inputs, and the ESP32
  drives the data net and DE through the 220 Ω resistors and reads the receiver's output
  straight. A firmware slip that drives both at once is limited to about 15 mA by the 220 Ω.
- **Undriven:** DE's pull-down keeps the driver off, and the 4.7 k pair holds the line at mark,
  which R25 did for J2 alone. U10's MCLR has no pull-down: until the bootloader drives IO42
  low, and while the ESP32 is in download mode, a flashed U10 runs TashTalk, which keeps DE
  low until it is asked to send. The app pulls IO11, IO12 and IO41 down, so U10's
  programming and CTS lines never float.
- **Untested for LocalTalk:** both receive pairs pass through 15 k, where rev A's J4 had 10 Ω.
  Against the receiver's load the signal arrives at about 85 % and its edges slow, which
  should be fine at LocalTalk's 230 kbit/s. If a built board's LocalTalk is marginal, smaller
  resistors there trade some of the RS-232 protection for signal.
- The case calls J2 port 1 and J4 port 2. The firmware works out each socket's job from what
  is plugged into it, and shares a printer on each at once (`W-1179 Rev 2026.11: plug anything
  into either socket and AirWriter works out what it is`, SPEC 3.19). The LC's one socket, J2,
  is a printer port only.

## GPIOs that changed

| GPIO | Net | Notes |
|---|---|---|
| IO3 | `J4_DE` | R44 to J4's DE (W-1176); a JTAG strap only with an eFuse no AirWriter burns |
| IO7 | `J2_DE` | R39 to J2's DE (W-1176) |
| IO9, IO10 | `J4_TX`, `J4_RX` | J4 as a printer port (W-1176) |
| IO11, IO12 | `LT2_UART_TX`, `LT2_UART_RX` | U10's UART and ICSP lines, through R34 and R35 (W-1176) |
| IO41, IO42 | `LT2_CTS`, `LT2_MCLR` | U10's CTS and MCLR (W-1176); the bootloader hook drives IO42 low |
| IO15 | `BOARD_LC` | R33 to GND, fitted on AirWriter LC only |
| IO16 | `BOARD_REV` | R32 to GND, fitted on every 2026.11 board; rev A floats high |
| IO13 | `J4_HSKO_DRV` | J4's HSKo, through U3's driver 1; no pull-down (below) |
| IO21 | `J4_HSKI_RX` | J4.2 through U3's receiver 1 |
| IO47 | `HSKI_DRV` | J2's HSKo, through U3's driver 2; R24 holds it low until firmware |
| IO35–IO37 | the N16R8's PSRAM | rev A's `HSKI_DRV` (IO35) and `J4_HSKI_RX` (IO37) |
| IO38–IO40 | `SPARE_IO38`–`40` | rev A's DIP switches; now bare pads on the back (below) |

Free: IO1, IO45, IO46, and IO38–IO40 on their pads.

## The LC's one light: D26 and JP1

The LC fits one LED of the 37: D26, the one under the test button, with its cap C46, and U9,
R20 and C29 to drive it. C21, the array's bulk cap, stays off. The chain starts at D10, so
JP1, a solder jumper marked LC on the silk south-east of D26, joins `LED_DATA5` (U9's output
after R20) to `LED_CH16` (D26's input). It is open on AirWriter, where it leaves stubs of
25 mm on `LED_DATA5` and 9 mm on `LED_CH16`; Wells bridges it on the LC. On the LC, D26 is
the first LED U9 drives, so it shows a frame's first pixel. The firmware tells the two
apart by `BOARD_LC`.

`LED_DATA5` runs east from JP1 on the front and down the back to TP5; `LED_CH16` passes under
D26's VBUS and `BTN_TEST` runs on the back. The front run closed off two patches of the front
pour, the ones D26.2, C46.2 and C21.2 reach ground through, so each has a via to the back
plane at (132.75, 97.6) and (139.45, 101.5). JP1 is in the schematic's BOM (konnect can't
clear the flag); `make_fab.sh` leaves solder jumpers off both orders' BOMs.

## Parts that moved from rev A

| Part | Rev A | Now | Why |
|---|---|---|---|
| C3 | (116.90, 152.40) | (112.90, 150.80) | under U2's VCC pin; R40 takes its old 3.3 V pad |
| C14 | (132.30, 152.40) | (134.20, 145.80) | above U6's VCC pin |
| U6 | (130.80, 148.20) | (132.45, 148.40) | over J4's center line; now a THVD1452 (W-1176) |
| R10 | (121.20, 152.40) | (128.75, 153.07) | 15 k in `LT_RXD_N`, beside J4.5 |
| R11 | (122.70, 152.40) | (130.30, 152.00) | 15 k in `LT_RXD_P`, beside J4.8; the other leg passes between its pads |
| SW2 | (144.50, 151.20) | (140.00, 152.70) | below IO0, so `BOOT_CFG` runs straight down; BOOT label to its right |

The maker's marks are on the top silk where S1 was, where the LC's clear lid shows them:
the WW mark (the Macintosh Mini board's two chevrons, 7 mm wide) above KiCad's OSHW logo
(7.5 × 8 mm, LOGO1, a board-only footprint).

Gone: S1, the eight DIP switches, and with them rev A's eight runs from the west edge to the
module. SW1 is now the BZCN TSB001A3518A side-press switch (the Macintosh Mini's), at
(157.60, 99.45) turned −90°: its button faces the case's reset pinhole, on rev A's axis, and
its two contacts sit over rev A's EN and GND pads. It is held by two plastic pegs in 0.9 mm
holes and two bracket pads; rev A's EVQP44B3M lifted off its two small pads. Its RESET label
is board silk now, not a field of the old footprint.

The new resistors sit beside what they serve: R24 below U3, R27 north-west of U7 beside its
pin 4, R21 above SW2, R32 and R33 above IO16 and IO15. W-1176's parts:

| Part | Where | |
|---|---|---|
| U10 | (124.70, 148.80) | J2's PIC, on U8's old spot; pins 2 and 3 face U2 |
| C68 | (122.03, 144.50) | U10's bypass, on the 3.3 V run into pin 1 |
| R36–R39 | between U2 and U10 | DE pull-down, and the 2.2 k and 220 Ω into `LT2_DATA` and `LT2_DE` |
| R40, R41 | below U2 | J2's TX pair to mark |
| R34, R42 | below U7's TASHTALK label | 100 Ω in U10's ICSPCLK; 2.2 k from U6's RO into `LT_DATA`, with `LT_DE` between its pads |
| R43, R35 | between U7 and the module | 220 Ω from IO9 into `LT_DATA`; 100 Ω in U10's ICSPDAT |
| R44 | (135.80, 128.60) | 220 Ω from IO3 to `LT_DE`, north of the module (below) |
| R45, R46 | below U6, with R12 and R13 | J4's TX pair to mark |

## The status LEDs

D10–D46 are OPSCO's SK6812MINI-012 (LCSC C2886570). Rev A's SK6812MINI-C (C7423117) is
discontinued at LCSC and no longer on OPSCO's list. By the two datasheets the -012 has the
same package, pads, chamfer corner and pin positions, the same GRB order and timing, so the
footprint, the nets, the chamfer rule in the order notes and `led_grid` are unchanged
(`tools_sch/led_part.py` renames the part in the schematic and the board). Two differences,
neither seen on a board yet: its blue is binned brighter (200–385 mcd against 160–320) and
5 nm longer, and its lens is called translucent where the -C's is silicone gel. On the first
board, look at white and Wi-Fi's blue through the lattice and the cover; if they have moved,
the balance is `led_grid_core.h`'s. The SK6812MINI-HS that LCSC lists as similar does not
fit: its pins are in other corners and it is 1.95 mm tall.

## Rev A copper that changed

- U3's 3.3 V feed runs from pin 16 under U3's body (x 117.3) to the branch at y 144.24,
  not down U3's right side.
- The printer port's handshakes rejoin rev A's routes by short steps from pins 7–10.
- `LT_RESET` runs west on the back under U7's north row to R27, north-west of U7, and
  `LT_MCLR` is a short front run from R27 to U7.4. `LT_ICSPCLK` and `LT_ICSPDAT` step east
  inside U7's rows.
- EN steps round SW1's bracket pad into its pad 1.
- `BOOT_CFG` goes straight down from IO0 to R21 and SW2.
- `LED_DATA`'s run north of the module is 0.37 mm higher, which makes room for R32 and R33's
  ground vias.
- J4's receive pair: R10 and R11 stand beside J4, so U6 reaches them on the front and the
  old runs from the J2 side are gone. J2's keeps its back route to J2.6 and J2.3.
- The VBUS via at the H4 crossing is 0.5 mm drill (rev A 0.4), not doubled: its plated wall
  now holds more copper than the 0.5 mm track that feeds it, so that track, unchanged from
  rev A, is the limit.
- D− reaches the ESP32 through U5's pad 3, as D+ goes through U5; U5.2 has a ground via
  under U5.

## New runs

- J4.1 and J4.2 from U3.14 and U3.13: down beside U3, under U10 (U8's old spot), a via each
  just below it, then the back layer to J4.
- IO21 (`J4_HSKI_RX`) and IO47 (`HSKI_DRV`): a via each by U3, then side by side on the
  back, along rev A's `HSKI_DRV` run, to a via each just west of the module and a short
  stub into pins 23 and 24.
- IO13 (`J4_HSKO_DRV`): a via beside U3.11, one straight run on the back between U7's two
  rows of pins, and a via just west of pin 21 (BTN_TEST passes on the front).
- Ground vias for R24, R32, R33, TP9's patch of the back pour and U5.2.
- W-1176: IO17, IO18, IO7, IO41 and IO42 run on the front under the module to a via, then
  west on the back at y 145.55 to 148.8, the last three dipping under U10. IO9 and IO12 hop
  on the back to R43 and R35, east of U7; from them `LT_DATA` runs west on the back to U7's
  own run, and `LT2_ICSPDAT` between U7's pins and under U10 into its pin 7. IO10 and IO11
  take back lanes into U7's rows. Two gaps in U7's south row carry two tracks each, 0.15 mm
  from the DIP pins. 3.3 V reaches U2.10 and C3 by a 1.45 mm back hop under `PR_HSKO`.
- A ground via at (123.75, 134.95) joins the front pour round U3.15 to the plane, which
  `LT_MCLR`'s run cut off.
- Spare pins: IO38, IO39 and IO40 each run straight down from the module's pin to a via
  and a bare 1 mm pad on the back (TP10–TP12, labelled 38, 39, 40), for trying an idea on
  a built board with a few wires; 3V3 is at TP2 and GND at TP9 nearby. On the back, so the
  front keeps only a short stub per pin and the ground pour still reaches C1 and C22. IO41
  has no way down past USB D− on the back, and IO42 has C1 over it.

## Deliberate, don't "fix"

- IO13, J4's HSKo drive, has no pull-down. A resistor fits only at the end of an 8 mm
  stub, and nothing on J4 reads pin 1 at power-up; firmware drives it within about 0.1 s.
  J2's (IO47) keeps R24: a StyleWriter on J2 watches its HSKo.
- TP9 and the old test pads sit on the back under U1's courtyard.
- Vias under U5's, U6's and U10's bodies are tented, as every via is.
- IO3 (`J4_DE`) leaves the module north: its pad has no room for a via (`LT2_UART_RX` and
  the 3.3 V run pass under it, IO46's pad beside it). So R44 sits north of the module, west
  of dot (7,4)'s light, and `LT_DE` reaches it by a 10 mm back run at y 128.35 from a via at
  R22.1.
- The 3.3 V feed from U6.10 down to R45, and by a via to R21 on the back, is 0.2 mm: at 0.3
  it comes within 0.15 mm of U6's Y pad. It carries only the pull-ups.
- Vias meet the pads of C3, R37, R38, R40 and R41 on their own nets, as stitch vias
  already met ground pads (`docs/ORDER-NOTES.md`, via fill).
- U2 and U6 are TI's THVD1452 (W-1176; the design had the SP3490 on J2, then the THVD1451),
  rated ±30 kV HBM on their bus pins, and their receivers work with ±15 V on their inputs
  (absolute maximum ±18 V), so an RS-232 printer's line is in range on its own. R29 and R30
  on J2, R10 and R11 on J4, stay in series with the receive pair at 15 k, as protection:
  against the receiver's load (at least 75 k at −15 V) a −15 V line arrives at about
  −12.5 V, and an RS-422 printer's 2 V as about 1.7 V. The fail-safe threshold (−20 to
  −200 mV) reads an idle, open or unplugged line as mark, so a printer switched off with the
  cable in sends no garbage.
- D3's cathode is marked K on the silk beside pad 1: the spacer hides the square pad.
- D3's axis sits about 0.5 mm east of the top case's slash centre line: the slash grew to
  about 1.0 × 5.75 mm in the Frame 16 art, and D3 can't move west without meeting J1. Its
  3 mm face still covers the slash's full width.
- R17's pads sit 0.58 mm in from the east edge: PCBWay's minimum is 0.30, with ±0.2 mm on a
  routed edge.

## Checks, 2026-11-20 (W-1176)

- ERC: 0 errors, 45 known warnings (40 `lib_symbol_mismatch`; 5 `isolated_pin_label`: TXD0,
  RXD0 and the three spare pins, each a label on one pin by design).
- `tools_sch/check_rev2026_11.py`: 48 nets, 35 parts, 12 removed.
- DRC with `--severity-all --schematic-parity --refill-zones`: 0 errors, 0 unconnected, 5
  `lib_footprint_mismatch` warnings, no field-parity warnings (`r13_sync.py` now copies the
  Description, Manufacturer and Datasheet fields too).
- `r12_gnd.py`: one GND component, no stranded pads.
- `r7_verify.py`: rev A's known FAIL lines, plus TP9 under U1; the same lines as before
  W-1176.
- `r13_check.py`: every pad of a part inside the board 0.30 mm or more from the edge, no
  front silk off the board, D3's K and SW1's RESET in place.
- `enclosure/build.sh`: all tests pass; the tray has no DIP slot (a board without S1 gets
  none), and the reset pinhole aims at the new button, 1.90 mm up. The lattice's clearances
  follow the new parts.
- `make_fab.sh`: 146 placements on AirWriter's panel, 69 on the LC's main board alone, drill
  floor 0.30 mm on both. Neither places J2, J4 or U7's PIC: Wells fits them after delivery,
  and U7's socket goes on empty. U10 goes on AirWriter only, blank.
