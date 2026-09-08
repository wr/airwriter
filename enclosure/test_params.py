"""params.py is where this project's *choices* live. The spec fixes these
numbers; this test is the spec's teeth."""
import unittest

import params as p


class TestFixedByTheSpec(unittest.TestCase):
    def test_wall_floor_and_stack(self):
        self.assertEqual(p.WALL, 3.00)
        self.assertEqual(p.FLOOR, 2.40)
        self.assertEqual(p.STANDOFF, 3.00)
        self.assertEqual(p.PCB, 1.60)
        self.assertEqual(p.PANEL, 1.60)
        self.assertEqual(p.CAVITY, 14.34)
        self.assertEqual(p.POCKET_CLEAR, 0.20)

    def test_cavity_is_the_minidin_plus_the_smtso_nut(self):
        # 12.74 shell top + 1.60 nut = 14.34. Not a free variable.
        self.assertAlmostEqual(p.CAVITY, 12.74 + 1.60, places=6)


class TestDerived(unittest.TestCase):
    def test_board_size_comes_from_the_board(self):
        self.assertAlmostEqual(p.BOARD_W, 62.00, places=3)
        self.assertAlmostEqual(p.BOARD_L, 104.27, places=3)

    def test_external_size(self):
        self.assertAlmostEqual(p.EXT_W, 68.00, places=3)
        self.assertAlmostEqual(p.EXT_L, 110.27, places=3)
        self.assertAlmostEqual(p.EXT_H, 22.94, places=3)

    def test_z_landmarks(self):
        self.assertAlmostEqual(p.Z_BOARD_BOTTOM, -1.60, places=3)
        self.assertAlmostEqual(p.Z_FLOOR_TOP, -4.60, places=3)
        self.assertAlmostEqual(p.Z_FLOOR_BOTTOM, -7.00, places=3)
        self.assertAlmostEqual(p.Z_LID_UNDER, 14.34, places=3)
        self.assertAlmostEqual(p.Z_WALL_TOP, 15.94, places=3)

    def test_effective_wall_after_pocket_clearance(self):
        # 3.00 nominal, 0.20 eaten by pocket clearance -> 2.80 of plastic.
        self.assertAlmostEqual(p.WALL - p.POCKET_CLEAR, 2.80, places=3)


if __name__ == "__main__":
    unittest.main()
