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
4. W-1176's two-way sockets: `r13_sync.py` with a fresh netlist (the new parts in; R12,
   R13 and R27 out; U7's socket becomes a SOIC and its label goes), then
   `tools_pcb/r15_clean.py` (the copper of both sockets' parts, their handshakes and the
   nets the schematic dropped; it stops if a piece it expects is missing), `r15_place.py`
   (one table of positions from the socket's centre line, set at J2 and again at J4, plus
   R24 and the marks), `r15_route.py` (one socket's copper drawn at each socket, then
   the runs between; a second run reports it is already drawn) and `r15_silk.py` (every
   label turned half a turn, the WW mark off the front, the nameplate on the back; a second
   run stops), `r15_names.py` (the sockets named Port 1 and Port 2, below) and, after
   `r13_sync.py` with a fresh netlist once more, `r15_shell.py` (J2's shell runs gone, so its
   shell reaches the ground pours as J4's does).
5. W-1185's port sense: `tools_sch/add_port_sense.py` adds the parts to the schematic, then
   `r13_sync.py` with a fresh netlist and `tools_pcb/r16_sense.py` (the parts placed, their
   copper drawn point by point; a second run redraws the same).
   `r15_route.py`'s docstring has the commands.

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

J2 and J4 have the same circuit, laid out the same way over each socket, so each runs a serial
printer or LocalTalk. Neither is named for a job: J2 is
Port 1 and J4 Port 2, in the schematic's values (Mini-DIN-8 Port 1 and Port 2) and on the
back's silk under each socket (PORT 1, PORT 2). Per socket:

| | J2 | J4 |
|---|---|---|
| Transceiver: THVD1452DGSR, VSSOP-10, ~RE to GND (receiver always on) | U2 | U6 |
| TashTalk PIC12F1840-I/SN, SOIC, placed blank on AirWriter | U10 | U7 |
| PIC data net (RA5, the transceiver's DI) | `LT2_DATA` | `LT_DATA` |
| PIC DE net (RA4, the transceiver's DE), 10 k to GND | `LT2_DE`, R36 | `LT_DE`, R22 |
| Receiver output: straight to a GPIO, to the data net through 2.2 k | `UART_RX` (IO18), R37 | `J4_RX` (IO10), R42 |
| GPIO to the data net through 220 Ω | `UART_TX` (IO17), R38 | `J4_TX` (IO9), R43 |
| GPIO to DE through 220 Ω | `J2_DE` (IO7), R39 | `J4_DE` (IO3), R44 |
| TX pair biased to mark, 4.7 k to 3V3 and to GND | R40, R41 | R45, R46 |
| Receive pair, 15 k in each leg | R29, R30 | R10, R11 |
| PIC MCLR / ICSPCLK / ICSPDAT / CTS | IO42 / IO11 via R34 / IO12 via R35 / IO41 | IO2 / IO4 via R15 / IO5 via R16 / IO6 |
| Shell | straight to GND | straight to GND |

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
| IO38 | `J4_SENSE` | J4's port sense, through R56 (W-1185, below); TP10 on the back |
| IO39 | `J2_SENSE` | J2's port sense, through R51 (W-1185, below); TP11 on the back; leaves reset as MTCK |
| IO40 | `SPARE_IO40` | rev A's DIP switches; now a bare pad on the back (below) |

Free: IO1, IO45, IO46, and IO40 on its pad.

## Port sense (W-1185)

Each socket can tell when a serial printer that has power is plugged into it, ready or not, so
the firmware never sends AppleTalk into one: an ImageWriter prints it as junk. A printer with
power holds its transmitter at idle mark, pin 8 near +4.5 V and often pin 5 near −4.7 V; a
LocalTalk device, a passive LocalTalk box, an unpowered printer and an empty socket hold
neither (Wells's measurements are in W-1185 and the port-sense design).

| | J2 | J4 |
|---|---|---|
| Dual NPN, BC847BDW1 (SOT-363) | Q1 | Q2 |
| Qa: pin 8 → 22 k → base, 6.8 k base to GND, emitter to GND | R47, R48 | R52, R53 |
| Qb: pin 5 → 15 k → emitter, 6.8 k emitter to GND, base to GND | R49, R50 | R54, R55 |
| Collectors joined, 4.7 k on to the GPIO (internal pull-up) | R51, IO39 | R56, IO38 |

- Qa turns on with pin 8 above about +2.5 V, Qb with pin 5 below about −3.5 V; the sense
  line reads low. Qa alone pulls the GPIO to +0.5 V at most. Qb's collector follows its
  emitter below ground, to about −0.6 V, so when Qb pulls alone (an RS-232 printer, pin 8
  grounded by its cable) the GPIO sits between 0 and −0.35 V with its pull-up on, and near
  −0.4 V on the pad's protection diode with it off, at under 0.1 mA through the 4.7 k. That
  needs no Schottky: the ESP32-S3's datasheet sets no voltage limit on an I/O pin (−0.3 V is
  the bottom of its input-low range) and tests latch-up at 200 mA.
- The taps are on the connector side of the 15 k receive legs, which are unchanged. They load
  pin 8 with 28.8 k and pin 5 with 21.8 k: pin 5 the heavier, so an empty socket's receiver
  still reads mark, never a break.
- Open-collector: an unpowered AirWriter can't be powered through it by a printer.
- 0402 resistors, the board's first, to fit: J2's group sits west of the socket between it and
  the edge, J4's in the 1.9 mm gap between the two sockets' bodies with Q2 just above. Each
  taps its socket's pins 8 and 5 on the back. R51 and R56 sit between IO39's and IO38's
  vias and their collector lines' vias, by TP11 and TP10; SW2's BOOT label moved down 1.7 mm
  and the back's 38/39/40 down 1.55 mm for them, and a ground via keeps the front pour east of
  them joined.
- The two collector lines cross the back in the one gap past J4's handshake runs, J4's over
  C16's ground via at (128.8, 149.95) and J2's under it. J2 takes IO39 and J4 IO38 so the
  lines never cross. J4's passes the via and the +3V3 run above it at 0.185 and 0.19 mm: inside
  the board's 0.15 mm rule, so the combined panel's 0.2 mm default counts them among its
  false clearance items (about 220 in all).
- The LC leaves all twelve off: it has one socket and never sends AppleTalk.
- The firmware reads it (SPEC 3.19, Port sense and Listening first); it's untested on
  hardware.

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
| SW2 | (144.50, 151.20) | (140.00, 152.70) | below IO0, so `BOOT_CFG` runs straight down; BOOT label to its right |

Rev A's parts round the sockets (U2, U6, U7, C3, C14, C16, R10, R11, R15, R16) are in
W-1176's two blocks now (below).

The silk reads with the sockets at the top, as AirWriter stands: from the front, and from
the back once the board is turned over left to right. Every label is turned half a turn
about its own centre from where the board's frame would put it (`r15_silk.py`), so a label's
angle is 180°, or 270° if it runs up the board.

KiCad's OSHW logo (7.5 × 8 mm, LOGO1, a board-only footprint, at (105.30, 134.20)) is on the
top silk where S1 was, in the west column above J2's parts, where the LC's clear lid shows
it. The Wells Workshop nameplate is on the back at the north edge, centred on the board,
30.6 × 10.7 mm, y 62.05 to 72.75: the mark 5 mm tall, and beside it the name in its own
lettering (2 mm, to its descender) over "AirWriter rev 2026.11" (1.1 mm), the repository and
the licence (0.9 mm), left-aligned, in a 0.2 mm frame with 0.35 mm corners. It is the one
stretch that wide with no via in it: a via under the art would print as a dot. The art is
`hardware/airwriter/art/wells-workshop.svg`.

Gone: S1, the eight DIP switches, and with them rev A's eight runs from the west edge to the
module. SW1 is now the BZCN TSB001A3518A side-press switch (the Macintosh Mini's), at
(157.60, 99.45) turned −90°: its button faces the case's reset pinhole, on rev A's axis, and
its two contacts sit over rev A's EN and GND pads. It is held by two plastic pegs in 0.9 mm
holes and two bracket pads; rev A's EVQP44B3M lifted off its two small pads. Its RESET label
is board silk now, not a field of the old footprint.

The new resistors sit beside what they serve: R24 between the two sockets' parts below U3,
R21 above SW2, R32 and R33 above IO16 and IO15.

W-1176's two blocks: each socket's parts, placed the same way against the socket. J4's are
J2's moved 19.5 mm east, so only J2's positions are given.

| J2 / J4 | J2's position, rotation | |
|---|---|---|
| U2 / U6 | (113.00, 148.40), 0° | transceiver, over the socket's centre line |
| U10 / U7 | (105.60, 145.06), 180° | TashTalk PIC, to the west |
| R39 / R44 | (110.60, 145.05), −90° | 220 Ω from the GPIO to DE |
| R37 / R42 | (112.15, 145.05), 90° | 2.2 k from RO to the data net |
| R38 / R43 | (113.70, 145.05), −90° | 220 Ω from the GPIO to the data net |
| R34 / R15 | (104.95, 140.75), −90° | 100 Ω in ICSPCLK, above the PIC |
| R35 / R16 | (106.55, 140.75), −90° | 100 Ω in ICSPDAT |
| R36 / R22 | (109.95, 140.75), 90° | 10 k DE pull-down |
| C68 / C16 | (107.80, 148.58), 0° | the PIC's bypass |
| R30 / R10 | (109.25, 153.07), 90° (R10 −90°) | 15 k in the B leg |
| R29 / R11 | (110.80, 152.05), 90° (R11 −90°) | 15 k in the A leg |
| R41 / R46 | (113.70, 152.05), −90° | 4.7 k from Z to GND |
| C3 / C14 | (115.25, 152.05), −90° | the transceiver's bypass |
| R40 / R45 | (116.80, 152.05), −90° | 4.7 k from 3.3 V to Y |

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

- U3.16 takes 3.3 V from C17 alone. Rev A's run on from it, under U3 and down to J2's
  parts, is gone: J2's block is fed from J4's (New runs).
- `LT_RESET` keeps rev A's back run at y 134.17, carried west to a via at (127.90, 134.17),
  then runs south on the front to U7.4. `LT_CTS` keeps rev A's back run at y 142.39 and
  `LT_UART_TX` and `LT_UART_RX` their back runs and vias, each carried on to U7's block.
- 3.3 V: a via where U7's socket pin 1 was, (131.71, 133.09), joins the front to the back,
  as that pin did. The run to U6 goes east from it and down at x 134.25 to U6.10.
- EN steps round SW1's bracket pad into its pad 1.
- `BOOT_CFG` goes straight down from IO0 to R21 and SW2.
- `LED_DATA`'s run north of the module is 0.37 mm higher, which makes room for R32 and R33's
  ground vias.
- Both sockets' bus pairs are their block's own front copper; rev A's runs to J4 from the
  J2 side and J2's back route to J2.6 and J2.3 are gone.
- The VBUS via at the H4 crossing is 0.5 mm drill (rev A 0.4), not doubled: its plated wall
  now holds more copper than the 0.5 mm track that feeds it, so that track, unchanged from
  rev A, is the limit.
- D− reaches the ESP32 through U5's pad 3, as D+ goes through U5; U5.2 has a ground via
  under U5.

## New runs

- J4.1 and J4.2 from U3.14 and U3.13: on the front down U3's east side and west of U7 to a
  via each, then the back layer to J4.
- IO21 (`J4_HSKI_RX`) and IO47 (`HSKI_DRV`): a via each by U3, then side by side on the
  back, along rev A's `HSKI_DRV` run, to a via each just west of the module and a short
  stub into pins 23 and 24.
- IO13 (`J4_HSKO_DRV`): a via beside U3.11, one straight run on the back at y 138.97,
  and a via just west of pin 21 (BTN_TEST passes on the front).
- Ground vias for R24, R32, R33, TP9's patch of the back pour and U5.2.
- W-1176, inside each block (the same copper at both sockets, front unless said): DE and
  the data net leave the transceiver west and turn north to the PIC's pins 3 and 2, DE on
  up to its pull-down with its 220 Ω hanging on it. RO runs north between R39 and R37, the
  data net north under the transceiver between R37 and R38. MCLR and CTS go north from the
  PIC; ICSPCLK and ICSPDAT run north under its body into R34 and R35 (R15 and R16). A, B
  and Z leave under the transceiver to the south: B passes between the pads of A's
  resistor, Z goes between the socket's rows to pin 3, and Y straight down to pin 6.
  3.3 V comes in at a via above the transceiver's bypass, up its east side to pin 10, and
  along the back to a via beside the PIC's bypass and pin 1, passing north of the
  transceiver's ground via so that pin 5 reaches the bypass and the socket's ground pin
  across the back plane. Ground vias under the
  transceiver, between it and the PIC, by the PIC's pin 8, by the pull-down and by the
  bypass.
- J2's lines from the module: IO17, IO18, IO7, IO41 and IO42 run on the front under the
  module to a via each, then west on the back in one band at y 145.3 to 146.9, under J4's
  block, to a via at each line's place in J2's block. IO11 and IO12 take a via beside the
  module's pin and run west on the back at y 137.6 and 138.05, under U3, to vias above R34
  and R35.
- J4's lines from the module: IO9 and IO10 take a via beside the pin, a short back run and
  a second via, then run south on the front into the block. IO3 (`J4_DE`) goes north off
  its pin to a via, west on the back at y 128.35, and south on the front to R44.
- 3.3 V to J2's block: a 0.3 mm back run along y 148.9 from J4's PIC via to J2's
  transceiver via, about 9.5 mm. R21 is fed on the back from J4's transceiver via.
- J2's handshakes, `PR_HSKO` and `PR_HSKI`, run on the front only: east below U3 from U3.8
  and R9, down between the two blocks, and west below J2's lower row into J2.2 and J2.1
  from below. R24 stands between them and J4's handshake pair, with a ground via below it.
- A ground via for C13.2, whose patch of the front pour `PR_HSKI` cut off.
- Spare pins: IO38, IO39 and IO40 each run straight down from the module's pin to a via
  and a bare 1 mm pad on the back (TP10–TP12, labelled 38, 39, 40), for trying an idea on
  a built board with a few wires (IO38 and IO39 now carry the port sense, above); 3V3 is at TP2 and GND at TP9 nearby. On the back, so the
  front keeps only a short stub per pin and the ground pour still reaches C1 and C22. IO41
  has no way down past USB D− on the back, and IO42 has C1 over it.

## Deliberate, don't "fix"

- IO13, J4's HSKo drive, has no pull-down. A resistor fits only at the end of an 8 mm
  stub, and nothing on J4 reads pin 1 at power-up; firmware drives it within about 0.1 s.
  J2's (IO47) keeps R24: a StyleWriter on J2 watches its HSKo.
- TP9 and the old test pads sit on the back under U1's courtyard.
- Vias under U2's, U5's and U6's bodies are tented, as every via is.
- IO3 (`J4_DE`) leaves the module north: its pad has no room for a via (`LT2_UART_RX` and
  the 3.3 V run pass under it, IO46's pad beside it). So `J4_DE` reaches R44 by a back run
  at y 128.35 from a via north of the module.
- The two blocks differ only where they must: R10 and R11 are turned half a turn from R30
  and R29, because the schematic numbers their pads the other way round (the copper is the
  same); J2's lines from the module reach their places in the block on the back, so each
  has a via there, where J4's arrive on the front (`LT_CTS` has a via in both); J4's
  transceiver also takes 3.3 V from the north; and the handshakes come from different
  places.
- J2's block takes its 3.3 V from J4's, by 9.5 mm of 0.3 mm back track: the other way was
  a hop of two vias under the handshakes. Both blocks draw a few milliamps.
- Each block's 3.3 V run up the transceiver's east side to pin 10 is 0.2 mm. It carries
  that block's few milliamps, and J4's carries J2's too.
- C3 and C14 sit in the row below their transceiver, about 3.5 mm of track from pin 10 at
  the 3.3 V via: beside the pin there is no room on J4 (the module and its pin 23 and 24
  vias), and the blocks match.
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

## Checks, 2026-11-21 (W-1176)

- ERC: 0 errors, 43 known warnings (40 `lib_symbol_mismatch`; 3 `isolated_pin_label`: TXD0,
  RXD0 and the spare IO40, each a label on one pin by design).
- `tools_sch/check_rev2026_11.py`: 51 nets, 47 parts, 17 removed.
- DRC with `--severity-all --schematic-parity --refill-zones`: 0 errors, 0 unconnected, 5
  `lib_footprint_mismatch` warnings, no field-parity warnings (`r13_sync.py` now copies the
  Description, Manufacturer and Datasheet fields too).
- `r12_gnd.py`: one GND component, no stranded pads.
- `r7_verify.py`: rev A's known FAIL lines, plus TP9 under U1; the same lines as before
  W-1176.
- `r13_check.py`: every pad of a part inside the board 0.30 mm or more from the edge, no
  front silk off the board, D3's K and SW1's RESET in place, every label reading with the
  sockets at the top, the OSHW logo on top and the nameplate on the back.
- `enclosure/build.sh`: all tests pass; the tray has no DIP slot (a board without S1 gets
  none), and the east wall is solid at SW1, where the reset jig's bore aims at the new button, 1.90 mm
  up. The lattice's clearances
  follow the new parts.
- `make_fab.sh`: 153 placements on AirWriter's panel, 63 on the LC's main board alone, drill
  floor 0.30 mm on both. Neither places J2 or J4: Wells fits them after delivery. U7 and U10
  go on AirWriter only, blank.
