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
- Min trace/space **6/6 mil**. Vias 0.6 mm pad / 0.3 mm drill; drill floor 0.30 mm.
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
6. Substitutions allowed: D3 (LCSC C72043, if still out of stock — any 0603 green LED,
   cathode per the silk bar) and R3/R14 (C25804 was restocked 2026-08-28; any 10 k 1%
   0603 stays pre-authorised as fallback). **No substitution** for U1–U9, J1, S1, SW1–SW3
   — and U1 must be the quad-PSRAM **N8R2** module, never an octal-PSRAM variant.
7. Please confirm polarity of U2, U3, U6, U8 (SOIC pin-1 dot), the U7 socket notch, D3
   and J1 against `combined-top.png` before placement.

## Files

| Upload | Never upload |
|---|---|
| `hardware/combined-fab.zip` | anything else — stale single-board packages were deleted 2026-08-28 |
| `hardware/combined/combined-top.png` (assembly/polarity reference) | |

Regenerate the package only via `python3 hardware/combine_boards.py` then
`sh hardware/combined/make_fab.sh`; verify with the battery in `docs/HANDOFF.md`
(ERC, both DRCs with `--severity-all`, drill grep, pos row count 127).
