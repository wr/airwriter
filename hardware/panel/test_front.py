# SPDX-License-Identifier: CERN-OHL-S-2.0
"""The art is drawn from the front (sockets at the top); front.to_board turns
it into the board's frame (sockets at the bottom) before anything reads it."""
import pathlib, re, tempfile, unittest

import front

HERE = pathlib.Path(__file__).parent
SVG = '<svg width="1100" height="1850" viewBox="0 0 1100 1850" fill="none">{}</svg>'


def nums(s):
    return [float(n) for n in re.findall(front._NUM, s)]


class TestToBoard(unittest.TestCase):
    def test_circle_turns(self):
        out = front.to_board(SVG.format('<circle cx="100" cy="100" r="50" fill="#DDA857"/>'))
        self.assertIn('<circle cx="1000" cy="1750" r="50" fill="#DDA857"/>', out)

    def test_placed_circle_turns_by_its_matrix(self):
        # Figma places a flipped circle with a matrix (lc.svg's small test-tab dot);
        # the turn goes on the outside of it, so cx/cy stay and the matrix turns.
        out = front.to_board(SVG.format(
            '<circle cx="8" cy="8" r="8" transform="matrix(-1 0 0 1 596 580)" fill="#111111"/>'))
        self.assertIn('<circle cx="8" cy="8" r="8" transform="matrix(1 0 0 -1 504 1270)" '
                      'fill="#111111"/>', out)

    def test_path_turns(self):
        out = front.to_board(SVG.format('<path d="M1051.3 1726.58L1060 1800Z" fill="#E7BA9B"/>'))
        self.assertIn('d="M48.7 123.42L40 50Z"', out)
        out = front.to_board(SVG.format('<path d="M10 20H30V40Z"/>'))
        self.assertIn('d="M1090 1830H1070V1810Z"', out)

    def test_arc_keeps_its_flags(self):
        out = front.to_board(SVG.format('<path d="M0 0A5 6 30 1 0 10 20Z"/>'))
        self.assertIn('d="M1100 1850A5 6 30 1 0 1090 1830Z"', out)

    def test_rect_turns(self):
        out = front.to_board(SVG.format('<rect x="0" y="1687" width="360" height="160"/>'))
        self.assertIn('<rect x="740" y="3" width="360" height="160"/>', out)
        out = front.to_board(SVG.format('<rect width="1100" height="1850" rx="20" fill="#111111"/>'))
        self.assertIn('<rect width="1100" height="1850" rx="20" fill="#111111"/>', out)
        out = front.to_board(SVG.format('<rect width="100" height="50"/>'))
        self.assertIn('<rect x="1000" y="1800" width="100" height="50"/>', out)
        # Figma writes x, y, width, height first and leaves out an x or y of 0;
        # so does the turn, so twice is the same text.
        rule = '<rect y="159" width="360" height="4" fill="white"/>'
        self.assertIn('<rect x="740" y="1687" width="360" height="4" fill="white"/>',
                      front.to_board(SVG.format(rule)))
        self.assertIn(rule, front.to_board(front.to_board(SVG.format(rule))))

    def test_twice_is_identity(self):
        for name in ("panel.svg", "lc.svg"):
            s = (HERE / "svg" / name).read_text()
            back = front.to_board(front.to_board(s))
            a, b = nums(s), nums(back)
            self.assertEqual(len(a), len(b), name)
            self.assertTrue(all(abs(x - y) < 1e-6 for x, y in zip(a, b)), name)

    def test_refuses_what_it_cannot_turn(self):
        for bad in ('<path d="M0 0L5 5" transform="rotate(180 550 925)"/>',
                    '<path d="m10 10l5 5"/>',
                    '<line x1="0" y1="0" x2="5" y2="5"/>',
                    '<polygon points="0,0 5,5 0,5"/>',
                    '<image href="x.png"/>',
                    '<linearGradient id="g"/>',
                    '<ellipse cx="5" cy="5" rx="2" ry="3"/>'):
            with self.assertRaises(ValueError, msg=bad):
                front.to_board(SVG.format(bad))
        with self.assertRaises(ValueError):
            front.to_board('<svg viewBox="0 0 100 100"><circle cx="1" cy="1" r="1"/></svg>')


class TestArtDrawnTheOldWay(unittest.TestCase):
    """An export with the sockets at the bottom, as the art was drawn before,
    must stop the build, not come out a row off."""

    def old_way(self, name):
        d = tempfile.mkdtemp()
        p = pathlib.Path(d) / name
        p.write_text(front.board_art(HERE / "svg" / name))
        return p

    def test_panel_layout_refuses_it(self):
        import panel_layout
        with self.assertRaisesRegex(AssertionError, "drawn from the front"):
            panel_layout.load_layout(self.old_way("panel.svg"))

    def test_lc_top_refuses_it(self):
        import lc_top
        with self.assertRaisesRegex(AssertionError, "drawn from the front"):
            lc_top.engraved(art=self.old_way("lc.svg"))


# The label sections along the socket edge, in the board's frame (mm): Figma's
# three rule cells, 0-360, 370-730 and 740-1100 px.
THIRDS = {"serial": (0.0, 20.3), "appletalk": (20.9, 41.1), "power": (41.7, 62.0)}
LABEL_TOP = 96.0        # letters start below this; the rules sit at 95.08


