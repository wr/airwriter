# Task 10 report: rounded corners

Branch `wells/airwriter-enclosure`, commit `9a1625c` "Enclosure: rounded
corners, matching the board and art panel (Task 10)". No brief file — this
task was dispatched directly with the requirements in the chat message.

## What was implemented

- **`enclosure/board_geom.py`** — `corner_radius(tol=1e-3) -> float`, plus a
  private `_circumradius(p1, p2, p3)` helper. `corner_radius()` walks every
  `gr_arc` block on the `Edge.Cuts` layer, pulls each arc's `start`/`mid`/
  `end` points, and computes the circumradius of the circle those three
  points trace (Heron's formula: `R = abc / 4*area`, using the triangle they
  form — no need to know the circle's centre). All radii found must agree to
  within `tol` (default 1e-3mm); if they don't, it raises `ValueError` with
  the disagreeing values rather than averaging them.

- **`enclosure/params.py`** — three new derived constants, in the `---
  derived ---` section (after `POCKET_W`/`POCKET_L`):
  - `CORNER_R = bg.corner_radius()` — read from the board file, not
    hardcoded.
  - `POCKET_R = CORNER_R + POCKET_CLEAR`
  - `EXT_R = CORNER_R + WALL`

  Each carries a docstring comment explaining it's derived, and from what.

- **`enclosure/case.py`**:
  - New private helper `_rounded_box(w, l, h, cx, cy, cz, radius)`: builds a
    `Pos(...) * Box(w, l, h)`, then fillets only the box's four **vertical**
    edges (`box.edges().filter_by(Axis.Z)`) to `radius`. Top and bottom rims
    are untouched.
  - `shell()`'s `outer` and `cavity` now both go through `_rounded_box`, at
    `EXT_R` and `POCKET_R` respectively, instead of building a plain `Box`
    directly.
  - `lite_lid()`'s `plate` now goes through `_rounded_box` at `POCKET_R`
    (matching the pocket it seats in), instead of a plain `Box`.
  - Docstrings on `shell()` and `lite_lid()` updated to explain the rounding
    and where the radii come from.
  - Nothing else in `case.py` was touched — `standoffs()`,
    `screw_holes()`, `connector_solid()`, `port_cutouts()`, `dip_slot()`,
    `reset_pinhole()`, `_print_reliefs()`, `tray()` are all unchanged.

- **`enclosure/hardware/`** — not touched (verified: no edits, no writes,
  only read-only `Read`/`grep` on `airwriter.kicad_pcb`).

## The arc-to-radius derivation and its evidence

Read directly from `hardware/airwriter/airwriter.kicad_pcb`, four
`Edge.Cuts` `gr_arc` blocks (lines ~31545–31628), e.g.:

```
(gr_arc
    (start 100 62)
    (mid 100.585786 60.585786)
    (end 102 60)
    ...
    (layer "Edge.Cuts")
)
```

`_circumradius((100,62), (100.585786,60.585786), (102,60))` computed by hand
via the triangle-area formula returns **1.9999985**, i.e. 2.0mm to the
file's own precision. All four arcs agree to ~1.5e-9mm (measured directly:
`1.9999985058501855, 1.9999985058502159, 1.9999985058502114,
1.9999985058502368`), so `corner_radius()` returns `round(mean, 6) =
1.999999`. `CORNER_R`, `POCKET_R`, `EXT_R` come out to `1.999999`,
`2.199999`, `4.999999` — 2.00/2.20/5.00 as specified, to float precision.

`hardware/panel/panel.kicad_pcb` was inspected manually (not read
programmatically — nothing in this project resolves that path) and confirmed
to carry the identical 62 x 104.27mm outline with the same four R2.0 arcs,
which is the documented basis for reusing `CORNER_R` for the panel/lid seat
without a second read.

## Red/green evidence

Every new test below was run against a **deliberately broken variant**
before being finalized, per the verification-bar requirement. The broken
variant for the geometry tests was produced by neutering `case._rounded_box`
to return a plain (unfilleted) `Box`, i.e. exactly the pre-Task-10 square
geometry; for the board-file tests it was produced by disabling the
mismatch check (naive average) or hardcoding `return 2.0`.

