# SPDX-License-Identifier: CERN-OHL-S-2.0
import json, pathlib, re, unittest
import lite_top

HERE = pathlib.Path(__file__).parent


class TestLiteTop(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.svg = lite_top.build()
        cls.lay = json.loads((HERE / "panel_layout.json").read_text())
        cls.engrave = re.search(r'<g id="engrave"[^>]*>(.*?)</g>', cls.svg, re.S).group(1)
        cls.cut = re.search(r'<g id="cut"[^>]*>(.*?)</g>', cls.svg, re.S).group(1)

    def test_committed_file_is_current(self):
        # The laser file is committed so it can be cut without running
        # anything; a re-export of the art must regenerate it.
        self.assertEqual((HERE / "airwriter-lite-top.svg").read_text(), self.svg,
                         "run: python3 hardware/panel/lite_top.py")

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

    def test_only_the_gold_dots_are_frosted(self):
        r = self.lay["grid"]["dot_dia"] / 2
        starts = {(round(float(x) + r, 2), round(float(y), 2)) for x, y in
                  re.findall(r'<path d="M([\d.]+) ([\d.]+)A', self.engrave)}
        for d in self.lay["dots"]:
            frosted = (round(d["x"], 2), round(d["y"], 2)) in starts
            self.assertEqual(frosted, d["mat"] == "G", f"dot r{d['r']}c{d['c']} ({d['mat']})")

    def test_weld_joins_crossing_shapes_and_leaves_the_rest(self):
        sq = lambda x, y, w: [[("M", [x, y]), ("H", [x + w]), ("V", [y + w]), ("H", [x]), ("Z", [])]]
        out = lite_top.weld([sq(0, 0, 10), sq(5, 5, 10), sq(30, 0, 10)])
        self.assertEqual(len(out), 2)
        ring = lite_top.flatten(out[0][0])
        self.assertAlmostEqual(abs(lite_top._area(ring)), 175.0, places=6)


if __name__ == "__main__":
    unittest.main()
