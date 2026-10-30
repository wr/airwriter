# Rev A hardware, as built

What rev A is, where it departs from `docs/SPEC.md` §2 and why, the things that look wrong
but are deliberate, and the rev B list. Edit in place. The design history is the dot-panel
cover spec (`docs/superpowers/specs/2026-08-20-dot-panel-cover-design.md`, read its amendment
block first), the review docs in `docs/`, and Linear `W-568 Design the AirWriter PCB (KiCad 10)`.
The current BOM is `hardware/combined/fab/combined-assembly-bom.csv`.

## What it is

Two boards ordered snap-apart in one PCBWay panel (`hardware/combined/`):

- **Main board** (`hardware/airwriter/`), 62 × 104.27 mm, 2-layer 1.6 mm FR4, 1 oz, matte black,
  ENIG. ESP32-S3-WROOM-1-N8R2, two mini-DIN-8 ports and USB-C on the bottom edge at 19.5 mm
  centres (J2 printer x = 113, J4 LocalTalk 132.5, J1 USB-C 152), 37 top-firing SK6812MINI-C
  (C7423117) under the panel's lit dots, an 8-way right-angle DIP bank (S1) through the west
  wall, the `test` button (SW3) under the panel's test dot.
- **Art panel** (`hardware/panel/`), same outline, passive: no components except four SMT
  standoffs. `hardware/panel/generate.sh` turns the Figma export
  (`hardware/panel/svg/panel.svg`, the single source of truth; fill colour is the material) into a
  byte-deterministic `panel.kicad_pcb`. Frame 9b: 56 dots, 37 lit (14 bare FR4 full-RGB pixels,
  9 single-mask and 14 double-mask teal), 19 gold, one of which is the `test` dot (3.8 mm plated
  bore, mask-knockout letters). The back is a solid copper flood with windows only at the lit dots,
  the FR4 slash and the test bore, because black mask passes teal light and the LEDs would bleed
  across the face. Four exposed rings take PEM SMTSO-class M2.5 blind nuts (BOM: Würth
  9774030151R), so screws climb from the feet through case, main board and lattice into the panel
  with no hardware on the art face.

Modes: (1) AirPrint only; (2) AirPrint + EtherTalk over Wi-Fi, firmware only; (3) AirPrint +
LocalTalk through J4, the PIC12F1840 running TashTalk, and the U6/U8 transceiver pair. The board
is built for mode 3. Only one AppleTalk transport can be live at a time without an AppleTalk
router; AirPrint is IP and runs alongside either. Only mode 1 exists in firmware.

## Geometry worth knowing

- LEDs sit at `board = panel + (100, 60)` from `panel_layout.json` (`place_leds.py` re-syncs them
  and asserts the serpentine chain and the plunger). Each LED's 100 nF cap is 2.87 mm north of it
  (not 2.6; the lattice uses 2.87).
- Mounting holes, M2.5 clearance (2.75 mm), non-plated, coaxial with the panel seats:
  H1 (105.636, 65.636) and H2 (156.364, 65.636) under the two gold top-corner dots,
  H3 (105.636, 151.930), H4 (153.364, 151.930). The pattern is deliberately asymmetric.
- S1's keys overhang the west edge to x = 97.8. Net `/DIP1` (S1 pin 1) is at the bottom, but
  the part's printed numbers run the other way: net DIPn sits under label 9−n, and firmware reads
  switches by printed label (`dip_config_core.h`). SW1 (Panasonic
  EVQP44B3M side-press, EN reset) is on the east edge, plate face x ≈ 159.4. SW2 (B3U-1000P) is
  BOOT on IO0, on the component side. SW3's plunger is centred under the test dot.
- The WROOM antenna is flush at x = 162 (no case bump); its keepout is the module width + 5 mm a
  side. The panel's back copper stays over the antenna; the range that leaves is unquantified.
- R/C and LED designators live on F.Fab; chips, connectors and switches keep silk designators.
- Before believing a placement change, run the alignment proof: `tools_pcb/r7_verify.py`
  (writes nothing) and the `r7_align.py` overlay. 37/37 LEDs, plunger and holes must sit coaxial
  with the panel at 0.0000 mm.

