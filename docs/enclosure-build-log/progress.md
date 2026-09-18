# SDD ledger — plan: docs/superpowers/plans/2026-09-08-airwriter-enclosure.md

Spec: docs/superpowers/specs/2026-09-08-airwriter-enclosure-design.md
Branch: wells/airwriter-enclosure (not a worktree — a feature branch off main;
the venv and the KiCad-relative paths make a worktree more trouble than it is
worth here, and no other session is touching this repo).
Base at start: 018d18efb5338f4b7114c86a2de4a3bed112c0b4

## Pre-flight conflict scan

### Task pairs sharing a file or an interface

| Producer | Consumer | Interface | Finding |
|---|---|---|---|
| T1 `outline()` | T2 `BOARD_W/BOARD_L` | `(min_x,min_y,max_x,max_y)` KiCad mm | clean |
| T1 `to_case()` | T3, T6, T7 | KiCad mm → case frame | clean |
| T1 `mount_holes()` | T4 `standoffs`/`screw_holes`, T7 `lite_lid` | `{ref:(X,Y)}` case frame | clean |
| T1 `footprint()` | T5 `connector_solid`, T6 `dip_slot`/`reset_pinhole` | dataclass | **DEFECT 1** — no model offset/scale/rotate fields |
| T1 `local_to_board()` | T6 `dip_slot`, T1's own S1 test | footprint-local → KiCad | clean |
| T2 constants | T3–T8 | module globals | clean |
| T3 `shell()` | T4, T5, T6 | `Part` | clean |
| T4 `standoffs()`/`screw_holes()` | T6 `tray()` | `Part` | clean |
| T5 `port_cutouts()` | T6 `tray()` | `Part` | clean |
| T6 `tray()` | T7 fit test, T8 export | `Part` | clean |
| T7 `lite_lid()` | T8 export | `Part` | clean |
| T8 `out/*.stl` | T9 GLB | file | clean |
| T3–T7 all edit `case.py` + `test_case.py` | — | serial edits, one task at a time | clean |

### Each task against itself

| Task | Finding |
|---|---|
| T1 | clean — tests assert values verified against the board file today |
| T2 | clean |
| T3 | Interfaces block claims it consumes `board_geom.to_case`; `shell()` uses only `params`. Cosmetic, no action. |
| T4 | clean — the coned counterbore test and the implementation agree |
| T5 | **DEFECT 1** (below) |
| T6 | **DEFECT 2** (below) |
| T7 | clean |
| T8 | clean — the printability gate is expected to fail first and drive fixes; the plan says so |
| T9 | clean |

### DEFECT 1 — Task 5's model-placement assumption is false

`connector_solid()` states "every model this project touches is placed at
offset 0, scale 1". Read from the board:

    J2, J4   model offset (0,0,0)        rotate (-90, 0, 180)   scale 1
    J1       model offset (-4.47,-3.65,0) rotate (-90, 0, 0)     scale 1
    S1       .wrl (OCCT cannot import) — parametric, as planned
    SW1      NO model at all — parametric, as planned

J1 carries a real offset, and all three STEPs carry a model rotation. Placing
them by footprint position and rotation alone puts the USB-C in the wrong
place and lays all three on the wrong axis.

**Ruling:** `Footprint` gains `model_offset`, `model_scale`, `model_rotate`,
and `connector_solid()` applies the full KiCad model transform — scale, then
rotation about X then Y then Z, then offset, then the footprint's own rotation
and position, then the case frame's Y flip. KiCad negates the stored rotate
values and its model frame's +Y is the board's −Y; rather than assert a
convention I cannot test from here, Task 5 gets three ground-truth assertions
the transform must satisfy, all independently derived from the board and the
build docs:

  1. J2/J4 mating faces at case Y = −2.75 (footprint origin IS the mating
     face; origin at KiCad y=167.02 against a board edge at 164.27)
  2. J2/J4 shells top out at Z = 12.74 (scripts/board-model/README.md in the
     shop repo, and the number CAVITY is derived from)
  3. J1's shell face at case Y ≈ −1.5 (F.Fab ±3.65 about origin y=162.12)

