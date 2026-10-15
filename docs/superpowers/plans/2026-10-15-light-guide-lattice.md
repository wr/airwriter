# Light-Guide Lattice Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A parametric build123d model of the Pro build's light-guide lattice in `enclosure/`, exported to `out/airwriter-lattice.{stl,step}`, with every position read from the board, the panel layout and KiCad's STEP export at build time.

**Architecture:** `board_geom.py` gains readers for the panel layout, the panel's FR4 windows, footprint courtyards and footprint keepout zones. A new `components.py` turns KiCad's own STEP export into per-part "stepped envelopes": the footprint of each 1 mm z-band and the band's top. Their vertical shadow is what the lattice must keep clear of, since it comes down onto the board from above. A new `lattice.py` unions grid + posts + rails + D3 tube, trims the result to the inset board outline, then subtracts post bores and pockets, the tube's void, every envelope shadow, the connector hulls, the antenna keepout and SW1's reset path.

**Tech Stack:** Python 3.14, build123d 0.11.1 (OCCT), `unittest`, kicad-cli 10 at `/Applications/KiCad/KiCad.app/Contents/MacOS/kicad-cli`.

**Spec:** `docs/superpowers/specs/2026-10-14-light-guide-lattice-design.md`

## Global Constraints

- Height = `params.CAVITY` = **13.04** (already landed with W-1036).
- Walls `LATTICE_WALL` **0.80**; outline inset `LATTICE_INSET` **0.25**; cell corner radius **1.00**.
- Posts **Ø8.0** at H1–H4; bore `SCREW_CLEAR_D` (2.80); standoff pocket **Ø6.40 × 3.30** from the top; a 45° cone below the pocket.
- Part clearance **0.40** sideways and **+0.50** on top; the envelope is voided from below the board up to its top (its shadow).
- D3 tube: the FR4 window + **0.25** inside, a **0.80** wall, thinnest wall at least **0.6** after the outline clip.
- West rail x from the inset to **7.00**; east rail x **50.00–55.75**.
- Antenna keepout from U1's footprint zone, grown **0.25**. **Deviation from the spec:** it is clipped at the grid's front wall, so the grid stays closed. The zone overlaps the grid by 1.85 mm, the module's antenna ends 1.7 mm before the grid, and cutting there opened lit cell r7c6.
- Print panel-face down, no supports: `test_printable.upward_flats(part, ceiling_z=CAVITY)` must be empty.
- Never write anything under `hardware/`. The build reads the board; it does not change it.
- Case frame: X = KiCad x − 100, Y = 164.27 − KiCad y, Z up from the board's top face. Panel mm → board mm is + (100, 60).

## Review Focus

1. **kicad-cli or the model library missing, or a part with no model** — expected: the build stops with a message naming the part, never a lattice silently missing a clearance. Test in Task 2: every F.Cu footprint has an envelope except H* and TP*.
2. **The board or panel art moving an LED off its dot** — expected: a failing test, not a lattice whose cells sit between LED and dot. Test in Task 1: each of the 37 lit dots is within 0.05 of exactly one SK6812.
3. **A stale `components.step` after a board edit** — expected: re-export. Test in Task 2: `needs_export` is True when the STEP is older than the board.
4. **A second FR4 window added to the panel art** — expected: the D3 tube still finds the window over D3, or fails loudly. Test in Task 3: `d3_window()` contains D3's centre.
5. **A clearance notch joining two lit cells** (visible bleed between dots) — expected: a failing test, not a by-eye surprise on the first print. Test in Task 3: `leaky_walls()` reports no lit–lit notch.

---

### Task 1: Board and panel readers

**Files:**
- Modify: `enclosure/board_geom.py` (append)
- Test: `enclosure/test_board_geom.py` (append a class)

**Interfaces:**
- Produces:
  - `PANEL_DIR: pathlib.Path`
  - `panel_to_case(px: float, py: float) -> tuple[float, float]`
  - `panel_grid() -> dict` (keys `cols, rows, pitch, dot_dia, origin, hole_dia`)
  - `panel_dots() -> list[dict]` (keys `r, c, mat, led, x, y`, case frame)
  - `fr4_windows() -> list[tuple[float, float, float, float]]` (`x0, x1, y0, y1`, case frame)
  - `courtyard(ref) -> tuple[float, float, float, float] | None` (`x0, x1, y0, y1`)
  - `keepout_zones(ref) -> list[list[tuple[float, float]]]`

- [ ] **Step 1: Write the failing tests** — append to `enclosure/test_board_geom.py`:

```python
class TestPanelAndFootprintReaders(unittest.TestCase):
    def test_panel_and_board_holes_are_coaxial(self):
        # MT1-MT4 (hardware/panel/emit_board.py STANDOFFS) sit on H1-H4.
        panel = [(5.6364, 5.6364), (56.3636, 5.6364), (5.6364, 91.93), (53.3636, 91.93)]
        holes = sorted(bg.mount_holes().values())
        for got, want in zip(sorted(bg.panel_to_case(*q) for q in panel), holes):
            self.assertAlmostEqual(got[0], want[0], places=3)
            self.assertAlmostEqual(got[1], want[1], places=3)

    def test_every_lit_dot_sits_on_exactly_one_sk6812(self):
        leds = [bg.to_case(f.x, f.y) for f in bg._all().values() if "SK6812" in f.lib]
        lit = [d for d in bg.panel_dots() if d["led"]]
        self.assertEqual(len(lit), 37)
        for d in lit:
            near = [q for q in leds if abs(q[0] - d["x"]) < 0.05 and abs(q[1] - d["y"]) < 0.05]
            self.assertEqual(len(near), 1, f"dot r{d['r']}c{d['c']}")

    def test_the_fr4_window_sits_over_d3(self):
        f = bg.footprint("D3")
        x, y = bg.to_case(f.x, f.y)
        hits = [w for w in bg.fr4_windows() if w[0] < x < w[1] and w[2] < y < w[3]]
        self.assertEqual(len(hits), 1)

    def test_courtyard_reads_the_footprint_in_the_case_frame(self):
        x0, x1, y0, y1 = bg.courtyard("SW3")
        f = bg.footprint("SW3")
        # SW3's origin is pin 1, a corner of its pin field; the courtyard
        # spans the 6 x 6 body around the pin field.
        self.assertLess(x1 - x0, 9.0); self.assertGreater(x1 - x0, 6.0)
        self.assertIsNone(bg.courtyard("TP2"))
        # S1's courtyard includes the keys, 2.2 past the board's west edge.
        self.assertLess(bg.courtyard("S1")[0], -2.0)

    def test_u1_carries_the_antenna_keepout(self):
        (zone,) = bg.keepout_zones("U1")
        xs = [q[0] for q in zone]; ys = [q[1] for q in zone]
        self.assertAlmostEqual(min(xs), 56.0, places=2)
        self.assertAlmostEqual(max(xs), 62.0, places=2)
        self.assertAlmostEqual(min(ys), 9.726, places=2)
        self.assertAlmostEqual(max(ys), 36.67, places=2)
```

- [ ] **Step 2: Run, expect failures** — `cd enclosure && .venv/bin/python -m unittest test_board_geom.TestPanelAndFootprintReaders -v` → AttributeError on `panel_to_case`.

- [ ] **Step 3: Implement** — add `import json` at the top of `board_geom.py`, then append:

```python
PANEL_DIR = REPO / "hardware" / "panel"
PANEL_DX, PANEL_DY = 100.0, 60.0
"""The board's KiCad frame is the panel's plus (100, 60): the boards share an
outline and their holes are coaxial (hardware/panel/gen_led_map.py)."""


def panel_to_case(px: float, py: float) -> tuple[float, float]:
    """Panel mm (panel_layout.json, *.polys.json) -> case-frame X, Y."""
    return to_case(px + PANEL_DX, py + PANEL_DY)


def _panel_layout() -> dict:
    return json.loads((PANEL_DIR / "panel_layout.json").read_text())


def panel_grid() -> dict:
    """panel_layout.json's grid block: cols, rows, pitch, dot_dia, origin, hole_dia."""
    return _panel_layout()["grid"]


def panel_dots() -> list[dict]:
    """Every dot, case frame: {"r", "c", "mat", "led", "x", "y"}."""
    out = []
    for d in _panel_layout()["dots"]:
        x, y = panel_to_case(d["x"], d["y"])
        out.append({"r": d["r"], "c": d["c"], "mat": d["mat"], "led": d["led"],
                    "x": x, "y": y})
    return out


def fr4_windows() -> list[tuple[float, float, float, float]]:
    """The panel's bare-FR4 windows (hardware/panel/fr4.polys.json) as case-frame
    boxes (x0, x1, y0, y1). Rev A has one, over the power LED D3."""
    out = []
    for poly in json.loads((PANEL_DIR / "fr4.polys.json").read_text()):
        pts = [panel_to_case(px, py) for px, py in poly["outline"]]
        xs = [q[0] for q in pts]
        ys = [q[1] for q in pts]
        out.append((min(xs), max(xs), min(ys), max(ys)))
    return out


def _sexpr(src: str, i: int) -> str:
    """The balanced (...) expression that opens at src[i], strings skipped."""
    depth, j = 0, i
    while True:
        c = src[j]
        if c == '"':
            j += 1
            while src[j] != '"' or src[j - 1] == "\\":
                j += 1
        elif c == "(":
            depth += 1
        elif c == ")":
            depth -= 1
            if depth == 0:
                return src[i:j + 1]
        j += 1


def _footprint_block(ref: str) -> str:
    for blk in _blocks(_text(), "footprint"):
        m = _REF.search(blk)
        if m and m.group(1) == ref:
            return blk
    raise KeyError(f"{ref} is not a footprint on {BOARD_PCB.name}")


_GR_PT = re.compile(r"\((start|end|mid|xy|center) ([-\d.]+) ([-\d.]+)\)")


def courtyard(ref: str) -> tuple[float, float, float, float] | None:
    """A footprint's F.CrtYd bounding box in the case frame (x0, x1, y0, y1),
    or None when it draws no courtyard. Footprint graphics are stored in the
    footprint's own unrotated frame, so each point goes through
    local_to_board()."""
    fp = footprint(ref)
    blk = _footprint_block(ref)
    pts: list[tuple[float, float]] = []
    for m in re.finditer(r"\((fp_line|fp_rect|fp_poly|fp_arc|fp_circle)\b", blk):
        sub = _sexpr(blk, m.start())
        if '"F.CrtYd"' not in sub:
            continue
        got = {k: (float(a), float(b)) for k, a, b in _GR_PT.findall(sub)}
        if m.group(1) == "fp_circle":
            (cx, cy), (ex, ey) = got["center"], got["end"]
            r = math.hypot(ex - cx, ey - cy)
            local = [(cx - r, cy - r), (cx + r, cy + r), (cx - r, cy + r), (cx + r, cy - r)]
        else:
            local = [(float(a), float(b)) for _, a, b in _GR_PT.findall(sub)]
        pts += [to_case(*local_to_board(fp, lx, ly)) for lx, ly in local]
    if not pts:
        return None
    xs = [q[0] for q in pts]
    ys = [q[1] for q in pts]
    return min(xs), max(xs), min(ys), max(ys)


def keepout_zones(ref: str) -> list[list[tuple[float, float]]]:
    """Zones drawn inside a footprint (U1's antenna keepout) as case-frame
    polygons. KiCad stores footprint zones in board coordinates."""
    blk = _footprint_block(ref)
    out = []
    for m in re.finditer(r"\(zone\b", blk):
        sub = _sexpr(blk, m.start())
        out.append([to_case(float(a), float(b))
                    for a, b in re.findall(r"\(xy ([-\d.]+) ([-\d.]+)\)", sub)])
    return out
```

