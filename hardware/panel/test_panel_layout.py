import unittest
from panel_layout import load_layout

class TestLayout(unittest.TestCase):
    def setUp(self):
        self.lay = load_layout()

    def test_counts(self):
        # Frame 9b's tally. Every one of the 56 cells carries a dot -- there are
        # no empties -- and the 37 LED-backed ones are exactly P + B + L.
        dots = self.lay["dots"]
        self.assertEqual(len(dots), 56)
        mats = [d["mat"] for d in dots]
        for m, n in dict(G=18, T=1, P=14, B=9, L=14).items():
            self.assertEqual(mats.count(m), n, m)
        self.assertEqual(sum(mats.count(m) for m in "GTPBL"), 56)
        self.assertEqual(sum(d["led"] for d in dots), 37)
        self.assertEqual(sum(d["led"] for d in dots if d["mat"] in "PBL"), 37)

    def test_test_dot_and_corners(self):
        by_rc = {(d["r"], d["c"]): d["mat"] for d in self.lay["dots"]}
        self.assertEqual(by_rc[(3, 3)], "T")
        # Both west corners are gold in Frame 9b, which is what lets the main
        # board put a mounting hole under (0,0) and the DIP switch under (7,0).
        self.assertEqual(by_rc[(0, 0)], "G")
        self.assertEqual(by_rc[(7, 0)], "G")
        self.assertEqual(by_rc[(0, 6)], "G")
        self.assertFalse(any(d["led"] for d in self.lay["dots"]
                             if (d["r"], d["c"]) in {(0, 0), (7, 0), (0, 6)}))

    def test_geometry(self):
        g = self.lay["grid"]
        self.assertAlmostEqual(g["pitch"], 150 * 62.0 / 1100, places=3)
        self.assertAlmostEqual(g["dot_dia"], 100 * 62.0 / 1100, places=3)
        self.assertAlmostEqual(g["origin"][0], 100 * 62.0 / 1100, places=3)
        self.assertAlmostEqual(self.lay["panel"]["h"], 1850 * 62.0 / 1100, places=2)
        self.assertAlmostEqual(g["hole_dia"], 3.6, places=2)
