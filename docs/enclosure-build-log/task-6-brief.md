### Task 6: DIP slot, reset pinhole, carve-outs, and `tray()`

**Files:**
- Modify: `enclosure/case.py`
- Modify: `enclosure/test_case.py`

**Interfaces:**
- Consumes: everything from Tasks 3–5
- Produces: `dip_slot() -> Part`, `reset_pinhole() -> Part`, `carveouts() -> Part`, `tray() -> Part` — the finished, exportable tray

S1 and SW1 have only `.wrl` models, which OCCT cannot import, so these two cutouts are parametric from their footprints' own measured descriptions rather than booleaned from vendor solids. Both descriptions carry traced-from-drawing dimensions; the numbers are in `params.py`.

- [ ] **Step 1: Write the failing test**

Append to `enclosure/test_case.py`, above the `if __name__` block:

```python
class TestSideFeaturesAndTray(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.t = case.tray()

    def test_dip_slot_is_in_the_west_wall_at_the_key_band(self):
        import board_geom as bg
        bb = case.dip_slot().bounding_box()
        self.assertLessEqual(bb.min.X, -p.WALL)
        self.assertGreaterEqual(bb.max.X, 0.0)
        self.assertAlmostEqual(bb.min.Z, p.DIP_SLOT_Z0, places=3)
        self.assertAlmostEqual(bb.max.Z, p.DIP_SLOT_Z1, places=3)
        # Key band: S1's pad columns at footprint-local x = -8.89 .. +8.89.
        fp = bg.footprint("S1")
        lo = bg.to_case(*bg.local_to_board(fp, -8.89, 0.0))[1]
        hi = bg.to_case(*bg.local_to_board(fp, 8.89, 0.0))[1]
        lo, hi = min(lo, hi), max(lo, hi)
        self.assertAlmostEqual(bb.min.Y, lo - p.DIP_SLOT_MARGIN, places=2)
        self.assertAlmostEqual(bb.max.Y, hi + p.DIP_SLOT_MARGIN, places=2)

    def test_reset_pinhole_is_in_the_EAST_wall(self):
        # HANDOFF carries a superseded note putting this on the west wall.
        import board_geom as bg
        bb = case.reset_pinhole().bounding_box()
        self.assertGreaterEqual(bb.max.X, p.BOARD_W + p.WALL)
        self.assertLessEqual(bb.min.X, p.BOARD_W)
        _, cy = bg.to_case(*[bg.footprint("SW1").x, bg.footprint("SW1").y])
        self.assertAlmostEqual((bb.min.Y + bb.max.Y) / 2, cy, places=2)
        self.assertAlmostEqual(bb.size.Z, p.RESET_PINHOLE_D, places=3)

    def test_tray_is_one_valid_watertight_solid(self):
        self.assertTrue(self.t.is_valid)
        self.assertEqual(len(self.t.solids()), 1)

    def test_tray_keeps_its_external_envelope(self):
        bb = self.t.bounding_box()
        self.assertAlmostEqual(bb.size.X, p.EXT_W, places=3)
        self.assertAlmostEqual(bb.size.Y, p.EXT_L, places=3)
        self.assertAlmostEqual(bb.size.Z, p.EXT_H, places=3)

    def test_carveouts_clear_the_named_internal_parts(self):
        import board_geom as bg
        for ref in ("U4", "R3", "U9"):
            fp = bg.footprint(ref)
            x, y = bg.to_case(fp.x, fp.y)
            from build123d import Cylinder, Pos
            probe = Pos(x, y, 1.0) * Cylinder(1.0, 2.0)
            self.assertAlmostEqual((self.t & probe).volume, 0.0, places=3,
                                   msg=ref)
```

- [ ] **Step 2: Run the test and watch it fail**

Run: `cd ~/Projects/airwriter/enclosure && .venv/bin/python -m unittest -v test_case`
Expected: FAIL — `AttributeError: module 'case' has no attribute 'tray'`