## Deviations from the spec, and why

SPEC §2.4's J2 table, the receiver polarity (§4.3) and the dropped polarity jumpers are already
amended in the spec. **Gone since the 08-20 pivot**, though SPEC §2.2/§2.3/§2.5 still name some:
D1/D2 WS2812B status LEDs, the D4–D6 logo LEDs, J3 (USB header), J6 (I2C/Qwiic), the MODE touch
pad, the BOOT pads and the back-face artwork. The rest:

- **U2 is an SP3490 (MAX3490 class), not a MAX3491.** The spec tied DE high and RE low, so the
  enable pins were dead weight. U3 is an SP3232. Both MaxLinear, bought since 08-22.
- **LocalTalk uses two half-duplex SP3485s (U6, U8)**, not one full-duplex part: full-duplex
  RS-422 transceivers keep going out of stock at LCSC; half-duplex RS-485 is commodity. The wiring
  is copied node for node from TashTalkUSB's netlist.
- **U7 is a PIC12F1840-I/P in a DIP-8 socket**, and the ESP32 flashes TashTalk into it in place
  over LVP (`pic_prog`, `/api/localtalk/flash`). No ICSP header.
- **MCLR runs to ESP32 IO2 (`LT_RESET`), and firmware owns the PIC's reset.** On a dongle with no
  console it recovers a wedged PIC without a power cycle and keeps the PIC quiet when LocalTalk is
  not the live transport. Low holds reset (and tristates the PIC's outputs); input lets the PIC's
  weak pull-up run it, which is the state through ESP32 boot. IO2 is a plain GPIO, not a strap,
  JTAG or console pin.
- **USB-C carries data** to the S3's native USB, so one cable powers, flashes and consoles the
  board (SPEC §2.5). There is no separate debug header any more.
- **37 SK6812MINI-C on one chain, powered from VBUS**, data from IO48 shifted up by U9
  (74AHCT1G125) through R20 (47 R). The -E variant is reverse-mount and fires into the board;
  rev A uses the top-firing -C on the stock PLCC4 footprint.
- **D3 is a plain 0603 green power LED** (C12624) across 3V3 via R17, no GPIO: it separates "board
  dead" from "firmware dead".
- **Power net class: 0.5 mm track for `/+3V3`, `/VBUS`, `/GND`**, 0.2 mm for signals. WiFi TX
  peaks near 500 mA; 0.2 mm in 1 oz carries it with no margin and a 10–15 °C rise.
- **Four DNP parts on the printer port**, fitted only if it misbehaves:

  | | Value | What it does | Fit it when |
  |---|---|---|---|
  | R5 | 120 R | Termination across the printer's TxD pair at U2's receiver | The cable rings. At 9600 baud on a printer cable it only loads the line |
  | R6, R7 | 4.7 k | Failsafe bias, 3V3 to TxD+ and TxD− to GND; fit both or neither | The receiver reads garbage with the printer off or unplugged |
  | R9 | 0 R | T1OUT → J2's HSKo pin | The board should drive the printer's DSR rather than only listen on HSKi |

- **Shells and holes.** J2's shell reaches GND through R8 (0 R, net `/SHELL`) so the tie can be
  lifted; the ImageWriter II is mains-powered with a metal chassis and a second, unbreakable ground
  path would put noise on the link. J4's shell is tied straight to GND (found in review #4;
  undecided whether deliberate). The mounting holes are non-plated and ungrounded for the same
  reason: a ground ring buys nothing in a printed case.

## Deliberate, don't "fix"

- U8's ~RE rides `LT_DE` (shutdown while the PIC transmits); see `docs/traps.md`.
- U1's twelve ground-pad vias are 0.30 mm on the board, not the library's 0.20 (drill floor);
  U1 shows a library mismatch in DRC because of it.
