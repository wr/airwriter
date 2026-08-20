# Panel Generator Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build `hardware/panel/` — a generator that turns Wells's Figma SVG exports plus a dot map into a DRC-clean, fab-ready art-panel `.kicad_pcb` (62 × 100 mm, matte black, ENIG, zero components).

**Architecture:** Two stages with a JSON boundary. Stage A runs on **system python3**: parse the dot map and panel config into `panel_layout.json`, and flatten the SVGs into filled polygons in `*.polys.json`. Stage B runs on **KiCad's bundled python** (`pcbnew` module): read those JSONs and emit `panel.kicad_pcb`. The JSON boundary means everything except the final emit is testable without KiCad, and the layout JSON later feeds the main-board plan (LED coordinates).

**Tech Stack:** python3 (stdlib `unittest`), `svgelements` (pip, stage A only), `pcbnew` via `/Applications/KiCad/KiCad.app/Contents/Frameworks/Python.framework/Versions/3.9/bin/python3`, `kicad-cli` at `/Applications/KiCad/KiCad.app/Contents/MacOS/kicad-cli`.

**Spec:** `docs/superpowers/specs/2026-08-20-dot-panel-cover-design.md`

## Global Constraints

- Panel: 62.0 × 100.0 × 1.6 mm, 2-layer, matte black mask, ENIG, corner radius 2.0 mm, **no components, no nets**.
- Fab floors (generator must enforce, not just DRC): drill ≥ 0.30 mm, copper clearance ≥ 0.15 mm, silk line ≥ 0.153 mm, silk text height ≥ 0.8 mm, mask sliver ≥ 0.10 mm.
- F is the viewer face; LEDs sit behind B.
- Current geometry (Wells will retune in Figma; these are the working values): side margin 3.2 mm, top margin 2.8 mm, bottom margin 1.8 mm, grid 7 cols × 8 rows, pitch = (62 − 2×3.2)/7 = 7.9429 mm, dot Ø = 0.66 × pitch = 5.242 mm, test-dot hole Ø 3.5 mm NPTH.
- Regeneration is the only edit path for `panel.kicad_pcb`; the generator must be idempotent (same inputs → byte-identical board file, KiCad timestamps aside).
- `pcbnew` units are **nanometres** — always `pcbnew.FromMM()` / `pcbnew.ToMM()`.
- Do not run stage B while the panel board is open in KiCad (the app writes stale copies back on save — known trap).
- Repo conventions: kicad-cli is not on PATH (path above); check `kicad-cli <sub> --help` before first use of any subcommand.

## Material → layer recipe (from the spec, used by Tasks 1 and 3)

| code | meaning | copper | F.Mask opening | B.Mask opening | LED |
|---|---|---|---|---|---|
| `G` | gold dot | filled disc on F.Cu | yes (same disc) | no | no |
| `T` | test dot | gold ring on F.Cu (disc minus hole annulus) | yes (ring) | no | no; 3.5 mm NPTH at centre |
| `P` | bare FR4 | none | yes | yes | yes |
| `B` | bright teal | none | no | yes | yes |
| `L` | deep teal | none | no | no | yes |
| `.` | empty | none | no | no | no |

`L` emits no geometry at all — it exists only in the layout JSON (it still gets an LED).

---

### Task 1: Dot map and layout (`panel_layout.py`)

**Files:**
- Create: `hardware/panel/dotmap.txt`
- Create: `hardware/panel/panel_layout.py`
- Test: `hardware/panel/test_panel_layout.py`

**Interfaces:**
- Produces: `panel_layout.json` with shape
  `{"panel": {"w": 62.0, "h": 100.0, "corner_r": 2.0}, "grid": {"cols": 7, "rows": 8, "pitch": 7.9429, "dot_dia": 5.242, "origin": [x0, y0]}, "dots": [{"r": 0, "c": 0, "mat": "G", "x": 6.37, "y": 6.77, "led": false}, ...]}`
  where `x, y` are dot centres in mm from the panel's top-left corner, `origin` is the centre of dot (0,0), and `led` is true for `P`/`B`/`L`.
  The main-board plan consumes this same file for LED placement (mirrored in x).
- Produces: `python3 panel_layout.py` writes `panel_layout.json` next to itself; importable as `load_layout() -> dict` for tests.

- [ ] **Step 1: Write `dotmap.txt`** — the Frame 7 transcription, test dot at row 3 col 4:

```
GPBGLP.
GBPBG.P
LPGLBLG
GBGPTPB
G.LBLGB
PBGPG.G
GGP.BPL
B.GBGPG
```