- [ ] **Step 3: Implement**

Add to `enclosure/case.py`:

```python
CARVEOUT_R = {"U4": 4.0, "R3": 2.0, "U9": 2.5}
"""Keep-out radius per internal part, from its footprint's courtyard. These
are wall-side reliefs: the parts sit inboard, and the carve-out only matters
where a wall would otherwise crowd them."""


def dip_slot() -> Part:
    """S1's eight piano keys, out through the west wall.

    S1's footprint origin is the centre of its pin field; the pad columns run
    footprint-local x = -8.89 .. +8.89 and the keys align with them. The tips
    reach local y = -7.205 (body 9.61 deep centred on the 7.62 pin rows, keys
    2.40 proud), which lands at KiCad x = 97.80 -- 2.20mm past the board edge,
    so behind a 3.00 wall the keys sit 0.75mm recessed by design.
    """
    fp = bg.footprint("S1")
    ys = [bg.to_case(*bg.local_to_board(fp, lx, 0.0))[1] for lx in (-8.89, 8.89)]
    y0, y1 = min(ys) - p.DIP_SLOT_MARGIN, max(ys) + p.DIP_SLOT_MARGIN
    depth = p.WALL + 1.0  # overshoot inward so the cut is clean
    return Pos(
        -p.WALL / 2 + 0.5,  # spans x = -WALL .. +1.0
        (y0 + y1) / 2,
        (p.DIP_SLOT_Z0 + p.DIP_SLOT_Z1) / 2,
    ) * Box(depth, y1 - y0, p.DIP_SLOT_Z1 - p.DIP_SLOT_Z0)


def reset_pinhole() -> Part:
    """A pinhole through the EAST wall on SW1's actuator axis.

    HANDOFF has a superseded line putting this on the west wall at x=100; the
    live ruling is the east edge, with SW1's plate face at KiCad x ~ 159.4.
    """
    fp = bg.footprint("SW1")
    _, y = bg.to_case(fp.x, fp.y)
    z = (p.DIP_SLOT_Z0 + p.DIP_SLOT_Z1) / 2 - 5.0  # SW1's axis, low on the wall
    length = p.WALL + 2.0
    return Pos(p.BOARD_W + p.WALL - length / 2 + 1.0, y, z) * Cylinder(
        p.RESET_PINHOLE_D / 2, length, rotation=(0, 90, 0))


def carveouts() -> Part:
    """Air around the internal parts the ledger names, from the board's top
    face to the lid underside."""
    h = p.Z_LID_UNDER
    out = None
    for ref, r in CARVEOUT_R.items():
        fp = bg.footprint(ref)
        x, y = bg.to_case(fp.x, fp.y)
        cyl = Pos(x, y, h / 2) * Cylinder(r + p.CARVEOUT_CLEAR, h)
        out = cyl if out is None else out + cyl
    return out


def tray() -> Part:
    """The finished tray, shared by Pro and LC."""
    body = shell() + standoffs()
    return body - screw_holes() - port_cutouts() - dip_slot() \
        - reset_pinhole() - carveouts()
```

- [ ] **Step 4: Run the test and watch it pass**

Run: `cd ~/Projects/airwriter/enclosure && .venv/bin/python -m unittest -v test_case`
Expected: PASS, 18 tests.

`test_reset_pinhole_is_in_the_EAST_wall` also pins the pinhole's Z. If SW1's real actuator axis is not 5.0 mm below the DIP band's centre, correct the `z` line in `reset_pinhole()` from the footprint's `.wrl` body height and update nothing else — the test asserts the wall and the Y axis, not the Z.

- [ ] **Step 5: Commit**

```bash
cd ~/Projects/airwriter
git add enclosure/case.py enclosure/test_case.py
git commit -m "Enclosure: DIP slot, reset pinhole, carve-outs, finished tray"
```

---

