# SPDX-License-Identifier: CERN-OHL-S-2.0
"""The reset jig (reset_jig.py): it stands against the tray and guides the
drill onto SW1's button. Its printability is in test_printable.py."""
import math
import unittest

from build123d import Box, Cylinder, Pos

import board_geom as bg
import case
import params as p
import reset_jig


def _vol(a, b):
    hit = a & b
    return hit.volume if hit else 0.0


class TestResetJig(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.jig = reset_jig.reset_jig()
        cls.tray = case.tray()
        cls.y, cls.z = case.reset_axis()

    def test_it_stands_against_the_tray_without_cutting_into_it(self):
        self.assertLess(_vol(self.jig, self.tray), 1e-4)
        self.assertLess(self.jig.distance_to(self.tray), 0.01)

    def test_it_stands_on_the_same_table(self):
        self.assertAlmostEqual(self.jig.bounding_box().min.Z,
                               self.tray.bounding_box().min.Z, places=6)

    def test_it_bears_on_the_east_and_back_walls_flat(self):
        # A sliver of the tray just inside each of the jig's two faces, along
        # the walls' flat runs, is all plastic: the faces meet the walls
        # there, past the corner's round and above the bottom chamfer.
        z0 = p.Z_FLOOR_BOTTOM + p.BOTTOM_CHAMFER + 0.2
        z1 = self.jig.bounding_box().max.Z - 0.2
        zc, zh = (z0 + z1) / 2, z1 - z0
        y0 = self.y - p.JIG_PAST + 0.2
        y1 = p.BOARD_L - p.CORNER_R - 0.2
        east = Pos(reset_jig.X_WALL - 0.05, (y0 + y1) / 2, zc) * Box(0.1, y1 - y0, zh)
        x0 = reset_jig.X_WALL - p.JIG_REACH + 0.2
        x1 = p.BOARD_W - p.CORNER_R - 0.2
        back = Pos((x0 + x1) / 2, reset_jig.Y_BACK - 0.05, zc) * Box(x1 - x0, 0.1, zh)
        bore_mouth = math.pi * (p.JIG_BORE_D / 2) ** 2 * 0.1
        for name, sliver, hole in (("east", east, bore_mouth), ("back", back, 0.0)):
            self.assertAlmostEqual(_vol(self.tray, sliver), sliver.volume, places=3, msg=name)
            # ...and the jig is right there, just outside, but for the bore.
            outside = (Pos(0.1, 0, 0) if name == "east" else Pos(0, 0.1, 0)) * sliver
            self.assertAlmostEqual(_vol(self.jig, outside), outside.volume - hole, delta=0.005,
                                   msg=name)

    def test_its_bore_is_open_and_on_sw1s_button(self):
        x1 = reset_jig.X_WALL + p.JIG_BORE_L
        rod = Pos((reset_jig.X_WALL + x1) / 2, self.y, self.z) * Cylinder(
            p.JIG_BORE_D / 2 - 0.05, p.JIG_BORE_L + 1, rotation=(0, 90, 0))
        self.assertLess(_vol(self.jig, rod), 1e-4)
        fp = bg.footprint("SW1")
        self.assertAlmostEqual(self.y, bg.to_case(fp.x, fp.y)[1], places=3)
        self.assertAlmostEqual(self.z, p.RESET_ACTUATOR_Z, places=3)
        # Carried on, the bore's axis runs into the east wall: the bit drills
        # the wall there, not a corner or a chamfer.
        wall = Pos(p.BOARD_W + p.WALL - p.THIN_WALL / 2, self.y, self.z) * Cylinder(
            p.RESET_DRILL_D / 2, p.THIN_WALL - 0.1, rotation=(0, 90, 0))
        self.assertAlmostEqual(_vol(self.tray, wall), wall.volume, places=4)

    def test_the_bit_goes_through_the_guide(self):
        self.assertGreater(p.JIG_BORE_D, p.RESET_DRILL_D)
        self.assertLessEqual(p.JIG_BORE_D - p.RESET_DRILL_D, 0.2)


if __name__ == "__main__":
    unittest.main()