- [ ] **Step 2: Write the failing tests**

```python
# hardware/panel/test_panel_layout.py
import unittest
from panel_layout import load_layout

class TestLayout(unittest.TestCase):
    def setUp(self):
        self.lay = load_layout()

    def test_counts(self):
        dots = self.lay["dots"]
        self.assertEqual(len(dots), 50)                      # 56 cells - 6 empties
        mats = [d["mat"] for d in dots]
        self.assertEqual(mats.count("G"), 18)                # 19 golds minus the T
        self.assertEqual(mats.count("T"), 1)
        self.assertEqual(sum(d["led"] for d in dots), 31)    # P+B+L

    def test_geometry(self):
        g = self.lay["grid"]
        self.assertAlmostEqual(g["pitch"], (62.0 - 2*3.2) / 7, places=4)
        self.assertAlmostEqual(g["dot_dia"], g["pitch"] * 0.66, places=4)
        d00 = next(d for d in self.lay["dots"] if (d["r"], d["c"]) == (0, 0))
        self.assertAlmostEqual(d00["x"], 3.2 + g["pitch"]/2, places=4)
        self.assertAlmostEqual(d00["y"], 2.8 + g["pitch"]/2, places=4)

    def test_grid_fits_panel(self):
        g = self.lay["grid"]
        self.assertLessEqual(2.8 + 8*g["pitch"], 100.0)      # grid inside panel height
```

- [ ] **Step 3: Run tests, verify they fail**

Run: `cd hardware/panel && python3 -m unittest test_panel_layout -v`
Expected: FAIL — `ModuleNotFoundError: No module named 'panel_layout'`

- [ ] **Step 4: Implement `panel_layout.py`**

```python
#!/usr/bin/env python3
"""Panel geometry: dotmap.txt + the config block -> panel_layout.json."""
import json, pathlib

CFG = dict(w=62.0, h=100.0, corner_r=2.0,
           margin_side=3.2, margin_top=2.8, margin_bottom=1.8,
           cols=7, dot_ratio=0.66, hole_dia=3.5)
LED_MATS = set("PBL")

def load_layout():
    here = pathlib.Path(__file__).parent
    rows = (here / "dotmap.txt").read_text().split()
    cols = CFG["cols"]
    assert all(len(r) == cols for r in rows), "ragged dotmap"
    pitch = (CFG["w"] - 2 * CFG["margin_side"]) / cols
    dia = pitch * CFG["dot_ratio"]
    dots = []
    for r, row in enumerate(rows):
        for c, ch in enumerate(row):
            if ch == ".":
                continue
            assert ch in "GTPBL", f"bad material {ch!r} at {r},{c}"
            dots.append(dict(r=r, c=c, mat=ch,
                             x=round(CFG["margin_side"] + (c + 0.5) * pitch, 4),
                             y=round(CFG["margin_top"] + (r + 0.5) * pitch, 4),
                             led=ch in LED_MATS))
    assert sum(d["mat"] == "T" for d in dots) == 1, "exactly one test dot"
    return dict(panel=dict(w=CFG["w"], h=CFG["h"], corner_r=CFG["corner_r"]),
                grid=dict(cols=cols, rows=len(rows), pitch=round(pitch, 4),
                          dot_dia=round(dia, 4),
                          origin=[dots[0]["x"], dots[0]["y"]],
                          hole_dia=CFG["hole_dia"]),
                dots=dots)

if __name__ == "__main__":
    out = pathlib.Path(__file__).parent / "panel_layout.json"
    out.write_text(json.dumps(load_layout(), indent=1) + "\n")
    print("wrote", out)
```

- [ ] **Step 5: Run tests, verify they pass**

Run: `cd hardware/panel && python3 -m unittest test_panel_layout -v`
Expected: PASS (3 tests). If `test_counts` fails on the material tallies, recount from `dotmap.txt` — the map is the truth, fix the test constants only after re-verifying by hand.

- [ ] **Step 6: Generate the JSON and commit**

```bash
cd hardware/panel && python3 panel_layout.py
git add hardware/panel/dotmap.txt hardware/panel/panel_layout.py \
        hardware/panel/test_panel_layout.py hardware/panel/panel_layout.json
git commit -m "Panel layout: dot map and geometry to JSON

Refs: W-573"
```

---

### Task 2: SVG flattener (`svg2poly.py`)

