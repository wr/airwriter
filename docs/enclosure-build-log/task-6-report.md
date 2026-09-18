# Task 6 report: DIP slot, reset pinhole, carve-outs, and `tray()`

Commit: `875b1d3` on `wells/airwriter-enclosure`.

## What was implemented

- `case.dip_slot()` -- S1's key band, cut through the west wall. Same geometry as
  the brief, with Ruling 2's typo fixed: `Pos(-p.WALL / 2 + 0.5, ...)`, spanning
  X from `-WALL` (flush with the outer face) to `+1.0` (1mm overshoot past the
  inner face). Docstring corrected from the brief's stale "0.75mm recessed" to
  the true **0.80mm** (recomputed below).
- `case.reset_pinhole()` -- a pinhole through the **east** wall on SW1's
  actuator axis, `p.RESET_PINHOLE_D` diameter, `p.WALL + 2.0` long (1mm
  overshoot each end). Z comes from the new `p.RESET_ACTUATOR_Z`, not the
  brief's guess.
- `case.tray()` -- `shell() + standoffs() - screw_holes() - port_cutouts() -
  dip_slot() - reset_pinhole()`. No `carveouts()` term (Ruling 1).
- `params.RESET_ACTUATOR_Z = 3.5 / 2` with the derivation below, replacing the
  now-dead `CARVEOUT_CLEAR` (nothing referenced it once `carveouts()` was
  dropped).
- `case._usbc_flare()`'s comment: `0.109mm` -> `0.091mm`. Nothing else in that
  comment or function touched.
- **`carveouts()` was deliberately not written.** Ruling 1 in the task brief:
  the brief's version would subtract cylinders from U4/R3/U9's positions, but
  the tray's cavity is a single open prism there and all three parts sit well
  inboard of every wall, so the boolean is a no-op and the brief's own test
  (`self.t & probe` where `probe` is never near the real tray) passes without
  asserting anything real. Replaced with
  `test_internal_parts_keep_at_least_1mm_of_air_to_the_tray` (see below).

## RULING 2 verification: the dip_slot() typo

Corrected expression: `-p.WALL / 2 + 0.5` (brief's `- 0.5 - 0.5`, i.e. the
extra `- 0.5`, was the bug). Added
`test_dip_slot_spans_the_wall_with_a_1mm_inward_overshoot`, which pins the
exact span: `bb.min.X == -p.WALL` (-3.000), `bb.max.X == 1.0`. With the
original typo reinstated, the slot centres on the wall's outer face instead
of overshooting past the inner one: `bb.min.X = -3.5`, `bb.max.X = 0.5` --
caught by this test (see red/green evidence below).

Recomputed the key recess with `WALL=3.00`: key tips land at KiCad x=97.80,
case X = -2.20. Wall outer face is at X = -WALL = -3.00. Recess = 3.00 - 2.20
= **0.80mm**, not the brief's stale 0.75mm (computed under an earlier,
non-uniform WALL). `dip_slot()`'s docstring now says 0.80mm; I did not touch
the older, unrelated `WALL` docstring in params.py which still says "0.75mm
recessed" -- see Concerns.

## SW1 actuator-Z derivation (replacing the brief's guess)

The brief's `z = (DIP_SLOT_Z0 + DIP_SLOT_Z1) / 2 - 5.0` (= 3.55) was an
explicit guess ("SW1's axis, low on the wall"). SW1 (`airwriter:
EVQP44B3M_SidePress`) has `model=None` on the board -- no STEP to boolean
against directly. Two independent readings, cross-checked and consistent:

1. **From the footprint's own descr.** `EVQP44B3M_SidePress.kicad_mod`'s descr
   records the body as "6.2 x 2.55 x 3.5 mm" and states "push plate faces the
   edge" -- a side-press SMD switch. Like any SMD component, the body sits
   flush on the board (Z=0 at its base); a side-actuated button is naturally
   centred on the body's own height. Half of 3.5mm = **1.75mm** above the
   board's top face.