The implementer iterates on the transform until all three hold. It must never
edit an expected value to make one pass — each is measured, not chosen.

**Cost if wrong:** the port openings land in the wrong place and the first
print is scrap. The three assertions are what makes that loud instead of
quiet.

### DEFECT 2 — Task 6's `carveouts()` is vacuous

The tray's interior is a single open prism from the floor's top face to the
wall tops. U4 (case 19.3, 48.17), R3 (51.8, 35.47) and U9 (46.7, 39.45) all
sit well inboard of every wall, and the tallest of them is ~1.1 mm against a
14.34 mm cavity. Subtracting a cylinder of air from air is a no-op, and
`test_carveouts_clear_the_named_internal_parts` passes trivially — it asserts
nothing. The spec's carve-out requirement was inherited from a case concept
that had internal structure; this one does not. S1's "top corner" carve-out
is real and is already the DIP slot.

**Ruling:** drop `carveouts()` and its subtraction from `tray()`. Replace the
vacuous test with one that *proves* the requirement's intent — that each named
part has at least 1.0 mm of air to the nearest tray surface — by probing a
cylinder at each part's position over the part's height and asserting the tray
does not intersect it. The requirement becomes verified rather than
cut. Spec §4's carve-out line is satisfied by that check.

**Cost if wrong:** if a later board revision moves one of those parts under a
wall or a boss, the check catches it — which the vacuous version would not
have. Lower risk than shipping dead geometry.

---

## Progress

### Task 1

BASE 018d18e. Implementer impl-t1 (agent ab9eb3766d5d387f1, sonnet).
Reported DONE_WITH_CONCERNS, commit 0f6b864, 9/9 passing.

Ruling: build123d 0.11.1's `Shape.is_valid` is a PROPERTY, not a method —
verified in the venv (`b.is_valid()` raises `TypeError: 'bool' object is not
callable`). The plan wrote `.is_valid()` in 8 places across Tasks 1, 3, 5, 6,
7 and 8. Amended the plan file to `.is_valid` throughout rather than let five
later implementers each rediscover it. Cost if wrong: none — the property is
verified in the actual installed version.

