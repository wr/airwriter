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