2. **From `EVQP44B3M_SidePress.wrl`** (present in the library, hand-authored
   per its own header comment, never wired to the footprint's `(model ...)`
   block -- and OCCT can't import `.wrl` regardless). Its header states values
   are stored as `mm / 2.54`. The body `Transform`'s `translation` is `0 0
   0.689` with `geometry Box { size ... 1.378 }` (Z-size). Converting:
   `0.689 * 2.54 = 1.7499mm` (body Z-centre) and `1.378 * 2.54 = 3.5000mm`
   (body Z-height) -- body spans Z 0 to 3.5mm exactly, confirming "sits flush
   on the board." The actuator-plate `Transform` uses the **same** Z
   translation (`0.689`), i.e. the actuator is explicitly centred on the body,
   independently confirming derivation 1.

Both land on **Z = 1.75mm** above the board's top face (= the case frame's
Z=0). `params.RESET_ACTUATOR_Z = 3.5 / 2`. I'm confident in this: it's not
just kept-the-guess or invented-a-number -- it's derived from the footprint's
own stated body height, and cross-checked against a second, independent
source (the `.wrl`'s explicit transform) that lands on the same number to
4 decimal places.

Added `test_reset_pinhole_z_matches_sw1s_derived_actuator_height`, asserting
the pinhole's Z-centre equals `3.5 / 2` (computed independently in the test,
not by importing `p.RESET_ACTUATOR_Z`).

## RULING 1 verification: internal-parts air test

Real vendor STEP bounding boxes for U4/R3/U9 (via KiCad's bundled 3D library
at `/Applications/KiCad/KiCad.app/Contents/SharedSupport/3dmodels`, since
`board_geom.py` only resolves `${KIPRJMOD}` paths, not the system
`${KICAD10_3DMODEL_DIR}` these three use -- not committed/wired into
`board_geom.py`, which is out of this task's file scope):

| ref | STEP | X x Y x Z (mm) |
|---|---|---|
| U4 | `Package_TO_SOT_SMD.3dshapes/SOT-223.step` | 7.0 x 6.5 x 1.7 |
| R3 | `Resistor_SMD.3dshapes/R_0603_1608Metric.step` | 1.6 x 0.8 x 0.45 |
| U9 | `Package_TO_SOT_SMD.3dshapes/SOT-353_SC-70-5.step` | 2.1 x 2.0 x 1.1 |

(Note: the ruling text's "tallest is ~1.1mm" undersells U4 at 1.7mm; doesn't
change the conclusion -- still tiny against the 14.34mm cavity.)

`test_internal_parts_keep_at_least_1mm_of_air_to_the_tray` builds, per part, a
cylinder centred at the part's case-frame (x, y): radius = (larger of X/Y
extent)/2 + 1.0mm, height = 0 to (Z-extent + 1.0mm). If this grown probe
doesn't intersect `tray()`, the part clears the tray by >=1mm in every
direction the probe reaches (radially and above). Confirmed real (non-probe)
clearance directly with `tray().distance_to(probe)` at each part's own
(un-grown) size: **4.60mm** for all three -- this is `p.STANDOFF + p.PCB`,
the fixed vertical gap from the board's top face down to the floor's top,
which turns out to be the binding constraint for all three (they're all tens
of mm inboard horizontally). Comfortably clears the 1.0mm floor with margin
to spare, and the test is sensitive to the case the ruling actually cares
about -- a wall or boss encroaching horizontally (see red/green evidence).

## Red/green evidence (every test, per the verification bar)

For each: broke the guarded thing, ran the single test, confirmed FAIL,
restored via the pre-edit backup, confirmed PASS. Diffs against the backups
were empty after every restore (verified with `diff`).

| test | break | result |
|---|---|---|
| `test_dip_slot_spans_the_wall_with_a_1mm_inward_overshoot` | reinstated the brief's typo (`- 0.5 - 0.5`) | `AssertionError: -3.5 != -3.0` |
| `test_reset_pinhole_z_matches_sw1s_derived_actuator_height` | reverted `z` to the brief's guess formula | `AssertionError: 3.55 != 1.75` |
| `test_dip_slot_is_in_the_west_wall_at_the_key_band` | dropped `DIP_SLOT_MARGIN` from `y0, y1` | `AssertionError: 21.06 != 20.46` |
| `test_reset_pinhole_is_in_the_EAST_wall` | mirrored the pinhole onto the west wall | `AssertionError: 1.0 not >= 65.0` |
| `test_tray_is_one_valid_watertight_solid` | added a disjoint floating cube to `tray()`'s body | `AssertionError: 2 != 1` |
| `test_tray_keeps_its_external_envelope` | added an oversized box to `tray()`'s body (EXT_W+5) | `AssertionError: 73.0 != 68.0` |
| `test_internal_parts_keep_at_least_1mm_of_air_to_the_tray` | inflated the test's own margin to 25mm (probe reaches the west wall) | `AssertionError: 1669.89 != 0.0` (U4) |

`test_dip_slot_is_in_the_west_wall_at_the_key_band`'s original X-span
assertions (`assertLessEqual`/`assertGreaterEqual`) are not sensitive to
Ruling 2's exact typo (both `-3.5<=-3.0` and `0.5>=0.0` still hold), which is
exactly why `test_dip_slot_spans_the_wall_with_a_1mm_inward_overshoot` was
added as an exact-span check -- it is what actually catches that typo.

Full suite before and after every break/restore cycle: 44/44 green
(`.venv/bin/python -m unittest discover -p 'test_*.py'`, 35.6s), no
regressions against the 37 that passed before this task.

## Files changed

- `enclosure/case.py` -- `dip_slot()`, `reset_pinhole()`, `tray()` added;
  `_usbc_flare()` comment digit fixed (0.109 -> 0.091), nothing else in that
  function touched.
- `enclosure/params.py` -- `RESET_ACTUATOR_Z` added (with derivation and
  Ruling 1 note) in place of the now-unused `CARVEOUT_CLEAR`.
- `enclosure/test_case.py` -- `TestSideFeaturesAndTray` added: 7 tests (the
  brief's 4 non-carveout tests, `test_dip_slot_spans_the_wall_with_a_1mm_
  inward_overshoot`, `test_reset_pinhole_z_matches_sw1s_derived_actuator_
  height`, and `test_internal_parts_keep_at_least_1mm_of_air_to_the_tray`
  replacing the brief's vacuous `test_carveouts_clear_the_named_internal_
  parts`).

## Self-review findings

- Removed `CARVEOUT_CLEAR` from `params.py` rather than leaving it dead --
  nothing referenced it once `carveouts()` was dropped, and an unused
  "chosen number" with no consumer is exactly the kind of drift AGENTS.md
  and this project's own conventions warn about.
- `dip_slot()` and `reset_pinhole()` both use a 1mm-per-end overshoot
  convention for clean booleans, but `dip_slot()`'s corrected expression
  only overshoots on one end (flush with the outer face on the other) while
  `reset_pinhole()` overshoots both ends. This asymmetry is Ruling 2's
  explicit instruction for `dip_slot()`, not something I introduced -- kept
  as specified.
- Checked `git status` before committing and staged only
  `enclosure/{case,params,test_case}.py`; left the pre-existing unrelated
  working-tree changes (`docs/superpowers/specs/...`, both `.kicad_pro`
  files, `firmware/` build artifacts) untouched, per "never modify anything
  under `hardware/`" and general hygiene about not bundling unrelated diffs.

## Concerns

- `params.py`'s `WALL` docstring still says "S1's piano keys sit 0.75mm
  recessed" -- stale now that `WALL` is 3.00 uniform (the true figure is
  0.80mm, as `dip_slot()`'s new docstring now says). This is outside the
  file list and correction I was explicitly given ("Also, one small
  correction while you are in `case.py`" named only the `0.109` ->
  `0.091` fix), so I left it rather than editing unscoped text. Flagging
  for a follow-up.
- The `test_internal_parts_keep_at_least_1mm_of_air_to_the_tray` test's real
  binding constraint (4.60mm, the fixed board-to-floor gap) is the same for
  all three parts and independent of each part's own footprint size or
  position -- it isn't testing "this specific part's own clearance" so much
  as "the fixed vertical gap plus radial reach," which is still exactly what
  Ruling 1 asked for (a test sensitive to a wall or boss moving over one of
  these parts), but is worth knowing if a future board revision moves one of
  these parts much closer to a wall (radius+1mm would then need to be
  compared against actual wall proximity, not just the vertical gap).
- U4/R3/U9's STEP-measured sizes (7.0x6.5x1.7, 1.6x0.8x0.45, 2.1x2.0x1.1) are
  hardcoded in the test with provenance comments rather than computed live,
  because resolving `${KICAD10_3DMODEL_DIR}` requires plumbing outside this
  task's file scope (`board_geom.py` isn't in the brief's Modify list). If
  the vendor footprints for these refs ever change, this test won't notice
  automatically -- unlike `case.connector_solid()`'s live STEP imports for
  J1/J2/J4.
