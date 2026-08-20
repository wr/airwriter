import unittest
from panel_layout import load_layout

class TestLayout(unittest.TestCase):
    def setUp(self):
        self.lay = load_layout()

    def test_counts(self):
        dots = self.lay["dots"]
        self.assertEqual(len(dots), 50)
        mats = [d["mat"] for d in dots]
        for m, n in dict(G=18, T=1, P=12, B=12, L=7).items():
            self.assertEqual(mats.count(m), n, m)
        self.assertEqual(sum(d["led"] for d in dots), 31)

    def test_test_dot_and_empties(self):
        by_rc = {(d["r"], d["c"]): d["mat"] for d in self.lay["dots"]}
        self.assertEqual(by_rc[(3, 3)], "T")
        for rc in [(0, 5), (1, 4), (4, 2), (5, 6), (6, 4), (7, 2)]:
            self.assertNotIn(rc, by_rc)          # the six empty cells

    def test_geometry(self):
        g = self.lay["grid"]
        self.assertAlmostEqual(g["pitch"], 150 * 62.0 / 1100, places=3)
        self.assertAlmostEqual(g["dot_dia"], 100 * 62.0 / 1100, places=3)
        self.assertAlmostEqual(g["origin"][0], 100 * 62.0 / 1100, places=3)
        self.assertAlmostEqual(self.lay["panel"]["h"], 1801 * 62.0 / 1100, places=2)
        self.assertAlmostEqual(g["hole_dia"], 2.25, places=2)
