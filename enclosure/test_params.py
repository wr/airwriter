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
        self.assertEqual(p.CAVITY, 13.04)
        self.assertEqual(p.POCKET_CLEAR, 0.30)

    def test_cavity_is_the_minidin_lattice_height_not_nut_plus_lattice(self):
        # CORRECTED 2026-09-08: CAVITY is the mini-DIN/lattice height alone
        # (12.74, measured off the board's own GLB in the case frame -- see
        # params.CAVITY), NOT 12.74 shell top + 1.60 nut stacked on top of
        # it. That double-counted the panel's own 1.60mm thickness (already
        # added separately as PANEL in Z_WALL_TOP = CAVITY + PANEL), which
        # is what put the wall 1.6mm proud of the panel. Pin the corrected
        # relationship so the old double-count can't silently come back.
        # MEASURED 2026-10-14: the shells stand 13.04 on rev A, and the panel
        # rests on them, so CAVITY is that measured height (MINIDIN_TOP).
        self.assertAlmostEqual(p.CAVITY, p.MINIDIN_TOP, places=6)
        self.assertAlmostEqual(p.CAVITY, 13.04, places=6)
        self.assertNotAlmostEqual(p.CAVITY, 13.04 + 1.60, places=2)


class TestDerived(unittest.TestCase):
    def test_board_size_comes_from_the_board(self):
        self.assertAlmostEqual(p.BOARD_W, 62.00, places=3)
        self.assertAlmostEqual(p.BOARD_L, 104.27, places=3)

    def test_external_size(self):
        self.assertAlmostEqual(p.EXT_W, 68.00, places=3)
        self.assertAlmostEqual(p.EXT_L, 110.27, places=3)
        # EXT_H derives from Z_WALL_TOP - Z_FLOOR_BOTTOM; check it against
        # that derivation rather than a pasted literal, so a future CAVITY/
        # WALL/FLOOR/STANDOFF/PCB/PANEL change can't silently drift EXT_H
        # without this test noticing.
        self.assertAlmostEqual(
            p.EXT_H, p.Z_WALL_TOP - p.Z_FLOOR_BOTTOM, places=6)
        self.assertAlmostEqual(p.EXT_H, 21.64, places=3)

    def test_z_landmarks(self):
        self.assertAlmostEqual(p.Z_BOARD_BOTTOM, -1.60, places=3)
        self.assertAlmostEqual(p.Z_FLOOR_TOP, -4.60, places=3)
        self.assertAlmostEqual(p.Z_FLOOR_BOTTOM, -7.00, places=3)
        self.assertAlmostEqual(p.Z_LID_UNDER, p.CAVITY, places=6)
        self.assertAlmostEqual(p.Z_LID_UNDER, 13.04, places=3)
        self.assertAlmostEqual(p.Z_WALL_TOP, p.CAVITY + p.PANEL, places=6)
        self.assertAlmostEqual(p.Z_WALL_TOP, 14.64, places=3)

    def test_effective_wall_after_pocket_clearance(self):
        # 3.00 nominal, 0.30 eaten by pocket clearance -> 2.70 of plastic
        # at the panel seat (the west wall is thinner below it: WEST_FLEX_T).
        self.assertAlmostEqual(p.WALL - p.POCKET_CLEAR, 2.70, places=3)


class TestLiteScrewLength(unittest.TestCase):
    def test_screw_reaches_the_insert_with_engagement_to_spare(self):
        # README.md ships M2.5 x 10 for the Lite lid, derived by hand from
        # Z_FLOOR_BOTTOM/SCREW_HEAD_DEPTH/INSERT_DEPTH arithmetic that
        # nothing in the suite ties back to params.py -- it already went
        # wrong once, silently, when STANDOFF moved 2.00 -> 3.00 (an 8mm
        # screw would have only reached 2.40mm into the insert). Guard the
        # arithmetic here instead of trusting the README's prose.
        screw_length = 10.0  # README's Lite screw: M2.5 x 10
        seat_z = p.Z_FLOOR_BOTTOM + p.SCREW_HEAD_DEPTH  # under the screw head
        tip_z = seat_z + screw_length
        # The insert bore opens at the board's top face (Z=0, the leg's own
        # free end) and runs up INSERT_DEPTH into the leg.
        engagement = tip_z - 0.0
        self.assertLessEqual(
            tip_z, p.INSERT_DEPTH,
            f"a {screw_length}mm screw's tip reaches {tip_z:.3f}mm past Z=0, "
            f"past the insert's own {p.INSERT_DEPTH}mm depth")
        self.assertGreaterEqual(
            engagement, 3.0,
            f"only {engagement:.3f}mm of engagement in the insert")


if __name__ == "__main__":
    unittest.main()
