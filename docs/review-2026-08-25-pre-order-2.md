# Pre-order board review #2 — 2026-08-25

Adversarial review of `hardware/combined/combined.kicad_pcb` (main board + art panel,
the file that goes to PCBWay) against what the firmware in `firmware/` needs. Six
reviewers ran in parallel — netlist/parity, datasheet pinouts, firmware fit,
power/thermal/signal, BOM/assembly/fab package, mechanical/panel alignment — each told
to find a dead-on-arrival defect and then try to refute its own finding before reporting.
Every finding below was re-verified by the controller with an independent method before
it was acted on.

## What the hardware must do (requirements, from the firmware)

The firmware is 2.7k lines and uses exactly this hardware:

1. 5 V in on USB-C; the S3's native USB on IO19 (D-) / IO20 (D+) is the console and flasher.
2. WiFi station; advertises AirPrint; takes a URF job; halftones it; streams ImageWriter
   escape codes.
3. UART1 TX IO17 -> SP3490 DI, RX IO18 <- SP3490 RO, 9600 8N1, RS-422 to mini-DIN 3/5/6/8.
4. Printer HSKo (mini-DIN 1) -> SP3232 R1IN -> R1OUT -> IO8 as active-low hardware CTS.
   The same pin is the 30 s paper-out watchdog.

Everything else on the board — the 37-LED art panel on IO48, the 8-way DIP, the test
button on IO14, the LocalTalk PIC and its two SP3485 pairs, reset/boot switches — has no
firmware yet. It was reviewed against the plan in `docs/HANDOFF.md` and
`docs/superpowers/specs/2026-08-20-dot-panel-cover-design.md`, because a wiring error
there is still a dead board.

## DOA — found and fixed

### 10 stranded top-side ground islands (would have killed the printer path, LocalTalk and half the LEDs)

The top-copper GND pour around 12 ground pads was fenced off by other-net tracks and had
no via, through-hole pad or track to the bottom ground plane. KiCad's own DRC reported it
as 10 `Zone[/GND] <-> Zone[/GND]` unconnected items; an earlier review rule had classed
zone-to-zone items as benign, which is how it slipped through the 08-22 review. Two
independent methods (KiCad DRC with `--refill-zones`; a pcbnew-Python connectivity
clustering over filled polygons, pad shapes, vias and tracks) agree on the list:

| Island | What was on it | Effect on a built board |
|---|---|---|
| U2.4 | SP3490's only GND pin | printer RS-422 dead |
| U6.5 | SP3485 LocalTalk driver's only GND | LocalTalk TX dead |
| U8.5 | SP3485 LocalTalk receiver's only GND | LocalTalk RX dead |
| D29.2 | LED 20 of 37 | D29 and the 18 LEDs after it dark |
| D37.2 + C57.2 | LED 28 + its bypass | moot behind D29 |
| C5.2 + C9.2 | both 100 nF caps on the WROOM 3V3 pin | ESP32 loses all HF decoupling |
| C12.2 | SP3232 V+ reservoir | HSKo/CTS path dead |
| R1.2 | CC1 5.1 k pulldown | C-to-C cable powers the board in one orientation only |
| U1.1 | WROOM pin 1 GND | benign (pins 40/41 still grounded) |
| U3.10 | SP3232 T2IN tie | input floats; datasheet says driver inputs have no pull-up and must be tied |

**Fix:** one 0.6/0.3 mm GND via inside each island where the bottom side was clear
(6 islands). Where bottom-side tracks ran under the whole island: U6.5 got a via just
below the DIP1 track; U8.5's via landed on a bottom-side GND strip that was itself
isolated between the DIP tracks, so that strip got its own stitch via at (102.6, 140.9);
U1.1 needed the bottom-side VBUS trunk jogged 0.76 mm east to make room; U3.10 needed
the bottom-side DIP5 diagonal shifted 0.6 mm south-west so a via fits on the pad between
it and the UART_CTS track. Result: 12 new vias, DRC 0 errors / 0 unconnected, and the GND
net is ONE copper cluster (pcbnew connectivity clustering over filled polygons, real pad
and via shapes, both layers).

### SW1 (side-press reset switch) footprint matched no real terminal — risk of EN shorted to GND