**Files:**
- Create: `hardware/panel/svg2poly.py`
- Create: `hardware/panel/svg/lockup.svg` (copy of `~/Desktop/Frame 27.svg`)
- Create: `hardware/panel/svg/labels.svg` (copy of `~/Desktop/Frame 25.svg`)
- Test: `hardware/panel/test_svg2poly.py`

**Interfaces:**
- Consumes: nothing from Task 1.
- Produces: `svg2poly.flatten(svg_path, target_w_mm) -> list[dict]`, each dict
  `{"outline": [[x,y],...], "holes": [[[x,y],...], ...]}` in mm, y-down, scaled so the
  SVG viewBox width equals `target_w_mm`. CLI: `python3 svg2poly.py in.svg 34.0 out.polys.json`.
  Task 3 reads these JSON files verbatim.

- [ ] **Step 1: Install the dependency and copy the SVGs**

```bash
python3 -m pip install --user svgelements
mkdir -p hardware/panel/svg
cp ~/Desktop/"Frame 27.svg" hardware/panel/svg/lockup.svg
cp ~/Desktop/"Frame 25.svg" hardware/panel/svg/labels.svg
```

- [ ] **Step 2: Write the failing tests**

```python
# hardware/panel/test_svg2poly.py
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
```

- [ ] **Step 3: Run tests, verify they fail**

Run: `cd hardware/panel && python3 -m unittest test_svg2poly -v`
Expected: FAIL — no module `svg2poly`

- [ ] **Step 4: Implement `svg2poly.py`**

Core approach: `svgelements` parses and applies transforms; sample each path segment
adaptively; classify subpaths into outlines vs holes by winding/containment.

```python
#!/usr/bin/env python3
"""Flatten SVG fills to polygons (mm). Outlines + holes, evenodd."""
import json, sys
from svgelements import SVG, Path, Shape

TOL = 0.05  # mm sampling tolerance at target scale

def _subpaths_to_points(path, scale):
    for sub in path.as_subpaths():
        seg = Path(sub)
        n = max(8, int(seg.length() * scale / TOL / 4))
        pts = [seg.point(i / n) for i in range(n)]
        yield [[round(p.real * scale, 4), round(p.imag * scale, 4)] for p in pts]

def _area(poly):
    s = 0.0
    for (x1, y1), (x2, y2) in zip(poly, poly[1:] + poly[:1]):
        s += x1 * y2 - x2 * y1
    return s / 2

def _contains(outer, pt):
    # ray cast
    x, y = pt; inside = False
    for (x1, y1), (x2, y2) in zip(outer, outer[1:] + outer[:1]):
        if (y1 > y) != (y2 > y) and x < (x2 - x1) * (y - y1) / (y2 - y1) + x1:
            inside = not inside
    return inside

def flatten(svg_path, target_w_mm):
    svg = SVG.parse(svg_path, reify=True)
    vb_w = svg.viewbox.width if svg.viewbox else svg.width
    scale = target_w_mm / float(vb_w)
    rings = []
    for el in svg.elements():
        if isinstance(el, Shape) and not isinstance(el, SVG):
            p = Path(el)
            if p.length() == 0:
                continue
            rings.extend(_subpaths_to_points(p, scale))
    # largest-area rings are outlines; a ring inside another ring is its hole
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
    inp, w_mm, outp = sys.argv[1], float(sys.argv[2]), sys.argv[3]
    with open(outp, "w") as f:
        json.dump(flatten(inp, w_mm), f)
    print("wrote", outp)
```

Known simplification: nested hole-in-hole (an island inside a counter) would
misclassify — no glyph in these two SVGs has one; if `test_real_lockup_flattens`
exposes anything odd, render a debug PNG of the polygons with PIL and inspect
before changing the algorithm. Note that `labels.svg` uses `<mask>` elements —
if masked strokes come out wrong, ask Wells for a flattened re-export (outline
strokes, no masks) rather than implementing SVG masks; put what's needed in the
error message the module raises.

- [ ] **Step 5: Run tests, verify they pass**

Run: `cd hardware/panel && python3 -m unittest test_svg2poly -v`
Expected: PASS (2 tests)

- [ ] **Step 6: Generate both polys files and eyeball a debug render**

```bash
cd hardware/panel
python3 svg2poly.py svg/lockup.svg 34.0 lockup.polys.json
python3 svg2poly.py svg/labels.svg 55.6 labels.polys.json
python3 - <<'EOF'
# throwaway proof render, not committed
import json
from PIL import Image, ImageDraw
for name in ("lockup", "labels"):
    polys = json.load(open(f"{name}.polys.json"))
    s = 10
    im = Image.new("L", (700, 400), 255); d = ImageDraw.Draw(im)
    for p in polys:
        d.polygon([(x*s, y*s) for x, y in p["outline"]], fill=0)
        for h in p["holes"]:
            d.polygon([(x*s, y*s) for x, y in h], fill=255)
    im.save(f"/tmp/{name}-proof.png")
EOF
```