def third_of(x0, x1):
    return next((k for k, (a, b) in THIRDS.items() if a <= x0 and x1 <= b), None)


def label_counts(boxes):
    """Shapes in the label band (bbox x0, y0, x1, y1 in mm), counted per socket's third."""
    counts = {}
    for x0, y0, x1, y1 in boxes:
        if y0 >= LABEL_TOP:
            k = third_of(x0, x1)
            counts[k] = counts.get(k, 0) + 1
    return counts


def overlaps(a, b):
    return a[0] <= b[2] and b[0] <= a[2] and a[1] <= b[3] and b[1] <= a[3]


class TestTheTurnedArt(unittest.TestCase):
    """What the generated files must show once the art is drawn from the front."""

    def test_labels_over_their_sockets(self):
        # Each word has its own number of shapes in the board's silk (serial
        # 6 letters and the i's dot, appletalk 9, power 5), so a label over the
        # wrong socket shows as the wrong count in that socket's third.
        import json, lc_top
        silk = json.loads((HERE / "silk.polys.json").read_text())
        (slash,) = json.loads((HERE / "fr4.polys.json").read_text())
        boxes = [(min(x for x, _ in p["outline"]), min(y for _, y in p["outline"]),
                  max(x for x, _ in p["outline"]), max(y for _, y in p["outline"])) for p in silk]
        self.assertEqual(label_counts(boxes), {"serial": 7, "appletalk": 9, "power": 5})
        # Swapped round (power over the printer port, serial over the power
        # light), the same shapes don't pass.
        swapped = [(62.0 - x1, y0, 62.0 - x0, y1) for x0, y0, x1, y1 in boxes]
        self.assertNotEqual(label_counts(swapped), {"serial": 7, "appletalk": 9, "power": 5})
        # Power's letters stay clear of the power light's slash.
        sb = (min(x for x, _ in slash["outline"]), min(y for _, y in slash["outline"]),
              max(x for x, _ in slash["outline"]), max(y for _, y in slash["outline"]))
        self.assertFalse(any(overlaps(b, sb) for b in boxes if b[1] >= LABEL_TOP))
        # The LC's top: serial and power, no appletalk; its frosted slash isn't a letter.
        (lc_slash,) = json.loads((HERE / "lc_slash.polys.json").read_text())
        lb = (min(x for x, _ in lc_slash["outline"]), min(y for _, y in lc_slash["outline"]),
              max(x for x, _ in lc_slash["outline"]), max(y for _, y in lc_slash["outline"]))
        engraved = [lc_top.bbox(lc_top.parse_path(d)) for d in
                    re.findall(r'<path d="([^"]+)"', (HERE / "airwriter-lc-top.svg").read_text())]
        self.assertEqual(label_counts([b for b in engraved if not overlaps(b, lb)]),
                         {"serial": 6, "power": 5})

    def test_rules_under_the_labels(self):
        # Three rules, 360 x 4 px, at the top of the label band in the board's
        # frame (y 1687 px), on the panel and engraved on the LC's top.
        import json, lc_top
        mm = 62.0 / 1100
        want = sorted((round(x * mm, 2), round(1687 * mm, 2)) for x in (0, 370, 740))
        silk = json.loads((HERE / "silk.polys.json").read_text())
        rules = sorted((round(min(x for x, _ in p["outline"]), 2), round(min(y for _, y in p["outline"]), 2))
                       for p in silk
                       if abs(max(x for x, _ in p["outline"]) - min(x for x, _ in p["outline"]) - 360 * mm) < 0.01
                       and abs(max(y for _, y in p["outline"]) - min(y for _, y in p["outline"]) - 4 * mm) < 0.01)
        self.assertEqual(rules, want)
        engraved = [lc_top.bbox(lc_top.parse_path(d)) for d in
                    re.findall(r'<path d="([^"]+)"', (HERE / "airwriter-lc-top.svg").read_text())]
        rules = sorted((round(x0, 2), round(y0, 2)) for x0, y0, x1, y1 in engraved
                       if abs(x1 - x0 - 360 * mm) < 0.01 and abs(y1 - y0 - 4 * mm) < 0.01)
        self.assertEqual(rules, want)

    def test_test_tab_keeps_its_dot_at_its_lower_right(self):
        # The tab's small dot sits in the tab's square corner, inside the test
        # dot's bounding square; turned back to read from the front, that
        # corner is at its lower right as seen from there, which is up and to
        # the left in the board's frame.
        import json
        lay = json.loads((HERE / "panel_layout.json").read_text())
        (t,) = [d for d in lay["dots"] if d["mat"] == "T"]
        r = lay["grid"]["dot_dia"] / 2
        silk = json.loads((HERE / "silk.polys.json").read_text())
        inside = [p for p in silk if all(abs(x - t["x"]) <= r and abs(y - t["y"]) <= r
                                         for x, y in p["outline"])]
        self.assertEqual(len(inside), 1, "the tab's one small dot")
        xs = [x for x, _ in inside[0]["outline"]]
        ys = [y for _, y in inside[0]["outline"]]
        self.assertLess(max(xs), t["x"])
        self.assertLess(max(ys), t["y"])


if __name__ == "__main__":
    unittest.main()