Noted (implementer's own fix, no action): the brief's `_blocks()` used a
literal `"\n\t(<opener> "` needle, which misses this board's `gr_line`/`gr_arc`
Edge.Cuts blocks because they put the opener on its own line. Fixed with a
word-boundary regex; the outline test is what caught it.

Task 1: review clean — spec ✅, quality Approved, no Critical/Important.
Task 1: minor (deferred): no test covers the identity default for a footprint with model=None (H1-H4, SW1) — verified by hand, uncovered by the suite.
Task 1: minor (deferred): `_model_sub_block`'s None-fallback for a partially specified model block is unexercised — 0 of 128 model-bearing footprints omit a sub-block.
Task 1: minor (deferred): `test_model_transform_defaults_to_identity` is misnamed — SW3's scale is 1.153846154, not identity.
Task 1: complete (commits 018d18e..0f6b864, review clean)

### Task 2
BASE 8dfd55d. Implementer impl-t2 (agent a8542c76e9282d780, haiku). DONE, 15/15.
Task 2: complete (commits 8dfd55d..02edccb, review clean)

### Task 3
BASE 02edccb. Implementer impl-t3 (agent a2e583cb6c74e6984, sonnet). DONE, 20/20.
Reviewer independently measured all four walls at 2.80 mm and confirmed the
volume equals the box minus the cavity exactly — no stray openings.
Task 3: minor (deferred): `test_cavity_is_hollow_from_floor_top_upward` builds its
probe from the same POCKET_W/POCKET_L formula the production cavity uses, so it
would not catch a future change to how POCKET_CLEAR is applied. Task 8's
`test_walls_are_at_least_2_80_thick` uses independent probes and covers this —
confirm at Task 8 that it actually does.
Task 3: complete (commits 02edccb..cb9b79f, review clean)

### Task 4
BASE cb9b79f. Implementer impl-t4 (agent a688a773a77e1c1ef, sonnet).
DONE_WITH_CONCERNS, commit a077a8f, 23/23 — but the concern is a real defect.

Ruling: the plan's `test_counterbore_transition_is_coned_not_a_flat_ledge`
inspects `screw_holes()` — the CUTTING TOOL — for downward-facing flats. A tool
solid has none there: at the counterbore/bore shoulder the tool's own material
is below and wider, so that face points UP. I verified it empirically —
downward faces on the tool: []; upward: 4 x 6.2 mm2. The test passes
unconditionally and cannot fail. The implementer found this itself and flagged
it rather than banking a green tick.
Applied instead to the boolean RESULT (`shell() + standoffs() - screw_holes()`)
the same check is exactly right, proven both directions:
  with the cone    -> []
  without the cone -> 4 downward faces of 13.5 mm2 at Z = -4.60
Amending the test to inspect the result. Task 8's printability gate already
applies `downward_flats()` to `case.tray()` (the result), so it is correct as
the plan writes it — this defect is Task 4's alone.
Cost if wrong: none — the corrected form is demonstrated to fail on the exact
regression it guards.
Task 4: fix round 1/5 (1 addressed, 0 open — vacuous counterbore test now checks
the boolean result; commits cb9b79f..1abcf72). Fix by fresh implementer
fix-t4-r1 (a3ec9d2f526e2df7f) — SendMessage is disabled in this session, so
rounds cannot resume a live agent; the report file is the memory.
Task 4: review clean — spec ✅, quality Approved. Reviewer independently rebuilt a
no-cone variant and confirmed the amended predicate scores 0 on real geometry
and 4 flats of 13.477 mm2 at Z=-4.60 on the regression, and that the ORIGINAL
predicate scored 0/0 (powerless). Both halves of the defect confirmed by a third
party.
Ruling: reviewer argues STANDOFF_D (6.00) belongs in params.py, not case.py,
since params.py's contract is "every number this project chooses" and it sits
beside SCREW_CLEAR_D/SCREW_HEAD_D which it derives from. Agreed on principle.
Not fixing it as its own commit: Task 7 introduces LEG_D, the identical case,
and edits both files anyway — folding both moves into Task 7 costs nothing and
makes the two constants consistent, where fixing it now costs a commit and
leaves LEG_D to be argued about again. Cost if wrong: a cosmetic constant lives
two tasks longer in the wrong file.
Task 4: complete (commits cb9b79f..1abcf72, review clean)

### Task 5
BASE 1abcf72. Implementer impl-t5 (agent aaad63d965f2b5fe2, sonnet).
DONE_WITH_CONCERNS, commit 8dd823e, 30/30. All three ground truths hold:
J2/J4 mating face Y = -2.75 exact, J1 face Y = -1.50 exact, J2/J4 shell top
Z = 12.75. The model transform is right.

Ruling 1 (STANDOFF, load-bearing — a real design bug the STEP check caught):
J2/J4's through-hole pins protrude 2.150 mm below the board (measured off the
Kycon vendor STEP). STANDOFF is 2.00, so the floor's top face at Z=-3.60 sits
0.15 mm INSIDE the pins at Z=-3.750 — the board cannot seat. The spec chose
2.00 to clear "the through-hole tails of J2/J4/S1/SW3/U7"; the intent was
right and the number was an estimate, and this is the first task with the
geometry to test it.
Raising STANDOFF 2.00 -> 3.00: floor top moves to -4.60, giving 0.85 mm under
the pin tips — room for hand-soldered THT fillets, which the STEP does not
model. EXT_H becomes 22.94. Rejected the alternative of local pin-relief
pockets in the floor: same clearance, more print failure modes, and it leaves
the next unmeasured part to rediscover the problem. Amending spec section 2's
stack table, params.py, and the plan's Global Constraints, and adding a test
that derives the requirement from the STEPs rather than restating a number.
Cost if wrong: the case is 1 mm taller than it needed to be.

