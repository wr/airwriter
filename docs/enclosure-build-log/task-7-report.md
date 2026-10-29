# Task 7 report: the LC lid

## What was implemented

`case.lc_lid() -> Part`: the panel stand-in for AirWriter LC builds.

- A plate the art panel's footprint (`POCKET_W x POCKET_L x PANEL`), seated
  with its top face at `Z_WALL_TOP` (flush with the wall tops, same rebate
  the Pro's panel uses).
- Four legs (one per mount hole, from `board_geom.mount_holes()`), constant
  diameter `LEG_D`, spanning `Z=0` (board top face) up to `Z=CAVITY` (the
  plate's own underside) -- reproducing the Pro's 14.34mm lattice+nut stack
  in plastic.
- An M2.5 insert bore (`INSERT_D` x `INSERT_DEPTH`) in each leg, cut from the
  leg's free end at `Z=0` so it opens downward at the board-face level (the
  screw reaches brass after a few mm, not 14mm of plastic).
- A bore over SW3's test plunger at KiCad (131, 91), matching the art
  panel's own 2.25mm NPTH.

`STANDOFF_D` and `LEG_D` now live in `params.py`, beside `SCREW_CLEAR_D` /
`SCREW_HEAD_D`, which they derive from -- `case.py`'s `standoffs()` and
`lc_lid()` both reference `p.STANDOFF_D` / `p.LEG_D`. No `case.py` module
attribute for either remains.

## Printability: the leg-to-plate junction

The lid prints plate-down (the plate's `Z_WALL_TOP` face on the bed, legs
pointing up into the air). Any model surface facing +Z (other than the
plate's own top) becomes, after that flip, a downward-facing overhang with
nothing under it -- exactly what Task 8's whole-part printability gate will
check for.

Traced through the actual geometry:
- Leg top (`Z=CAVITY`) is exactly coincident with the plate's bottom
  (`Z=CAVITY`), same XY footprint circle, and the two are unioned (`+`) --
  the shared interface is interior to the resulting solid, not an exposed
  face. No shoulder is created there.
- The leg's own two end faces (bottom at `Z=0`, and the bore's blind-hole
  floor at `Z=INSERT_DEPTH`) both face `-Z` in model space, which is the
  *upward* (sky-facing, unproblematic) direction once flipped for printing --
  confirmed by working through the 180-degree-flip normal transform by hand
  (documented in the `lc_lid()` docstring).
- The bore itself opens at the leg's free end and is never capped above by
  solid printed later, so it is a simple blind hole open at the print's top,
  not a bridged void.

No downward flat exists in this model besides the plate's own bed-contact
face.

## TDD evidence

Baseline before this task: 44/44 tests passing (`enclosure`, discover).

1. Appended `TestLCLid` (6 tests) to `test_case.py`, exactly as specified
   in the brief.
2. Ran `test_case.TestLCLid` before implementing `lc_lid()`:
   ```
   AttributeError: module 'case' has no attribute 'lc_lid'
   Ran 0 tests in 0.000s
   FAILED (errors=1)
   ```
3. Implemented `lc_lid()` in `case.py`.
4. Ran `test_case.TestLCLid`: 6/6 pass in 12.25s.
5. Full suite: `Ran 50 tests in 39.184s -- OK` (44 old + 6 new, no
   regressions).

## Red/green evidence for every new test

Each test was broken with a targeted, temporary edit to `lc_lid()`, run
alone to confirm red, then restored and re-run to confirm green. All six:

| Test | Break | Red result | Restored |
|---|---|---|---|
| `test_is_one_valid_solid` | Shrunk+shifted a leg so it no longer touches the plate | `AssertionError: 5 != 1` | ok |
| `test_plate_matches_the_art_panel_and_seats_flush` | Widened the plate box by 1.0mm in X | `AssertionError: 63.4 != 62.4 within 3 places` | ok |
| `test_legs_span_the_cavity_to_the_board_face` | Shifted+shortened legs so they stop 2mm above `Z=0` | `AssertionError: 2.0 != 0.0 within 3 places` | ok |
| `test_it_fits_the_tray_with_no_interference` | Widened every leg radius by 4.0mm | `AssertionError: 437.836 != 0.0 within 3 places` | ok |
| `test_plunger_bore_sits_over_SW3` | Stopped subtracting the plunger cut | `AssertionError: 0.804 != 0.0 within 3 places` | ok |
| `test_insert_bores_open_downward_at_the_board_face` | Moved the bore up 1mm and removed its overshoot, leaving a plug at the leg's bottom | `AssertionError: 9.079 != 0.0 within 3 places` | ok |

All six confirmed red, then confirmed green again after restoring the exact
original code (verified with `git diff` showing a clean diff against the
committed version after each restore, and a final full-suite run).

Per the brief's specific ask, `test_it_fits_the_tray_with_no_interference`
was proven capable of catching real interference (437.8 mm^3 with legs
widened by 4mm each) rather than trusting an empty intersection blindly.

## The four carried-over corrections

1. **`STANDOFF_D` / `LEG_D` moved into `params.py`.** Both now live in the
   `# --- fasteners ---` section, directly after `SCREW_HEAD_DEPTH` and
   before `INSERT_D` (the params they sit beside/derive from). `case.py`'s
   `standoffs()` uses `p.STANDOFF_D`; `lc_lid()` uses `p.LEG_D`. No
   `case.py`-level definition of either remains (checked with grep).

2. **SW1 actuator-Z confidence language softened.** Rewrote
   `RESET_ACTUATOR_Z`'s docstring in `params.py`: it now says the 1.75mm
   value is "a derivation from the footprint descr... unverified against a
   Panasonic mechanical drawing (not in this repo)", and that the
   `EVQP44B3M_SidePress.wrl` agreement is "consistent with (NOT confirmed
   by)" the derivation, explaining explicitly that the WRL shares the same
   "centred on the body" assumption and so was guaranteed to agree
   regardless of whether that assumption is correct. **The value
   (`3.5 / 2` = 1.75mm) is unchanged.**

