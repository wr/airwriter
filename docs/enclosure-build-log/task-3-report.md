# Task 3 report: The tray's shell — floor, walls, pocket, lid seat

## What I implemented

- `enclosure/test_case.py` — the 5-test suite exactly as specified in the brief:
  `test_is_a_valid_single_solid`, `test_external_bounding_box`,
  `test_sits_in_the_case_frame`, `test_cavity_is_hollow_from_floor_top_upward`,
  `test_floor_is_solid_under_the_pocket`.
- `enclosure/case.py` — `shell() -> Part`, exactly as specified: an outer box
  `EXT_W × EXT_L × EXT_H` positioned so its bounding box starts at
  `(-WALL, -WALL, Z_FLOOR_BOTTOM)`, minus a cavity box `POCKET_W × POCKET_L ×
  (Z_WALL_TOP - Z_FLOOR_TOP)` open from the floor's top face up through the
  wall top (no lid — later tasks add the lid seat's own geometry). No
  openings, no `board_geom` import (per your note — `shell()` only needs
  `params`).

Followed the brief's code verbatim; no deviations.

## TDD evidence

RED:

```
$ cd /Users/wells/Projects/airwriter/enclosure && .venv/bin/python -m unittest -v test_case
test_case (unittest.loader._FailedTest.test_case) ... ERROR

======================================================================
ERROR: test_case (unittest.loader._FailedTest.test_case)
----------------------------------------------------------------------
ImportError: Failed to import test module: test_case
Traceback (most recent call last):
  ...
  File "/Users/wells/Projects/airwriter/enclosure/test_case.py", line 4, in <module>
    import case
ModuleNotFoundError: No module named 'case'

----------------------------------------------------------------------
Ran 1 test in 0.000s

FAILED (errors=1)
```

GREEN:

```
$ cd /Users/wells/Projects/airwriter/enclosure && .venv/bin/python -m unittest -v test_case
test_cavity_is_hollow_from_floor_top_upward ... ok
test_external_bounding_box ... ok
test_floor_is_solid_under_the_pocket ... ok
test_is_a_valid_single_solid ... ok
test_sits_in_the_case_frame ... ok

----------------------------------------------------------------------
Ran 5 tests in 0.045s

OK
```

Full-suite regression check (Tasks 1, 2, 3 together):

```
$ cd /Users/wells/Projects/airwriter/enclosure && .venv/bin/python -m unittest discover -p 'test_*.py' -v
... (20 tests: test_board_geom.py x9, test_case.py x5, test_params.py x6)
----------------------------------------------------------------------
Ran 20 tests in 0.045s

OK
```

All 20 tests pass, including the 9 from Task 1 (`test_board_geom.py`) and the 6
from Task 2 (`test_params.py`) — no regressions.

## Files changed

- Created: `enclosure/case.py`
- Created: `enclosure/test_case.py`

## Commit

`cb9b79f Enclosure: tray shell` — `enclosure/case.py`, `enclosure/test_case.py`
(2 files, 77 insertions).

## Self-review

- **Completeness against the brief:** matches verbatim — same test file,
  same `shell()` implementation, same commit message. No extra methods, no
  extra exports.
- **Do the tests actually constrain the geometry?** Yes:
  - `test_is_a_valid_single_solid` catches non-manifold/multi-body results
    from a bad boolean.
  - `test_external_bounding_box` + `test_sits_in_the_case_frame` pin all six
    faces of the outer box to the exact frame-relative coordinates (not just
    sizes) — a translation error in `Pos(...)` would fail this even if the
    overall size were still right.
  - `test_cavity_is_hollow_from_floor_top_upward` and
    `test_floor_is_solid_under_the_pocket` are real geometric probes (via `&`
    intersection and `.volume`), not just "did subtraction run" checks — they
    would catch, e.g., a cavity box mis-sized to leave a shelf, or a cavity
    that also ate into the floor.
  - One gap I noticed but did not add: nothing directly asserts wall
    thickness (that the cavity's X/Y footprint sits centered and inset by
    exactly `WALL` on all four sides) — it's implied by
    `POCKET_W = BOARD_W + 2*POCKET_CLEAR` and the shared `BOARD_W/2` centering
    in both boxes, and the bounding-box test already pins the outer footprint
    to the frame, so a wall-thickness bug would have to be a very specific
    self-cancelling error to slip through. I left this alone since the brief
    was explicit and adding assertions beyond what it specifies risks
    diverging from the plan other tasks build on.
- **YAGNI:** `shell()` does exactly floor + walls + pocket, nothing else. No
  screw holes, no port cutouts, no standoffs — correctly deferred to later
  tasks per the brief's own docstring. I did not import `board_geom` since
  `shell()` has no use for it yet (per your note in the task instructions).

## Concerns

None. No build123d API surprises beyond the already-known `.is_valid`
property (used correctly here, as `.is_valid` not `.is_valid()`, per your
correction). `Box`, `Pos`, `-` (boolean subtract), `&` (boolean intersect),
and `.volume` all behaved exactly as the brief's code assumed.