- [ ] **Step 4: Run, expect pass** — same command → 5 OK. Then the whole file: `.venv/bin/python -m unittest test_board_geom -v`.

- [ ] **Step 5: Commit** — `git add enclosure/board_geom.py enclosure/test_board_geom.py && git commit -m "board_geom: panel layout, FR4 windows, courtyards, keepout zones" -m "Refs: W-1038"`

---

### Task 2: Component envelopes from KiCad's STEP export

**Files:**
- Create: `enclosure/components.py`
- Test: `enclosure/test_components.py`

**Interfaces:**
- Consumes: `bg.courtyard`, `bg._all`, `bg.BOARD_PCB`, `bg.ORIGIN_X/Y`, `params.PCB`.
- Produces:
  - `needs_export(step: Path, board: Path) -> bool`
  - `export(force: bool = False) -> Path` (runs kicad-cli when stale)
  - `envelopes() -> dict[str, list[tuple[float, float, float, float, float]]]`: ref → bands `(x0, x1, y0, y1, top)`, case frame, top above the board face.

- [ ] **Step 1: Write the failing tests** — `enclosure/test_components.py`:

```python
"""components.py: every part on the board as boxes the lattice keeps clear of."""
import os
import pathlib
import re
import tempfile
import unittest

import board_geom as bg
import components


class TestExportCache(unittest.TestCase):
    def test_stale_when_missing_or_older_than_the_board(self):
        with tempfile.TemporaryDirectory() as d:
            board = pathlib.Path(d, "b.kicad_pcb"); board.write_text("x")
            step = pathlib.Path(d, "c.step")
            self.assertTrue(components.needs_export(step, board))
            step.write_text("y")
            os.utime(step, (1, 1))
            self.assertTrue(components.needs_export(step, board))
            os.utime(step, None)
            os.utime(board, (1, 1))
            self.assertFalse(components.needs_export(step, board))


class TestEnvelopes(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.env = components.envelopes()

    def test_every_part_has_one_except_holes_and_test_points(self):
        missing = [r for r, f in bg._all().items()
                   if f.layer == "F.Cu" and r not in self.env
                   and not re.fullmatch(r"(H|TP)\d+", r)]
        self.assertEqual(missing, [])

    def test_sw3_is_stepped_body_then_plunger(self):
        bands = self.env["SW3"]
        bottom, top = bands[0], bands[-1]
        self.assertGreater(bottom[1] - bottom[0], 5.5)   # 6 mm body
        self.assertLess(top[1] - top[0], 4.0)            # 3.5 mm plunger
        self.assertAlmostEqual(top[4], 15.0, delta=0.1)

    def test_no_model_parts_and_overrides(self):
        self.assertAlmostEqual(max(b[4] for b in self.env["S1"]), 9.41)
        self.assertAlmostEqual(max(b[4] for b in self.env["U7"]), 9.00)
        self.assertAlmostEqual(max(b[4] for b in self.env["D13"]), 1.60)

    def test_d3_is_where_the_board_puts_it(self):
        (x0, x1, y0, y1, top), = self.env["D3"]
        f = bg.footprint("D3"); x, y = bg.to_case(f.x, f.y)
        self.assertTrue(x0 < x < x1 and y0 < y < y1)
        self.assertLess(top, 1.2)
```

- [ ] **Step 2: Run, expect failure** — `.venv/bin/python -m unittest test_components -v` → ModuleNotFoundError.

- [ ] **Step 3: Implement** — `enclosure/components.py`:

```python
"""The board's parts as boxes the light-guide lattice must keep clear of.

KiCad's own STEP export places every 3D model exactly where pcbnew does, so it
is the authority for where each body is. kicad-cli writes it to
out/components.step (gitignored), redone whenever the board file is newer.
Each part becomes a "stepped envelope": the XY footprint of each BAND-tall
slice and that slice's top above the board face. The lattice voids each
band's footprint from below the board up to the band's top -- its vertical
shadow, because the lattice is lowered onto the board from above. One box per
part would be enough for flat parts, but it would turn SW3 (a 6 mm body under
a 3.5 mm plunger to 15 mm) into a 15 mm block and cut the test cell's walls
away."""
import math
import os
import pathlib
import re
import subprocess

from build123d import Box, Pos, import_step

import board_geom as bg
import params as p

KICAD_CLI = os.environ.get(
    "KICAD_CLI", "/Applications/KiCad/KiCad.app/Contents/MacOS/kicad-cli")
MODEL_DIR = os.environ.get(
    "KICAD10_3DMODEL_DIR",
    "/Applications/KiCad/KiCad.app/Contents/SharedSupport/3dmodels")
STEP = pathlib.Path(__file__).resolve().parent / "out" / "components.step"

NO_MODEL = {"S1": 9.41, "SW1": 3.50}
"""Top above the board for parts with no 3D model, boxed from their courtyard.
S1: the DP-08RP body top (test_case / the shop's dp08rp.mjs). SW1: its
footprint descr, 6.2 x 2.55 x 3.5."""
NO_MODEL_LIB = {"LED_SK6812MINI_PLCC4_3.5x3.5mm_P1.75mm": 1.60}
"""KiCad 10 ships no SK6812MINI model; 1.60 is conservative over the
3.5 x 3.5 x 1.45 package."""
TOP_OVERRIDE = {"U7": 9.00}
"""U7's model is the empty DIP socket; the PIC seated in it stands ~9 mm."""
FLAT = re.compile(r"(H|TP)\d+")
BAND = 1.0
STEPPED_ABOVE = 2.5
"""Parts taller than this get one band per BAND mm; shorter ones one box."""

Band = tuple[float, float, float, float, float]


def needs_export(step: pathlib.Path, board: pathlib.Path) -> bool:
    return not step.exists() or step.stat().st_mtime < board.stat().st_mtime


def export(force: bool = False) -> pathlib.Path:
    if force or needs_export(STEP, bg.BOARD_PCB):
        if not os.path.exists(KICAD_CLI):
            raise RuntimeError(f"kicad-cli not found at {KICAD_CLI}; set KICAD_CLI")
        STEP.parent.mkdir(exist_ok=True)
        r = subprocess.run(
            [KICAD_CLI, "pcb", "export", "step", "--no-board-body", "--subst-models",
             "-D", f"KICAD10_3DMODEL_DIR={MODEL_DIR}", "-f", "-o", str(STEP),
             str(bg.BOARD_PCB)], capture_output=True, text=True)
        if r.returncode or not STEP.exists():
            raise RuntimeError(f"kicad-cli STEP export failed:\n{r.stdout}\n{r.stderr}")
    return STEP


def _norm(s: str) -> str:
    return s.replace(".", "_")  # KiCad writes '.' in model names as '_' in STEP labels


def _to_case_box(bb) -> tuple[float, float, float, float, float, float]:
    # STEP frame: X = KiCad x, Y = -KiCad y, Z = 0 at the board's underside.
    return (bb.min.X - bg.ORIGIN_X, bb.max.X - bg.ORIGIN_X,
            bg.ORIGIN_Y + bb.min.Y, bg.ORIGIN_Y + bb.max.Y,
            bb.min.Z - p.PCB, bb.max.Z - p.PCB)


```

Continue the same file:

```python
def _bands(shape) -> list[Band]:
    bb = shape.bounding_box()
    x0, x1, y0, y1, z0, z1 = _to_case_box(bb)
    if z1 <= STEPPED_ABOVE:
        return [(x0, x1, y0, y1, z1)]
    cx, cy = (bb.min.X + bb.max.X) / 2, (bb.min.Y + bb.max.Y) / 2
    w, l = bb.size.X + 2, bb.size.Y + 2
    out = []
    z = 0.0
    while z < z1 - 1e-6:
        top = min(z + BAND, z1)
        slab = Pos(cx, cy, p.PCB + (z + top) / 2) * Box(w, l, top - z)
        sl = shape & slab
        if sl is not None and sl.volume > 1e-6:
            sx0, sx1, sy0, sy1, _, _ = _to_case_box(sl.bounding_box())
            out.append((sx0, sx1, sy0, sy1, top))
        z = top
    return out


_ENV: dict[str, list[Band]] | None = None


def envelopes() -> dict[str, list[Band]]:
    """ref -> stepped envelope (see module docstring), for every part on F.Cu
    except the flat ones (mounting holes, test points). Raises on a part with
    neither a 3D model nor a NO_MODEL entry, rather than leave it unguarded."""
    global _ENV
    if _ENV is not None:
        return _ENV
    fps = bg._all()
    stem = {r: _norm(pathlib.Path(f.model).stem) if f.model else None
            for r, f in fps.items()}
    env: dict[str, list[Band]] = {}
    used: set[str] = set()
    for child in import_step(str(export())).children:
        bb = child.bounding_box()
        cx, cy = (bb.min.X + bb.max.X) / 2, -(bb.min.Y + bb.max.Y) / 2
        cands = sorted((math.hypot(f.x - cx, f.y - cy), r) for r, f in fps.items()
                       if stem[r] == _norm(child.label) and r not in used)
        if not cands:
            raise RuntimeError(f"STEP body {child.label!r} matches no footprint")
        ref = cands[0][1]
        used.add(ref)
        env[ref] = _bands(child)
    for ref, f in fps.items():
        if ref in env or FLAT.fullmatch(ref) or f.layer != "F.Cu":
            continue
        lib = f.lib.split(":")[-1]
        top = NO_MODEL.get(ref, NO_MODEL_LIB.get(lib))
        cy = bg.courtyard(ref)
        if top is None or cy is None:
            raise RuntimeError(f"{ref} ({lib}) has no 3D model and no NO_MODEL height")
        env[ref] = [(*cy, top)]
    for ref, top in TOP_OVERRIDE.items():
        xs0 = min(b[0] for b in env[ref]); xs1 = max(b[1] for b in env[ref])
        ys0 = min(b[2] for b in env[ref]); ys1 = max(b[3] for b in env[ref])
        env[ref] = [(xs0, xs1, ys0, ys1, top)]
    _ENV = env
    return env
```