3. **Stale recess figure fixed.** `WALL`'s docstring in `params.py` now says
   S1's keys sit "0.80mm recessed" (was 0.75mm). Also fixed a second,
   identical stale figure in the same file: `DIP_SLOT_Z0`/`DIP_SLOT_Z1`'s
   docstring said "a pen tip can work a key recessed 0.75mm" -- same fact,
   same file, same error; corrected to 0.80mm for consistency.
   `case.py`'s `dip_slot()` docstring already said 0.80mm and needed no
   change.

4. **LC screw length M2.5 x 10, not x 8.** No `enclosure/*.py` file
   currently has a comment stating the screw length (grepped for
   "screw"/"M2.5" across `case.py`/`params.py`/`test_case.py` -- none
   exists), so `lc_lid()`'s own docstring was written to already say the
   correct thing ("brass after a few millimetres... not 14mm of printed
   plastic", with no numeric screw length asserted). The only place in the
   repo that actually stated a screw length was
   `docs/superpowers/specs/2026-09-08-airwriter-enclosure-design.md`
   section 5 ("LC: M2.5 x 8 ..."), which I corrected to "M2.5 x 10".

## Files changed

- `enclosure/case.py` -- added `lc_lid()`; removed the local
  `STANDOFF_D` (now `p.STANDOFF_D`); `standoffs()` updated to match.
- `enclosure/params.py` -- added `STANDOFF_D`, `LEG_D`; corrected the two
  stale 0.75mm recess figures to 0.80mm; rewrote the SW1 actuator-Z
  docstring's confidence language (value unchanged).
- `enclosure/test_case.py` -- added `TestLCLid` (6 tests), exactly as
  specified in the brief.
- `docs/superpowers/specs/2026-09-08-airwriter-enclosure-design.md` -- LC
  screw length corrected from M2.5 x 8 to M2.5 x 10 (section 5).

Note: this file already carried one *other*, pre-existing uncommitted edit
at session start (Four 2.00mm bosses -> 3.00mm, matching the STANDOFF=3.00
change from Task 6/2b83354) that I did not make and left untouched other
than staging it alongside my own edit to the same file, since it's the same
kind of spec-truing correction to the same document and splitting it out
would have meant either reverting someone else's correct-looking edit or
leaving the working tree in a half-committed state. It is now part of this
commit.

Never touched `hardware/`. Two unrelated pre-existing modified files
(`hardware/airwriter/airwriter.kicad_pro`, `hardware/combined/combined.kicad_pro`)
and some untracked `firmware/` build artifacts were present at session start
and remain untouched and unstaged.

## Self-review of the diff

- Checked `git diff --stat` before committing: only the four intended files
  touched, no stray whitespace-only hunks, no accidental `.venv` or cache
  changes.
- Grepped for leftover debug markers (`BROKEN`, `XXX`, `TODO`, `FIXME`) in
  the touched files after the red/green break-and-restore cycles: none
  found -- every temporary break was fully reverted.
- Confirmed `case.STANDOFF_D` / `case.LEG_D` no longer exist as module
  attributes (grep across `*.py`), and that no other test or module
  referenced the old `case.STANDOFF_D` location before the move.
- Re-read the full `lc_lid()` diff once more for the coincident-union
  argument (leg top == plate bottom, no exposed shoulder) and the bore
  overshoot direction (`-0.01`/`+0.02`, opening below `Z=0`, matching the
  "insert bore at the BOTTOM of each leg" requirement) -- both check out
  geometrically and are exercised by
  `test_legs_span_the_cavity_to_the_board_face` and
  `test_insert_bores_open_downward_at_the_board_face` respectively.
- Verified the DIP_SLOT_Z0/Z1 docstring's second stale "0.75mm recessed"
  instance (not explicitly named in the brief, but the same fact, same
  file) -- fixed it too rather than leaving one file internally
  inconsistent between 0.80mm (WALL) and 0.75mm (DIP_SLOT).

## Concerns

- None blocking. The one judgment call: extending correction 4's "wherever
  it appears" to the spec doc's prose (not just code comments), and fixing
  the second stale-recess instance beyond the brief's literally-named one.
  Both are narrow, factual, one-line-scale corrections consistent with the
  stated intent, but flagging the judgment call for visibility.
- Full pre-deploy gate (`npm run verify`-equivalent for this project --
  `build.sh`, STL export, KiCad interference check) was not run; this task's
  scope was the build123d model + its test suite only, per the brief.
