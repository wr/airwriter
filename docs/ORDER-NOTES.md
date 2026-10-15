# PCBWay order notes — combined panel (final, 2026-08-28)

Canonical, paste-ready. Supersedes the notes inside `docs/review-2026-08-25-pre-order-2.md`.
Upload **only** `hardware/combined-fab.zip` + `hardware/combined/combined-top.png`.
Standing rule: quote to Wells before the order is placed.

## Board spec (order form)

- One customer panel, profile **62.0 × 208.54 mm**, containing **TWO different designs**:
  main board 62 × 104.27 mm (bottom half of the file), art cover panel 62 × 104.27 mm
  (top half), butted flush.
- Separate with **ONE V-score** along y = 60, full panel width — drawn and labelled
  "V-CUT" on the User.Comments layer. There is **one 2 × 4 mm routed relief at each end**
  of the score (rounded corners). Copper is set back **0.5 mm** from the score line on
  both sides and both layers, by design.
- **2 layer, 1.6 mm FR4, 1 oz, matte BLACK soldermask both sides, white silkscreen, ENIG.**
- Min trace/space **0.15 mm (6 mil)** generally. **Exception: four spots need 0.12 mm
  (4.7 mil) space**, all around U4's SOT-223 tab at approx. x 115–120 / y 113–120 mm of
  the main-board half (the project's `GND_STITCH_012` rule area). Please quote/accept
  4.7 mil space at those points; everywhere else is ≥ 0.15 mm. Vias 0.6 mm pad / 0.3 mm
  drill; drill floor 0.30 mm.
- **NPTH:** 2 × 0.65 mm (J1 pegs) and 4 × 2.75 mm (mounting) only.
  The **3.8 mm hole is PLATED on purpose** (netless gold bore on the art panel — it is in
  the PTH drill file; do not convert it to NPTH).

## Assembly

1. Single side (top side of the file), ~127 placements per the position file.
2. **DO NOT fit or buy J2, J4** (mini-DIN-8; customer hand-solders after delivery). They
   are absent from the position file by design and appear in the BOM only with
   DO-NOT-ASSEMBLE notes.
3. **U7: fit ONLY the DIP-8 socket, LCSC C72124.** Do not buy or fit the PIC.
4. **MT1–MT4: Wurth 9774030151R** M2.5 SMT standoff — not on LCSC; source from Digi-Key
   or consigned. If consigned, the package is labelled **"MT1-MT4, Wurth 9774030151R,
   qty 4 + 2 spares"**. Reflow on the top side; paste is the ring aperture in the paste
   layer; the seat centre is bare ENIG with no paste.
5. **D10–D46 (37 × SK6812MINI-C, C7423117): align by the CHAMFERED corner**, which must
   sit on the footprint's chamfer/notch corner — the top-left corner of every LED in the
   top-view image. The footprint's pad numbering is rotated 180° from the datasheet's pin
   numbers: align by the chamfer, not by "pad 1". A 180° placement breaks the whole chain.
6. Substitutions allowed: D3 (LCSC C12624 KT-0603G; if out of stock — any 0603 green LED,
   cathode per the silk bar) and R3/R14 (C25804 was restocked 2026-08-28; any 10 k 1%
   0603 stays pre-authorised as fallback). **No substitution** for U1–U9, J1, S1, SW1–SW3
   — and U1 must be the quad-PSRAM **N8R2** module, never an octal-PSRAM variant.
7. Please confirm polarity of U2, U3, U6, U8 (SOIC pin-1 dot), the U7 socket notch, D3
   and J1 against `combined-top.png` before placement.

## Known engineering-query answers

- **Soldermask bridges (asked 2026-08-25 on order W654223ASS53):** no bridges needed on
  the fine-pitch USB-C (J1) pin rows — gang-open them. The file's 0.20 mm dams there are
  under PCBWay's 0.22 mm matte-black-ink minimum, and shrinking apertures onto 0.3 mm pads
  is worse than no dams. Every other IC's dams clear 0.22 mm; keep the mask as designed.
  The art-panel half's fine mask features are decorative — produce as-is.
- **Order W654223ASS53 was placed 08-24 with superseded files** (pre-dating the 08-25 DOA
  fixes and everything since). It must never build as uploaded: replace its files with the
  current `combined-fab.zip` or cancel it.

- **Via fill (asked 2026-08-29):** no resin fill — all vias standard plated, unfilled.
  Do not add the via-fill/cap process. For the record, **13 vias touch SMD pads**, all on
  GND: the 0.3 mm thermal vias in U1's centre paddle and the five in U4's tab, plus GND
  stitch vias whose 0.3 mm drill sits fully inside the pad on **U3.10, C9.2, C12.2 and
  U1.1**, and four more tangent to a pad edge (D29.2, D37.2, U8.5, R1.2). Minor paste
  wicking into those barrels is acceptable; no fill, no cap, no plug.

- **File replacement (2026-09-02, order T-S54W654223A):** PCBWay's 09-02 BOM quote and
  centroid were generated from the 08-28 upload (identical to main's zip at the time).
  Message sent with `combined-fab-rev4-32859f3.zip`: "Please replace the Gerber/drill files
  with the attached zip. Same outline, same BOM and centroid (no parts or positions
  changed); the update adds thermal reliefs on the through-hole pads, re-routes J2, and adds
  four 0.3 mm vias. Board spec: four spots around U4's SOT-223 tab need 0.12 mm (4.7 mil)
  space, everywhere else ≥ 0.15 mm. Via fill: none; 13 vias touch SMD ground pads, minor
  wicking acceptable. Question: your centroid places SW1 at X 157.95 vs 157.225 in our file
  — confirm that is your body-centre correction and the push plate faces the board edge."
  Their BOM was checked line by line against ours: all parts, quantities and DNP flags
  match; D3 substituted with NCD0603G1 (fine); MT1–4 sourced at $0.48; $57.29 for 3 units.

## Not part of the order, but buy alongside it

- **Printer cable (J2):** use Apple's own Mac-to-printer cable (590-0552-A / M0197, null-modem: 1↔2,
3↔5, 6↔8) or any cable that works between a real Mac and the printer; many reproduction
"ImageWriter II" cables are wired straight-through and will not work with either (buzz
pin 1 on one end to pin 2 on the other before trusting one). Avoid mini-DIN-8 cables sold
for S-Video or Roland gear — they may drop conductors. (Research 2026-09-02: pinouts.ru,
colino.net's bench trace, VCF forum thread; colino.net found Pro Signal and CableLeader
"ImageWriter" cables wired straight.)

## Files

| Upload | Never upload |
|---|---|
| `hardware/combined-fab.zip` | anything else — stale single-board packages were deleted 2026-08-28 |
| `hardware/combined/combined-top.png` (assembly/polarity reference) | |

Regenerate the package only via `python3 hardware/combine_boards.py` then
`sh hardware/combined/make_fab.sh`; verify with the battery in `AGENTS.md` (Verification)
(ERC, both DRCs with `--severity-all`, drill grep, pos row count 127).
