# AirWriter enclosure fit fixes — 2026-09-08

Commit: `f61d457` — "Enclosure: fix three measured fit defects (wall/panel flush, DIP slot, boss relief)"

## Summary

Three measured fit defects fixed in `enclosure/`, per the vendor VRML and board GLB
measurements supplied in the task. All numbers derive from `params.py`; nothing is
hardcoded twice.

## Fix 1 — CAVITY double-counted the panel thickness

- `CAVITY`: 14.34 → **12.74** (`enclosure/params.py`)
- Derived automatically: `Z_LID_UNDER` 14.34 → **12.74**, `Z_WALL_TOP` 15.94 → **14.34**,
  `EXT_H` 22.94 → **21.34**
- Verified `lite_lid()`'s plate still lands at Z 12.74..14.34 (leg from Z=0 to `p.CAVITY`,
  plate top at `p.Z_WALL_TOP`) — confirmed by `test_case.TestLiteLid.test_legs_span_the_cavity_to_the_board_face`
  and the new `test_lid_top_matches_the_walls_own_top_nothing_stands_proud`, which compares
  `case.shell()`'s own built wall-top geometry against `case.lite_lid()`'s own built
  plate-top geometry directly (not just each independently against the shared param).
- Rewrote `CAVITY`'s provenance comment in `params.py` and the spec
  (`docs/superpowers/specs/2026-09-08-airwriter-enclosure-design.md` §2 + stack table) to
  record the GLB measurement and **flag** the conflict with the spec's original nut-stack
  derivation: if the PEM nuts really do stand proud of the panel's underside (rather than
  sitting in a lattice counterbore, which the GLB doesn't model), the case will be 1.6mm
  short and this needs re-checking against real hardware before printing.

**Verification:** temporarily reverted `CAVITY` to 14.34 and confirmed
`test_wall_floor_and_stack`, `test_cavity_is_the_minidin_lattice_height_not_nut_plus_lattice`,
`test_external_size`, and `test_z_landmarks` all go red (EXT_H 22.94 vs new 21.34,
Z_LID_UNDER 14.34 vs new 12.74, etc.), then restored. Note:
`test_lid_top_matches_the_walls_own_top_nothing_stands_proud` does **not** independently
catch this bug — both `shell()` and `lite_lid()` derived Z_WALL_TOP from the same (buggy)
constant, so they were self-consistent throughout. That test instead guards the two code
paths from diverging from *each other* going forward; the actual ground truth for this bug
is external (the real GLB), which isn't something the test suite can check without
vendoring the GLB.

## Fix 2 — DIP slot too small in both axes

- Added `DIP_KEY_HALF_WIDTH = 0.65` (from the vendor VRML: keys are 1.300mm wide).
- `DIP_SLOT_MARGIN`: 0.60 → **0.80**, now added to the key band's own edges
  (`centre ± DIP_KEY_HALF_WIDTH`), not the pad-column centres directly
  (`case.dip_slot()`).
- `DIP_SLOT_Z0`/`DIP_SLOT_Z1`: 7.60/9.50 (1.90mm) → **6.60/10.60** (4.00mm), giving ~1.5mm
  of tool access above and below the 8.11–9.01 key band.
- New slot Y span, measured directly off `case.dip_slot().bounding_box()`: **19.61 .. 40.29**
  (the real key band's own edges are 20.41/39.49; the slot's edges sit 0.80mm — `DIP_SLOT_MARGIN`
  — further out on each side: 20.41-0.80=19.61, 39.49+0.80=40.29).

**Verification:** reproduced the exact old bug by temporarily hardcoding `edge = 0.60` in
`case.dip_slot()` (matching the old margin-on-pad-centres behavour) and confirmed both
`test_dip_slot_is_in_the_west_wall_at_the_key_band` and the new
`test_dip_slot_fully_contains_the_key_band_with_real_margin` go red — the failure
reproduces the reported defect almost exactly: slot edge at Y=20.46 vs. the real key edge
at Y=20.41, a 0.05mm intrusion, matching the task's own numbers. Restored and confirmed
green.

## Fix 3 — standoffs poking through the board's bottom layers

- Added `BOSS_RELIEF = 0.10` (`enclosure/params.py`).
- `standoffs()` boss height now stops at `Z_BOARD_BOTTOM - BOSS_RELIEF` instead of running
  flush to `Z_BOARD_BOTTOM`. `STANDOFF` itself is unchanged.
- New test `test_standoffs_clear_the_boards_underside_by_boss_relief`.

**Verification:** temporarily set `BOSS_RELIEF = 0.0` and confirmed the new dedicated test
goes red (`gap == 0.0` not `> 0.0`). Note: the existing
`test_standoffs_sit_at_the_mount_holes_and_reach_the_board` did **not** go red at
`BOSS_RELIEF=0` — it derives its expectation from the same param the code uses
(`p.Z_BOARD_BOTTOM - p.BOSS_RELIEF`), so it's a self-consistency check, not independent
coverage. The new dedicated test (`assertGreater(gap, 0.0)`) is what actually proves relief
is being applied at all, independent of the param's value.

## New / changed values (from the test run and `export.py`)

- `CAVITY` = 12.74, `Z_LID_UNDER` = 12.74, `Z_WALL_TOP` = 14.34
- `EXT_H` = **21.34** (external footprint stays 68.00 × 110.27 × 21.34mm — confirmed by
  `export.py`: `airwriter-tray: 68.00 x 110.27 x 21.34 mm, 34.3 cm3`)
- Lite lid: `airwriter-lite-lid: 62.00 x 104.27 x 14.34 mm, 11.6 cm3`
- DIP slot Z span: **6.60 .. 10.60** (4.00mm open, was 7.60..9.50 / 1.90mm)
- DIP slot Y span: **19.61 .. 40.29**, measured directly off `case.dip_slot().bounding_box()`.
  The key band's own real edges (pad-column extreme ± `DIP_KEY_HALF_WIDTH`) are at
  Y≈20.41/39.49; the slot now sits `DIP_SLOT_MARGIN` (0.80mm) further out on each side.
  With the old buggy margin=0.60 added straight to the pad-column centre (no half-width),
  the slot's own edge measured **Y=20.46** against the real key edge at **Y=20.41** — a
  0.05mm intrusion into the wall, matching the task's report exactly.
- `BOSS_RELIEF` = 0.10; standoff boss top now at `Z_BOARD_BOTTOM - 0.10` = -1.70 (was -1.60).

## Test suite

68 → **71 tests**, all passing. `./enclosure/build.sh` runs end to end (tests + export),
~62s for tests. `out/` is gitignored and was not committed. Printability gate
(`test_printable.py`, both `TestTray` and `TestLid`) stayed green with no changes needed —
the widened DIP slot's roof (`_dip_slot_roof()`) is parametric off `dip_slot().bounding_box()`
and followed the new Z span automatically without exceeding the unsupported-area threshold.

## Docs

Updated `docs/superpowers/specs/2026-09-08-airwriter-enclosure-design.md`:
- §2 stack table: cavity 14.34 → 12.74, external height 22.94 → 21.34.
- §2 body text: replaced the nut-stack derivation with the GLB measurement, kept the
  original derivation as "superseded, kept for the record", and added the same open flag
  as `params.py` about unverified real-hardware nut stand-off.
- §5 (Fastening): the Lite leg's spacer length reference, 14.34mm → 12.74mm.

## Constraints honored

- Nothing under `hardware/` modified.
- External footprint unchanged (68.00 × 110.27); only height changed.
- `unittest`, no pytest; `Shape.is_valid` used as a property throughout.
- No subagents dispatched — all work done directly in this session.