Ruling 2 (hull offset, accept): build123d's `offset()` returns null/degenerate
on the real vendor STEPs, so `_grown()` offsets the connector's convex hull
instead. Accepted. The hull contains the solid, so clearance is >= PORT_CLEAR
everywhere; it over-cuts only where the solid is concave. Measured impact: the
mini-DIN openings become the body cross-section (14.8 x 17.3) rather than a
barrel-sized hole — which is CORRECT here, because the socket's whole front
flange sits in the wall, not just its barrel. 32.2% of the front wall's volume
is removed, leaving 4.7 mm of wall between the two mini-DINs, 2.79 mm above
and 1.85 mm below. Adding a guard test for the material above/below each port
so a future connector change cannot silently eat the wall.
Cost if wrong: a port opening is more generous than strictly needed; no
clearance is ever lost, only gained.

Ruling 3 (12.75 vs 12.74, no action): the shell top measures 12.75 against the
shop README's 12.74. 0.01 mm, an order of magnitude below a 0.2 mm layer.
CAVITY stays 14.34. Cost if wrong: nothing measurable.

Task 5: minor (deferred): board_geom resolves ${KIPRJMOD} but not
${KICAD10_3DMODEL_DIR}, so SW3/U4/R3/U9's stock-library STEPs cannot be
loaded. Not needed by this task (both mini-DINs and the USB-C are KIPRJMOD),
but it caps how much of the board a future interference check can see.
Task 5: fix round 1/5 (2 addressed, 0 open — STANDOFF 3.00 + front-wall guard;
commits 8dd823e..8d03d6e). Fresh implementer fix-t5-r1 (ad7372714b5355f23).
Task 5: review (opus) — spec ❌, quality Needs work. The transform is CORRECT and
principled: reviewer confirmed all three ground truths exactly, then validated
it independently by parsing J2's 11 through-hole pads out of the board file and
probing the placed STEP at each — all 11 hit metal, and a 1 mm shift returns
zero. It also matches KiCad's real composition line for line.
But the clearance requirement is violated: J1 clears the wall by 0.0324 mm
against a required 0.30.
Fixed my own incomplete spec amendment: section 4 still said "Four 2.00 mm
bosses" after section 2 moved to 3.00.
Task 5: fix round 2/5 (6 findings + 2 minors addressed, 2 open; commits
8d03d6e..40e817d). Fresh implementer fix-t5-r2 (a77da6f706989774b).
Clearance now J2/J4/J1 = 0.3849 mm (was J1 0.0324); containment exact
(connector - envelope = 0.000000 mm3); USB-C flare now widens outward,
12.120 mm at the outer face to 9.760 inside; floor nick gone, not relocated.
Re-reviewer broke and restored every amended test to prove it goes red.
Open into round 3:
  (a) Important, NEW: board_geom.py:145 `_FP_LAYER.match(blk)` never matches —
      `_blocks()` yields blocks starting "\n\t(footprint", and re.match anchors
      at position 0. All 138 footprints silently take layer="F.Cu", so the
      back-side guard added this round is dead code. One word: .match -> .search.
      The implementer "verified" it by monkeypatching a cached Footprint, which
      bypassed the parser — the same blind-test pattern this round was fixing.
  (b) Important: Finding 3's USB-C flare fix is correct and quantified but has
      no regression guard — restoring the original inverted cone leaves the
      whole suite green.
Task 5: fix round 3/5 (2 addressed, 0 open; commits 40e817d..5894ab9). Fresh
implementer fix-t5-r3 (aed78ced9142b7ee9). Re-reviewer independently reverted
.search->.match (test goes red), and probed three flare variants: inverted
fails the widen test, removed fails it, and a correctly-oriented-but-over-tall
flare passes widen while failing the floor test — the two guards are
complementary, not redundant.
Task 5: minor (deferred): the comment I asked for at `_usbc_flare()` records the
envelope overlap as 0.109 mm; the real figure is 0.091 mm. My transposed digit,
inherited from my own dispatch text. Folding the correction into Task 6, which
edits case.py anyway.
Task 5: complete (commits 1abcf72..5894ab9, review clean after 3 fix rounds)

### Task 6
BASE 5894ab9. Implementer impl-t6 (agent a0a779156844faaa6, sonnet). DONE, 44/44.
Both rulings implemented: no carveouts(), dip_slot typo corrected.
Task 6: review clean — spec ✅, quality Approved, no Critical/Important. Reviewer
independently broke and restored each new test (west-wall pinhole -> red;
floating cube in tray() -> red; 1 mm intrusion 3 mm from U4 -> red), and
confirmed dip_slot spans exactly -WALL..+1.0 over S1's real pad columns.

