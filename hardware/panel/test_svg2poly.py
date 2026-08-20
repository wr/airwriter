import unittest, tempfile, pathlib
from svg2poly import flatten

DONUT = """<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
<rect width="100" height="100" fill="#dda857"/>
<path fill="#DDA857" fill-rule="evenodd" d="M10 10 H90 V90 H10 Z M40 40 H60 V60 H40 Z"/>
<path fill="white" d="M0 95 H100 V96 H0 Z"/>
</svg>"""

class TestFlatten(unittest.TestCase):
    def _write(self, text):
        f = tempfile.NamedTemporaryFile(suffix=".svg", delete=False, mode="w")
        f.write(text); f.close()
        return f.name

    def test_fill_filter_scale_and_holes(self):
        polys = flatten(self._write(DONUT), scale=0.5, fills={"#dda857"})
        self.assertEqual(len(polys), 1)            # rect excluded, white path excluded
        self.assertEqual(len(polys[0]["holes"]), 1)
        xs = [p[0] for p in polys[0]["outline"]]
        self.assertAlmostEqual(min(xs), 5.0, places=2)   # 10px * 0.5
        self.assertAlmostEqual(max(xs), 45.0, places=2)

    def test_real_panel_classes(self):
        svg = str(pathlib.Path(__file__).parent / "svg" / "panel.svg")
        s = 62.0 / 1100
        gold = flatten(svg, scale=s, fills={"#dda857"})
        silk = flatten(svg, scale=s, fills={"white", "#ffffff"})
        fr4 = flatten(svg, scale=s, fills={"#e7ba9b"})
        self.assertGreaterEqual(len(gold), 3)      # wordmark, subtitle, marks
        self.assertGreaterEqual(len(silk), 4)      # rules, three labels, test tag
        self.assertEqual(len(fr4), 1)              # the slash
        ys = [y for p in gold for x, y in p["outline"]]
        self.assertGreater(min(ys), 67.0)          # gold art sits below the grid
        for polys in (gold, silk, fr4):
            for p in polys:
                for x, y in p["outline"]:
                    self.assertTrue(0 <= x <= 62.0 and 0 <= y <= 101.6)
