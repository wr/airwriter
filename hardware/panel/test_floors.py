import json, pathlib, unittest
from floors import check_floors

SQUARE = lambda x0, y0, x1, y1: dict(
    outline=[[x0, y0], [x1, y0], [x1, y1], [x0, y1]], holes=[])

class TestFloors(unittest.TestCase):
    def test_warn_band_gap_triggers(self):
        # Two 1x1mm squares 0.05mm apart - inside the 0.02-0.101mm warn band.
        a, b = SQUARE(0, 0, 1, 1), SQUARE(1.05, 0, 2.05, 1)
        warnings = check_floors({"test": [a, b]})
        self.assertTrue(any("gap" in w for w in warnings), warnings)

    def test_sub_ignore_gap_is_silent(self):
        # 0.005mm apart - below the ignore floor, reads as a designed touch.
        a, b = SQUARE(0, 0, 1, 1), SQUARE(1.005, 0, 2.005, 1)
        warnings = check_floors({"test": [a, b]})
        self.assertEqual(warnings, [])

    def test_real_panel_reports_known_subtitle_pinch(self):
        here = pathlib.Path(__file__).parent
        classes = {name: json.loads((here / f"{name}.polys.json").read_text())
                   for name in ("gold", "silk", "fr4")}
        warnings = check_floors(classes)
        pinches = [w for w in warnings if w.startswith("gold pinch")]
        self.assertTrue(pinches, warnings)
        # The known microprint pinch sits near x=6.87, y=83.93 (subtitle glyph
        # counter close to its own outline) - allow a couple mm of slack since
        # the exact sample point depends on svg2poly's curve sampling.
        self.assertTrue(
            any(abs(float(w.split("x ")[1].split(",")[0]) - 6.87) < 2
                and abs(float(w.split("y ")[1]) - 83.93) < 2
                for w in pinches),
            pinches)