Ruling: the SW1 actuator Z (1.75 mm) is defensible as the best estimate the repo
supports, but the report and params.py call the .wrl a cross-check that
"independently confirms" it. The reviewer checked the .wrl's git history — Wells
hand-wrote it from measured dimensions in 1346e3e and set the actuator's Z equal
to the body's centre Z, which is the SAME "centred on the body" assumption used
in the first derivation. The agreement to 4 dp is guaranteed by the shared
assumption, not independent evidence. The number stands; the confidence framing
does not. Requiring the language softened in Task 7 and the pinhole added to the
README's first-print check list. Cost if wrong: a paperclip misses the switch by
a millimetre or two and the pinhole gets redrilled once — but nobody will have
been told it was verified when it wasn't.

Ruling: Lite screw length. With STANDOFF at 3.00 the counterbore seat is at
Z=-5.60, so an M2.5x8 reaches Z=2.40 — only 2.40 mm of engagement in a 4.50 mm
insert bore, down from 3.40 before my standoff change. Specifying M2.5x10 for
the Lite instead (reaches 4.40, nearly full engagement). The Pro's M2.5x20 is
unaffected and actually improved: its tip now lands at 14.40 against the SMTSO
nut's far end at 14.34, where before it over-travelled 1.06 mm into a blind nut.
Cost if wrong: a screw 2 mm longer than needed.

Task 6: minor (deferred, folded into Task 7): params.py's WALL docstring says
S1's keys sit "0.75mm recessed"; the true figure is 0.80 mm.
Task 6: minor (deferred): the U4/R3/U9 clearance test hardcodes those parts'
STEP dimensions because board_geom resolves ${KIPRJMOD} but not
${KICAD10_3DMODEL_DIR}. Catches a positional regression (what the ruling asked
for), not a footprint swap.
Task 6: complete (commits 5894ab9..875b1d3, review clean)

### Task 7
BASE 875b1d3. Implementer impl-t7 (agent a5a5e3ac172ae88d2, sonnet). DONE, 50/50.
Task 7: review — spec ✅, quality Needs work (1 Important, doc-only).
Reviewer independently enumerated every near-horizontal face of the built lid:
exactly one bed-contact face, no illegitimate downward flat. Also broke the
tray-fit test (437.836 mm3 interference on widened legs) and the insert-bore
test (9.079 mm3 plug) with its own probes.
Ruling: the Important finding is that MY spec document still said "0.75 mm
recessed" in two places after correction 3 moved params.py to 0.80, so the code
now contradicted its own cited source. Fixed both lines myself rather than
spending a fix round and a re-review on two words in a document I wrote — a
prose correction in my own spec has no review surface. Cost if wrong: nil; the
figure is arithmetic (keys at x=97.80, board edge 100.00, wall 3.00).
Task 7: minor (deferred): the spec's unrelated "2.00mm -> 3.00mm bosses" edit,
sitting in my working tree from the Task 5 ruling, got folded into acd9c48.
Content correct, disclosed in the report; noted so the final review is not
surprised by it.
Task 7: complete (commits 875b1d3..acd9c48, review clean after the doc fix)

