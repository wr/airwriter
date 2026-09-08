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

    def test_back_side_footprints_read_b_cu_through_the_real_parser(self):
        # _FP_LAYER used to be applied with re.match, which anchors at
        # position 0 -- but _blocks() yields blocks that begin with the
        # literal characters "\n\t(footprint ...", so the pattern (which
        # starts with a literal "(footprint") never matched anything, on
        # ANY block, front or back. Every footprint silently fell through to
        # the dataclass default layer="F.Cu", so a genuinely back-side part
        # could never trip connector_solid()'s NotImplementedError guard.
        # This has to go through footprint()/the real board-file parser, not
        # a fixture, or it re-creates the exact blind spot that hid the bug:
        # a previous round "fixed and verified" this by monkeypatching a
        # cached Footprint straight to B.Cu, which bypassed the parser
        # entirely and would have passed on the broken .match() code too.
        for ref in ("TP2", "TP4", "TP5", "TP7", "TP8"):
            self.assertEqual(bg.footprint(ref).layer, "B.Cu", ref)
        self.assertEqual(bg.footprint("J2").layer, "F.Cu")

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


class TestCornerRadius(unittest.TestCase):
    """board_geom.outline() is a bounding box -- it says nothing about the
    four Edge.Cuts arcs that actually round the board's corners. corner_
    radius() derives that radius from the arcs' own start/mid/end points
    rather than assuming the 2.0mm the board happens to use today."""

    def test_reads_2_0_from_the_file_not_a_hardcoded_constant(self):
        # Read directly off the four gr_arc blocks, independent of
        # corner_radius() itself: start (100, 62), mid (100.585786,
        # 60.585786), end (102, 60) is one of the board's real corners
        # (hardware/airwriter/airwriter.kicad_pcb around line 31545). Three
        # points on a circle are enough to recover its radius without
        # knowing the centre.
        r = bg._circumradius((100.0, 62.0), (100.585786, 60.585786), (102.0, 60.0))
        self.assertAlmostEqual(r, 2.0, places=3)
        # And corner_radius() itself must agree, reading the live file.
        self.assertAlmostEqual(bg.corner_radius(), 2.0, places=3)

    def test_reflects_a_different_consistent_radius_not_stuck_at_2_0(self):
        # A hardcoded `return 2.0` would pass the test above for free. Prove
        # corner_radius() actually tracks the file: scale every Edge.Cuts
        # arc's start/mid/end coordinates by 1.5x in a scratch copy (a
        # uniform scale about the origin multiplies every circumradius by
        # the same factor, regardless of where that particular circle's own
        # centre sits), and confirm the derived radius scales with it to
        # ~3.0mm, not stuck at 2.0.
        import re
        import pathlib
        import tempfile

        factor = 1.5
        src = bg.BOARD_PCB.read_text()
        coord = re.compile(r"\((start|mid|end) ([-\d.]+) ([-\d.]+)\)")
        scaled_src = src
        touched = 0
        for blk in bg._blocks(src, "gr_arc"):
            if '"Edge.Cuts"' not in blk:
                continue
            new_blk = coord.sub(
                lambda m: f"({m.group(1)} {float(m.group(2)) * factor:.6f} "
                          f"{float(m.group(3)) * factor:.6f})",
                blk)
            self.assertNotEqual(new_blk, blk)
            self.assertIn(blk, scaled_src)
            scaled_src = scaled_src.replace(blk, new_blk, 1)
            touched += 1
        self.assertEqual(touched, 4, "expected all four corner arcs")

        with tempfile.TemporaryDirectory() as d:
            scratch = pathlib.Path(d) / "airwriter.kicad_pcb"
            scratch.write_text(scaled_src)
            orig_board_pcb = bg.BOARD_PCB
            bg.BOARD_PCB = scratch
            try:
                self.assertAlmostEqual(bg.corner_radius(), 2.0 * factor, places=2)
            finally:
                bg.BOARD_PCB = orig_board_pcb

    def test_raises_on_arcs_with_mismatched_radii(self):
        # A board with mismatched corner radii needs a human, not a silent
        # average. Corrupt ONE of the four arcs' `mid` point in a scratch
        # copy of the real board file so that arc alone traces a ~2.85mm
        # radius instead of the other three's 2.0mm (verified directly:
        # _circumradius((100,62), (101.0,61.5), (102,60)) == 2.850mm), and
        # confirm corner_radius() refuses to return anything for it.
        src = bg.BOARD_PCB.read_text()
        needle = "(mid 100.585786 60.585786)"
        self.assertIn(needle, src, "fixture arc not found in the real board file")
        corrupted = src.replace(needle, "(mid 101.0 61.5)", 1)

        import pathlib
        import tempfile
        with tempfile.TemporaryDirectory() as d:
            scratch = pathlib.Path(d) / "airwriter.kicad_pcb"
            scratch.write_text(corrupted)
            orig_board_pcb = bg.BOARD_PCB
            bg.BOARD_PCB = scratch
            try:
                with self.assertRaises(ValueError):
                    bg.corner_radius()
            finally:
                bg.BOARD_PCB = orig_board_pcb

        # And the unmodified file must still be clean -- proves the raise
        # above came from the corruption, not from some permanent state
        # corner_radius() left behind.
        self.assertAlmostEqual(bg.corner_radius(), 2.0, places=3)


if __name__ == "__main__":
    unittest.main()
