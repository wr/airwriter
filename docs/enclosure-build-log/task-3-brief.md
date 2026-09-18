### Task 3: The tray's shell — floor, walls, pocket, lid seat

**Files:**
- Create: `enclosure/case.py`
- Create: `enclosure/test_case.py`

**Interfaces:**
- Consumes: `params` (all constants), `board_geom.to_case`
- Produces: `shell() -> Part` — the tray with no openings in it yet. Later tasks in this plan subtract from what `shell()` returns; `tray()` arrives in Task 6.

- [ ] **Step 1: Write the failing test**

Create `enclosure/test_case.py`:

```python
import unittest

import params as p
import case


class TestShell(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.s = case.shell()

    def test_is_a_valid_single_solid(self):
        self.assertTrue(self.s.is_valid)
        self.assertEqual(len(self.s.solids()), 1)

    def test_external_bounding_box(self):
        bb = self.s.bounding_box()
        self.assertAlmostEqual(bb.size.X, p.EXT_W, places=3)
        self.assertAlmostEqual(bb.size.Y, p.EXT_L, places=3)
        self.assertAlmostEqual(bb.size.Z, p.EXT_H, places=3)

    def test_sits_in_the_case_frame(self):
        # X=0 is the west wall's inner face line, so the solid starts at -WALL.
        bb = self.s.bounding_box()
        self.assertAlmostEqual(bb.min.X, -p.WALL, places=3)
        self.assertAlmostEqual(bb.min.Y, -p.WALL, places=3)
        self.assertAlmostEqual(bb.min.Z, p.Z_FLOOR_BOTTOM, places=3)
        self.assertAlmostEqual(bb.max.Z, p.Z_WALL_TOP, places=3)

    def test_cavity_is_hollow_from_floor_top_upward(self):
        # A probe block filling the pocket must not intersect the shell.
        from build123d import Box, Pos
        probe = Pos(p.BOARD_W / 2, p.BOARD_L / 2,
                    (p.Z_WALL_TOP + p.Z_FLOOR_TOP) / 2) * Box(
            p.POCKET_W, p.POCKET_L, p.Z_WALL_TOP - p.Z_FLOOR_TOP)
        self.assertAlmostEqual((self.s & probe).volume, 0.0, places=3)

    def test_floor_is_solid_under_the_pocket(self):
        from build123d import Box, Pos
        probe = Pos(p.BOARD_W / 2, p.BOARD_L / 2,
                    (p.Z_FLOOR_TOP + p.Z_FLOOR_BOTTOM) / 2) * Box(
            p.BOARD_W, p.BOARD_L, p.FLOOR)
        self.assertAlmostEqual((self.s & probe).volume, probe.volume, places=2)


if __name__ == "__main__":
    unittest.main()
```

- [ ] **Step 2: Run the test and watch it fail**

Run: `cd ~/Projects/airwriter/enclosure && .venv/bin/python -m unittest -v test_case`
Expected: FAIL — `ModuleNotFoundError: No module named 'case'`

- [ ] **Step 3: Write `shell()` in `case.py`**

Create `enclosure/case.py`:

```python
"""The AirWriter enclosure: a tray shared by both builds, and a Lite lid.

Everything is modelled in the case frame (see board_geom): X=0 is the west
wall's inner face, Y=0 is the front (port) wall's inner face, Z=0 is the
board's top face with +Z up.
"""
from build123d import Box, Part, Pos

import params as p


def shell() -> Part:
    """Floor plus four walls, hollow from the floor's top face upward.

    No openings: the port cutouts, the DIP slot, the pinhole, the screw holes
    and the carve-outs are all subtracted by tray().
    """
    outer = Pos(
        p.BOARD_W / 2,
        p.BOARD_L / 2,
        (p.Z_WALL_TOP + p.Z_FLOOR_BOTTOM) / 2,
    ) * Box(p.EXT_W, p.EXT_L, p.EXT_H)

    cavity = Pos(
        p.BOARD_W / 2,
        p.BOARD_L / 2,
        (p.Z_WALL_TOP + p.Z_FLOOR_TOP) / 2,
    ) * Box(p.POCKET_W, p.POCKET_L, p.Z_WALL_TOP - p.Z_FLOOR_TOP)

    return outer - cavity
```

- [ ] **Step 4: Run the test and watch it pass**

Run: `cd ~/Projects/airwriter/enclosure && .venv/bin/python -m unittest -v test_case`
Expected: PASS, 5 tests.

- [ ] **Step 5: Commit**

```bash
cd ~/Projects/airwriter
git add enclosure/case.py enclosure/test_case.py
git commit -m "Enclosure: tray shell"
```

---

