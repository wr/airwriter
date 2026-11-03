### Task 7: The LC lid

**Files:**
- Modify: `enclosure/case.py`
- Modify: `enclosure/test_case.py`

**Interfaces:**
- Consumes: `params`, `board_geom.mount_holes()`, `board_geom.footprint("SW3")`
- Produces: `lc_lid() -> Part`

- [ ] **Step 1: Write the failing test**

Append to `enclosure/test_case.py`, above the `if __name__` block:

```python
class TestLCLid(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.lid = case.lc_lid()

    def test_is_one_valid_solid(self):
        self.assertTrue(self.lid.is_valid)
        self.assertEqual(len(self.lid.solids()), 1)

    def test_plate_matches_the_art_panel_and_seats_flush(self):
        bb = self.lid.bounding_box()
        self.assertAlmostEqual(bb.size.X, p.POCKET_W, places=3)
        self.assertAlmostEqual(bb.size.Y, p.POCKET_L, places=3)
        self.assertAlmostEqual(bb.max.Z, p.Z_WALL_TOP, places=3)

    def test_legs_span_the_cavity_to_the_board_face(self):
        bb = self.lid.bounding_box()
        self.assertAlmostEqual(bb.min.Z, 0.0, places=3)
        self.assertAlmostEqual(bb.size.Z, p.CAVITY + p.PANEL, places=3)

    def test_it_fits_the_tray_with_no_interference(self):
        self.assertAlmostEqual((case.tray() & self.lid).volume, 0.0, places=3)

    def test_plunger_bore_sits_over_SW3(self):
        # SW3's plunger is 3.95 from the footprint origin; HANDOFF puts it at
        # KiCad (131, 91), under the art panel's test dot.
        from build123d import Cylinder, Pos
        import board_geom as bg
        x, y = bg.to_case(131.0, 91.0)
        probe = Pos(x, y, p.Z_WALL_TOP - p.PANEL / 2) * Cylinder(0.4, p.PANEL)
        self.assertAlmostEqual((self.lid & probe).volume, 0.0, places=3)

    def test_insert_bores_open_downward_at_the_board_face(self):
        import board_geom as bg
        from build123d import Cylinder, Pos
        for x, y in bg.mount_holes().values():
            probe = Pos(x, y, p.INSERT_DEPTH / 2) * Cylinder(
                p.INSERT_D / 2 - 0.1, p.INSERT_DEPTH)
            self.assertAlmostEqual((self.lid & probe).volume, 0.0, places=3)
```

- [ ] **Step 2: Run the test and watch it fail**

Run: `cd ~/Projects/airwriter/enclosure && .venv/bin/python -m unittest -v test_case`
Expected: FAIL — `AttributeError: module 'case' has no attribute 'lc_lid'`

- [ ] **Step 3: Implement**

Add to `enclosure/case.py`:

```python
LEG_D = 6.00
"""LC lid leg diameter -- enough plastic round a 3.60 insert bore."""


def lc_lid() -> Part:
    """The panel stand-in for boards built without the dot-grid art panel.

    A plate the panel's size and thickness, seating in the same place, on four
    legs that reproduce in plastic the 14.34mm the lattice and the panel's
    SMTSO nuts span on a Pro. Each leg takes an M2.5 heat-set insert at its
    BOTTOM, level with the board's top face, so the screw from below engages
    brass after a few millimetres instead of threading printed plastic.
    """
    plate = Pos(
        p.BOARD_W / 2,
        p.BOARD_L / 2,
        p.Z_WALL_TOP - p.PANEL / 2,
    ) * Box(p.POCKET_W, p.POCKET_L, p.PANEL)

    body = plate
    for x, y in bg.mount_holes().values():
        leg = Pos(x, y, p.CAVITY / 2) * Cylinder(LEG_D / 2, p.CAVITY)
        body = body + leg

    for x, y in bg.mount_holes().values():
        bore = Pos(x, y, p.INSERT_DEPTH / 2 - 0.01) * Cylinder(
            p.INSERT_D / 2, p.INSERT_DEPTH + 0.02)
        body = body - bore

    px, py = bg.to_case(131.0, 91.0)
    plunger = Pos(px, py, p.Z_WALL_TOP - p.PANEL / 2) * Cylinder(
        p.PLUNGER_BORE_D / 2, p.PANEL + 0.02)
    return body - plunger
```

- [ ] **Step 4: Run the test and watch it pass**

Run: `cd ~/Projects/airwriter/enclosure && .venv/bin/python -m unittest -v test_case`
Expected: PASS, 24 tests.

- [ ] **Step 5: Commit**

```bash
cd ~/Projects/airwriter
git add enclosure/case.py enclosure/test_case.py
git commit -m "Enclosure: LC lid"
```

---

