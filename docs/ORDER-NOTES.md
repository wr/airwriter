# PCBWay order notes: rev 2026.11

Two orders, one main board. **AirWriter** is ordered on the combined panel (main board and
art cover, snapped apart). **AirWriter LC** is ordered on the main board alone: on the panel it
would pay for a cover it never uses and the cover's four standoffs. Neither order fits the
mini-DINs (J2, J4): Wells fits them after delivery (below). AirWriter's order places the two
TashTalk PICs, U7 and U10, blank; the board programs them.
Standing rule: quote to Wells before either order is placed. Rev A's order and its notes are in git history.

| Order | Upload only | Placements |
|---|---|---|
| AirWriter | `hardware/combined-fab.zip`, `hardware/combined/combined-top.png`, `hardware/combined/airwriter-top.png` | 141 |
| AirWriter LC | `hardware/airwriter-lc-fab.zip`, `hardware/combined/airwriter-top.png` | 63 |

Each order's section below is paste-ready on its own.

## Order 1: AirWriter (combined panel)

### Board spec (order form)

- One customer panel, profile **62.0 × 208.54 mm**, containing **TWO different designs**:
  main board 62 × 104.27 mm (bottom half of the file), art cover panel 62 × 104.27 mm
  (top half), butted flush.
- Separate with **ONE V-score** along y = 60, full panel width, drawn and labelled "V-CUT"
  on the User.Comments layer. There is **one 2 × 4 mm routed relief at each end** of the
  score (rounded corners). Copper is set back **0.5 mm** from the score line on both sides
  and both layers, by design.
