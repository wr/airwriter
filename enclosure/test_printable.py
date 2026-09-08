"""The gate between "the model closes" and "the print will not fail".

Both parts print flat-side-down with no supports. The check that matters is
unsupported DOWNWARD-FACING FLATS: a curved overhang (a port hole's top arc)
prints fine at these diameters, a horizontal ledge in mid-air does not.
"""
import unittest

from build123d import GeomType

import case
import params as p

MIN_WALL = 2.80
"""3.00 nominal minus the 0.20 pocket clearance."""


def downward_flats(part, floor_z, min_area=2.0):
    """Real bridges only: a PLANAR face, facing straight down, big enough to
    matter, above the actual build plate.

    The geom_type == PLANE guard is not in the brief's original sketch of
    this helper -- it was added after the first run flagged the east wall's
    reset pinhole (a plain 1.20mm-diameter through-bore) as a 10.56mm^2
    offender. That bore is GeomType.CYLINDER: normal_at() evaluated at the
    face's parametric centre happens to point straight down there (the
    bottom of the barrel), but the face itself is the bore's whole curved
    lateral surface, not a flat cap -- exactly the "a curved overhang...
    prints fine" case this module's own docstring already carves out.
    Sampling one point's normal can't tell a curved face from a planar one;
    checking geom_type does. Confirmed against the case: a round hole
    through a wall, correctly, produces nothing here once this guard is in
    place (see task-8-report.md for the before/after)."""
    out = []
    for f in part.faces():
        if f.geom_type != GeomType.PLANE:
            continue
        try:
            n = f.normal_at(f.center())
        except Exception:
            continue
        if abs(n.Z + 1.0) < 1e-6 and f.area > min_area and f.center().Z > floor_z + 1e-6:
            out.append(f)
    return out


class TestTray(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.t = case.tray()

    def test_single_watertight_solid(self):
        self.assertTrue(self.t.is_valid)
        self.assertEqual(len(self.t.solids()), 1)

    def test_no_unsupported_downward_flats(self):
        flats = downward_flats(self.t, p.Z_FLOOR_BOTTOM)
        self.assertEqual(
            [(round(f.center().X, 1), round(f.center().Y, 1),
              round(f.center().Z, 1), round(f.area, 1)) for f in flats], [])

    def test_walls_are_at_least_2_80_thick(self):
        # Probe each wall mid-height, away from any opening.
        from build123d import Box, Pos
        probes = [
            ("west", Pos(-p.WALL / 2, 90.0, 12.0), (p.WALL, 4.0, 2.0)),
            ("east", Pos(p.BOARD_W + p.WALL / 2, 90.0, 12.0), (p.WALL, 4.0, 2.0)),
            ("back", Pos(31.0, p.BOARD_L + p.WALL / 2, 12.0), (4.0, p.WALL, 2.0)),
        ]
        for name, pos, size in probes:
            probe = pos * Box(*size)
            got = (self.t & probe).volume
            want = MIN_WALL * size[0] * size[1] * size[2] / p.WALL
            self.assertGreaterEqual(got, want - 0.05, name)


class TestLid(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.lid = case.lite_lid()

    def test_single_watertight_solid(self):
        self.assertTrue(self.lid.is_valid)
        self.assertEqual(len(self.lid.solids()), 1)

    def test_no_unsupported_downward_flats_printed_plate_down(self):
        # Printed plate-down, the plate's own underside is the build surface,
        # so "floor" is the top of the plate's underside: Z_WALL_TOP - PANEL.
        flats = downward_flats(self.lid, 0.0)
        offenders = [f for f in flats
                     if abs(f.center().Z - (p.Z_WALL_TOP - p.PANEL)) > 1e-6]
        self.assertEqual(
            [(round(f.center().X, 1), round(f.center().Y, 1),
              round(f.center().Z, 1)) for f in offenders], [])


if __name__ == "__main__":
    unittest.main()