`EVQP44B3M_SidePress.kicad_mod` said so itself ("DERIVED, NOT TRACED ... verify against a
physical part before a fab run"). It had four 1.3 x 0.9 pads at (+-2.55, +-1.0), left pair
= EN, right pair = GND. Panasonic ANCTB27E (page 2, "PWB land pattern for reference",
short-push-travel variant — read from the drawing at 600 dpi, and cross-checked against
LCSC's C79164 footprint) puts the two switch contacts 1.45 x 1.05 mm at +-2.275 mm across
the body, 2.05-3.1 mm back from the board edge, and two 0.6 x 0.8 mm anchor tabs at
+-3.2 mm, 0.45-1.25 mm from the edge, with the push plate facing the edge. On the old
pattern each contact landed in the gap between two pads (missing each by ~0.05 mm) and
each anchor tab overlapped one pad by 0.3 x 0.18 mm — so EN bonded to one frame tab and
GND to the other. If the frame commons the tabs (usual, and Panasonic does not say
otherwise), EN is tied to GND and the ESP32 never leaves reset.

**Fix:** footprint redrawn from the drawing (pads 1/2 = contacts, two un-netted anchor
tabs, body/plate on F.Fab, courtyard), SW1 placed inboard of the case wall by Wells (origin 157.225, 99.454; push plate at
x = 159.35), EN rerouted to the
south contact, GND via + stub to the north contact. The case pinhole and lattice carve-out are unchanged.

## DEGRADED — fixed

- **H4 screw head sat over a 3V3 track and the VBUS layer-change via** (0.39 mm and
  0.85 mm from the hole edge on the bottom side). The 3V3 diagonal is now jogged
  south-west (>=1.2 mm from the hole edge) and the via moved to 1.08 mm.
- **The whole 5 V rail crossed layers through single 0.4 mm vias twice.** The crossing by
  U1/D10 now has a second 0.8/0.4 via in parallel. The crossing by H4 stays single
  (there is no place for a partner that is not under the screw head); a 0.4 mm barrel
  rates 2.2 A vs the 1.9 A worst case, so this is redundancy, not ampacity.
- **Outline layer was not CAM-clean**: "V-CUT" text and a third coincident seam line were
  on Edge.Cuts (DRC `text_on_edge_cuts`, `invalid_outline`). `combine_boards.py` now puts
  the score line and label on User.Comments; the two closed outlines share the y=60 edge.
- **H_TEST (panel test hole) appeared in the position file as a bottom-side part.**
  `emit_board.py` marks it exclude-from-pos/BOM; the pos file is now 129 rows, all top.
- **Firmware had no way to flip the CTS sense** if the HSKo polarity (resolved on paper
  only) turns out wrong on the real printer. `CONFIG_AIRWRITER_INVERT_CTS` added; it
  flips both the UART peripheral's CTS and the watchdog's paper sensor. The comment in
  `serial_out.c` said the opposite of what the code does; corrected.
- `hardware/combined/make_fab.sh` now regenerates the whole order package (gerbers,
  drills, pos, BOM with the assembly notes and the standoff line, zip). It reproduces the
  08-24 package byte-for-byte apart from timestamps.

## DEGRADED — not fixed, needs a decision or a later revision

1. **Panel copper flood sits over the WiFi antenna.** The panel's back copper covers
   104 of the 114 mm2 antenna rectangle, ~12-13 mm above it (lattice height). Espressif
   wants >=15 mm clear of metal. Expect some detuning / a few dB of range. Options: window
   the flood over the antenna rectangle (nearest lit dot ends 2.6 mm away; the front has no
   copper there so a window shows as a bare-FR4 patch under the black mask), or ground the
   flood (nothing connects it today). **Decision (Wells, 2026-08-25): keep the copper.**
   Measure WiFi range on board 1.
2. **No LED current cap in hardware.** 37 LEDs all-white is 1.33 A on top of ~0.5 A WiFi
   peaks. A 500 mA USB-A host will brown out the S3 in ~26 us. The `led_grid` firmware
   must cap global brightness at 48/255 unless it can prove a 1.5 A / 3 A source (163/255
   / no cap). CC1/CC2 are plain 5.1 k pulldowns with no ADC tap, so it cannot detect the
   source today; two resistors to an ADC pin in the next revision would unlock that.
3. **`serial_out_write()` blocks forever on a stalled CTS** (printer unplugged or out of
   paper). The watchdog only reports. Give the write a deadline.
4. **LT_DE floats while the PIC is in reset** (no pull resistor; only the PIC's RA5 drives
   it). Firmware must not hold `LT_RESET` low for long; a 10 k pulldown on LT_DE belongs in
   the next revision.
5. **Z-stack is not pinned.** Three SW3 heights are on record (13 / 15 / 16 mm). Mini-DIN
   bodies are 12.75 mm tall and the panel is the lid over them. Plan 4 (lattice + case)
   must measure SW3 and fix the lattice height before printing. Not an order blocker.
6. **SW2 (BOOT) faces the lattice, not the case bottom**, and SW1 (reset) is on the
   east edge under gold dot (4,6), not west as the older handoff text says. The case plan
   must use the real positions.
7. **Inrush ~30 uF effective on VBUS with no soft-start**; some powered hubs latch off.
8. **Two BOM lines are out of stock at LCSC** (D3 C72043, R3/R14 C25804). Pre-authorise
   generic substitutes in the order notes.
9. **ESP-IDF is not installed** (`~/esp/esp-idf` is gone; `firmware/sdkconfig` is a C6
   build). The firmware edits are not compiled. Reinstall IDF, `idf.py set-target
   esp32s3`, build.

## Verified OK (by name, so nobody re-derives them)

- ERC 0 errors (44 benign lib-mismatch warnings). Main-board DRC with project rules,
  refill and schematic parity: 0 errors, 0 unconnected, 3 warnings (lib mismatch). Combined-file DRC
  under the combined project's default rules shows ~320 false clearance errors — the
  combined `.kicad_pro` has no netclasses; the source board is clean under its own rules.
- Combined file = main board + panel: 138/138 footprints, every pad->net, 968 segments, 90
  (now 101) vias identical; panel items at the documented mirror. The un-mirror transform
  `board = panel + (100, 60)` puts all 37 lit dots over their LEDs (max 0.0007 mm), the
  test hole over SW3's plunger (0.0000 mm), MT1-4 seats over H1-H4 (0.0000 mm).
- Firmware pins: IO17->U2 DI, IO18<-U2 RO, IO8<-U3 R1OUT<-J2.1 HSKo, IO19=D-/IO20=D+ via
  U5 (not swapped), CC 5.1 k both sides, strapping pins boot from flash with USB-SJ
  console, N8R2 quad PSRAM leaves IO35-37 free (IO35 is HSKI_DRV; do not substitute an
  octal-PSRAM module), partitions fit in 4 MB of the 8 MB flash.
- Planned features: IO48 -> 74AHCT1G125 (VCC 5 V, OE low) -> 47 R -> D10...D46 in order,
  tail open; AHCT VIH 2.0 V vs S3 VOH ~3 V; DIP1-8 = IO38/39/40/41/42/47/21/13 to GND
  (internal pull-ups; JTAG pins are fine because the S3 routes JTAG over USB by default);
  IO14 touch-capable test button; PIC: IO4->RA1 RX, IO5<-RA0 TX, IO2->MCLR, IO6<-RA2 CTS.
- Power: AP7361C pinout matches (IN/GND/OUT on SOT-223-3_TabPin2); LDO tab on a 244 mm2
  island with 5 vias, ~60-70 C/W, Tj ~88 C at a sustained 600 mA; dropout margin >0.6 V
  at full load; 3V3 drop to U1 27 mV; EN RC 10 k/1 uF releases ~14 ms after 3V3; IO0 on
  the internal pull-up with 100 nF; SP3232 charge-pump caps 100 nF on the right pins.
- RS-422/485: SP3490 pinout (1 VCC, 2 RO, 3 DI, 4 GND, 5 Y, 6 Z, 7 B, 8 A) with
  A<-TxD+, B<-TxD-, Y->RxD+, Z->RxD-; R5/R6/R7 DNP so the receiver fail-safe holds RO high
  with the printer off. If R5 (120 R) is ever fitted, R6/R7 must be too. SP3485 pair: U6
  driver, U8 receiver, ~RE on GND, A/B per Apple convention; idles high with J4 open.
- HSKo path with the printer unplugged: SP3232's internal 5 k pulldown commits R1OUT high
  -> CTS deasserted -> firmware reports out-of-paper after 30 s. Deterministic.
- BOM: 116 populated parts, every one in the assembly BOM with an LCSC number, quantities
  match, R5/R6/R7/R9 DNP excluded, R8 populated on its own row. All 27 LCSC numbers
  resolve to the right MPN and package (SK6812MINI-C is the top-firing part). Assembly
  is one-sided (all pos rows top). Parts ~$23-25/board.
- Fab: 21 gerber layers, PTH/NPTH drills (NPTH: 2x0.65 J1 pegs, 4x2.75 mounting,
  1x3.8 test hole), min track 0.2, min clearance 0.15 (order 6/6 mil), vias 0.6/0.3, mask
  dams >=0.2 mm at the 0.5 mm-pitch USB-C, no silk on pads, antenna keepout empty on both
  main-board layers, nothing crosses the V-cut seam.
- Mini-DIN shells grounded (R8 0R + C15); no discrete ESD on the serial lines and none
  needed (+/-15 kV on the transceivers).

## Datasheet pinout pass

The datasheet reviewer reported late but in full. Its one blocker (SW1, above) is fixed.
Confirmed correct pin-by-pin against datasheets: ESP32-S3-WROOM-1 (every used pad vs
Table 3-1, IO35 legal on the quad-PSRAM N8R2), SP3490 (5 Y, 6 Z, 7 B, 8 A — not the order
one assumes; polarity correct end to end), SP3232 (all 16 pins, narrow-body EN suffix is
right), AP7361C SOT-223 (IN/GND/OUT; the reversed part is the -ER), USBLC6-2SC6, SP3485
x2 (non-inverting A on the "+" line; meets TashTalk's "receiver floats high" rule),
PIC12F1840 vs TashTalk's `one-chip.asm` header (UART not crossed; TashTalk runs the UART
at 1 Mbps), 74AHCT1G125, USB-C HRO TYPE-C-31-M-12 (A6/B6 and A7/B7 are separate contacts
and are correctly shorted on the board), mini-DIN-8 numbering (verified against CUI
MD-80SKS's "PCB LAYOUT TOP VIEW": no mirror error), DP-08RP, B3U-1000P (2-terminal),
TS-1002 pad pairing, D3 polarity, all 38 LED bypass caps. Also covered by the other
reviewers and the controller: AP7361C SOT-223 pinout, SP3490, SP3485 x2, SP3232 (pin map,
0.1 uF charge-pump caps at 3.3 V, internal 5 k receiver pulldown; transmitter inputs have
NO pull-up and must not float — which is why the U3.10 island was fixed rather than
waived), PIC12F1840 PDIP-8 and TashTalk's RA0-RA5 use, 74AHCT1G125, ESP32-S3-WROOM-1 pad
numbering and strapping, USB-C receptacle (C165948) pad map, SK6812MINI-C pad geometry and
orientation (pad 3 = DIN at the chamfer; 180-degree trap noted in the order notes). The
Kycon KMDGX-8S-BS footprint's pin numbering was derived from the socket-face view and
cross-checked against two other vendors' drawings in the 08-2x work (HANDOFF "pin-numbering
discrepancy is resolved"); it was not re-derived here.

Two residual notes from that pass: the Kycon 8-pin shield-post span (13.5 mm on the board;
the 9S/10S drawings say 14.5, the 8S drawing was unreachable; the STEP render and Adam
Tech's 8-pin drawing both agree with 13.5) and the mini-DIN footprint's descr, which calls
its source "(Top View)" where Kycon's sibling drawings say "(Bottom View)" — numbering is
right, fix the note. Still unverified on real hardware: HSKo polarity (firmware now has the invert switch) and
the RS-422 pair polarity (firmware has had those switches since the start).

## PCBWay order notes (paste into the order)

1. One panel, 62.1 x 208.6 mm, TWO different designs (main board 62 x 104.27 mm at the
   bottom, cover panel 62 x 104.27 mm at the top), butted flush. Separate with ONE V-score
   along y = 60 (drawn and labelled "V-CUT" on the User.Comments layer). The four 2 x 4 mm
   rounded-corner voids at the seam ends are routed.
2. 2-layer, 1.6 mm FR4, 1 oz, matte BLACK soldermask both sides, ENIG. Min trace/space
   6/6 mil, min via 0.3 mm drill. The 3.8 mm and 2.75 mm holes are NPTH.
3. Assembly: single side (top side of the file).
4. DO NOT fit or buy J2, J4 (mini-DIN; customer solders later). U7: fit ONLY the DIP-8
   socket LCSC C72124; do not buy or fit the PIC.
5. MT1-MT4: Wurth 9774030151R M2.5 SMT standoff (not on LCSC; source from Digi-Key or
   consign). Reflow on the top side; paste is the ring in the paste layer; the seat is bare
   ENIG copper with no paste in the centre.
6. D10-D46 (37x SK6812MINI-C, C7423117): the CHAMFERED corner of the LED (datasheet pin 1,
   DIN) must sit on the silkscreen notch / chamfer corner of the footprint — the TOP-LEFT
   corner of every LED in the top-view image. The footprint's pad numbering is rotated
   180 degrees from the datasheet's pin numbers: align by the chamfer, not by "pad 1". A
   180-degree placement breaks the whole chain.
7. Substitutions allowed for out-of-stock D3 (any 0603 green LED, cathode per silk bar)
   and R3/R14 (any 10 k 1% 0603). No substitution for U1-U9, J1, S1, SW1-SW3.
8. Please confirm polarity of U2, U3, U6, U8 (SOIC pin-1 dot), U7 socket notch, D3 and J1
   against the attached image (`hardware/combined/combined-top.png`) before placement.

Files to upload: `hardware/combined-fab.zip` (gerbers, both drills, pos, BOM) plus
`hardware/combined/combined-top.png` as the assembly reference. Do not send
`hardware/airwriter-fab.zip` or `hardware/panel-fab.zip`; they are stale single-board
packages.