- J2 and J4 are excluded from the position file (hand-soldered, DO NOT ASSEMBLE in the BOM).
- The `PANEL_AU` decorative net on the panel's test pad, annulus and tab art.
- The THT thermal reliefs on both /GND zones: J2/J4 are hand-soldered, and J4's shield posts sat
  in solid 1 oz flood on both layers.
- The 0.12 mm `GND_STITCH_012` space exception around U4's tab (`airwriter.kicad_dru`; the order
  notes declare it).

## Rev B candidates

Rev B became rev 2026.11 (`W-1091 Rev B: the main board's next revision`): its design,
`docs/superpowers/specs/2026-10-28-rev-2026-11-board-design.md`, says which of these it took,
changed or dropped, and `docs/hardware-rev-2026-11.md` is the board as drawn. The list below
is the one filed on 2026-10-24, kept for its reasons. Most come from the fourth
pre-order review (2026-09-01).

- 10 k pulldown on `LT_DE` (across U6 pins 3–5; it floats with the PIC absent or in reset).
- 100 k from U9.2 to GND (IO48 floats until firmware claims it; brownout loop risk on 500 mA ports).
- Tie U3.11 T1IN to GND, or pull `HSKI_DRV` (IO35) low; firmware clamps it today.
- 1 k in series in `LT_RESET`, to protect IO2 from a mistaken high-voltage ICSP.
- 10 k pulldown on IO3.
- 10 k pull-up on IO0 (`/BOOT_CFG`) and C7 left off: with only the S3's internal pull-up to
  charge it, C7 sometimes holds IO0 low past EN and the board powers up in download mode.
- 1 k in series in every switch and DIP leg: in USB-OTG download boot the ROM drives GPIO40
  (DIP3) high.
- Pull-up on IO17 so UART TX idles at mark through reset and download mode.
- A partner VBUS via at the H4 crossing (one 0.4 mm via carries a modelled 1.94 A worst case).
- U5 (USB ESD) sits on an 8 mm stub on D−; put it in line.
- 74LVC1G125 for U9: the AHCT part wants 4.5 V minimum against USB's 4.40 V.
- Decide J4's shell: an R/C tie like J2's R8, or document why it is hard GND.
- Port legends print under the connector bodies; SW1 has no silk.
- A GND test pad beside TP7/TP8.
- Rename the `LT_A/LT_B/LT_Y/LT_Z` nets: the names are swapped but U6/U8 polarity is right.
  Rename, do not rewire.
- 0 Ω cable-option pads on J2.
- J4 pin 1 to a spare GPIO through 1 k, so the cable test reaches the handshake leg: through a
  crossover it lands on J2 pin 2 (HSKi, read through U3).
- Inrush soft-start, if USB compliance ever matters (VBUS bulk is ~4× the inrush budget).
- J2's receive pair is a MAX3490 (RS-422, common-mode −7 to +12 V) against RS-232's −12 V:
  check its absolute maximum, or route J2 pin 5 through U3's spare RS-232 receiver, before an
  RS-232 printer (a LaserJet, the original ImageWriter) is wired both ways.
- Through-hole dome power LED: `W-1043 Rev B: through-hole dome LED for the power indicator`.
- Scope HSKo first: if its swing is modest, a divider and clamp into a GPIO could delete U3.
  Whatever replaces U3 has to pass the 1 MHz clock below.
- U3 to an SP3232EU (SP3232EUCN-L, LCSC C2997661): same SOIC-16 and pinout, so HSKi can carry
  the ~1 MHz clock a QuickDraw LaserWriter drives in its fast mode (`W-1102 QuickDraw
  LaserWriters on the printer port (Personal LaserWriter LS/300, Select 300)`). The fitted
  SP3232EEN's receiver skew is 200 ns typical and 1 µs maximum against a 500 ns half-cycle;
  the EU's is 50 ns. About $0.60 a board more at prototype quantities ($1.01 against $0.36 at
  LCSC, 2026-10-26, 11,029 in stock); LCSC carries only the 0–70 °C grade in SOIC-16, and one
  of its listings for the part says not recommended for new designs, so check MaxLinear's
  status before ordering. A rev A board can take the swap by hand to test it first.
