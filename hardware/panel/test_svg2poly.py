import unittest, tempfile, pathlib
from svg2poly import flatten

DONUT = """<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
<rect width="100" height="100" fill="#dda857"/>
<path fill="#DDA857" fill-rule="evenodd" d="M10 10 H90 V90 H10 Z M40 40 H60 V60 H40 Z"/>
<path fill="white" d="M0 95 H100 V96 H0 Z"/>
</svg>"""

# Plate with a letter-shaped hole containing a counter island (rect-in-rect-in-rect
# donut): a 10-110 plate (with margin, so it isn't itself excluded as a background-
# sized shape), a 30-90 hole cut into it, and a 50-70 island re-solidified inside that
# hole - the shape of a letter like "a"/"e"/"p" reversed out of a silk cell.
DONUT3 = """<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 120 120">
<path fill="#dda857" fill-rule="evenodd"
      d="M10 10 H110 V110 H10 Z M30 30 H90 V90 H30 Z M50 50 H70 V70 H50 Z"/>
</svg>"""

# A <mask> definition's own content (a plain rect, no fill of its own - it
# inherits fill="white" from the <mask> element) must never leak into the
# output as if it were a real painted shape. The masked path (a "rule" band
# straddling the mask rect's top edge by design, like a label cell's rule bar
# against its background rectangle) must come back clipped to the mask rect,
# not at its own full unclipped extent.
MASKED_BAND = """<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 200">
<mask id="m1" fill="white">
<path d="M0 40H100V200H0V40Z"/>
</mask>
<path d="M0 36V44H100V36V32H0V36Z" fill="white" mask="url(#m1)"/>
</svg>"""

def _interior_point(ring):
    # Point guaranteed strictly inside ring's own interior: the centroid of the
    # ear triangle at ring's extreme (lowest-y) vertex, which is always convex.
    # Deliberately not a ring vertex - a sampled vertex can sit exactly on
    # another ring's edge and give a false containment reading.
    i = min(range(len(ring)), key=lambda k: (ring[k][1], ring[k][0]))
    a, b, c = ring[i - 1], ring[i], ring[(i + 1) % len(ring)]
    return ((a[0] + b[0] + c[0]) / 3, (a[1] + b[1] + c[1]) / 3)

def _contains(outer, pt):
    x, y = pt; inside = False
    for (x1, y1), (x2, y2) in zip(outer, outer[1:] + outer[:1]):
        if (y1 > y) != (y2 > y) and x < (x2 - x1) * (y - y1) / (y2 - y1) + x1:
            inside = not inside
    return inside

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
                    self.assertTrue(0 <= x <= 62.0 and 0 <= y <= 104.4)

    def test_counter_island_resolidifies_as_its_own_poly(self):
        polys = flatten(self._write(DONUT3), scale=1.0, fills={"#dda857"})
        self.assertEqual(len(polys), 2)            # plate-with-hole, plus the island
        islands = [p for p in polys if not p["holes"]]
        plates = [p for p in polys if p["holes"]]
        self.assertEqual(len(islands), 1)
        self.assertEqual(len(plates), 1)
        self.assertEqual(len(plates[0]["holes"]), 1)
        xs = [x for x, y in islands[0]["outline"]]
        self.assertAlmostEqual(min(xs), 50.0, delta=1.0)   # the island, not the hole
        self.assertAlmostEqual(max(xs), 70.0, delta=1.0)

    def test_silk_holes_are_not_nested_within_sibling_holes(self):
        # A counter (e.g. the bowl of a/e/p) must come back as its own top-level
        # poly (an island), never as a hole nested inside another hole of the
        # same plate - that nesting is exactly what made counters render as
        # voided space instead of plate-coloured islands.
        svg = str(pathlib.Path(__file__).parent / "svg" / "panel.svg")
        s = 62.0 / 1100
        silk = flatten(svg, scale=s, fills={"white", "#ffffff"})
        for p in silk:
            holes = p["holes"]
            for i, h in enumerate(holes):
                pt = _interior_point(h)
                for j, other in enumerate(holes):
                    if i == j:
                        continue
                    self.assertFalse(_contains(other, pt))

    def test_mask_content_excluded_and_masked_path_clipped_to_rect(self):
        polys = flatten(self._write(MASKED_BAND), scale=1.0, fills={"white", "#ffffff"})
        # Only the clipped band should appear - not the mask's own rect content
        # (which would otherwise leak in as a second, much bigger, solid plate).
        self.assertEqual(len(polys), 1)
        ys = [y for x, y in polys[0]["outline"]]
        self.assertAlmostEqual(min(ys), 40.0, delta=0.5)   # clipped to the mask rect...
        self.assertAlmostEqual(max(ys), 44.0, delta=0.5)   # ...not the band's own 32-44 extent

    def test_silk_has_no_leaked_mask_plates(self):
        # A leaked mask-definition rect (the defect this fix targets) shows up
        # as an oversized solid plate: a full label cell is ~20.3 x 8.9mm =~
        # 180mm^2. The largest legitimate silk shape is a rule, ~20.3 x 0.23mm
        # =~ 4.7mm^2. 30mm^2 is comfortably between the two.
        svg = str(pathlib.Path(__file__).parent / "svg" / "panel.svg")
        s = 62.0 / 1100
        silk = flatten(svg, scale=s, fills={"white", "#ffffff"})
        for p in silk:
            xs = [x for x, y in p["outline"]]
            ys = [y for x, y in p["outline"]]
            bbox_area = (max(xs) - min(xs)) * (max(ys) - min(ys))
            self.assertLess(bbox_area, 30.0)
