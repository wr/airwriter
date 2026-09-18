### Task 4: Board standoffs and the screw stack

**Files:**
- Modify: `enclosure/case.py`
- Modify: `enclosure/test_case.py`

**Interfaces:**
- Consumes: `case.shell()`, `board_geom.mount_holes()`
- Produces: `standoffs() -> Part` (the four bosses, to be added), `screw_holes() -> Part` (the four clearance-plus-counterbore solids, to be subtracted)

- [ ] **Step 1: Write the failing test**

Append to `enclosure/test_case.py`, above the `if __name__` block:

```python
class TestStandoffsAndScrews(unittest.TestCase):
    def test_standoffs_sit_at_the_mount_holes_and_reach_the_board(self):
        import board_geom as bg
        s = case.standoffs()
        bb = s.bounding_box()
        self.assertAlmostEqual(bb.min.Z, p.Z_FLOOR_TOP, places=3)
        self.assertAlmostEqual(bb.max.Z, p.Z_BOARD_BOTTOM, places=3)
        self.assertEqual(len(s.solids()), 4)
        holes = bg.mount_holes()
        centres = sorted((round(sl.center().X, 3), round(sl.center().Y, 3))
                         for sl in s.solids())
        want = sorted((round(x, 3), round(y, 3)) for x, y in holes.values())
        self.assertEqual(centres, want)

    def test_screw_holes_pass_right_through_the_floor(self):
        h = case.screw_holes()
        bb = h.bounding_box()
        self.assertLessEqual(bb.min.Z, p.Z_FLOOR_BOTTOM)
        self.assertGreaterEqual(bb.max.Z, p.Z_BOARD_BOTTOM)
        self.assertEqual(len(h.solids()), 4)

    def test_counterbore_transition_is_coned_not_a_flat_ledge(self):
        # Printed floor-down, a flat annulus between a 5.0 counterbore and a
        # 2.8 clearance hole is an unsupported bridge. It must be a 45 cone.
        h = case.screw_holes()
        flats = [f for f in h.faces()
                 if abs(f.normal_at(f.center()).Z + 1.0) < 1e-6
                 and f.area > 2.0
                 and f.center().Z > p.Z_FLOOR_BOTTOM + 1e-6]
        self.assertEqual(flats, [], "downward flat inside a screw hole")
```

- [ ] **Step 2: Run the test and watch it fail**

Run: `cd ~/Projects/airwriter/enclosure && .venv/bin/python -m unittest -v test_case`
Expected: FAIL — `AttributeError: module 'case' has no attribute 'standoffs'`

- [ ] **Step 3: Implement both**

Add to `enclosure/case.py` — extend the import line to
`from build123d import Box, Cone, Cylinder, Part, Pos` and add:

```python
import board_geom as bg

STANDOFF_D = 6.00
"""Boss diameter under each mounting hole. Wide enough to carry the board and
to leave a printable wall around the 2.80 clearance bore."""


def standoffs() -> Part:
    """Four bosses from the floor's top face up to the board's underside."""
    h = p.Z_BOARD_BOTTOM - p.Z_FLOOR_TOP
    out = None
    for x, y in bg.mount_holes().values():
        boss = Pos(x, y, p.Z_FLOOR_TOP + h / 2) * Cylinder(STANDOFF_D / 2, h)
        out = boss if out is None else out + boss
    return out


def screw_holes() -> Part:
    """Clearance bore up through floor and boss, with a counterbore for the
    screw head in the floor's underside.

    The counterbore opens onto the print's first layer, so its transition to
    the clearance bore is a 45-degree cone rather than a flat annulus -- a
    flat there is an unsupported bridge (see the test)."""
    top = p.Z_BOARD_BOTTOM + 0.01
    bottom = p.Z_FLOOR_BOTTOM - 0.01
    out = None
    for x, y in bg.mount_holes().values():
        bore = Pos(x, y, (top + bottom) / 2) * Cylinder(
            p.SCREW_CLEAR_D / 2, top - bottom)
        cb_top = p.Z_FLOOR_BOTTOM + p.SCREW_HEAD_DEPTH
        cbore = Pos(x, y, (cb_top + bottom) / 2) * Cylinder(
            p.SCREW_HEAD_D / 2, cb_top - bottom)
        # 45-degree run-out from the counterbore up to the clearance bore.
        cone_h = (p.SCREW_HEAD_D - p.SCREW_CLEAR_D) / 2
        cone = Pos(x, y, cb_top + cone_h / 2) * Cone(
            p.SCREW_HEAD_D / 2, p.SCREW_CLEAR_D / 2, cone_h)
        one = bore + cbore + cone
        out = one if out is None else out + one
    return out
```

- [ ] **Step 4: Run the test and watch it pass**

Run: `cd ~/Projects/airwriter/enclosure && .venv/bin/python -m unittest -v test_case`
Expected: PASS, 8 tests.

- [ ] **Step 5: Commit**

```bash
cd ~/Projects/airwriter
git add enclosure/case.py enclosure/test_case.py
git commit -m "Enclosure: board standoffs and the screw stack"
```

---