Read both proof PNGs; the lockup must read as "AirWriter" with correct counters
(holes in A, e, R), the labels as serial/appletalk/power with their rules.

- [ ] **Step 7: Commit**

```bash
git add hardware/panel/svg2poly.py hardware/panel/test_svg2poly.py \
        hardware/panel/svg/ hardware/panel/lockup.polys.json hardware/panel/labels.polys.json
git commit -m "Panel: flatten Figma SVG exports to polygon JSON

Refs: W-573"
```

---

### Task 3: Board emit (`emit_board.py`) — outline, dots, test hole

**Files:**
- Create: `hardware/panel/emit_board.py`
- Create: `hardware/panel/generate.sh`
- Output: `hardware/panel/panel.kicad_pcb` (committed once DRC-clean in Task 4)

**Interfaces:**
- Consumes: `panel_layout.json` (Task 1 shape), `lockup.polys.json` / `labels.polys.json` (Task 2 shape).
- Produces: `panel.kicad_pcb`; `generate.sh` = the one command that reruns the whole chain.

KiCad python is `KIPY=/Applications/KiCad/KiCad.app/Contents/Frameworks/Python.framework/Versions/3.9/bin/python3`. No unittest here — `pcbnew` only exists in that interpreter; verification is DRC plus render (Task 4).

- [ ] **Step 1: Implement the board skeleton + dots**

```python
#!/usr/bin/env python3
"""panel_layout.json + *.polys.json -> panel.kicad_pcb. KiCad python only."""
import json, pathlib, pcbnew

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
        # 90-degree arc; set start point on the circle, then sweep
        import math
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
drill through copper is what a plated ring around a hole looks like in gerbers;
verify the annulus survives in the Task 4 render, and if the fab plots it doughnut-wrong,
switch the T disc to an explicit ring polygon.

- [ ] **Step 2: Write `generate.sh`**

```bash
#!/bin/sh
# The one supported way to regenerate the panel. Close KiCad first.
set -e
cd "$(dirname "$0")"
KIPY=/Applications/KiCad/KiCad.app/Contents/Frameworks/Python.framework/Versions/3.9/bin/python3
python3 panel_layout.py
python3 svg2poly.py svg/lockup.svg 34.0 lockup.polys.json
python3 svg2poly.py svg/labels.svg 55.6 labels.polys.json
"$KIPY" emit_board.py
```

- [ ] **Step 3: Run it and sanity-check the output**

```bash
chmod +x hardware/panel/generate.sh && hardware/panel/generate.sh
grep -c 'gr_circle\|fp_circle' hardware/panel/panel.kicad_pcb   # expect > 60 (dots x faces)
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

### Task 4: Lockup, labels, floors, DRC, render

**Files:**
- Modify: `hardware/panel/emit_board.py` (add polygon emit + floor checks)
- Output: `hardware/panel/panel.kicad_pcb`, `hardware/panel/panel-front.png`

**Interfaces:**
- Consumes: `lockup.polys.json`, `labels.polys.json` (Task 2 shape).
- Produces: the committed, DRC-clean board + a render for Wells.

- [ ] **Step 1: Add polygon emit to `emit_board.py`**

```python
def poly_shape(board, layer, polys, off_x, off_y):
    for p in polys:
        chain = pcbnew.SHAPE_POLY_SET()
        ol = pcbnew.SHAPE_LINE_CHAIN()
        for x, y in p["outline"]:
            ol.Append(mm(x + off_x), mm(y + off_y))
        ol.SetClosed(True)
        chain.AddOutline(ol)
        for hole in p["holes"]:
            hl = pcbnew.SHAPE_LINE_CHAIN()
            for x, y in hole:
                hl.Append(mm(x + off_x), mm(y + off_y))
            hl.SetClosed(True)
            chain.AddHole(hl)
        chain.Fracture()
        s = pcbnew.PCB_SHAPE(board, pcbnew.SHAPE_T_POLY)
        s.SetPolyShape(chain); s.SetFilled(True); s.SetWidth(0); s.SetLayer(layer)
        board.Add(s)
```

In `main()`, after the dots (positions are the current working values; Wells's
Figma retune changes only these numbers):

