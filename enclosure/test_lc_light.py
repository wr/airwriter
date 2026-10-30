# SPDX-License-Identifier: CERN-OHL-S-2.0
"""The LC light tube (lc_light.py): the loose tube round D26."""
import unittest

from build123d import Box, Cylinder, Pos

import board_geom as bg
import components
import lc_frame
import lc_light
import params as p


def _box(x0, x1, y0, y1, z0, z1):
    return Pos((x0 + x1) / 2, (y0 + y1) / 2, (z0 + z1) / 2) * Box(x1 - x0, y1 - y0, z1 - z0)


def _vol(part):
    return part.volume if part else 0.0


class TestLCLight(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tube = lc_light.lc_light()

    def test_is_one_valid_solid_on_the_board_under_the_acrylic(self):
        self.assertTrue(self.tube.is_valid)
        self.assertEqual(len(self.tube.solids()), 1)
        bb = self.tube.bounding_box()
        self.assertAlmostEqual(bb.min.Z, 0.0, places=3)
        self.assertAlmostEqual(p.ACRYLIC_UNDERSIDE - bb.max.Z, p.LC_LIGHT_PLAY, places=3)

    def test_it_is_over_d26_and_its_dot(self):
        fp = bg.footprint("D26")
        x, y = lc_light.dot()
        dx, dy = bg.to_case(fp.x, fp.y)
        self.assertAlmostEqual(x, dx, delta=0.01)
        self.assertAlmostEqual(y, dy, delta=0.01)

    def test_it_clears_every_part_and_holds_d26_and_c46(self):
        # D26 and C46 inside, LC_LIGHT_CLEAR clear of their courtyards; the
        # walls on nothing, JP1's solder blob and SW3 included.
        (ix0, ix1, iy0, iy1), _ = lc_light.light_rects()
        for ref, bands in components.envelopes().items():
            for x0, x1, y0, y1, top in bands:
                if ref in ("D26", "C46"):
                    c = p.LC_LIGHT_CLEAR - 1e-6
                    self.assertTrue(ix0 + c <= x0 and x1 <= ix1 - c and iy0 + c <= y0 and y1 <= iy1 - c, ref)
                hit = _vol(self.tube & _box(x0, x1, y0, y1, -1, top))
                self.assertAlmostEqual(hit, 0.0, places=4, msg=f"the tube runs into {ref}")

    def test_light_leaves_only_through_the_aperture(self):
        # Walls LATTICE_WALL all round from the board to the cap; the cap
        # solid but for a round opening centred on the dot.
        (ix0, ix1, iy0, iy1), (ox0, ox1, oy0, oy1) = lc_light.light_rects()
        z1 = lc_light.TOP - p.FRAME_CAP
        walls = _box(ox0, ox1, oy0, oy1, 0, z1) - _box(ix0, ix1, iy0, iy1, -1, z1 + 1)
        self.assertAlmostEqual(_vol(self.tube & walls), walls.volume, places=3)
        x, y = lc_light.dot()
        hole = Pos(x, y, (z1 + lc_light.TOP) / 2) * Cylinder(p.LC_LIGHT_APERTURE_D / 2, p.FRAME_CAP)
        cap = _box(ix0, ix1, iy0, iy1, z1, lc_light.TOP)
        self.assertAlmostEqual(_vol(self.tube & cap), cap.volume - hole.volume, places=3)
        self.assertAlmostEqual(_vol(self.tube & hole), 0.0, places=4)

    def test_aperture_stays_under_the_dot_wherever_the_tube_sits(self):
        # The tube can wander LC_LIGHT_CLEAR plus the courtyards' own margin
        # round the parts (0.25 on the LED): its opening stays inside the dot.
        r = bg.panel_grid()["dot_dia"] / 2
        self.assertLessEqual(p.LC_LIGHT_APERTURE_D / 2 + p.LC_LIGHT_CLEAR + 0.25, r)

    def test_it_clears_the_frame(self):
        self.assertGreater(lc_frame.lc_frame().distance_to(self.tube), 1.0)


if __name__ == "__main__":
    unittest.main()
