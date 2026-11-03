import json, pathlib, unittest
from front_flood import FLOOD_INSET, front_windows

HERE = pathlib.Path(__file__).parent


class TestFrontFlood(unittest.TestCase):
    """The top case's front copper: a window only where light leaves."""

    def setUp(self):
        lay = json.loads((HERE / "panel_layout.json").read_text())
        self.dots, self.g = lay["dots"], lay["grid"]
        self.fr4 = json.loads((HERE / "fr4.polys.json").read_text())
        self.windows = front_windows(self.dots, self.g, self.fr4)

    def circles(self, mat):
        at = {(d["x"], d["y"]) for d in self.dots if d["mat"] == mat}
        return [w for w in self.windows if w["kind"] == "circle" and (w["x"], w["y"]) in at]

    def test_teal_dots_open_exactly_the_dot(self):
        # B and L dots keep mask over the dot, so the copper edge IS the dot's edge
        for mat in ("B", "L"):
            ws = self.circles(mat)
            self.assertEqual(len(ws), sum(d["mat"] == mat for d in self.dots))
            for w in ws:
                self.assertAlmostEqual(w["dia"], 5.6364, places=4)

    def test_bare_fr4_dots_open_a_hair_wider_than_their_mask(self):
        # P dots' mask opening is the dot; copper 0.05 mm wider all round so a
        # misregistered mask never shows a sliver of copper at the rim
        ws = self.circles("P")
        self.assertEqual(len(ws), 14)
        for w in ws:
            self.assertAlmostEqual(w["dia"], 5.6364 + 0.10, places=4)

    def test_gold_and_test_dots_stay_copper(self):
        self.assertEqual(self.circles("G"), [])
        self.assertEqual(self.circles("T"), [])

    def test_the_slash_opens_its_outline_grown_a_little(self):
        slashes = [w for w in self.windows if w["kind"] == "poly"]
        self.assertEqual(len(slashes), len(self.fr4))
        for w, poly in zip(slashes, self.fr4):
            self.assertEqual(w["outline"], poly["outline"])
            self.assertAlmostEqual(w["grow"], 0.05)

    def test_nothing_else_opens(self):
        self.assertEqual(len(self.windows), 37 + len(self.fr4))

    def test_flood_stops_where_the_back_one_does(self):
        self.assertEqual(FLOOD_INSET, 0.55)


if __name__ == "__main__":
    unittest.main()
