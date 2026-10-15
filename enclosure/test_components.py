"""components.py: every part on the board as boxes the lattice keeps clear of."""
import os
import pathlib
import re
import tempfile
import unittest

import board_geom as bg
import components


class TestExportCache(unittest.TestCase):
    def test_stale_when_missing_or_older_than_the_board(self):
        with tempfile.TemporaryDirectory() as d:
            board = pathlib.Path(d, "b.kicad_pcb")
            board.write_text("x")
            step = pathlib.Path(d, "c.step")
            self.assertTrue(components.needs_export(step, board))
            step.write_text("y")
            os.utime(step, (1, 1))
            self.assertTrue(components.needs_export(step, board))
            os.utime(step, None)
            os.utime(board, (1, 1))
            self.assertFalse(components.needs_export(step, board))


class TestEnvelopes(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.env = components.envelopes()

    def test_every_part_has_one_except_holes_and_test_points(self):
        missing = [r for r, f in bg._all().items()
                   if f.layer == "F.Cu" and r not in self.env
                   and not re.fullmatch(r"(H|TP)\d+", r)]
        self.assertEqual(missing, [])

    def test_sw3_is_stepped_body_then_plunger(self):
        bands = self.env["SW3"]
        bottom, top = bands[0], bands[-1]
        self.assertGreater(bottom[1] - bottom[0], 5.5)   # 6 mm body
        self.assertLess(top[1] - top[0], 4.0)            # 3.5 mm plunger
        self.assertAlmostEqual(top[4], 15.0, delta=0.1)

    def test_no_model_parts_and_overrides(self):
        self.assertAlmostEqual(max(b[4] for b in self.env["S1"]), 9.41)
        self.assertAlmostEqual(max(b[4] for b in self.env["U7"]), 9.00)
        self.assertAlmostEqual(max(b[4] for b in self.env["D13"]), 1.60)

    def test_d3_is_where_the_board_puts_it(self):
        (x0, x1, y0, y1, top), = self.env["D3"]
        f = bg.footprint("D3")
        x, y = bg.to_case(f.x, f.y)
        self.assertTrue(x0 < x < x1 and y0 < y < y1)
        self.assertLess(top, 1.2)


if __name__ == "__main__":
    unittest.main()
