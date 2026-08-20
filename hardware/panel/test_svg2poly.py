import unittest, tempfile, pathlib
from svg2poly import flatten

DONUT = """<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
<path fill-rule="evenodd" d="M10 10 H90 V90 H10 Z M40 40 H60 V60 H40 Z"/>
</svg>"""

class TestFlatten(unittest.TestCase):
    def _write(self, text):
        f = tempfile.NamedTemporaryFile(suffix=".svg", delete=False, mode="w")
        f.write(text); f.close()
        return f.name

    def test_donut_becomes_outline_plus_hole(self):
        polys = flatten(self._write(DONUT), target_w_mm=50.0)
        self.assertEqual(len(polys), 1)
        self.assertEqual(len(polys[0]["holes"]), 1)
        xs = [p[0] for p in polys[0]["outline"]]
        self.assertAlmostEqual(min(xs), 5.0, places=2)   # 10/100 * 50mm
        self.assertAlmostEqual(max(xs), 45.0, places=2)

    def test_real_lockup_flattens(self):
        polys = flatten(str(pathlib.Path(__file__).parent / "svg" / "lockup.svg"),
                        target_w_mm=34.0)
        self.assertGreater(len(polys), 5)                 # many glyphs
        for p in polys:
            self.assertGreaterEqual(len(p["outline"]), 3)