```python
    lockup = json.loads((HERE / "lockup.polys.json").read_text())
    labels = json.loads((HERE / "labels.polys.json").read_text())
    grid_bottom = 2.8 + g["rows"] * g["pitch"]
    lockup_h = max(y for pp in lockup for x, y in pp["outline"])
    labels_h = max(y for pp in labels for x, y in pp["outline"])
    gap = (p["h"] - grid_bottom - lockup_h - labels_h - 1.8) / 2
    ly = grid_bottom + gap
    poly_shape(board, pcbnew.F_Cu,   lockup, 3.2, ly)   # gold = copper...
    poly_shape(board, pcbnew.F_Mask, lockup, 3.2, ly)   # ...plus mask opening
    poly_shape(board, pcbnew.F_SilkS, labels, 3.2, p["h"] - 1.8 - labels_h)
```

- [ ] **Step 2: Add the floor checks (fail loudly, before save)**

```python
def check_floors(lay, lockup, labels):
    g = lay["grid"]
    assert g["hole_dia"] >= 0.30, "drill floor"
    assert g["pitch"] - g["dot_dia"] >= 0.15, "dot-to-dot clearance floor"
    def min_feature(polys):
        # cheap proxy: shortest edge of any outline; true sliver analysis is DRC's job
        m = 1e9
        for p in polys:
            for ring in [p["outline"]] + p["holes"]:
                for (x1, y1), (x2, y2) in zip(ring, ring[1:] + ring[:1]):
                    d = ((x2-x1)**2 + (y2-y1)**2) ** 0.5
                    if d > 1e-6: m = min(m, d)
        return m
    assert min_feature(lockup) > 0.0, "empty lockup"
    assert min_feature(labels) > 0.0, "empty labels"
```

The real sliver/silk-width enforcement is the DRC run below — these asserts catch
config-level violations (a Figma retune that squeezes dots into the clearance floor)
with a readable message instead of a DRC report.

- [ ] **Step 2b: Record the finish in the stackup**

The handoff rule: mask colour and finish live in the board's stackup so renders and
STEP exports show what will arrive, and the order can't forget them. In `main()`
before `board.Save(...)`:

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
matte black + ENIG", verified by opening Board Setup → Physical Stackup or by grepping
the saved file for `ENIG` and `Black`.

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
is real. Silk-over-copper warnings at the labels would mean the labels drifted over
the lockup — a layout bug, fix the offsets, don't suppress the check.

- [ ] **Step 4: Render both faces and send to Wells**

```bash
/Applications/KiCad/KiCad.app/Contents/MacOS/kicad-cli pcb render \
    --side top -o hardware/panel/panel-front.png hardware/panel/panel.kicad_pcb
/Applications/KiCad/KiCad.app/Contents/MacOS/kicad-cli pcb render \
    --side bottom -o /tmp/panel-back.png hardware/panel/panel.kicad_pcb
```

Read both renders yourself first: grid present, gold dots gold, lockup legible,
labels at the bottom, test hole visible. Two render caveats from the handoff: check
`kicad-cli pcb render --help` before running (KiCad 10 flags), and bare-FR4 regions
render tinted by the mask colour — the P dots will look nearly black in the render
but not on the real board. Then send `panel-front.png` to Wells with that caveat.

- [ ] **Step 5: Verify idempotence and commit the board**

```bash
hardware/panel/generate.sh && shasum hardware/panel/panel.kicad_pcb
hardware/panel/generate.sh && shasum hardware/panel/panel.kicad_pcb   # same hash
git add hardware/panel/
git commit -m "Panel: gold lockup, silk labels, floors; first DRC-clean board

Refs: W-573"
```

If the hashes differ, find the nondeterminism (usually dict ordering or timestamps
in the writer) and fix it — regeneration being diff-clean is what makes Figma
iteration reviewable.

- [ ] **Step 6: Post the checkpoint to Linear**

Comment on W-573: panel generator working end to end, board DRC-clean, render
attached path, and the two follow-ups it exposes (Wells's Figma retune lands by
re-exporting SVGs + editing the CFG block; main-board plan consumes
`panel_layout.json`).

---

## Out of scope for this plan (later plans)

- Main-board rework (LED array from `panel_layout.json`, DIP bank, test switch, shrink + re-route).
- Snap-apart panelization + swatch rail + PCBWay quote (fab plan; quote goes to Wells before any order).
- Light-guide lattice + enclosure.
- Firmware `led_grid` / `dip_config`.
