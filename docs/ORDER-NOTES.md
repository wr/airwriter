# PCBWay order notes: rev 2026.11 combined panel

Paste-ready. Upload **only** `hardware/combined-fab.zip`, `hardware/combined/combined-top.png` and
`hardware/combined/airwriter-top.png`.
Standing rule: quote to Wells before the order is placed. Rev A's order (W654223ASS53,
T-S54W654223A) and its notes are in git history.

## Board spec (order form)

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
- Vias: 0.6 mm pad / 0.3 mm drill, plus five 0.8/0.4, one 0.6/0.4 and one 0.9/0.5 (the
  VBUS via). Drill floor 0.30 mm.
- **NPTH:** 2 × 0.65 mm (J1 pegs) and 4 × 2.75 mm (mounting) only. The **3.8 mm hole is
  PLATED on purpose** (netless gold bore on the art panel; it is in the PTH drill file, do
  not convert it to NPTH).

## Assembly: two variants from the same panel

| Variant | Position file | BOM | Placements |
|---|---|---|---|
| AirWriter | `combined-pos.csv` | `combined-assembly-bom.csv` | 136 |
| AirWriter Lite | `combined-lite-pos.csv` | `combined-lite-assembly-bom.csv` | 59 |

Lite leaves off the LED array (D10–D46 and C30–C66), U9, R20, C21 and C29, and fits R33;
AirWriter leaves R33 off. Both are single-sided (the top side of the file).

1. **J2 (Kycon KMDGX-8S-BS mini-DIN-8): ASSEMBLE.** Not at LCSC; source from Mouser.
   Through-hole, 11 joints.
2. **J4: DO NOT ASSEMBLE, and keep its 11 holes free of solder.** It is fitted by hand with
   the TashTalk add-on, and is absent from both position files. If J2 is wave or selectively
   soldered, mask J4's holes.
3. **U7: fit ONLY the DIP-8 socket, LCSC C72124.** Do not buy or fit the PIC.
4. **D3: through-hole 3 mm flat-top green LED (LCSC C20613324) standing on a HIWA LED4*8
   nylon spacer (LCSC C92326):** leads through the spacer's bore, the LED's flange seated
   on the spacer, lens up. Cathode (pad 1) toward R17, where the silk reads K;
   the spacer hides the square pad.
   **SW1 (BZCN TSB001A3518A, LCSC C2888448):** its two plastic pegs go into the two 0.9 mm
   holes; the two bracket pads are soldered, on no net. The button faces the board's east
   edge.
5. **Through-hole parts (J2, SW3, U7's socket, D3): solder every joint fully.** Rev A's
   first boards came back with U7 and SW3 only tacked.
6. **MT1–MT4: Wurth 9774030151R** M2.5 SMT standoff, not at LCSC; source from Digi-Key or
   consign (labelled "MT1-MT4, Wurth 9774030151R, qty 4 + 2 spares"). Reflow on the top
   side; paste is the ring aperture in the paste layer; the seat centre is bare ENIG with
   no paste.
7. **D10–D46 (37 × SK6812MINI-C, C7423117): align by the CHAMFERED corner**, which must sit
   on the footprint's chamfer/notch corner, the top-left corner of every LED in the top-view
   image. The footprint's pad numbering is rotated 180° from the datasheet's pin numbers:
   align by the chamfer, not by "pad 1". A 180° placement breaks the whole chain.
8. Substitutions allowed: any 10 k 1 % 0603 for C25804, any 15 k 1 % 0603 for C22809 and any
   1 k 1 % 0603 for C21190.
   **No substitution** for U1–U9, J1, J2, SW1–SW3 or D3, and U1 must be the
   **ESP32-S3-WROOM-1-N16R8** (16 MB flash, 8 MB octal PSRAM, LCSC C2913202), never a
   quad-PSRAM (R2) variant: the firmware is built for the octal PSRAM.
9. Please confirm polarity of U2, U3, U6, U8 (SOIC pin-1 dot), U5 and U9, the U7 socket
   notch, D3 and J1 against `airwriter-top.png` (the main board alone, at about 40 px/mm)
   before placement.

## Known engineering-query answers

- **Soldermask bridges:** no bridges needed on the fine-pitch USB-C (J1) pin rows; gang-open
  them. The file's 0.20 mm dams there are under PCBWay's 0.22 mm matte-black-ink minimum,
  and shrinking apertures onto 0.3 mm pads is worse than no dams. Every other IC's dams clear
  0.22 mm; keep the mask as designed. The art-panel half's fine mask features are
  decorative: produce as-is.
- **Via fill:** none. All vias standard plated, unfilled; do not add the via-fill or cap
  process. Thirteen board vias meet SMD pads, all on GND: the five in U4's tab, and stitch
  vias on C9.2, C12.2, C17.2, D29.2, D37.2, R1.2, U1.1 and U8.5 (U1's centre paddle has its
  own thermal holes in the footprint). Minor paste wicking into those barrels is acceptable.

## Not part of the order, but buy alongside it

- **Printer cable (J2):** use Apple's own Mac-to-printer cable (590-0552-A / M0197,
  null-modem: 1↔2, 3↔5, 6↔8) or any cable that works between a real Mac and the printer.
  Many reproduction "ImageWriter II" cables are wired straight through and will not work
  with either (buzz pin 1 on one end to pin 2 on the other before trusting one). Avoid
  mini-DIN-8 cables sold for S-Video or Roland gear: they may drop conductors, and the cable
  test reports a cable without its handshake wires.

## Files

| Upload | Never upload |
|---|---|
| `hardware/combined-fab.zip` | anything else |
| `hardware/combined/combined-top.png` (the panel) | |
| `hardware/combined/airwriter-top.png` (the main board, assembly and polarity reference) | |

Regenerate the package only with `python3 hardware/combine_boards.py` then
`sh hardware/combined/make_fab.sh`, and check it with the battery in `AGENTS.md`
(Verification): ERC, both DRCs with `--severity-all`, the drill floor, 136 and 59 position
rows. Both pictures are `kicad-cli pcb render --side top --quality high
--use-board-stackup-colors`, flattened onto white: `combined-top.png` from
`hardware/combined/combined.kicad_pcb` at `-w 2328 -h 1264`, `airwriter-top.png` from
`hardware/airwriter/airwriter.kicad_pcb` at `-w 2600 -h 4200`.
