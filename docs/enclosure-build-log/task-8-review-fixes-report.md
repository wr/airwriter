# Task 8 review-fixes report: printability gate corrections

Branch `wells/airwriter-enclosure`. No geometry changed except one comment
in `case.py` (lines ~444-457, the insert-bore cone).

## Finding 1 (CRITICAL) -- lid check was checking the wrong model direction

`downward_flats()` looked for model -Z faces on the lid; the lid prints
plate-down, which inverts the model (print-down is model +Z), so that check
could only ever see the always-supported plate-top side.

Fix: split the shared engine (`_overhangs(part, sign, bed_z, min_area)`)
from two clearly-named, clearly-documented public entry points --
`downward_flats()` for a part printed in its own modelled orientation (the
tray) and `upward_flats()` for a part printed inverted relative to its model
(the lid). Each docstring states which model direction is print-down for
that part. `TestLid` now calls `upward_flats(self.lid, ceiling_z=p.Z_WALL_TOP)`.

**Broken-variant proof** (reviewer's stepped-leg lid: r=3.00 -> r=1.50 above
Z=7.00):
- Corrected `upward_flats()`: **4 offenders**, each 21.21 mm^2, normal
  (0,0,1), at model Z=7.00 (print height 8.94 mm -- `Z_WALL_TOP(15.94) -
  7.00`), matching the reviewer's numbers exactly.
- The original blind check (`downward_flats(lid, 0.0)`, brief's Step-1
  sketch, filtered against `Z_WALL_TOP - PANEL`): **0 offenders** -- silently
  blind, reproduced.
- Restored (real `lite_lid()`): `upward_flats()` returns `[]` -- confirmed
  clean on the shipped geometry, unchanged.

## Finding 2 (Important) -- only exact-horizontal faces were caught

`abs(n.Z + 1.0) < 1e-6` missed anything tilted even slightly off flat.

Fix: `FDM_OVERHANG_LIMIT = cos(45deg)`, compared as `sign * n.Z >
FDM_OVERHANG_LIMIT + 1e-6` (a strict `>`, with a small float-noise margin) --
so a face exactly at 45 degrees passes and anything flatter fails, matching
this project's own 45-degree ridge roofs and insert-bore cones exactly at
the limit.

**Broken-variant proof** (synthetic tilted-ceiling pocket, `min_area=0.1`):

| tilt | OLD (exact-horizontal) | NEW (45deg limit) |
|---|---|---|
| 0.00 deg | 1 offender | 1 offender |
| 0.05 deg | 1 offender | 1 offender |
| 0.50 deg | 0 offenders (blind) | 1 offender |
| 5.00 deg | 0 offenders (blind) | 1 offender |
| 44.99 deg | 0 offenders | 1 offender |
| **45.00 deg** | 0 offenders | **0 offenders (passes)** |
| 45.01 deg | 0 offenders | 1 offender |
| 60.00 deg | 0 offenders | 0 offenders |

Chose strict `>` over `>=` so exactly-45-degree faces pass: this project's
own ridge roofs and insert-bore cones sit exactly at that limit by
construction, and `>=` would flag them.

## Finding 3 (Important) -- per-face min_area hid a tiled ceiling

Fix: candidates are grouped into bands by `round(center.Z, 2)` and the
threshold applies to each band's summed area.

Applying this (still at the original `min_area=2.0`) surfaced a **real**,
previously-invisible residual on the shipped tray: the J2/J4 port roofs
(`_port_roof()`) use a rectangular base sized to each grown connector hull's
full bounding box, but the hull's actual top is domed/tapering, not flat
edge-to-edge -- so the roof's own dead-flat rectangular base sticks out past
the hull's real (smaller, curved) apex in a few places, leaving small
facets of the *roof's own underside* exposed on the finished tray. Measured:
52 facets, 9.70 mm^2 total across 3 Z-bands (2.16 / 3.37 / 4.17 mm^2 each,
mixing J2 and J4 since both land at nearly identical heights), every single
facet individually under 2.0 mm^2 (max 1.77 mm^2) -- exactly the mechanism
Finding 3 describes, and consistent with the review's own measurement
(~4 mm^2/port, safe at 0.4 mm nozzle).

