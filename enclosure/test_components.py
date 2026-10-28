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


class TestExport(unittest.TestCase):
    # A real kicad-cli run against the real board (a few seconds each).
    def test_writes_a_step_although_the_sk6812_models_are_missing(self):
        # KiCad 10 ships no SK6812MINI model, so kicad-cli exits 2 on every
        # export; those 37 are covered by NO_MODEL_LIB and must not fail it.
        with tempfile.TemporaryDirectory() as d:
            step = pathlib.Path(d, "c.step")
            self.assertEqual(components.export(force=True, step=step), step)
            self.assertTrue(step.exists())

    def test_a_broken_model_library_fails_and_leaves_no_cache(self):
        with tempfile.TemporaryDirectory() as d:
            step = pathlib.Path(d, "c.step")
            with self.assertRaises(RuntimeError) as e:
                components.export(force=True, step=step, model_dir="/nonexistent")
            self.assertIn("C3", str(e.exception))
            self.assertFalse(step.exists())
            self.assertEqual(list(pathlib.Path(d).iterdir()), [])


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

    def test_d3_stands_on_its_spacer_where_the_board_puts_it(self):
        # Rev 2026.11: a 3 mm LED on a 4 x 8 mm spacer, two models on one
        # footprint. Both count: the spacer at the bottom, the lens near 11.85.
        bands = self.env["D3"]
        f = bg.footprint("D3")
        x, y = bg.to_case(f.x, f.y)
        low = [b for b in bands if b[4] <= 1.0 + 1e-6]   # the bottom slice: spacer and leads
        x0, x1 = min(b[0] for b in low), max(b[1] for b in low)
        y0, y1 = min(b[2] for b in low), max(b[3] for b in low)
        self.assertTrue(x0 < x < x1 and y0 < y < y1)
        self.assertGreater(x1 - x0, 3.9)                 # the 4.0 mm spacer
        self.assertGreater(y1 - y0, 3.9)
        self.assertAlmostEqual(max(b[4] for b in bands), 11.85, delta=0.3)


if __name__ == "__main__":
    unittest.main()