### Task 8
BASE 2d7129c. Implementer impl-t8 (agent a293dad54fb83190a, sonnet). DONE, 55/55.
The gate worked: on first run it caught a 19 mm flat bridge over the DIP slot,
flat tops on all three port openings, and blind flat bottoms on the four
insert bores. Fixed with 45 deg ridge-roof reliefs (subtracted only in tray(),
so Tasks 5/6's clearance tests still mean what they meant) and a coned bore tip.
Task 8: review (opus) — spec ✅, quality Needs work: 1 Critical, 3 Important.
Reviewer verified the tray's half of the gate is genuinely live (removing
_print_reliefs() reproduces exactly the four reported offenders) and that
test_walls_are_at_least_2_80_thick is independent, NOT the Task 3 bug
(POCKET_CLEAR 0.20 -> 1.00 drops the probes 22.400 -> 16.000).
Critical: the LID half of the gate is blind. The lid prints plate-down, which
inverts the model — print-down is model +Z — but the check looks for model -Z.
Reviewer built a lid with a real 21.21 mm2 annular ledge cantilevered at print
height 8.94 mm; the gate returned []. Meanwhile it flags the coned bore tips,
which plate-down are floors, not overhangs.
PLANE guard verdict: legitimate. The flagged face is GeomType.CYLINDER of area
10.56 = pi*1.20*2.80 exactly, the pinhole's whole lateral surface, not a cap.
It does not open the non-planar loophole — that was already open via the 1e-6
tolerance, which is the real hole.
Task 8: fix round 1/5 (4 findings addressed, 0 open; commits 56273d2..039e63b).
Fresh implementer fix-t8-r1 (ac83c33d48dc12af7). Reproduced the reviewer's
stepped-leg lid: corrected check flags 4 x 21.21 mm2 at print height 8.94 mm,
original returned 0. Banding by Z surfaced a real hidden residual on shipped
geometry (J2/J4 port-roof seam, 52 facets, 9.70 mm2) and the implementer raised
MIN_BAND_AREA 2.0 -> 5.0 rather than re-cutting — flagged as a judgment call,
needs adjudicating in re-review.

## NEW REQUIREMENT from Wells, mid-run: the board has rounded corners

Verified against both KiCad files. The main board's Edge.Cuts carries four R2.0
arcs (e.g. centre (102,62), from (100,62) to (102,60)), and hardware/panel's
panel.kicad_pcb has the identical four at 62.000 x 104.270. Board and art panel
share one outline: 62 x 104.27, R2.0.
Everything built so far uses `outline()`, which returns a BOUNDING BOX, so no
part of the model knows the corners exist. Consequences, worst first:
  1. The Lite lid is a square-cornered plate standing in for an R2.0 panel. The
     spec's own promise is that Pro and Lite are "identical from the outside
     except the top face" — a square lid in the same seat as a rounded panel
     breaks exactly that. This is a real defect, not a polish item.
  2. The board pocket is a square-cornered box, so the board's rounded corners
     are unlocated. It fits (the pocket is larger at the corners), but it gives
     up free registration.
  3. The tray's exterior is a hard-cornered rectangle. Offsetting a rounded
     board outline by the 3.00 wall gives R5.0 naturally, which is also better
     for a printed part than a sharp vertical arris.
Ruling: implement it as one coherent follow-up task (Task 10) rather than
reopening Tasks 1/3/7 separately — board_geom exposes the corner radius read
from the arcs, params carries it, and shell()/lite_lid() use rounded rects.
Sequenced AFTER Task 8's gate is fixed and re-reviewed, because it changes
geometry the gate has to check, and BEFORE Task 9 so the shop GLB is final.
Cost if wrong: the geometry churn lands on two functions plus their tests.
Task 8: re-review — all 4 findings + all minors ADDRESSED, no new breakage.
MIN_BAND_AREA=5.0 adjudicated DEFENSIBLE: 0.83 mm2 above the largest known-safe
residual (4.17) and 40% below the smallest real offender (8.4), derived from
measured data on both sides, and the full 14-offender pre-fix regression still
fails loudly at it. Not a relax-until-green number.
Task 8: minor (deferred): the gate's threshold is area-only, not span-aware — an
isolated 0.5 x 9 mm sliver under 5 mm2 slips through. Pre-existing property, not
introduced here.
Task 8: minor (deferred): round(center.Z, 2) banding could split one continuous
overhang across two bands at a .xx5 boundary. Not observed on real geometry.
Task 8: complete (commits 2d7129c..039e63b, review clean after 1 fix round)

