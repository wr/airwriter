import unittest
from panel_layout import load_layout

class TestLayout(unittest.TestCase):
    def setUp(self):
        self.lay = load_layout()

    def test_counts(self):
        dots = self.lay["dots"]
        self.assertEqual(len(dots), 50)                      # 56 cells - 6 empties
        mats = [d["mat"] for d in dots]
        self.assertEqual(mats.count("G"), 18)                # 19 golds minus the T
        self.assertEqual(mats.count("T"), 1)
        self.assertEqual(sum(d["led"] for d in dots), 31)    # P+B+L

    def test_geometry(self):
        g = self.lay["grid"]
        self.assertAlmostEqual(g["pitch"], (62.0 - 2*3.2) / 7, places=4)
        self.assertAlmostEqual(g["dot_dia"], g["pitch"] * 0.66, places=4)
        d00 = next(d for d in self.lay["dots"] if (d["r"], d["c"]) == (0, 0))
        self.assertAlmostEqual(d00["x"], 3.2 + g["pitch"]/2, places=4)
        self.assertAlmostEqual(d00["y"], 2.8 + g["pitch"]/2, places=4)

    def test_grid_fits_panel(self):
        g = self.lay["grid"]
        self.assertLessEqual(2.8 + 8*g["pitch"], 100.0)      # grid inside panel height
