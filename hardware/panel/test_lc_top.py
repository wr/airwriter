# SPDX-License-Identifier: CERN-OHL-S-2.0
import json, pathlib, re, unittest
import lc_top

HERE = pathlib.Path(__file__).parent


class TestLCTop(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.svg = lc_top.build()
        cls.lay = json.loads((HERE / "panel_layout.json").read_text())
        cls.engrave = re.search(r'<g id="engrave"[^>]*>(.*?)</g>', cls.svg, re.S).group(1)
        cls.cut = re.search(r'<g id="cut"[^>]*>(.*?)</g>', cls.svg, re.S).group(1)

    def test_committed_file_is_current(self):
        # The laser file is committed so it can be cut without running
        # anything; a re-export of the art must regenerate it.
        self.assertEqual((HERE / "airwriter-lc-top.svg").read_text(), self.svg,
                         "run: python3 hardware/panel/lc_top.py")
        self.assertEqual(json.loads(lc_top.SLASH.read_text()), lc_top.slash(),
                         "run: python3 hardware/panel/lc_top.py")

    def test_cut_is_the_panel_outline_and_the_test_hole(self):
        p, g = self.lay["panel"], self.lay["grid"]
        rect = re.search(r"<rect ([^>]*)/>", self.cut).group(1)
        self.assertIn(f'width="{p["w"]:g}" height="{p["h"]:g}"', rect)
        self.assertIn(f'rx="{p["corner_r"]:g}"', rect)
        t = next(d for d in self.lay["dots"] if d["mat"] == "T")
        cx, cy, r = (float(v) for v in re.search(
            r'<circle cx="([\d.]+)" cy="([\d.]+)" r="([\d.]+)"', self.cut).groups())
        self.assertAlmostEqual(cx, t["x"], places=3)
        self.assertAlmostEqual(cy, t["y"], places=3)
        self.assertAlmostEqual(2 * r, g["hole_dia"], places=3)
        self.assertEqual(self.cut.count("<"), 2, "the cut layer is the outline and one hole")

    def test_only_the_arts_gold_dots_are_frosted(self):
        # The LC's art draws its 18 gold dots in gold, and the Pro's 37 lit
        # ones in the background's #111111, which stay clear, but for D26's:
        # the LC's one light (the dot under the test dot) is gold, frosted.
        r = self.lay["grid"]["dot_dia"] / 2
        starts = {(round(float(x) + r, 2), round(float(y), 2)) for x, y in
                  re.findall(r'<path d="M([\d.]+) ([\d.]+)A', self.engrave)}
        circles, _ = lc_top.read_art(lc_top.ART.read_text())
        s = self.lay["panel"]["w"] / 1100
        dots = [c for c in circles if abs(c["r"] * s - r) < 0.01]
        self.assertEqual(len(dots), 56)
        for c in dots:
            at = (round(c["cx"] * s, 2), round(c["cy"] * s, 2))
            is_test = at == (31.0, 31.0)
            self.assertEqual(at in starts, c["fill"] == lc_top.GOLD and not is_test,
                             f"dot at {at} ({c['fill']})")
        gold = sum(c["fill"] == lc_top.GOLD for c in dots)
        self.assertEqual(gold, 20, "18 gold dots, the test dot and D26's")
        lit = [d for d in self.lay["dots"] if d["led"] and (round(d["x"], 2), round(d["y"], 2)) in starts]
        self.assertEqual([(d["r"], d["c"]) for d in lit], [(4, 3)], "D26's dot, and no other lit one")

    def test_slash_and_labels_sit_where_the_pros_do(self):
        # The power LED is under the Pro's slash (fr4.polys.json), so the
        # LC's frosted slash must cover its middle, and the labels must sit
        # as close to the front edge as the Pro's.
        (fr4,) = json.loads((HERE / "fr4.polys.json").read_text())
        xs, ys = zip(*fr4["outline"])
        led = ((min(xs) + max(xs)) / 2, (min(ys) + max(ys)) / 2)
        boxes = [lc_top.bbox(lc_top.parse_path(d))
                 for d in re.findall(r'<path d="([^"]+)"', self.engrave)]
        (slash,) = [b for b in boxes if b[0] <= led[0] <= b[2] and b[1] <= led[1] <= b[3]]
        self.assertGreater(slash[3] - slash[1], 4.0, "the shape over the LED is the slash")
        lowest = max(b[3] for b in boxes if b[2] < 55)
        self.assertGreater(lowest, self.lay["panel"]["h"] - 2.0)

    def test_weld_joins_crossing_shapes_and_leaves_the_rest(self):
        sq = lambda x, y, w: [[("M", [x, y]), ("H", [x + w]), ("V", [y + w]), ("H", [x]), ("Z", [])]]
        out = lc_top.weld([sq(0, 0, 10), sq(5, 5, 10), sq(30, 0, 10)])
        self.assertEqual(len(out), 2)
        ring = lc_top.flatten(out[0][0])
        self.assertAlmostEqual(abs(lc_top._area(ring)), 175.0, places=6)


if __name__ == "__main__":
    unittest.main()
