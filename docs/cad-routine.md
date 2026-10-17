# CAD routine (`enclosure/`)

How to design, check and iterate a printed part. Follow it for any new part or change in
`enclosure/`. Gotchas are in `docs/traps.md` § Enclosure.

## The loop

1. **Measure first.** Before modelling anything that touches a real part, measure it with
   calipers. KiCad's models are often wrong: the mini-DINs stand 13.04, not 12.75; SW3's
   plunger is 3.1 tapering to 3.5, standing 0.2 E / 0.2 S of the model's axis. Put each
   measurement in `params.py` as a named constant, with who measured it, when and how.
2. **Read positions, never retype them.**
   - Board and panel: `board_geom` (outline, footprints, courtyards, keepouts, panel dots and
     windows).
   - Part bodies: `components.envelopes()`, from KiCad's STEP export.
   - Connectors: `case.connector_solid()`, from the vendor STEP.

   A board change then re-cuts the part on the next build.
3. **Design, and get it approved.** For a new part, write a spec in `docs/superpowers/specs/`,
   then a plan. For a tweak, a few lines in chat. Decide up front:
   - **Print orientation.** Keep every overhang at 45° or steeper in that orientation: a cone
     instead of a flat ledge; a flat lintel only as a short bridge across an opening.
   - **How it goes in.** Anything lowered onto the board must clear the parts' vertical
     shadows. Anything that slides or rocks in needs an insertion sweep
     (`docs/enclosure-build-log/flex-wall/sweep.py`).
   - **Whether it flexes or carries load.** If so, run FEA (`flex-wall/fea.py`) and name the
     material it assumes.
4. **Test first.** Write the test for the behaviour the part exists for, watch it fail, then
   model. Examples: a light column is open, a tube is sealed, the part clears part X, fits the
   tray, prints without supports. `./build.sh` runs the suite and writes `out/*.{step,stl}`.
5. **Look before printing.**
   - `.venv/bin/python tools/render.py out/<part>.stl /tmp/x.png`, plus `--box x0 x1 y0 y1` for
     a close-up of what changed. Check it with your own eyes.
   - For a fit-critical tweak, print a coupon first:
     `.venv/bin/python tools/coupon.py <part> x0 x1 y0 y1`, which writes
     `out/coupons/<part>.stl` (gitignored).
   - The helpers need `pip install -r tools/requirements.txt` in the venv.
6. **Print, measure, encode.**
   - Send the STL with its material and orientation.
   - Turn every finding into a parameter change plus a test that pins the measured value.
   - Two prints that straddle the answer: split the difference.
7. **Close out.**
   - `README.md`: how to print it and assemble it.
   - The spec's amendment block.
   - AGENTS.md state.
   - The Linear ticket.
   - Commit the STLs.

## Numbers that have worked

| What | Value | Where |
|---|---|---|
| Board / panel pocket | 0.30 a side | `POCKET_CLEAR` (0.20 bound on PCBWay's ±0.2 outline) |
| Connector windows | vendor hull + 0.40 | `PORT_CLEAR`; front part of the hull only |
| Part clearance under a lowered part | +0.40 sideways, +0.50 on top | `PART_CLEAR`, `PART_TOP_CLEAR` |
| Guiding fit (steadies, no drag) | 0.20–0.25 a side | test-button collar |
| Snug against the tray | 0.10 | lattice lip |
| Light-tight wall | 0.80 (two perimeters) | lattice walls |
| Wall that flexes in use | 1.70 PETG, the same on every wall (0.80 split; 1.20 on one wall only showed lines) | tray walls below the rim |
| Light pipe in clear PETG | made D3 dimmer | dropped; rev B dome LED, W-1043 |
