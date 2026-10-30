# SPDX-License-Identifier: CERN-OHL-S-2.0
"""The LC light tube (lc_light.py): the loose part round D26 and SW3."""
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
        # walls on nothing, JP1's solder blob included. SW3's model plunger
        # is a 3.5 square, which the round collar would clip at its corners:
        # the collar is checked against the measured plunger below.
        (ix0, ix1, iy0, iy1), _ = lc_light.light_rects()
        for ref, bands in components.envelopes().items():
            for x0, x1, y0, y1, top in bands:
                if ref in ("D26", "C46"):
                    c = p.LC_LIGHT_CLEAR - 1e-6
                    self.assertTrue(ix0 + c <= x0 and x1 <= ix1 - c and iy0 + c <= y0 and y1 <= iy1 - c, ref)
                if ref == "SW3":
                    top = min(top, lc_light.TOP - p.COLLAR_H)
                hit = _vol(self.tube & _box(x0, x1, y0, y1, -1, top))
                self.assertAlmostEqual(hit, 0.0, places=4, msg=f"the tube runs into {ref}")

    def test_the_sleeve_hugs_the_switch_body(self):
        # At the body's middle height, a wall LC_SWITCH_CLEAR off each of its
        # four sides (the guiding fit, 0.20-0.25 a side): the switch holds the
        # part, and the part braces the switch.
        self.assertLessEqual(p.LC_SWITCH_CLEAR, 0.25)
        x0, x1, y0, y1, top = lc_light.switch_body()[-1]
        c, z = p.LC_SWITCH_CLEAR, top - 0.5
        probes = {"west": _box(x0 - c - 0.1, x0 - c, y0 + 1, y1 - 1, z, z + 0.2),
                  "east": _box(x1 + c, x1 + c + 0.1, y0 + 1, y1 - 1, z, z + 0.2),
                  "south": _box(x0 + 1, x1 - 1, y0 - c - 0.1, y0 - c, z, z + 0.2),
                  "north": _box(x0 + 1, x1 - 1, y1 + c, y1 + c + 0.1, z, z + 0.2)}
        for side, probe in probes.items():
            self.assertAlmostEqual(_vol(self.tube & probe), probe.volume, places=4, msg=side)
            gap = probe.moved(Pos({"west": 0.1, "east": -0.1}.get(side, 0),
                                  {"south": 0.1, "north": -0.1}.get(side, 0), 0))
            self.assertAlmostEqual(_vol(self.tube & gap), 0.0, places=4, msg=f"{side} gap")

    def test_the_collar_steadies_the_plunger_under_the_test_hole(self):
        # The lattice's collar: COLLAR_BORE_D through the top COLLAR_H, solid
        # round it, 0.20-0.25 a side on the measured plunger, centred on the
        # switch and under the acrylic's test hole.
        sx, sy = lc_light.switch_axis()
        z0 = lc_light.TOP - p.COLLAR_H
        bore = Pos(sx, sy, (z0 + lc_light.TOP) / 2) * Cylinder(p.COLLAR_BORE_D / 2, p.COLLAR_H)
        ring = Pos(sx, sy, (z0 + lc_light.TOP) / 2) * Cylinder(p.COLLAR_BORE_D / 2 + 1.0, p.COLLAR_H)
        self.assertAlmostEqual(_vol(self.tube & bore), 0.0, places=4)
        self.assertAlmostEqual(_vol(self.tube & ring), ring.volume - bore.volume, places=3)
        self.assertGreaterEqual((p.COLLAR_BORE_D - p.TEST_PLUNGER_D_TOP) / 2, 0.20)
        plunger = Pos(sx, sy, lc_light.TOP / 2 + 2) * Cylinder(p.TEST_PLUNGER_D_BASE / 2, lc_light.TOP)
        self.assertAlmostEqual(_vol(self.tube & (plunger - ring)), 0.0, places=4, msg="the plunger's travel")
        (t,) = [d for d in bg.panel_dots() if d["mat"] == "T"]
        self.assertLess(abs(sx - t["x"]) + abs(sy - t["y"]), 0.15, "collar off the acrylic's hole")

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
