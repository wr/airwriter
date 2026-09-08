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


if __name__ == "__main__":
    unittest.main()
