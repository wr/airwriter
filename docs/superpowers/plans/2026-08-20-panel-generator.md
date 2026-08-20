# Panel Generator Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build `hardware/panel/` — a generator that turns Wells's single full-panel Figma SVG export into a DRC-clean, fab-ready art-panel `.kicad_pcb` (62 × 101.51 mm, matte black, ENIG, zero components).

**Architecture:** One source of truth — `hardware/panel/svg/panel.svg` (Wells's "Frame 7" export). Fill colour encodes material; coordinates are the layout. Two stages with a JSON boundary. Stage A runs on **system python3**: parse the `<circle>` grid into `panel_layout.json`, and flatten fill-filtered `<path>` art into `*.polys.json` — all in absolute panel millimetres. Stage B runs on **KiCad's bundled python** (`pcbnew`): read those JSONs and emit `panel.kicad_pcb`. No derived layout math anywhere: Wells retunes in Figma, re-exports, regenerates.

**Tech Stack:** python3 (stdlib `unittest`), `svgelements` (already importable on this machine), `pcbnew` via `/Applications/KiCad/KiCad.app/Contents/Frameworks/Python.framework/Versions/3.9/bin/python3`, `kicad-cli` at `/Applications/KiCad/KiCad.app/Contents/MacOS/kicad-cli`.

**Spec:** `docs/superpowers/specs/2026-08-20-dot-panel-cover-design.md` (amended 2026-08-20 by Wells's full-panel export: exact proportions supersede the spec's working values).

## Global Constraints

- Source: `hardware/panel/svg/panel.svg`, viewBox `0 0 1100 1801`. **SCALE = 62.0/1100 mm per px** (px × SCALE = mm). Panel = 62.0 × 101.51 mm, 1.6 mm FR4, 2-layer, matte black mask, ENIG, corner radius 2.0 mm (radius is a fab/mechanical add, not in the SVG), **no components, no nets**.
- Measured geometry (assert, don't assume): grid 7 cols × 8 rows of `<circle>`; centres at px (100 + 150·c, 100 + 150·r) → first centre (5.6364, 5.6364) mm, pitch 8.4545 mm, dot Ø 5.6364 mm (r=50 px). Test hole: the one black circle r=20 px → Ø 2.2545 mm ≈ **2.25 mm NPTH** at grid r3 c3.
- Material encoding (case-insensitive fill compare):

| fill | meaning | geometry emitted |
|---|---|---|
| `#DDA857` circle | G gold dot | F.Cu disc + F.Mask opening |
| `#DDA857` circle under the black r=20 circle | T test dot | same as G + 2.25 mm NPTH at centre |
| `#E7BA9B` circle | P bare FR4 dot | F.Mask + B.Mask openings |
| `#849BFF` circle | B single-mask dot | B.Mask opening only |
| `#A4ACCE` circle | L double-mask dot | nothing (LED only) |
| `#111111` circle | empty cell | nothing at all |
| `#DDA857` path | gold art (lockup, subtitle, marks) | F.Cu poly + F.Mask poly |
| `#E7BA9B` path | bare-FR4 art (the slash) | F.Mask + B.Mask polys |
| `white`/`#FFFFFF` path | silk (rules, labels, `test` tag) | F.SilkS poly |
| any rect, any full-panel shape | background | ignored |

- `led` is true exactly for P/B/L. True tallies of the export: 50 dots = 18 G + 1 T + 12 P + 12 B + 7 L, plus 6 empties; 31 LEDs.
- Fab floors (generator asserts what it can; DRC catches the rest): drill ≥ 0.30 mm, copper clearance ≥ 0.15 mm, silk line ≥ 0.153 mm / text ≥ 0.8 mm, mask sliver ≥ 0.10 mm.
- F is the viewer face; LEDs sit behind B. The white `test` tag overlaps the gold dot's lower edge in the design; fabs clip silk over mask openings — emit as drawn, judge legibility in the Task 4 render.
- Regeneration is the only edit path for `panel.kicad_pcb`; same inputs → byte-identical output (KiCad timestamps aside).
- `pcbnew` units are **nanometres** — always `pcbnew.FromMM()` / `pcbnew.ToMM()`.
- Do not run stage B while the panel board is open in KiCad (stale-write trap).
- kicad-cli is not on PATH (path above); check `kicad-cli <sub> --help` before first use of any subcommand.

---

### Task 1: Grid parse and layout (`panel_layout.py`)

**Files:**
- Create: `hardware/panel/panel_layout.py`
- Create (commit): `hardware/panel/svg/panel.svg` (already staged at that path by the controller)
- Generated: `hardware/panel/panel_layout.json`, `hardware/panel/dotmap.txt` (derived, human-readable)
- Test: `hardware/panel/test_panel_layout.py`
- Note: `hardware/panel/` already contains committed artifacts of this task's earlier revision (`dotmap.txt`, `panel_layout.py`, `test_panel_layout.py`, `panel_layout.json`) built from a hand transcription that the SVG export supersedes. Replace their contents; keep the filenames.

**Interfaces:**
- Produces: `panel_layout.json` with shape
  `{"panel": {"w": 62.0, "h": 101.51, "corner_r": 2.0}, "grid": {"cols": 7, "rows": 8, "pitch": 8.4545, "dot_dia": 5.6364, "origin": [5.6364, 5.6364], "hole_dia": 2.25}, "dots": [{"r": 0, "c": 0, "mat": "P", "x": 5.6364, "y": 5.6364, "led": true}, ...]}`
  — x, y in mm from panel top-left; `led` true exactly for P/B/L. The main-board plan later consumes this file for LED placement.
- Produces: `load_layout() -> dict` importable by tests; `python3 panel_layout.py` writes both generated files.

- [ ] **Step 1: Write the failing tests**

```python
# hardware/panel/test_panel_layout.py
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
```

- [ ] **Step 2: Run tests, verify they fail**

Run: `cd hardware/panel && python3 -m unittest test_panel_layout -v`
Expected: FAIL — the committed earlier-revision module still parses `dotmap.txt` with the old geometry, so every test fails (wrong counts positions/pitch). If instead you see import errors, that's an acceptable failure mode too — the point is a red run before the rewrite.

- [ ] **Step 3: Rewrite `panel_layout.py`**

```python
#!/usr/bin/env python3
"""svg/panel.svg circles -> panel_layout.json (+ dotmap.txt, derived)."""
import json, pathlib, re

VIEW_W_PX = 1100.0
SCALE = 62.0 / VIEW_W_PX          # mm per px
CFG = dict(w=62.0, corner_r=2.0)
MAT_BY_FILL = {"#dda857": "G", "#e7ba9b": "P", "#849bff": "B", "#a4acce": "L",
               "#111111": "."}
LED_MATS = set("PBL")
GRID0_PX, PITCH_PX = 100.0, 150.0

def _circles(svg_text):
    for tag in re.findall(r"<circle[^>]*>", svg_text):
        def attr(name):
            m = re.search(rf'\b{name}="([^"]+)"', tag)
            return m.group(1) if m else None
        yield float(attr("cx")), float(attr("cy")), float(attr("r")), attr("fill").lower()

def load_layout():
    here = pathlib.Path(__file__).parent
    svg = (here / "svg" / "panel.svg").read_text()
    m = re.search(r'viewBox="0 0 (\d+) (\d+)"', svg)
    assert m and float(m.group(1)) == VIEW_W_PX, "unexpected viewBox"
    panel_h = round(float(m.group(2)) * SCALE, 2)

    cells, hole = {}, None
    for cx, cy, r, fill in _circles(svg):
        col = (cx - GRID0_PX) / PITCH_PX
        row = (cy - GRID0_PX) / PITCH_PX
        assert abs(col - round(col)) < 0.01 and abs(row - round(row)) < 0.01, \
            f"circle off-grid at ({cx},{cy})"
        rc = (int(round(row)), int(round(col)))
        if fill == "black" and r < 30:
            hole = (rc, round(2 * r * SCALE, 2)); continue
        cells[rc] = MAT_BY_FILL[fill]
    assert hole, "no test hole circle found"
    assert cells[hole[0]] == "G", "test hole must sit on a gold dot"
    cells[hole[0]] = "T"

    rows = 1 + max(rc[0] for rc in cells)
    cols = 1 + max(rc[1] for rc in cells)
    assert (rows, cols) == (8, 7) and len(cells) == 56, "grid shape drifted"
    dots = [dict(r=rc[0], c=rc[1], mat=mat,
                 x=round((GRID0_PX + rc[1] * PITCH_PX) * SCALE, 4),
                 y=round((GRID0_PX + rc[0] * PITCH_PX) * SCALE, 4),
                 led=mat in LED_MATS)
            for rc, mat in sorted(cells.items()) if mat != "."]
    return dict(panel=dict(w=CFG["w"], h=panel_h, corner_r=CFG["corner_r"]),
                grid=dict(cols=cols, rows=rows,
                          pitch=round(PITCH_PX * SCALE, 4),
                          dot_dia=round(100.0 * SCALE, 4),
                          origin=[round(GRID0_PX * SCALE, 4)] * 2,
                          hole_dia=hole[1]),
                dots=dots, _cells={f"{r},{c}": m for (r, c), m in sorted(cells.items())})

if __name__ == "__main__":
    here = pathlib.Path(__file__).parent
    lay = load_layout()
    cells = lay.pop("_cells")
    (here / "panel_layout.json").write_text(json.dumps(lay, indent=1) + "\n")
    grid = [["?"] * lay["grid"]["cols"] for _ in range(lay["grid"]["rows"])]
    for key, m in cells.items():
        r, c = map(int, key.split(","))
        grid[r][c] = m
    (here / "dotmap.txt").write_text(
        "# generated from svg/panel.svg - do not edit\n"
        + "\n".join("".join(r) for r in grid) + "\n")
    print("wrote panel_layout.json and dotmap.txt")
```

(`load_layout()` returns `_cells` for the dotmap writer; the JSON file must NOT
contain it — the `pop` handles that. Tests read only the public keys.)

- [ ] **Step 4: Run tests, verify they pass**

Run: `cd hardware/panel && python3 -m unittest test_panel_layout -v`
Expected: PASS (3 tests). If a count assertion fails, the SVG is the truth — re-derive the expected constant by reading the SVG circles yourself before touching any test number.

- [ ] **Step 5: Regenerate outputs and commit**

```bash
cd hardware/panel && python3 panel_layout.py
git add hardware/panel/svg/panel.svg hardware/panel/dotmap.txt \
        hardware/panel/panel_layout.py hardware/panel/test_panel_layout.py \
        hardware/panel/panel_layout.json
git commit -m "Panel layout: parse the Figma export directly

Wells's full-panel SVG supersedes the hand transcription: fill colour
is the material, coordinates are the layout. Grid pitch 8.45mm, dots
5.64mm, test dot at r3c3 with a 2.25mm hole, panel 62x101.51.

Refs: W-573"
```

---

### Task 2: SVG art flattener (`svg2poly.py`)

**Files:**
- Create: `hardware/panel/svg2poly.py`
- Generated: `hardware/panel/gold.polys.json`, `hardware/panel/silk.polys.json`, `hardware/panel/fr4.polys.json`
- Test: `hardware/panel/test_svg2poly.py`
- Note: delete `hardware/panel/svg/lockup.svg` and `hardware/panel/svg/labels.svg` if present (staged for an earlier revision, unused and uncommitted).

**Interfaces:**
- Consumes: `svg/panel.svg` only.
- Produces: `flatten(svg_path, scale, fills) -> list[dict]`, each dict
  `{"outline": [[x,y],...], "holes": [[[x,y],...], ...]}` in **absolute panel mm**
  (px × scale, y-down), including only `<path>` elements whose fill (lowercased)
  is in `fills`; rects, circles, and any shape whose bbox covers ≥ 90% of the
  viewBox are excluded. CLI:
  `python3 svg2poly.py svg/panel.svg <scale> <fills-comma-separated> out.json`.
  Task 4 reads the three JSON files verbatim, no offsets.

- [ ] **Step 1: Confirm the dependency**

`python3 -c "import svgelements; print(svgelements.__version__)"` — it is already
importable on this machine. If it genuinely is not, install with
`python3 -m pip install --user --break-system-packages svgelements` (this Mac's
python is PEP-668 managed).

- [ ] **Step 2: Write the failing tests**

```python
# hardware/panel/test_svg2poly.py
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
```

- [ ] **Step 3: Run tests, verify they fail**

Run: `cd hardware/panel && python3 -m unittest test_svg2poly -v`
Expected: FAIL — no module `svg2poly`

- [ ] **Step 4: Implement `svg2poly.py`**

```python
#!/usr/bin/env python3
"""Flatten fill-filtered SVG <path> art to polygons in absolute mm."""
import json, sys
from svgelements import SVG, Path

TOL = 0.05  # mm sampling tolerance

def _subpaths_to_points(path, scale):
    for sub in path.as_subpaths():
        seg = Path(sub)
        if seg.length() == 0:
            continue
        n = max(8, int(seg.length() * scale / TOL / 4))
        pts = [seg.point(i / n) for i in range(n)]
        yield [[round(p.real * scale, 4), round(p.imag * scale, 4)] for p in pts]

def _area(poly):
    s = 0.0
    for (x1, y1), (x2, y2) in zip(poly, poly[1:] + poly[:1]):
        s += x1 * y2 - x2 * y1
    return s / 2

def _contains(outer, pt):
    x, y = pt; inside = False
    for (x1, y1), (x2, y2) in zip(outer, outer[1:] + outer[:1]):
        if (y1 > y) != (y2 > y) and x < (x2 - x1) * (y - y1) / (y2 - y1) + x1:
            inside = not inside
    return inside

def flatten(svg_path, scale, fills):
    fills = {f.lower() for f in fills}
    svg = SVG.parse(svg_path, reify=True)
    vb = svg.viewbox
    rings = []
    for el in svg.elements():
        if type(el).__name__ != "Path":
            continue
        fill = str(getattr(el, "fill", "")).lower()
        if fill not in fills:
            continue
        bb = el.bbox()
        if bb and vb and (bb[2]-bb[0]) * (bb[3]-bb[1]) >= 0.9 * vb.width * vb.height:
            continue                       # background-sized shape
        rings.extend(_subpaths_to_points(Path(el), scale))
    rings.sort(key=lambda r: abs(_area(r)), reverse=True)
    polys = []
    for ring in rings:
        for poly in polys:
            if _contains(poly["outline"], ring[0]):
                poly["holes"].append(ring)
                break
        else:
            polys.append(dict(outline=ring, holes=[]))
    return polys

if __name__ == "__main__":
    inp, scale, fills, outp = sys.argv[1], float(sys.argv[2]), sys.argv[3], sys.argv[4]
    with open(outp, "w") as f:
        json.dump(flatten(inp, scale, set(fills.split(","))), f)
    print("wrote", outp)
```

Two caveats the tests may expose. (1) `svgelements` may report Figma paths as a
subclass — if the `type(el).__name__` check drops everything, loosen it to
`isinstance(el, Path)` plus excluding `Rect`/`Circle`/`SimpleLine` imports.
(2) svgelements normalizes fills to `#rrggbb` lowercase or named colors — if the
white filter misses, print the distinct fills it saw and match on those. Nested
hole-in-hole would misclassify — no glyph here has one; if the real-panel test
fails oddly, render a debug PNG of the polygons with PIL and look before
changing the algorithm.

- [ ] **Step 5: Run tests, verify they pass**

Run: `cd hardware/panel && python3 -m unittest test_svg2poly -v`
Expected: PASS (2 tests)

- [ ] **Step 6: Generate the three class files and eyeball a debug render**

```bash
cd hardware/panel
S=$(python3 -c "print(62.0/1100)")
python3 svg2poly.py svg/panel.svg $S "#dda857" gold.polys.json
python3 svg2poly.py svg/panel.svg $S "white,#ffffff" silk.polys.json
python3 svg2poly.py svg/panel.svg $S "#e7ba9b" fr4.polys.json
python3 - <<'EOF'
# throwaway proof render, not committed
import json
from PIL import Image, ImageDraw
im = Image.new("RGB", (620, 1016), (20, 20, 23)); d = ImageDraw.Draw(im)
for name, color in (("gold", (221,168,87)), ("silk", (235,235,235)), ("fr4", (231,186,155))):
    for p in json.load(open(f"{name}.polys.json")):
        d.polygon([(x*10, y*10) for x, y in p["outline"]], fill=color)
        for h in p["holes"]:
            d.polygon([(x*10, y*10) for x, y in h], fill=(20, 20, 23))
im.save("/tmp/panel-art-proof.png")
EOF
```

Read `/tmp/panel-art-proof.png` yourself: the AirWriter wordmark and subtitle in
gold with correct counters (holes in A, e, R), the three silk label rules and
serial/appletalk/power text at the bottom, the white `test` tag mid-grid, the
peach slash at bottom-right. Judging this image is part of the task.

- [ ] **Step 7: Clean up the stale SVGs and commit**

```bash
cd hardware/panel
rm -f svg/lockup.svg svg/labels.svg
git add svg2poly.py test_svg2poly.py gold.polys.json silk.polys.json fr4.polys.json
git commit -m "Panel: flatten fill-classed art from the panel export

Refs: W-573"
```

---

### Task 3: Board emit (`emit_board.py`) — outline, dots, test hole

**Files:**
- Create: `hardware/panel/emit_board.py`
- Create: `hardware/panel/generate.sh`
- Output: `hardware/panel/panel.kicad_pcb` (committed once DRC-clean in Task 4)

**Interfaces:**
- Consumes: `panel_layout.json` (Task 1 shape). Task 4 extends this same file to consume the three `*.polys.json`.
- Produces: `panel.kicad_pcb`; `generate.sh` = the one command that reruns the whole chain.

KiCad python is `KIPY=/Applications/KiCad/KiCad.app/Contents/Frameworks/Python.framework/Versions/3.9/bin/python3`. No unittest here — `pcbnew` only exists in that interpreter; verification is DRC plus render (Task 4).

- [ ] **Step 1: Implement the board skeleton + dots**

```python
#!/usr/bin/env python3
"""panel_layout.json + *.polys.json -> panel.kicad_pcb. KiCad python only."""
import json, math, pathlib, pcbnew

HERE = pathlib.Path(__file__).parent
mm = pcbnew.FromMM

def rounded_outline(board, w, h, r):
    for (x1, y1, x2, y2) in [(r, 0, w - r, 0), (w, r, w, h - r),
                             (w - r, h, r, h), (0, h - r, 0, r)]:
        s = pcbnew.PCB_SHAPE(board, pcbnew.SHAPE_T_SEGMENT)
        s.SetStart(pcbnew.VECTOR2I(mm(x1), mm(y1)))
        s.SetEnd(pcbnew.VECTOR2I(mm(x2), mm(y2)))
        s.SetLayer(pcbnew.Edge_Cuts); s.SetWidth(mm(0.1))
        board.Add(s)
    for (cx, cy, a0) in [(r, r, 180), (w - r, r, 270), (w - r, h - r, 0), (r, h - r, 90)]:
        a = pcbnew.PCB_SHAPE(board, pcbnew.SHAPE_T_ARC)
        a.SetCenter(pcbnew.VECTOR2I(mm(cx), mm(cy)))
        sx = cx + r * math.cos(math.radians(a0)); sy = cy + r * math.sin(math.radians(a0))
        a.SetStart(pcbnew.VECTOR2I(mm(sx), mm(sy)))
        a.SetArcAngleAndEnd(pcbnew.EDA_ANGLE(90, pcbnew.DEGREES_T), False)
        a.SetLayer(pcbnew.Edge_Cuts); a.SetWidth(mm(0.1))
        board.Add(a)

def filled_circle(board, layer, x, y, dia):
    c = pcbnew.PCB_SHAPE(board, pcbnew.SHAPE_T_CIRCLE)
    c.SetCenter(pcbnew.VECTOR2I(mm(x), mm(y)))
    c.SetEnd(pcbnew.VECTOR2I(mm(x + dia / 2), mm(y)))
    c.SetFilled(True); c.SetWidth(0); c.SetLayer(layer)
    board.Add(c)

def npth(board, x, y, dia):
    fp = pcbnew.FOOTPRINT(board)
    fp.SetPosition(pcbnew.VECTOR2I(mm(x), mm(y)))
    fp.SetReference("H_TEST"); fp.Reference().SetVisible(False)
    pad = pcbnew.PAD(fp)
    pad.SetAttribute(pcbnew.PAD_ATTRIB_NPTH)
    pad.SetShape(pcbnew.PAD_SHAPE_CIRCLE)
    pad.SetSize(pcbnew.VECTOR2I(mm(dia), mm(dia)))
    pad.SetDrillSize(pcbnew.VECTOR2I(mm(dia), mm(dia)))
    pad.SetPosition(pcbnew.VECTOR2I(mm(x), mm(y)))
    pad.SetLayerSet(pcbnew.PAD.UnplatedHoleMask())
    fp.Add(pad)
    board.Add(fp)

RECIPE = {  # mat -> (copper_F, mask_F, mask_B)
    "G": (True, True, False), "T": (True, True, False),
    "P": (False, True, True), "B": (False, False, True), "L": (False, False, False),
}

def main():
    lay = json.loads((HERE / "panel_layout.json").read_text())
    board = pcbnew.CreateEmptyBoard()
    p, g = lay["panel"], lay["grid"]
    rounded_outline(board, p["w"], p["h"], p["corner_r"])
    for d in lay["dots"]:
        cu, mf, mb = RECIPE[d["mat"]]
        dia = g["dot_dia"]
        if cu: filled_circle(board, pcbnew.F_Cu, d["x"], d["y"], dia)
        if mf: filled_circle(board, pcbnew.F_Mask, d["x"], d["y"], dia)
        if mb: filled_circle(board, pcbnew.B_Mask, d["x"], d["y"], dia)
        if d["mat"] == "T":
            npth(board, d["x"], d["y"], g["hole_dia"])
    board.Save(str(HERE / "panel.kicad_pcb"))
    print("wrote panel.kicad_pcb:", len(lay["dots"]), "dots")

if __name__ == "__main__":
    main()
```

API names drift between KiCad majors — if any call above throws `AttributeError`,
interrogate the live module (`KIPY -c "import pcbnew; print([n for n in dir(pcbnew) if 'ARC' in n])"`)
rather than guessing from training data; the geometry intent is what's specified here.
Note the T dot emits a full copper/mask disc and then the NPTH punches its centre —
verify the gold annulus survives in the Task 4 render. The 2.25 mm hole clears the
0.30 mm drill floor by an order of magnitude; the plunger that fits it is the
main-board plan's problem, noted there.

- [ ] **Step 2: Write `generate.sh`**

```bash
#!/bin/sh
# The one supported way to regenerate the panel. Close KiCad first.
set -e
cd "$(dirname "$0")"
KIPY=/Applications/KiCad/KiCad.app/Contents/Frameworks/Python.framework/Versions/3.9/bin/python3
S=$(python3 -c "print(62.0/1100)")
python3 panel_layout.py
python3 svg2poly.py svg/panel.svg "$S" "#dda857" gold.polys.json
python3 svg2poly.py svg/panel.svg "$S" "white,#ffffff" silk.polys.json
python3 svg2poly.py svg/panel.svg "$S" "#e7ba9b" fr4.polys.json
"$KIPY" emit_board.py
```

- [ ] **Step 3: Run it and sanity-check the output**

```bash
chmod +x hardware/panel/generate.sh && hardware/panel/generate.sh
grep -c 'gr_circle' hardware/panel/panel.kicad_pcb
```

Expected: script exits 0, board file exists, circle count matching the recipe:
19 gold-family dots (18 G + 1 T) on F.Cu and F.Mask = 38, 12 P on F.Mask and
B.Mask = 24, 12 B on B.Mask = 12, 7 L emit nothing → **74**.
Recount from the committed `dotmap.txt` if the tally differs.

- [ ] **Step 4: Commit**

```bash
git add hardware/panel/emit_board.py hardware/panel/generate.sh
git commit -m "Panel: emit outline, dots and test hole to kicad_pcb

Refs: W-573"
```

---

### Task 4: Art polys, floors, stackup, DRC, render

**Files:**
- Modify: `hardware/panel/emit_board.py` (add polygon emit + floor checks + stackup)
- Output: `hardware/panel/panel.kicad_pcb`, `hardware/panel/panel-front.png`

**Interfaces:**
- Consumes: `gold.polys.json`, `silk.polys.json`, `fr4.polys.json` (Task 2 shape — absolute mm, no offsets).
- Produces: the committed, DRC-clean board + a render for Wells.

- [ ] **Step 1: Add polygon emit to `emit_board.py`**

```python
def poly_shape(board, layer, polys):
    for p in polys:
        chain = pcbnew.SHAPE_POLY_SET()
        ol = pcbnew.SHAPE_LINE_CHAIN()
        for x, y in p["outline"]:
            ol.Append(mm(x), mm(y))
        ol.SetClosed(True)
        chain.AddOutline(ol)
        for hole in p["holes"]:
            hl = pcbnew.SHAPE_LINE_CHAIN()
            for x, y in hole:
                hl.Append(mm(x), mm(y))
            hl.SetClosed(True)
            chain.AddHole(hl)
        chain.Fracture()
        s = pcbnew.PCB_SHAPE(board, pcbnew.SHAPE_T_POLY)
        s.SetPolyShape(chain); s.SetFilled(True); s.SetWidth(0); s.SetLayer(layer)
        board.Add(s)
```

In `main()`, after the dots:

```python
    gold = json.loads((HERE / "gold.polys.json").read_text())
    silk = json.loads((HERE / "silk.polys.json").read_text())
    fr4 = json.loads((HERE / "fr4.polys.json").read_text())
    check_floors(lay, gold, silk, fr4)
    poly_shape(board, pcbnew.F_Cu, gold)     # gold = copper...
    poly_shape(board, pcbnew.F_Mask, gold)   # ...plus mask opening
    poly_shape(board, pcbnew.F_SilkS, silk)
    poly_shape(board, pcbnew.F_Mask, fr4)    # bare FR4 = open both faces
    poly_shape(board, pcbnew.B_Mask, fr4)
```

- [ ] **Step 2: Add the floor checks (fail loudly, before save)**

```python
def check_floors(lay, gold, silk, fr4):
    g = lay["grid"]
    assert g["hole_dia"] >= 0.30, "drill floor"
    assert g["pitch"] - g["dot_dia"] >= 0.15, "dot-to-dot clearance floor"
    for name, polys, lo in (("gold", gold, 3), ("silk", silk, 4), ("fr4", fr4, 1)):
        assert len(polys) >= lo, f"{name} art missing (got {len(polys)})"
        for p in polys:
            for x, y in p["outline"]:
                assert -0.1 <= x <= lay["panel"]["w"] + 0.1, f"{name} x out of panel"
                assert -0.1 <= y <= lay["panel"]["h"] + 0.1, f"{name} y out of panel"
```

The real sliver/silk-width enforcement is the DRC run below — these asserts catch
class-level breakage (a re-export that drops a fill class or moves art off-panel)
with a readable message instead of a DRC report.

- [ ] **Step 2b: Record the finish in the stackup**

The handoff rule: mask colour and finish live in the board's stackup so renders and
STEP exports show what will actually arrive. In `main()` before `board.Save(...)`:

```python
    ds = board.GetDesignSettings()
    stack = ds.GetStackupDescriptor()
    stack.BuildDefaultStackupList(ds, 2)
    stack.SetFinishType("ENIG")
    for item in stack.GetList():
        if item.GetType() == pcbnew.BS_ITEM_TYPE_SOLDERMASK:
            item.SetColor("Black")
```

As with the rest of the pcbnew surface: if these exact names don't exist in KiCad 10's
module, `dir()` the live objects for the equivalents; the requirement is "stackup says
matte black + ENIG", verified by grepping the saved file for `ENIG` and `Black`.

- [ ] **Step 3: Regenerate, run DRC, verify clean**

```bash
hardware/panel/generate.sh
/Applications/KiCad/KiCad.app/Contents/MacOS/kicad-cli pcb drc \
    --format json -o /tmp/panel-drc.json hardware/panel/panel.kicad_pcb
python3 -c "import json; d=json.load(open('/tmp/panel-drc.json')); \
print('violations:', len(d['violations'])); \
assert not d['violations'], [v['description'] for v in d['violations'][:5]]"
```

Expected: `violations: 0`. A no-net board has no unconnected items; any violation
is real. One likely legitimate finding: silk clipped where the `test` tag crosses
the gold dot's mask opening — if DRC flags silk-over-mask-opening there, that is
the drawn design; note it for the render review rather than deleting the tag.

- [ ] **Step 4: Render both faces and send to Wells**

```bash
/Applications/KiCad/KiCad.app/Contents/MacOS/kicad-cli pcb render \
    --side top -o hardware/panel/panel-front.png hardware/panel/panel.kicad_pcb
/Applications/KiCad/KiCad.app/Contents/MacOS/kicad-cli pcb render \
    --side bottom -o /tmp/panel-back.png hardware/panel/panel.kicad_pcb
```

Read both renders yourself first: grid present, gold dots gold, lockup legible,
labels at the bottom, test hole visible with a surviving gold annulus. Two render
caveats from the handoff: check `kicad-cli pcb render --help` before running
(KiCad 10 flags), and bare-FR4 regions render tinted by the mask colour — the P
dots will look nearly black in the render but not on the real board.

- [ ] **Step 5: Verify idempotence and commit the board**

```bash
hardware/panel/generate.sh && shasum hardware/panel/panel.kicad_pcb
hardware/panel/generate.sh && shasum hardware/panel/panel.kicad_pcb   # same hash
git add hardware/panel/
git commit -m "Panel: art polys, stackup, floors; first DRC-clean board

Refs: W-573"
```

If the hashes differ, find the nondeterminism (usually dict ordering or timestamps
in the writer) and fix it — regeneration being diff-clean is what makes Figma
iteration reviewable.

- [ ] **Step 6: Post the checkpoint to Linear**

Comment on W-573: panel generator working end to end from the single Figma export,
board DRC-clean, render path, and the follow-ups it exposes (Wells retunes by
re-exporting `svg/panel.svg` and rerunning `generate.sh`; main-board plan consumes
`panel_layout.json`; the 2.25 mm test hole constrains the switch plunger choice).

---

## Out of scope for this plan (later plans)

- Main-board rework (LED array from `panel_layout.json`, DIP bank, test switch — plunger must fit the 2.25 mm hole, shrink + re-route).
- Snap-apart panelization + swatch rail + PCBWay quote (fab plan; quote goes to Wells before any order).
- Light-guide lattice + enclosure.
- Firmware `led_grid` / `dip_config`.