- **2 layer, 1.6 mm FR4, 1 oz, matte BLACK soldermask both sides, white silkscreen, ENIG.**
- Min trace/space **0.15 mm (6 mil)** generally. **Exception: four spots need 0.12 mm
  (4.7 mil) space**, all around U4's SOT-223 tab at approx. x 115–120 / y 113–120 mm of the
  main-board half (the project's `GND_STITCH_012` rule area). Everywhere else is ≥ 0.15 mm.
- Vias: 0.6 mm pad / 0.3 mm drill, plus six 0.8/0.4, one 0.6/0.4 and one 0.9/0.5 (the
  VBUS via). Drill floor 0.30 mm.
- **NPTH:** 2 × 0.65 mm (J1 pegs), 2 × 0.9 mm (SW1 pegs) and 4 × 2.75 mm (mounting) only.
  The **3.8 mm hole is PLATED on purpose** (netless gold bore on the art panel; it is in the
  PTH drill file, do not convert it to NPTH).

### Assembly

Position file `combined-pos.csv`, BOM `combined-assembly-bom.csv`, 141 placements, all on the
top side of the file. R33 is left off; its pads stay bare.

1. **J2 and J4 (mini-DIN-8): DO NOT ASSEMBLE, and keep all 22 of their holes free of
   solder.** They are fitted after delivery and are absent from the position file and the
   BOM. If the through-hole parts are wave or selectively soldered, mask J2's and J4's holes.
2. **U7 and U10 (PIC12F1840-I/SN, LCSC C39594) are placed BLANK, as LCSC ships them: do not
   program them.** The board programs them.
3. **D3: through-hole 3 mm flat-top green LED (LCSC C20613324) standing on a HIWA LED4*8
   nylon spacer (LCSC C92326):** leads through the spacer's bore, the LED's flange seated
   on the spacer, lens up. Cathode (pad 1) toward R17, where the silk reads K;
   the spacer hides the square pad.
   **SW1 (BZCN TSB001A3518A, LCSC C2888448):** its two plastic pegs go into the two 0.9 mm
   holes; the two bracket pads are soldered, on no net. The button faces the board's east
   edge.
4. **Through-hole parts (SW3, D3): solder every joint fully.** Rev A's first boards came
   back with U7's socket and SW3 only tacked.
5. **MT1–MT4: Wurth 9774030151R** M2.5 SMT standoff, not at LCSC; source from Digi-Key or
   consign (labelled "MT1-MT4, Wurth 9774030151R"): four for every board, plus six spares
   for the order. Reflow on the top side; paste is the ring aperture in the paste layer;
   the seat centre is bare ENIG with no paste.
6. **D10–D46 (37 × SK6812MINI-012, C2886570): align by the CHAMFERED corner**, which must sit
   on the footprint's chamfer/notch corner, the top-left corner of every LED in the top-view
   image. The footprint's pad numbering is rotated 180° from the datasheet's pin numbers:
   align by the chamfer, not by "pad 1". A 180° placement breaks the whole chain.
7. Substitutions allowed: any 100 nF 50 V X7R 10 % 0603 for C14663, any 10 k 1 % 0603 for
   C25804, any 15 k 1 % 0603 for C22809, and any 2.2 k, 220 Ω or
   4.7 k 1 % 0603 for C4190, C22962 or C23162.
   **No substitution** for U1–U10, J1, SW1–SW3 or D3, and U1 must be the
   **ESP32-S3-WROOM-1-N16R8** (16 MB flash, 8 MB octal PSRAM, LCSC C2913202), never a
   quad-PSRAM (R2) variant: the firmware is built for the octal PSRAM.
8. Please confirm polarity of U2 and U6 (VSSOP-10 pin-1 dot), U3, U7 and U10 (SOIC pin-1
   dot), U5 and U9, D3 and J1 against `airwriter-top.png` (the main board alone, at about 40 px/mm)
   before placement.

## Order 2: AirWriter LC (main board alone)

### Board spec (order form)

- One design, **62.0 × 104.27 mm**, routed outline with 2 mm corner radii. No panel, no
  V-score. Copper sits 0.5 mm back from the top (north) edge, by design.
- **2 layer, 1.6 mm FR4, 1 oz, matte BLACK soldermask both sides, white silkscreen, ENIG.**
- Min trace/space **0.15 mm (6 mil)** generally. **Exception: four spots need 0.12 mm
  (4.7 mil) space**, all around U4's SOT-223 tab at approx. x 115–120 / y 113–120 mm (the
  project's `GND_STITCH_012` rule area). Everywhere else is ≥ 0.15 mm.
- Vias: 0.6 mm pad / 0.3 mm drill, plus six 0.8/0.4, one 0.6/0.4 and one 0.9/0.5 (the
  VBUS via). Drill floor 0.30 mm.
- **NPTH:** 2 × 0.65 mm (J1 pegs), 2 × 0.9 mm (SW1 pegs) and 4 × 2.75 mm (mounting) only.

### Assembly

Position file `airwriter-lc-pos.csv`, BOM `airwriter-lc-assembly-bom.csv`, 63 placements, all
on the top side. **Of the LED array fit only D26 (the LED under the test button), C46, U9,
R20 and C29. Leave off D10–D25, D27–D46, C30–C45, C47–C66, C21, U7 and U10.** They are absent
from the position file and the BOM; their pads stay bare. R33 is fitted.

1. **J2 and J4 (mini-DIN-8): DO NOT ASSEMBLE, and keep all 22 of their holes free of
   solder.** They are fitted after delivery and are absent from the position file and the
   BOM. If the through-hole parts are wave or selectively soldered, mask J2's and J4's holes.
2. **D3: through-hole 3 mm flat-top green LED (LCSC C20613324) standing on a HIWA LED4*8
   nylon spacer (LCSC C92326):** leads through the spacer's bore, the LED's flange seated
   on the spacer, lens up. Cathode (pad 1) toward R17, where the silk reads K;
   the spacer hides the square pad.
   **SW1 (BZCN TSB001A3518A, LCSC C2888448):** its two plastic pegs go into the two 0.9 mm
   holes; the two bracket pads are soldered, on no net. The button faces the board's east
   edge.
3. **Through-hole parts (SW3, D3): solder every joint fully.** Rev A's first boards came
   back with U7's socket and SW3 only tacked.
4. **D26 (SK6812MINI-012, C2886570): align by the CHAMFERED corner**, which must sit on the
   footprint's chamfer/notch corner, the top-left corner of the LED in the top-view image.
   The footprint's pad numbering is rotated 180° from the datasheet's pin numbers: align by
   the chamfer, not by "pad 1".
5. Substitutions allowed: any 100 nF 50 V X7R 10 % 0603 for C14663, any 10 k 1 % 0603 for
   C25804, any 15 k 1 % 0603 for C22809, and any 2.2 k, 220 Ω or
   4.7 k 1 % 0603 for C4190, C22962 or C23162.
   **No substitution** for U1–U10, J1, SW1–SW3 or D3, and U1 must be the
   **ESP32-S3-WROOM-1-N16R8** (16 MB flash, 8 MB octal PSRAM, LCSC C2913202), never a
   quad-PSRAM (R2) variant: the firmware is built for the octal PSRAM.
6. Please confirm polarity of U2 and U6 (VSSOP-10 pin-1 dot), U3 (SOIC pin-1 dot), U5 and
   U9, D3, D26 and J1 against `airwriter-top.png` (the main board, at about 40 px/mm; it
   also shows the 36 LEDs this order leaves off) before placement.

## Known engineering-query answers (both orders)

- **Soldermask bridges:** no bridges needed on the fine-pitch USB-C (J1) pin rows or between
  the pins of the two transceivers (U2 and U6, VSSOP-10); gang-open them. The file's dams
  there (0.20 mm on J1, 0.15 mm on U2 and U6) are under PCBWay's 0.22 mm matte-black-ink
  minimum, and shrinking apertures onto 0.3–0.35 mm pads is worse than no dams. Every other
  IC's dams clear 0.22 mm; keep the mask as designed. On the AirWriter panel, the art-panel half's fine mask
  features are decorative: produce as-is.
- **JP1** (two gold pads marked LC, south-east of D26): a solder jumper, not a part. No
  paste, nothing fitted, left open; Wells bridges it on the LC after delivery.
- **Via fill:** none. All vias standard plated, unfilled; do not add the via-fill or cap
  process. Twelve board vias meet SMD pads, each on its pad's own net: the five in U4's tab, and
  ground stitch vias on C9.2, C12.2, C17.2, D29.2, D37.2, R1.2 and U1.1 (U1's center paddle
  has its own thermal holes in the footprint). Minor paste wicking into those barrels is
  acceptable.

## Wells's part, after delivery

- **Proved before ordering:** a new, blank PIC12F1840 (the DIP version) took TashTalk in
  board 1's U7 socket (2026-10-29), so a part straight from the factory flashes on the
  board.
- **Per board, after delivery:** solder J2 (Kycon KMDGX-8S-BS, Mouser), and on AirWriter J4
  too. On AirWriter, flash TashTalk into U7 (the AppleTalk Port card's Flash TashTalk key, or
  `POST /api/localtalk/flash`). The LC's J4 stays empty. On the LC, bridge JP1
  (the pads marked LC beside D26) with solder: it feeds D26, the LC's one LED, straight from
  U9, since the LEDs ahead of it in the chain are left off.

## Not part of either order, but buy alongside them

- **Printer cable (J2):** use Apple's own Mac-to-printer cable (590-0552-A / M0197,
  null-modem: 1↔2, 3↔5, 6↔8) or any cable that works between a real Mac and the printer.
  Many reproduction "ImageWriter II" cables are wired straight through and will not work
  with either (buzz pin 1 on one end to pin 2 on the other before trusting one). Avoid
  mini-DIN-8 cables sold for S-Video or Roland gear: they may drop conductors, and the cable
  test reports a cable without its handshake wires.

## Files

Upload only the files in the table at the top. Never upload anything else, and never
`hardware/airwriter/fab/`, a stale rev A package.

Regenerate both packages only with `python3 hardware/combine_boards.py` then
`sh hardware/combined/make_fab.sh`, and check them with the battery in `AGENTS.md`
(Verification): ERC, both DRCs with `--severity-all`, the drill floor, 141 and 63 position
rows (the script asserts both). Both pictures are `kicad-cli pcb render --side top
--quality high --use-board-stackup-colors`, flattened onto white: `combined-top.png` from
`hardware/combined/combined.kicad_pcb` at `-w 2328 -h 1264`, `airwriter-top.png` from
`hardware/airwriter/airwriter.kicad_pcb` at `-w 2600 -h 4200`.