- [ ] **Step 4: Run, expect pass** — `.venv/bin/python -m unittest test_components -v` → 5 OK (the first run exports the STEP, about 3 s).

- [ ] **Step 5: Commit** — `git add enclosure/components.py enclosure/test_components.py && git commit -m "components: stepped part envelopes from KiCad's STEP export" -m "Refs: W-1038"`

---

### Task 3: The lattice

**Files:**
- Modify: `enclosure/params.py` (append a lattice section)
- Create: `enclosure/lattice.py`
- Test: `enclosure/test_lattice.py`

**Interfaces:**
- Consumes: Task 1 readers, `components.envelopes()`, `case._rounded_box`, `case._grown`, `case.connector_solid`, `case.tray`, `test_printable.upward_flats`.
- Produces: `lattice.lattice() -> Part`, `lattice.cell_bounds() -> dict[(r, c), (x0, x1, y0, y1)]`, `lattice.wall_lines() -> (xs, ys)`, `lattice.d3_window() -> (x0, x1, y0, y1)`, `lattice.leaky_walls(part) -> list[str]`, `lattice.H`.

- [ ] **Step 1: Params** — append to `enclosure/params.py`:

```python
# --- light-guide lattice ---------------------------------------------------
# docs/superpowers/specs/2026-10-14-light-guide-lattice-design.md. Its height
# is CAVITY: the panel rests on lattice and mini-DIN shells together.

LATTICE_WALL = 0.80
"""Cell walls: two 0.4mm perimeters, opaque in black (Wells, 2026-10-14; his
earlier hand-drawn lattice used 0.60)."""

LATTICE_INSET = 0.25
"""Lattice outline inside the board outline; with POCKET_CLEAR that is 0.55
to the tray walls."""

CELL_R = 1.00
"""Inner corner radius of each cell, as Wells's earlier lattice had."""

POST_D = 8.00
"""Posts at H1-H4: a 0.80 wall round the standoff pocket."""

NUT_POCKET_D = 6.40
NUT_POCKET_DEPTH = 3.30
"""Pocket in each post top for the panel's Würth 9774030151R standoff (Ø5.1
x 3.0, soldered to a 6.0 land): the land plus fillet, and 0.3 below the
standoff's end so the panel bears on the lattice, not on the standoffs."""

PART_CLEAR = 0.40
PART_TOP_CLEAR = 0.50
"""Grown onto each part envelope sideways / on top (components.py)."""

D3_TUBE_CLEAR = 0.25
"""The power LED's tube: the panel's FR4 window plus this, inside."""

WEST_RAIL_X1 = 7.00
EAST_RAIL_X0 = 50.00
EAST_RAIL_X1 = 55.75
"""Rails forward to the H3 / H4 posts. East: over U1's shield, west of the
antenna keepout (56.00) by KEEPOUT_CLEAR."""

KEEPOUT_CLEAR = 0.25
```

- [ ] **Step 2: Write the failing tests** — `enclosure/test_lattice.py`:

```python
"""The light-guide lattice: fit, light paths, clearances, printability."""
import unittest

from build123d import Box, Cylinder, Pos, Vector

import board_geom as bg
import case
import components
import lattice
import params as p
from test_printable import upward_flats

H = p.CAVITY


def _vol(a, b):
    x = a & b
    return x.volume if x is not None else 0.0


class TestLattice(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.L = lattice.lattice()

    def test_one_valid_solid_cavity_tall(self):
        self.assertTrue(self.L.is_valid)
        self.assertEqual(len(self.L.solids()), 1)
        bb = self.L.bounding_box()
        self.assertAlmostEqual(bb.min.Z, 0.0, places=3)
        self.assertAlmostEqual(bb.max.Z, H, places=3)
        self.assertGreaterEqual(bb.min.X, p.LATTICE_INSET - 1e-3)
        self.assertLessEqual(bb.max.X, p.BOARD_W - p.LATTICE_INSET + 1e-3)

    def test_every_lit_dot_has_an_open_column_the_dot_wide(self):
        r = bg.panel_grid()["dot_dia"] / 2 - 0.01
        for d in bg.panel_dots():
            if d["led"]:
                col = Pos(d["x"], d["y"], H / 2) * Cylinder(r, H + 2)
                self.assertLess(_vol(self.L, col), 1e-3, f"r{d['r']}c{d['c']}")

    def test_the_test_plunger_cell_is_open(self):
        (t,) = [d for d in bg.panel_dots() if d["mat"] == "T"]
        col = Pos(t["x"], t["y"], H / 2) * Cylinder(bg.panel_grid()["hole_dia"] / 2, H + 2)
        self.assertLess(_vol(self.L, col), 1e-3)

    def test_posts_bores_and_standoff_pockets(self):
        for ref, (x, y) in bg.mount_holes().items():
            bore = Pos(x, y, H / 2) * Cylinder(p.SCREW_CLEAR_D / 2 - 0.05, H + 2)
            self.assertLess(_vol(self.L, bore), 1e-3, ref)
            nut = Pos(x, y, H - 1.5) * Cylinder(5.1 / 2 + 0.1, 3.0)
            self.assertLess(_vol(self.L, nut), 1e-3, ref)
            ring = Pos(x, y, 6) * (Cylinder(3.5, 4) - Cylinder(1.5, 4))
            frac = _vol(self.L, ring) / ring.volume
            self.assertGreater(frac, 0.95 if ref in ("H1", "H2") else 0.35, ref)

    def test_clears_every_part_shadow(self):
        for ref, bands in components.envelopes().items():
            if ref in ("J1", "J2", "J4"):
                continue
            for x0, x1, y0, y1, top in bands:
                shadow = Pos((x0 + x1) / 2, (y0 + y1) / 2, top / 2) * Box(x1 - x0, y1 - y0, top)
                self.assertLess(_vol(self.L, shadow), 1e-3, ref)
        for ref in ("J1", "J2", "J4"):
            self.assertLess(_vol(self.L, case.connector_solid(ref)), 1e-3, ref)

    def test_clears_the_antenna_keepout_in_front_of_the_grid(self):
        (zone,) = bg.keepout_zones("U1")
        xs = [q[0] for q in zone]; ys = [q[1] for q in zone]
        y1 = min(max(ys), lattice.front_line() - p.LATTICE_WALL / 2)
        ko = Pos((min(xs) + max(xs)) / 2, (min(ys) + y1) / 2, H / 2) * Box(
            max(xs) - min(xs), y1 - min(ys), H)
        self.assertLess(_vol(self.L, ko), 1e-3)

    def test_clears_the_reset_paperclip_path(self):
        _, y = bg.to_case(bg.footprint("SW1").x, bg.footprint("SW1").y)
        path = Pos(p.BOARD_W - 1.0, y, p.RESET_ACTUATOR_Z) * Cylinder(
            p.RESET_PINHOLE_D / 2, 3.0, rotation=(0, 90, 0))
        self.assertLess(_vol(self.L, path), 1e-3)

    def test_fits_the_tray(self):
        self.assertLess(_vol(self.L, case.tray()), 1e-3)

    def test_d3_tube_is_open_over_the_window_and_walled_all_round(self):
        x0, x1, y0, y1 = lattice.d3_window()
        col = Pos((x0 + x1) / 2, (y0 + y1) / 2, H / 2) * Box(x1 - x0, y1 - y0, H + 2)
        self.assertLess(_vol(self.L, col), 1e-3)
        c = p.D3_TUBE_CLEAR
        ix0, ix1, iy0, iy1 = x0 - c, x1 + c, y0 - c, y1 + c
        t = 0.6 - 0.01  # the thinnest wall the spec allows
        pts = []
        for k in range(1, 10):
            fx = ix0 + k / 10 * (ix1 - ix0)
            fy = iy0 + k / 10 * (iy1 - iy0)
            pts += [(fx, iy0 - t), (fx, iy1 + t), (ix0 - t, fy), (ix1 + t, fy)]
        d = t / 2 ** 0.5
        pts += [(ix0 - d, iy0 - d), (ix1 + d, iy0 - d), (ix0 - d, iy1 + d), (ix1 + d, iy1 + d)]
        for px, py in pts:
            self.assertTrue(self.L.is_inside(Vector(px, py, H - 0.5)),
                            f"no wall at ({px:.2f}, {py:.2f})")

    def test_grid_walls_are_lattice_wall_thick(self):
        xs, _ = lattice.wall_lines()
        x0, x1, y0, y1 = lattice.cell_bounds()[(2, 3)]
        probe = Pos(xs[4], (y0 + y1) / 2, 6.5) * Box(2.0, 2.0, 1.0)
        self.assertAlmostEqual(_vol(self.L, probe), p.LATTICE_WALL * 2.0, delta=0.05)

    def test_no_notch_joins_two_lit_cells(self):
        self.assertEqual([w for w in lattice.leaky_walls(self.L) if "lit|lit" in w], [])

    def test_d3_window_is_the_one_over_d3(self):
        x0, x1, y0, y1 = lattice.d3_window()
        f = bg.footprint("D3"); x, y = bg.to_case(f.x, f.y)
        self.assertTrue(x0 < x < x1 and y0 < y < y1)

    def test_prints_panel_face_down_without_supports(self):
        flats = upward_flats(self.L, ceiling_z=H)
        self.assertEqual([(round(f.center().X, 1), round(f.center().Y, 1),
                           round(f.center().Z, 1), round(f.area, 1)) for f in flats], [])
```

- [ ] **Step 3: Run, expect failure** — `.venv/bin/python -m unittest test_lattice -v` → ModuleNotFoundError: lattice.

- [ ] **Step 4: Implement** — `enclosure/lattice.py`:

```python
"""The light-guide lattice: the printed part between the main board and the
dot-grid art panel in the Pro build.

docs/superpowers/specs/2026-10-14-light-guide-lattice-design.md. An egg-crate
grid of open cells on the dot pitch, posts at H1-H4 carrying the panel's
standoffs, side rails forward to the H3/H4 posts, and a tube up to the panel's
FR4 window over the power LED D3. Everything is read from the board, the panel
layout and KiCad's STEP export (components.py), then the parts' shadows, the
connector hulls, U1's antenna keepout and SW1's reset path are cut away.
Printed panel-face down."""
import functools
import operator

from build123d import Box, Cone, Cylinder, Part, Pos, RectangleRounded, extrude

import board_geom as bg
import case
import components
import params as p

H = p.CAVITY


def _union(parts):
    return functools.reduce(operator.add, parts)


def _box(x0, x1, y0, y1, z0, z1) -> Part:
    return Pos((x0 + x1) / 2, (y0 + y1) / 2, (z0 + z1) / 2) * Box(x1 - x0, y1 - y0, z1 - z0)


def wall_lines() -> tuple[list[float], list[float]]:
    """Case-frame lines midway between dot centres: 8 in X (west to east) and
    9 in Y (back to front)."""
    g = bg.panel_grid()
    (ox, oy), pitch = g["origin"], g["pitch"]
    xs = [bg.panel_to_case(ox + (c - 0.5) * pitch, 0)[0] for c in range(g["cols"] + 1)]
    ys = [bg.panel_to_case(0, oy + (r - 0.5) * pitch)[1] for r in range(g["rows"] + 1)]
    return xs, ys


def front_line() -> float:
    """The grid's front wall centre: the line in front of the last row."""
    return wall_lines()[1][-1]


def cell_bounds() -> dict[tuple[int, int], tuple[float, float, float, float]]:
    """(r, c) -> (x0, x1, y0, y1), each cell's open interior. Outer cells run
    out to the inset outline less one wall."""
    xs, ys = wall_lines()
    g = bg.panel_grid()
    w, i = p.LATTICE_WALL, p.LATTICE_INSET
    out = {}
    for r in range(g["rows"]):
        for c in range(g["cols"]):
            x0 = i + w if c == 0 else xs[c] + w / 2
            x1 = p.BOARD_W - i - w if c == g["cols"] - 1 else xs[c + 1] - w / 2
            y1 = p.BOARD_L - i - w if r == 0 else ys[r] - w / 2
            y0 = ys[r + 1] + w / 2
            out[(r, c)] = (x0, x1, y0, y1)
    return out


def _outline() -> Part:
    i = p.LATTICE_INSET
    return case._rounded_box(p.BOARD_W - 2 * i, p.BOARD_L - 2 * i, H,
                             p.BOARD_W / 2, p.BOARD_L / 2, H / 2, p.CORNER_R - i)


def _grid() -> Part:
    block = _box(0, p.BOARD_W, front_line() - p.LATTICE_WALL / 2, p.BOARD_L, 0, H)
    cells = [Pos((x0 + x1) / 2, (y0 + y1) / 2, H / 2) * extrude(
        RectangleRounded(x1 - x0, y1 - y0, p.CELL_R), H / 2 + 1, both=True)
        for x0, x1, y0, y1 in cell_bounds().values()]
    return block - _union(cells)


def _posts() -> Part:
    return _union([Pos(x, y, H / 2) * Cylinder(p.POST_D / 2, H)
                   for x, y in bg.mount_holes().values()])


def _post_voids() -> Part:
    """Bore through each post; the standoff pocket from the top; a 45-degree
    cone from the pocket floor down into the bore, so printed panel-face down
    the pocket floor is not a flat ceiling."""
    rb, rp = p.SCREW_CLEAR_D / 2, p.NUT_POCKET_D / 2
    z_floor = H - p.NUT_POCKET_DEPTH
    out = []
    for x, y in bg.mount_holes().values():
        out.append(Pos(x, y, H / 2) * Cylinder(rb, H + 2))
        out.append(Pos(x, y, z_floor + (p.NUT_POCKET_DEPTH + 1) / 2) * Cylinder(rp, p.NUT_POCKET_DEPTH + 1))
        out.append(Pos(x, y, z_floor - (rp - rb) / 2) * Cone(rb, rp, rp - rb))
    return _union(out)


def _rails() -> Part:
    yf = front_line()
    (_, y3), (_, y4) = bg.mount_holes()["H3"], bg.mount_holes()["H4"]
    return (_box(p.LATTICE_INSET, p.WEST_RAIL_X1, y3, yf, 0, H)
            + _box(p.EAST_RAIL_X0, p.EAST_RAIL_X1, y4, yf, 0, H))


def d3_window() -> tuple[float, float, float, float]:
    """The panel FR4 window that D3 sits under."""
    f = bg.footprint("D3")
    x, y = bg.to_case(f.x, f.y)
    hits = [w for w in bg.fr4_windows() if w[0] < x < w[1] and w[2] < y < w[3]]
    if len(hits) != 1:
        raise RuntimeError(f"expected one FR4 window over D3, found {len(hits)}")
    return hits[0]


def _d3_tube() -> tuple[Part, Part]:
    """(tube + arm to the H4 post, the tube's void)."""
    x0, x1, y0, y1 = d3_window()
    c, w = p.D3_TUBE_CLEAR, p.LATTICE_WALL
    ix0, ix1, iy0, iy1 = x0 - c, x1 + c, y0 - c, y1 + c
    tube = _box(ix0 - w, ix1 + w, iy0 - w, iy1 + w, 0, H)
    hx, hy = bg.mount_holes()["H4"]
    arm = _box(hx, ix0, iy1 - 1.0, hy, 0, H)
    return tube + arm, _box(ix0, ix1, iy0, iy1, -1, H + 1)


def _part_voids() -> Part:
    """Every part's shadow (components.py), grown by PART_CLEAR and
    PART_TOP_CLEAR, from below the board to its top. The connectors use their
    vendor hulls grown by PORT_CLEAR instead, as the tray does."""
    c = p.PART_CLEAR
    out = []
    for ref, bands in components.envelopes().items():
        if ref in ("J1", "J2", "J4"):
            continue
        for x0, x1, y0, y1, top in bands:
            out.append(_box(x0 - c, x1 + c, y0 - c, y1 + c, -1, top + p.PART_TOP_CLEAR))
    out += [case._grown(ref, p.PORT_CLEAR) for ref in ("J1", "J2", "J4")]
    return _union(out)


def _keepout_void() -> Part:
    """U1's antenna keepout, grown by KEEPOUT_CLEAR, clipped at the grid's
    front wall: the zone runs 1.85 into the grid, but the module's antenna
    ends 1.7 short of it, and cutting the grid there opened a lit cell."""
    (zone,) = bg.keepout_zones("U1")
    k = p.KEEPOUT_CLEAR
    xs = [q[0] for q in zone]
    ys = [q[1] for q in zone]
    y1 = min(max(ys) + k, front_line() - p.LATTICE_WALL / 2)
    return _box(min(xs) - k, max(xs) + k, min(ys) - k, y1, -1, H + 1)


def _reset_path_void() -> Part:
    """SW1's reset paperclip path, from the switch out through the east edge,
    and its shadow down to the board."""
    _, y = bg.to_case(bg.footprint("SW1").x, bg.footprint("SW1").y)
    half = p.RESET_PINHOLE_D / 2 + p.PART_CLEAR
    x0 = bg.courtyard("SW1")[1] - 1.0
    return _box(x0, p.BOARD_W + 1, y - half, y + half, -1, p.RESET_ACTUATOR_Z + half)


def lattice() -> Part:
    tube, tube_void = _d3_tube()
    body = (_grid() + _posts() + _rails() + tube) & _outline()
    return (body - _post_voids() - tube_void - _part_voids() - _keepout_void()
            - _reset_path_void())


def leaky_walls(part: Part) -> list[str]:
    """Each lit cell's walls that a clearance notch has opened, as
    "r{r}c{c} {side} lit|lit" or "...lit|dark" (dark: an unlit cell, or
    outside the grid). Lit-to-dark bleed is accepted (the spec); lit-to-lit is
    what a viewer would see."""
    g = bg.panel_grid()
    lit = {(d["r"], d["c"]) for d in bg.panel_dots() if d["led"]}
    cells = cell_bounds()
    w = p.LATTICE_WALL
    out = []
    for (r, c) in sorted(lit):
        x0, x1, y0, y1 = cells[(r, c)]
        sides = {"west": ((x0 - w, x0, y0, y1), (r, c - 1)),
                 "east": ((x1, x1 + w, y0, y1), (r, c + 1)),
                 "back": ((x0, x1, y1, y1 + w), (r - 1, c)),
                 "front": ((x0, x1, y0 - w, y0), (r + 1, c))}
        for side, ((a, b, cc, d), nb) in sides.items():
            probe = _box(a, b, cc, d, 0, H)
            got = part & probe
            if (got.volume if got is not None else 0.0) < 0.98 * probe.volume:
                kind = "lit|lit" if nb in lit else "lit|dark"
                out.append(f"r{r}c{c} {side} {kind}")
    return out
```