This is real, harmless geometry (not something `case.py` may be changed to
fix per the task's constraints), so raised `MIN_BAND_AREA` from 2.0 to
**5.0 mm^2** -- comfortably above the largest real band (4.17 mm^2) and
still far below every genuine offender this gate has ever caught.

**Broken-variant proof** (reverted `_print_reliefs()` entirely, i.e. the
pre-Task-8 geometry): 14 offenders at `min_area=5.0`, reproducing the
original findings exactly -- DIP slot ceiling 53.1 mm^2, J2/J4 port tops
32.9 mm^2 each, J1 (USB-C) port top 8.4 mm^2, all far above the 5.0 mm^2
threshold. Restored: `downward_flats()` returns `[]` on the real `tray()`.

## Finding 4 (Important) -- front-wall regression net missed the roofs

`TestPorts.setUpClass` only ever built `tray_shell = shell() -
port_cutouts()`, which never subtracts `_print_reliefs()`.

Fix: added `cls.tray_full = case.tray()` and two new tests re-running the
same probes against it --
`test_openings_clear_each_connector_by_at_least_0_30_on_the_full_tray` and
`test_front_wall_retains_material_above_and_below_each_port_on_the_full_tray`.

Measured on the real, shipped `tray()`: clearance 0.3849 mm for all three
connectors (spec floor 0.30), every top/bottom band 0.8000 mm (floor 0.50) --
matches the review's own numbers exactly.

**Broken-variant proof** (deliberately oversized port-roof rise, 5.0 mm
instead of the real ~1.4 mm, eating up into the top band):
- OLD test (against `tray_shell`, blind to `_print_reliefs()`): J2/J4 top
  stayed **0.8000 mm** -- didn't move, because it never saw the roof at all.
- NEW test (against `tray_full`, the real assembled tray with the broken
  roof): J2/J4 top dropped to **0.4574 mm**, correctly **below** the 0.50 mm
  floor -- fails. J1 unaffected (0.8000 mm), as expected (J1's roof wasn't
  touched).
- Restored (real `tray()`): both new tests pass with the 0.3849 mm /
  0.8000 mm numbers above.

## Minor fixes

- `test_printable.py`'s wall-thickness test: replaced the dead `/ p.WALL`
  arithmetic (which canceled to the constant 22.4 for every probe
  regardless of shape) with `want` spelled out per probe as `min_thickness *
  lateral_area`, and added a fourth probe for the floor (previously
  unchecked): `p.FLOOR * 4.0 * 4.0 = 38.4 mm^3` at a point clear of every
  mount hole and standoff (case (31.0, 52.0)). Passes on shipped geometry.
- `case.py` lines ~444-457: corrected the insert-bore-cone comment, which
  claimed the blind bore's flat cap was "an unsupported bridge once the lid
  prints plate-down" -- it isn't. Verified directly (bare leg + bore, no
  cone): that cap's normal is model (0,0,-1); plate-down print-down is model
  +Z, so a model -Z face is print-UP, i.e. an ordinary supported floor. The
  cone is kept (matches `screw_holes()`'s pattern, gives the insert a
  lead-in) but the comment no longer claims it's fixing a bridge.
- `README.md`: softened "if it passes, it slices" to state plainly that the
  gate is a geometric proxy (unsupported flats + minimum wall thickness),
  not a guarantee against warping, adhesion, or real bridging-distance
  limits, and points at the first-print checklist as the backstop.

## Full suite

```
.venv/bin/python -m unittest discover -p 'test_*.py'
Ran 57 tests in ~57s
OK
```

55 previously-passing tests, unchanged and still green, + 2 new tests
(`TestPorts` full-tray front-wall/clearance regressions). No regressions.
`hardware/` untouched; the two `.kicad_pro` files shown modified in `git
status` are pre-existing, unrelated to this session.

## Concerns carried forward

- `MIN_BAND_AREA = 5.0` is a judgment call, not something the brief
  specified a number for -- it's derived from the actual measured harmless
  residual (4.17 mm^2 max band) plus headroom, and cross-checked against
  every known real offender (8.4 - 53.1 mm^2) to confirm it still catches
  them. If a future revision needs the gate more sensitive than 5.0 mm^2 in
  a *legitimately* risky region, this constant would need re-deriving --
  it's documented inline in `test_printable.py` with the exact numbers and
  reasoning.
- The J2/J4 roof-vs-hull seam itself (Finding 3's root cause) is not fixed
  in geometry, per the task's explicit "no geometry changes" constraint. It
  is real, measured, and the review already judged it safe at these
  dimensions; `_port_roof()`'s docstring cross-references still apply.
