"""The board is the source of truth. These values were read out of
hardware/airwriter/airwriter.kicad_pcb on 2026-09-08; if the board moves,
this test is supposed to fail."""
import unittest

import board_geom as bg


class TestOutline(unittest.TestCase):
    def test_outline_is_62_by_104_27(self):
        min_x, min_y, max_x, max_y = bg.outline()
        self.assertAlmostEqual(min_x, 100.000, places=3)
        self.assertAlmostEqual(min_y, 60.000, places=3)
        self.assertAlmostEqual(max_x, 162.000, places=3)
        self.assertAlmostEqual(max_y, 164.270, places=3)

    def test_case_frame_flips_y_and_is_right_handed(self):
        # KiCad top-left corner -> case (0, 104.27); KiCad bottom-left -> case (0, 0).
        self.assertEqual(bg.to_case(100.0, 60.0), (0.0, 104.27))
        self.assertEqual(bg.to_case(100.0, 164.27), (0.0, 0.0))


class TestFootprints(unittest.TestCase):
    def test_known_positions(self):
        for ref, kx, ky, rot in [
            ("J2", 113.000, 167.020, 180.0),
            ("J4", 132.500, 167.020, 180.0),
            ("J1", 152.000, 162.120, 0.0),
            ("S1", 105.005, 134.320, 90.0),
            ("SW1", 157.225, 99.454, 0.0),
            ("SW3", 133.250, 87.750, -90.0),
        ]:
            fp = bg.footprint(ref)
            self.assertAlmostEqual(fp.x, kx, places=3, msg=ref)
            self.assertAlmostEqual(fp.y, ky, places=3, msg=ref)
            self.assertAlmostEqual(fp.rot % 360, rot % 360, places=3, msg=ref)

    def test_minidin_carries_a_step_model(self):
        fp = bg.footprint("J2")
        self.assertIsNotNone(fp.model)
        self.assertTrue(fp.model.endswith(".step"), fp.model)

    def test_s1_key_tips_reach_x_97_80(self):
        # S1's footprint origin is the centre of the pin field (pads at +/-8.89,
        # +/-3.81). Body 9.61 deep centred on the rows -> front face at local
        # y = -4.805; keys protrude 2.40 past it -> local y = -7.205. At rot 90
        # that maps to KiCad x = 105.005 - 7.205. HANDOFF records "x=97.8".
        fp = bg.footprint("S1")
        x, _ = bg.local_to_board(fp, 0.0, -7.205)
        self.assertAlmostEqual(x, 97.800, places=3)

    def test_mount_holes_in_case_frame(self):
        holes = bg.mount_holes()
        self.assertEqual(set(holes), {"H1", "H2", "H3", "H4"})
        for ref, cx, cy in [
            ("H1", 5.636, 98.634),
            ("H2", 56.364, 98.634),
            ("H3", 5.636, 12.340),
            ("H4", 53.364, 12.340),
        ]:
            gx, gy = holes[ref]
            self.assertAlmostEqual(gx, cx, places=3, msg=ref)
            self.assertAlmostEqual(gy, cy, places=3, msg=ref)

    def test_mount_pattern_is_asymmetric_at_h4(self):
        # The tray keys off this: H4 is 3.0mm west of where a symmetric
        # pattern would put it, so the board only goes in one way.
        holes = bg.mount_holes()
        self.assertNotAlmostEqual(holes["H4"][0], holes["H2"][0], places=2)

    def test_model_transform_defaults_to_identity(self):
        # SW3 has a (model ...) block with an explicit but non-identity scale
        # and identity offset/rotate: offset (0,0,0), rotate (0,0,0),
        # scale (1,1,1.153846154).
        fp = bg.footprint("SW3")
        self.assertEqual(fp.model_offset, (0.0, 0.0, 0.0))
        self.assertEqual(fp.model_rotate, (0.0, 0.0, 0.0))
        self.assertAlmostEqual(fp.model_scale[0], 1.0, places=6)
        self.assertAlmostEqual(fp.model_scale[1], 1.0, places=6)
        self.assertAlmostEqual(fp.model_scale[2], 1.153846154, places=6)

    def test_model_transform_j2_and_j1(self):
        # Read directly off the board file's (model ...) sub-blocks for the
        # two MiniDIN/USB-C connectors, per the Task 1 amendment.
        j2 = bg.footprint("J2")
        self.assertEqual(j2.model_offset, (0.0, 0.0, 0.0))
        self.assertEqual(j2.model_rotate, (-90.0, 0.0, 180.0))
        self.assertEqual(j2.model_scale, (1.0, 1.0, 1.0))

        j1 = bg.footprint("J1")
        self.assertEqual(j1.model_offset, (-4.47, -3.65, 0.0))
        self.assertEqual(j1.model_rotate, (-90.0, 0.0, 0.0))
        self.assertEqual(j1.model_scale, (1.0, 1.0, 1.0))


if __name__ == "__main__":
    unittest.main()