| Test | Red (broken) | Green (fixed) |
|---|---|---|
| `test_board_geom.TestCornerRadius.test_reads_2_0_from_the_file_not_a_hardcoded_constant` | n/a (asserts against the real file both ways) | pass |
| `test_board_geom.TestCornerRadius.test_reflects_a_different_consistent_radius_not_stuck_at_2_0` | **FAIL** — hardcoded `return 2.0`: `AssertionError: 2.0 != 3.0` (test scales all 4 arcs by 1.5x in a scratch file, expects `corner_radius()` to track it to ~3.0) | pass |
| `test_board_geom.TestCornerRadius.test_raises_on_arcs_with_mismatched_radii` | **FAIL** — naive-average impl: `AssertionError: ValueError not raised` (one arc's `mid` corrupted to trace ~2.85mm instead of the other three's 2.0mm) | pass |
| `test_case.TestCornerRounding.test_tray_exterior_corners_are_rounded_at_ext_r` | **FAIL** — `0.0025mm^3 of square-corner material where a rounded corner should be empty` (all 4 corners) | pass |
| `test_case.TestCornerRounding.test_pocket_corners_are_rounded_at_pocket_r` | **FAIL** — `the square-corner wedge a rounded pocket should retain as material is only 0.0000/0.0025mm^3 solid` (all 4 corners) | pass |
| `test_case.TestCornerRounding.test_lite_lid_plate_corners_are_rounded_at_pocket_r` | **FAIL** — same signature as the tray-exterior test | pass |
| `test_case.TestCornerRounding.test_lid_outline_matches_the_pocket_it_seats_in` | **FAIL** — `6.648mm^3 of the lid plate sticks out past the pocket's own rounded footprint` | pass |

Exact commands and output are reproduced in the session transcript; the
short version: patch `case._rounded_box`/`board_geom.corner_radius` to the
broken variant → run the specific test → record failure → restore the real
file from a backup copy → rerun → confirm green. Full suite was rerun clean
after every restore.

Each corner probe targets a fixed point along the corner's own 45° diagonal
(a "tip" just outside any fillet arc regardless of radius, and a "body"
point just inside the fillet's own radius) so the assertion is sensitive to
*whether* the corner is rounded, not just to the exact radius value — this
is what makes it fail identically and immediately against the square
geometry rather than needing the exact broken radius to reason about.

### Note on the volume-based outline-match test

`test_lid_outline_matches_the_pocket_it_seats_in` initially compared
`self.lid & pocket_shape` volumes directly and passed even for the broken
(square) lid: a square footprint is always a superset of the smaller rounded
one, so checking "the rounded pocket fits inside the lid" cannot detect a
square lid sticking out at the corners — only the reverse containment
(lid ⊆ pocket) can. Rewrote it to check both directions (`plate -
pocket_shape` and `pocket_shape - plate`, both must be ~0), which is what
produced the FAIL row above. Also had to subtract SW3's plunger bore from
the independently-built reference shape — that bore is a real, intentional
hole in the plate, and without excluding it the "missing material" check
flagged it as a false corner defect (~6.36mm³, matching the bore's own
volume almost exactly). This uses `delta=1.0` rather than `places=N`: two
solids built with coincident boundaries (the lid's plate and the
independently-constructed rounded reference both use `POCKET_R`/`POCKET_W`/
`POCKET_L`) produce small boolean-kernel noise (~1.6mm³ observed) at
touching faces; the real defect signal (a square vs. rounded corner over the
full PANEL thickness) is ~6.6mm³, comfortably above that noise floor.

## Pre-existing tests fixed as a consequence

Rounding the geometry legitimately changed what four pre-existing tests
should see, and they failed/errored on first full-suite run after the
production change (before I touched the tests):

1. **`test_case.TestShell.test_cavity_is_hollow_from_floor_top_upward`** —
   used a square probe sized to the full pocket footprint; failed with
   `85.337mm^3 != 0.0` because the shell now legitimately retains material
   in the pocket's square corners (that's the whole point of Task 10 item
   2). Fixed by rounding the probe at `POCKET_R` too (built independently
   via `Box` + `fillet`, not by calling `case._rounded_box`). Verified this
   fixed version still passes against the broken (square) `case.py` too —
   expected and correct, since this test's job is "is the cavity hollow",
   not "is the corner rounded"; the latter is `TestCornerRounding`'s job.

2. **`test_case.TestLiteLid.test_it_fits_the_tray_with_no_interference`**,
   **`test_plunger_bore_sits_over_SW3`**, **`test_insert_bores_open_
   downward_at_the_board_face`** — all three did `(self.lid & probe).volume`
   directly. With genuinely rounded (not just numerically near-touching)
   boundaries, a clean miss returns `None` from the boolean op rather than a
   zero-volume shape, which raised `AttributeError` instead of failing the
   assertion. Fixed with the same `hit = ...; vol = hit.volume if hit else
   0.0` guard already used elsewhere in this file (e.g.
   `test_no_connector_touches_the_tray`) — not a new pattern, just applying
   the existing one to three spots that had gotten away with skipping it.

## Exported parts' final dimensions