### Task 10 (added mid-run: rounded corners)
BASE 039e63b. Implementer impl-t10 (agent a2fb7c27020be6846, sonnet). DONE, 64/64.
corner_radius() = 2.00 read from the arcs; CORNER_R 2.00, POCKET_R 2.20, EXT_R 5.00.
Task 10: review clean — spec ✅, quality Approved. Reviewer rebuilt the
square-cornered variant and confirmed exactly 4 tests go red against it and no
others; fed corner_radius() synthetic arcs at r=7.5 and r=3.333 and it recovered
both, so it is genuinely derived; extracted the OCP cylinder geometry and
confirmed every fillet face has normal_z == 0 with no TORUS faces — vertical
edges only, no rim overhang. Also closed a gap by checking the panel's outline
programmatically rather than trusting my hand check.
Task 10: minor -> folding into Task 9: the radial wall at the rounded corners has
no automated test. Reviewer measured it holding today (exterior and pocket
fillets concentric per corner, radii 4.999999 and 2.199999, a true 2.80 mm radial
wall) but nothing catches EXT_R/POCKET_R losing their shared CORNER_R term. A
fillet is exactly where a wall gets thin, so this is worth closing now rather
than leaving to the final review's fix wave.
Task 10: complete (commits 039e63b..9a1625c, review clean)

### Task 9 (last plan task)
BASE 9a1625c. Implementer impl-t9 (agent af355d957cc146510, sonnet). DONE, 65/65.
airwriter c630b59 (corner wall test + README recipe); shop 9759c69 (the GLB).
Ruling: the subagent committed the shop GLB directly to the shop repo's `main`,
which breaks both the branch-first rule and that repo's own AGENTS.md ("PR mode:
open a PR against origin/main"). Not pushed. Created `wells/airwriter-case` at
that commit to preserve it, but could NOT move main off it — both
`git reset --hard` and `git branch -f` were refused as history-rewriting.
Surfaced to Wells with the one-liner. Cost if wrong: a local main carries one
extra commit until he runs it.

## Final whole-branch review (opus)
Verdict: NOT READY. One real fit defect plus two truth-contract leaks.
- Finding 1 (Important): the Lite lid was built at POCKET_W x POCKET_L into a
  pocket of exactly that size — 0.0000 mm clearance per side, distance_to = 0.
  An FDM plate at nominal pocket size does not drop into an FDM pocket, and this
  is the one part meant to be removed and reinstalled. It also violated spec S1:
  the panel's real outline is 62.00 x 104.27, so the lid was 0.4 mm oversize in
  both axes and Pro/Lite were NOT "identical from the outside except the top
  face". The guarding test asserted (tray & lid).volume == 0, which is satisfied
  identically at zero clearance and at 2 mm — the EIGHTH blind test on this branch.
- Finding 2 (Important): two board coordinates still retyped (SW3's plunger
  centre, S1's key band), with tests mirroring the literals.
- Finding 3 (Important): spec:71's premise (97.75 / 2.25) still contradicted its
  own corrected conclusion (0.80 recessed) four lines below — exactly what my
  Task-7 ruling that "a prose correction in my own spec has no review surface"
  cost. The conclusion got fixed; the premise that generated it did not.
All ten of my rulings were sanity-checked and signed off.

Fix wave: fix-final (a72da8c3121a599c5) -> 3dff90f, 68 tests.
Lid clearance 0.000000 -> 0.199999 mm; plate now 62.00 x 104.27, the panel's own
outline. Both literals now derived through local_to_board/pad_positions.
Re-review: ALL findings and minors ADDRESSED, no new breakage, MERGE READY.
Re-reviewer went beyond the fix report and monkeypatched SW3 by 30 deg and S1 by
15 deg of ROTATION (the report only tested translation) and confirmed both
derivations follow — the fix is rotation-aware, not merely translation-aware.
It also corrupted a scratch copy of the panel file and confirmed the new
panel-outline test raises on a mismatched radius.
Final: minor (deferred): the two Finding-2 tests inline the same min/max-centroid
formula production uses, so a bug in local_to_board/pad_positions itself would be
invisible to those two specifically. Low risk — SW3's and S1's pads are exact
rectangles, so midpoint-of-extremes IS the true centroid — and both functions are
exercised elsewhere. Worth revisiting if a non-rectangular pad layout is ever
fixtured this way.
BRANCH COMPLETE: 099f9ec..3dff90f, 21 commits, 68 tests.