- [ ] **Step 5: Run, expect pass** — `.venv/bin/python -m unittest test_lattice -v` (about 1 min). If a test fails, fix the geometry, not the test, unless the test contradicts the spec.

- [ ] **Step 6: Commit** — `git add enclosure/params.py enclosure/lattice.py enclosure/test_lattice.py && git commit -m "Enclosure: light-guide lattice" -m "Refs: W-1038"`

---

### Task 4: Export, docs, first look

**Files:**
- Modify: `enclosure/export.py`, `enclosure/README.md`, `docs/superpowers/specs/2026-10-14-light-guide-lattice-design.md` (keepout deviation), `docs/HANDOFF.md`
- Output: `enclosure/out/airwriter-lattice.stl`

- [ ] **Step 1: Export** — in `export.py`, `import lattice` and add `"airwriter-lattice": lattice.lattice,` to `PARTS`; after the loop print `lattice.leaky_walls(...)` for the lattice part. Run `./build.sh`; expect every test OK and three parts written.

- [ ] **Step 2: README** — a `## Light-guide lattice (Pro)` section after Hardware: what it is; print panel-face down in matte black, no supports; sits on the board, panel on top; screws pass through the posts; Pro screws M2.5 × 18; first-print checks (bleed between lit dots by eye; the panel seats flush on lattice and shells together; the posts drop over the screws). Add `out/airwriter-lattice.stl` to the parts list at the top.

- [ ] **Step 3: Spec deviation** — in the spec's §3 "Antenna keepout" bullet, replace "The grid's front-east corner and the H4 post lose a sliver" with: "clipped at the grid's front wall (the zone overlaps the grid by 1.85 mm, the antenna ends 1.7 mm short of the grid, and cutting there opened lit cell r7c6); the H4 post loses a sliver." In §2 "SW3", note the stepped envelopes (components.py).

- [ ] **Step 4: Render and look** — render top/underside/iso views of `out/airwriter-lattice.stl` and check: closed cells over every lit dot; posts with pockets; the D3 tube walled all round.

- [ ] **Step 5: Commit** — `git add -A enclosure/export.py enclosure/README.md enclosure/out/airwriter-lattice.stl docs/ && git commit -m "Enclosure: export the lattice; README, spec deviation, HANDOFF" -m "Refs: W-1038"`