From `./build.sh` (full 64-test suite, then `export.py`):

```
airwriter-tray: 68.00 x 110.27 x 22.94 mm, 36.0 cm3
airwriter-lite-lid: 62.40 x 104.67 x 15.94 mm, 11.9 cm3
```

Matches the required external envelope exactly (bounding box unchanged, only
the corners changed shape). `out/airwriter-tray.{step,stl}` and
`out/airwriter-lite-lid.{step,stl}` were regenerated.

## Printability gate

Stayed clean. `test_printable.py`'s `TestTray` and `TestLid` — including
`test_no_unsupported_downward_flats` and
`test_no_unsupported_downward_flats_printed_plate_down` — all pass. This
matches the task's own prediction: filleting only the vertical edges turns
a sharp vertical arris into a vertical **cylindrical** face, which
`_overhangs()` never flags (it only looks at `GeomType.PLANE` faces). No new
horizontal flat was introduced by the fillets; I did not need to work around
anything.

`test_walls_are_at_least_2.80_thick` also stayed green — its probes are at
mid-wall positions (`y=90`, `x=31`, etc.), well clear of the ~2–5mm corner
radii.

## Full suite

```
$ .venv/bin/python -m unittest discover
Ran 64 tests in 57.347s
OK
```

57 pre-Task-10 tests (4 of which were adjusted, none removed) + 7 new tests
(3 in `test_board_geom.py`, 4 in `test_case.py`). Zero regressions.

## Files changed

- `enclosure/board_geom.py` — `corner_radius()`, `_circumradius()`
- `enclosure/params.py` — `CORNER_R`, `POCKET_R`, `EXT_R`
- `enclosure/case.py` — `_rounded_box()`; `shell()` and `lite_lid()` use it
- `enclosure/test_board_geom.py` — `TestCornerRadius` (3 tests)
- `enclosure/test_case.py` — `TestCornerRounding` (4 tests); 4 existing
  tests in `TestShell`/`TestLiteLid` adjusted (see above)

`enclosure/out/*.step`, `enclosure/out/*.stl` regenerated by `build.sh` but
not part of the diff worth reviewing (build artifacts).

## Self-review

- Confirmed the exterior fillet centre and the pocket fillet centre land at
  the *same point* (both equal `BOARD_W/2 - EXT_W/2 + EXT_R = -WALL +
  CORNER_R + WALL = CORNER_R`, and `BOARD_W/2 - POCKET_W/2 + POCKET_R =
  -POCKET_CLEAR + CORNER_R + POCKET_CLEAR = CORNER_R` — same for the Y
  axis), i.e. both radii are struck from the board's own actual corner
  centre. That's a good internal-consistency signal, not a requirement I
  had to build in by hand — it falls out of `EXT_R = CORNER_R + WALL` and
  `POCKET_R = CORNER_R + POCKET_CLEAR` sharing the same `CORNER_R`.
- The radial wall thickness at each corner is `EXT_R - POCKET_R = WALL -
  POCKET_CLEAR = 2.80mm`, matching `MIN_WALL` elsewhere in the project —
  another sign the derivation is consistent with the rest of the design.
- Did not add a test asserting the two fillet centres coincide or that
  corner wall thickness is exactly 2.80mm; `test_walls_are_at_least_2_80_
  thick` (pre-existing, mid-wall only) and the new corner tests don't
  directly cover the *radial* thickness at the diagonal. This is a gap: a
  future change to `EXT_R`/`POCKET_R`'s derivation that broke this
  coincidence wouldn't be caught by anything today.
- Did not modify `standoffs()`, `screw_holes()`, port/DIP/pinhole code as
  instructed; confirmed by diff review these functions are byte-identical
  before/after.
- Did not touch anything under `hardware/`.

## Concerns

1. **Radial corner wall thickness isn't directly tested** (see self-review
   above) — worth a follow-up test if this project continues past Task 10,
   but out of scope for what was asked here.
2. **`hardware/airwriter/airwriter.kicad_pro` and
   `hardware/combined/combined.kicad_pro` show as modified in `git status`**
   at the start of this session (before any of my edits) — diffs show KiCad
   project-preference churn (a `step` export path added, a `top_level_
   sheets` entry added, trailing-newline changes), the kind of thing KiCad
   itself rewrites when a project is opened, not anything this task touched.
   Verified none of my commands wrote to `hardware/` (all access was
   `Read`/`grep`, read-only). Flagging since the constraint was explicit,
   even though I'm confident this predates and is unrelated to this task.
3. Committed as `9a1625c`, matching the pattern every prior task in this
   branch's history used (one commit per task, reviewed against its diff
   afterward).
