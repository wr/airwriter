# AirWriter Enclosure Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A parametric build123d model in `enclosure/` that exports a printable tray shared by both AirWriter builds and a Lite lid that replaces the dot-grid art panel, with every board-derived dimension read out of `airwriter.kicad_pcb` at build time.

**Architecture:** Three Python modules and a shell driver. `board_geom.py` parses the KiCad board and hands back positions in a case-local right-handed frame. `params.py` holds the numbers this project *chooses* (wall, floor, clearances), each commented with where it came from. `case.py` builds two solids — `tray()` and `lite_lid()` — using `board_geom` for anything the board decides and importing the real connector STEP files to cut the port openings, so no connector diameter is ever guessed. `build.sh` exports STEP and STL and runs the printability assertions.

**Tech Stack:** Python 3.14, build123d 0.11.1 (OCCT via cadquery-ocp), `unittest` (matching `hardware/panel/test_*.py` — the repo has no pytest), plain `sh` for the driver.

**Spec:** `docs/superpowers/specs/2026-09-08-airwriter-enclosure-design.md`

## Global Constraints

- **Wall thickness: uniform 3.00 mm.** Nominal; the 0.20 mm pocket clearance makes the effective wall 2.80 mm.
- **External size: 68.00 × 110.27 × 21.94 mm.**
- **Cavity, board top face → lid underside: 14.34 mm.** Not a free variable (mini-DIN shells 12.74 + SMTSO nut 1.6).
- **Floor 2.40 mm. Board standoffs 2.00 mm. PCB 1.60 mm. Panel/lid 1.60 mm.**
- **Board pocket clearance: 0.20 mm per side.**
- **Print process assumed: FDM, 0.4 mm nozzle, 0.2 mm layers, tray printed floor-down, no supports.**
- **Screws: M2.5. Pro M2.5 × 20 into the panel's SMTSO nuts; Lite M2.5 × 8 into heat-set inserts at the bottom of each lid leg.**
- **Never modify anything under `hardware/`.** This project reads the board; it does not write it.
- Board file is at `hardware/airwriter/airwriter.kicad_pcb`; connector STEPs at `hardware/airwriter/lib/airwriter.3dshapes/`.

### The case-local coordinate frame (used by every task)

KiCad board coordinates have +y running *down* the page. Using them directly in CAD gives a left-handed frame. Every module in this project works in a right-handed **case frame**:

```
X = kicad_x - 100.0            # 0 .. 62.00,   X=0 is the WEST wall (S1's DIP)
Y = 164.27 - kicad_y           # 0 .. 104.27,  Y=0 is the FRONT wall (the ports)
Z = 0 at the board's TOP face, +Z up
```

Derived Z landmarks: board bottom −1.60, floor top −3.60, floor bottom −6.00, lid underside +14.34, wall top +15.94.

Everything below is quoted in the case frame unless it says "KiCad".

---

### Task 1: Toolchain and the board-geometry reader

**Files:**
- Create: `enclosure/requirements.txt`
- Create: `enclosure/board_geom.py`
- Create: `enclosure/test_board_geom.py`
- Modify: `.gitignore` (append the venv)

**Interfaces:**
- Consumes: nothing.
- Produces:
  - `BOARD_PCB: pathlib.Path` — resolved path to `airwriter.kicad_pcb`
  - `outline() -> tuple[float, float, float, float]` — `(min_x, min_y, max_x, max_y)` in **KiCad** mm
  - `to_case(kx: float, ky: float) -> tuple[float, float]` — KiCad mm → case-frame X, Y
  - `footprint(ref: str) -> Footprint` where `Footprint` is a `dataclass` with fields `ref: str`, `x: float`, `y: float` (KiCad mm), `rot: float` (degrees), `lib: str`, `model: str | None` (absolute path to its 3D model, or None)
  - `local_to_board(fp: Footprint, lx: float, ly: float) -> tuple[float, float]` — footprint-local mm → KiCad mm
  - `mount_holes() -> dict[str, tuple[float, float]]` — `{"H1": (X, Y), ...}` in the **case frame**

- [ ] **Step 1: Create the venv and pin the dependency**

```bash
cd ~/Projects/airwriter/enclosure 2>/dev/null || mkdir -p ~/Projects/airwriter/enclosure && cd ~/Projects/airwriter/enclosure
printf 'build123d==0.11.1\n' > requirements.txt
python3 -m venv .venv
.venv/bin/pip install -q -r requirements.txt
printf '\n# enclosure\nenclosure/.venv/\nenclosure/out/\n' >> ../.gitignore
```

- [ ] **Step 2: Smoke-test the toolchain before writing anything against it**

This exists to catch build123d API drift immediately rather than in Task 3.

Run:

```bash
cd ~/Projects/airwriter/enclosure && .venv/bin/python -c "
from build123d import Box, export_step, export_stl
b = Box(10, 20, 30)
print('volume', b.volume)
bb = b.bounding_box()
print('size', bb.size.X, bb.size.Y, bb.size.Z)
print('valid', b.is_valid)
"
```

Expected: `volume 6000.0`, `size 10.0 20.0 30.0`, `valid True`.

If any name here does not exist, STOP and report the actual build123d 0.11.1 API before continuing — every later task uses `Box`, `bounding_box()`, `.volume`, `.is_valid`, `export_step`, `export_stl`.

- [ ] **Step 3: Write the failing test**

Create `enclosure/test_board_geom.py`:

```python
"""The board is the source of truth. These values were read out of
hardware/airwriter/airwriter.kicad_pcb on 2026-09-08; if the board moves,
this test is supposed to fail."""
import unittest

import board_geom as bg


class TestOutline(unittest.TestCase):
    def test_outline_is_62_by_104_27(self):
        min_x, min_y, max_x, max_y = bg.outline()
        self.assertAlmostEqual(min_x, 100.000, places=3)
        self.assertAlmostEqual(min_y, 60.000, places=3)
        self.assertAlmostEqual(max_x, 162.000, places=3)
        self.assertAlmostEqual(max_y, 164.270, places=3)

    def test_case_frame_flips_y_and_is_right_handed(self):
        # KiCad top-left corner -> case (0, 104.27); KiCad bottom-left -> case (0, 0).
        self.assertEqual(bg.to_case(100.0, 60.0), (0.0, 104.27))
        self.assertEqual(bg.to_case(100.0, 164.27), (0.0, 0.0))


class TestFootprints(unittest.TestCase):
    def test_known_positions(self):
        for ref, kx, ky, rot in [
            ("J2", 113.000, 167.020, 180.0),
            ("J4", 132.500, 167.020, 180.0),
            ("J1", 152.000, 162.120, 0.0),
            ("S1", 105.005, 134.320, 90.0),
            ("SW1", 157.225, 99.454, 0.0),
            ("SW3", 133.250, 87.750, -90.0),
        ]:
            fp = bg.footprint(ref)
            self.assertAlmostEqual(fp.x, kx, places=3, msg=ref)
            self.assertAlmostEqual(fp.y, ky, places=3, msg=ref)
            self.assertAlmostEqual(fp.rot % 360, rot % 360, places=3, msg=ref)

    def test_minidin_carries_a_step_model(self):
        fp = bg.footprint("J2")
        self.assertIsNotNone(fp.model)
        self.assertTrue(fp.model.endswith(".step"), fp.model)

    def test_s1_key_tips_reach_x_97_80(self):
        # S1's footprint origin is the centre of the pin field (pads at +/-8.89,
        # +/-3.81). Body 9.61 deep centred on the rows -> front face at local
        # y = -4.805; keys protrude 2.40 past it -> local y = -7.205. At rot 90
        # that maps to KiCad x = 105.005 - 7.205. HANDOFF records "x=97.8".
        fp = bg.footprint("S1")
        x, _ = bg.local_to_board(fp, 0.0, -7.205)
        self.assertAlmostEqual(x, 97.800, places=3)

    def test_mount_holes_in_case_frame(self):
        holes = bg.mount_holes()
        self.assertEqual(set(holes), {"H1", "H2", "H3", "H4"})
        for ref, cx, cy in [
            ("H1", 5.636, 98.634),
            ("H2", 56.364, 98.634),
            ("H3", 5.636, 12.340),
            ("H4", 53.364, 12.340),
        ]:
            gx, gy = holes[ref]
            self.assertAlmostEqual(gx, cx, places=3, msg=ref)
            self.assertAlmostEqual(gy, cy, places=3, msg=ref)

    def test_mount_pattern_is_asymmetric_at_h4(self):
        # The tray keys off this: H4 is 3.0mm west of where a symmetric
        # pattern would put it, so the board only goes in one way.
        holes = bg.mount_holes()
        self.assertNotAlmostEqual(holes["H4"][0], holes["H2"][0], places=2)


if __name__ == "__main__":
    unittest.main()
```

- [ ] **Step 4: Run the test and watch it fail**

Run: `cd ~/Projects/airwriter/enclosure && .venv/bin/python -m unittest -v test_board_geom`
Expected: FAIL — `ModuleNotFoundError: No module named 'board_geom'`

- [ ] **Step 5: Write `board_geom.py`**

Create `enclosure/board_geom.py`:

```python
"""Read the AirWriter main board's geometry straight out of the KiCad file.

Nothing in this project retypes a board dimension. If a footprint moves, the
case re-cuts itself on the next build instead of quietly mismatching.

The parser is deliberately small: KiCad's s-expression board file is read as
text, top-level `(footprint ...)` blocks are bracket-matched, and only the
fields this project needs are pulled out. It does NOT need pcbnew, which only
exists inside KiCad's own bundled Python.
"""
from __future__ import annotations

import math
import pathlib
import re
from dataclasses import dataclass

REPO = pathlib.Path(__file__).resolve().parent.parent
BOARD_PCB = REPO / "hardware" / "airwriter" / "airwriter.kicad_pcb"
SHAPES = REPO / "hardware" / "airwriter" / "lib" / "airwriter.3dshapes"

# The case frame's origin in KiCad coordinates: the board outline's west edge
# and its FRONT (port) edge. Y is flipped so the frame is right-handed.
ORIGIN_X = 100.00
ORIGIN_Y = 164.27


@dataclass(frozen=True)
class Footprint:
    ref: str
    x: float
    y: float
    rot: float
    lib: str
    model: str | None


def _text() -> str:
    return BOARD_PCB.read_text()


def _blocks(src: str, opener: str):
    """Yield every top-level `(<opener> ...)` block, brackets matched, strings skipped."""
    needle = "\n\t(" + opener + " "
    i = 0
    while True:
        i = src.find(needle, i)
        if i < 0:
            return
        j = i + 2
        depth = 0
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
                    break
            j += 1
        yield src[i : j + 1]
        i = j


_COORD = re.compile(r"\((?:start|end|mid|xy) ([-\d.]+) ([-\d.]+)\)")


def outline() -> tuple[float, float, float, float]:
    """The Edge.Cuts bounding box in KiCad mm: (min_x, min_y, max_x, max_y)."""
    src = _text()
    pts: list[tuple[float, float]] = []
    for kind in ("line", "rect", "arc", "poly"):
        for blk in _blocks(src, "gr_" + kind):
            if '"Edge.Cuts"' in blk:
                pts += [(float(a), float(b)) for a, b in _COORD.findall(blk)]
    if not pts:
        raise RuntimeError(f"no Edge.Cuts geometry in {BOARD_PCB}")
    xs = [p[0] for p in pts]
    ys = [p[1] for p in pts]
    return min(xs), min(ys), max(xs), max(ys)


def to_case(kx: float, ky: float) -> tuple[float, float]:
    """KiCad mm -> case-frame X, Y. Y flips, so the frame is right-handed."""
    return round(kx - ORIGIN_X, 6), round(ORIGIN_Y - ky, 6)


_REF = re.compile(r'\(property "Reference" "([^"]+)"')
_AT = re.compile(r"\n\t\t\(at ([-\d.]+) ([-\d.]+)(?: ([-\d.]+))?\)")
_LIB = re.compile(r'\(footprint "([^"]+)"')
_MODEL = re.compile(r'\(model "([^"]+)"')


def _all() -> dict[str, Footprint]:
    src = _text()
    out: dict[str, Footprint] = {}
    for blk in _blocks(src, "footprint"):
        ref = _REF.search(blk)
        at = _AT.search(blk)
        if not ref or not at:
            continue
        model = _MODEL.search(blk)
        path = None
        if model:
            path = model.group(1).replace("${KIPRJMOD}", str(BOARD_PCB.parent))
        out[ref.group(1)] = Footprint(
            ref=ref.group(1),
            x=float(at.group(1)),
            y=float(at.group(2)),
            rot=float(at.group(3) or 0.0),
            lib=_LIB.search(blk).group(1),
            model=path,
        )
    return out


_CACHE: dict[str, Footprint] | None = None


def footprint(ref: str) -> Footprint:
    global _CACHE
    if _CACHE is None:
        _CACHE = _all()
    try:
        return _CACHE[ref]
    except KeyError:
        raise KeyError(f"{ref} is not a footprint on {BOARD_PCB.name}") from None


def local_to_board(fp: Footprint, lx: float, ly: float) -> tuple[float, float]:
    """Footprint-local mm -> KiCad mm.

    KiCad's positive footprint rotation is counter-clockwise on a screen whose
    y runs down, which makes the board-space transform:
        x = px + lx*cos + ly*sin
        y = py - lx*sin + ly*cos
    """
    t = math.radians(fp.rot)
    c, s = math.cos(t), math.sin(t)
    return fp.x + lx * c + ly * s, fp.y - lx * s + ly * c


def mount_holes() -> dict[str, tuple[float, float]]:
    """H1-H4 in the case frame."""
    return {r: to_case(footprint(r).x, footprint(r).y) for r in ("H1", "H2", "H3", "H4")}
```

- [ ] **Step 6: Run the test and watch it pass**

Run: `cd ~/Projects/airwriter/enclosure && .venv/bin/python -m unittest -v test_board_geom`
Expected: PASS, 7 tests.

- [ ] **Step 7: Commit**

```bash
cd ~/Projects/airwriter
git add enclosure/requirements.txt enclosure/board_geom.py enclosure/test_board_geom.py .gitignore
git commit -m "Enclosure: read board geometry out of the KiCad file"
```

---

### Task 2: The chosen numbers, in one place

**Files:**
- Create: `enclosure/params.py`
- Create: `enclosure/test_params.py`

**Interfaces:**
- Consumes: `board_geom.outline()`
- Produces: module-level constants `WALL`, `FLOOR`, `STANDOFF`, `PCB`, `PANEL`, `CAVITY`, `POCKET_CLEAR`, `SCREW_CLEAR_D`, `SCREW_HEAD_D`, `INSERT_D`, `INSERT_DEPTH`, `PORT_CLEAR`, `RESET_PINHOLE_D`, `PLUNGER_BORE_D`, `DIP_SLOT_Z0`, `DIP_SLOT_Z1`, `DIP_SLOT_MARGIN`, and derived `BOARD_W`, `BOARD_L`, `EXT_W`, `EXT_L`, `EXT_H`, `Z_BOARD_BOTTOM`, `Z_FLOOR_TOP`, `Z_FLOOR_BOTTOM`, `Z_LID_UNDER`, `Z_WALL_TOP`.

- [ ] **Step 1: Write the failing test**

Create `enclosure/test_params.py`:

```python
"""params.py is where this project's *choices* live. The spec fixes these
numbers; this test is the spec's teeth."""
import unittest

import params as p


class TestFixedByTheSpec(unittest.TestCase):
    def test_wall_floor_and_stack(self):
        self.assertEqual(p.WALL, 3.00)
        self.assertEqual(p.FLOOR, 2.40)
        self.assertEqual(p.STANDOFF, 2.00)
        self.assertEqual(p.PCB, 1.60)
        self.assertEqual(p.PANEL, 1.60)
        self.assertEqual(p.CAVITY, 14.34)
        self.assertEqual(p.POCKET_CLEAR, 0.20)

    def test_cavity_is_the_minidin_plus_the_smtso_nut(self):
        # 12.74 shell top + 1.60 nut = 14.34. Not a free variable.
        self.assertAlmostEqual(p.CAVITY, 12.74 + 1.60, places=6)


class TestDerived(unittest.TestCase):
    def test_board_size_comes_from_the_board(self):
        self.assertAlmostEqual(p.BOARD_W, 62.00, places=3)
        self.assertAlmostEqual(p.BOARD_L, 104.27, places=3)

    def test_external_size(self):
        self.assertAlmostEqual(p.EXT_W, 68.00, places=3)
        self.assertAlmostEqual(p.EXT_L, 110.27, places=3)
        self.assertAlmostEqual(p.EXT_H, 21.94, places=3)

    def test_z_landmarks(self):
        self.assertAlmostEqual(p.Z_BOARD_BOTTOM, -1.60, places=3)
        self.assertAlmostEqual(p.Z_FLOOR_TOP, -3.60, places=3)
        self.assertAlmostEqual(p.Z_FLOOR_BOTTOM, -6.00, places=3)
        self.assertAlmostEqual(p.Z_LID_UNDER, 14.34, places=3)
        self.assertAlmostEqual(p.Z_WALL_TOP, 15.94, places=3)

    def test_effective_wall_after_pocket_clearance(self):
        # 3.00 nominal, 0.20 eaten by pocket clearance -> 2.80 of plastic.
        self.assertAlmostEqual(p.WALL - p.POCKET_CLEAR, 2.80, places=3)


if __name__ == "__main__":
    unittest.main()
```

- [ ] **Step 2: Run the test and watch it fail**

Run: `cd ~/Projects/airwriter/enclosure && .venv/bin/python -m unittest -v test_params`
Expected: FAIL — `ModuleNotFoundError: No module named 'params'`

- [ ] **Step 3: Write `params.py`**

Create `enclosure/params.py`:

```python
"""Every number this project CHOOSES, with where it came from.

Anything the board decides lives in board_geom.py instead. If you are about to
add a board dimension here, you are about to introduce drift -- read it from
the board file.

Source for the rulings below:
docs/superpowers/specs/2026-09-08-airwriter-enclosure-design.md
"""
import board_geom as bg

# --- ruled by the spec -----------------------------------------------------

WALL = 3.00
"""Uniform, all four walls. Set by the mini-DIN mating faces, which stand
2.75mm proud of the board edge and want to land 0.25mm recessed. Wells ruled
uniform over honouring S1's 2.25mm west-wall placement, so S1's piano keys sit
0.75mm recessed -- spec section 3."""

FLOOR = 2.40
"""12 layers at 0.2mm."""

STANDOFF = 2.00
"""Board sits this far off the floor, clearing the through-hole tails of
J2/J4/S1/SW3/U7."""

PCB = 1.60
PANEL = 1.60
"""The art panel, and the Lite lid that stands in for it."""

CAVITY = 14.34
"""Board top face to lid underside. NOT a free variable: the mini-DIN shells
top out 12.74mm above the board face, the lattice is squashed to exactly that,
and the panel's PEM SMTSO-M25-3ET nuts stand 1.60mm proud of its underside."""

POCKET_CLEAR = 0.20
"""Per side, on the board pocket and the panel/lid seat."""

# --- fasteners -------------------------------------------------------------

SCREW_CLEAR_D = 2.80
"""M2.5 clearance."""

SCREW_HEAD_D = 5.00
"""Counterbore for an M2.5 pan head, in the floor's underside."""

SCREW_HEAD_DEPTH = 1.40

INSERT_D = 3.60
"""Bore for an M2.5 brass heat-set insert in the Lite lid's legs. Final MPN is
an open item in the spec; re-cut this if the chosen insert differs."""

INSERT_DEPTH = 4.50

# --- openings --------------------------------------------------------------

PORT_CLEAR = 0.40
"""Radial clearance added to each connector's own solid when it is subtracted
from the front wall. The spec's assertion floor is 0.30."""

USBC_CHAMFER = 1.20
"""45-degree chamfer on the OUTSIDE of the USB-C opening. The receptacle sits
1.5mm behind the wall, so a plug's overmould needs somewhere to go. Tuned on
the first print against a real cable -- open item 3 in the spec."""

RESET_PINHOLE_D = 1.20
"""East wall, on SW1's actuator axis."""

PLUNGER_BORE_D = 2.25
"""SW3's test plunger. Matches the art panel's own 2.25mm NPTH so the Lite lid
behaves identically -- the plunger is <= 2.0mm."""

DIP_SLOT_Z0 = 7.60
DIP_SLOT_Z1 = 9.50
"""S1's key slot, z above the board's top face. HANDOFF records the key tips
travelling 8.11-9.01; 0.5mm of reach is added each way so a pen tip can work a
key recessed 0.75mm. Confirm against the STEP interference check."""

DIP_SLOT_MARGIN = 0.60
"""Added at each end of the key band (the pad columns at footprint-local
x = -8.89 .. +8.89)."""

CARVEOUT_CLEAR = 0.50
"""Radial air added around an internal part's keep-out cylinder."""

# --- derived ---------------------------------------------------------------

_min_x, _min_y, _max_x, _max_y = bg.outline()
BOARD_W = round(_max_x - _min_x, 6)
BOARD_L = round(_max_y - _min_y, 6)

EXT_W = round(BOARD_W + 2 * WALL, 6)
EXT_L = round(BOARD_L + 2 * WALL, 6)

Z_BOARD_BOTTOM = -PCB
Z_FLOOR_TOP = Z_BOARD_BOTTOM - STANDOFF
Z_FLOOR_BOTTOM = Z_FLOOR_TOP - FLOOR
Z_LID_UNDER = CAVITY
Z_WALL_TOP = CAVITY + PANEL

EXT_H = round(Z_WALL_TOP - Z_FLOOR_BOTTOM, 6)

POCKET_W = round(BOARD_W + 2 * POCKET_CLEAR, 6)
POCKET_L = round(BOARD_L + 2 * POCKET_CLEAR, 6)
```

- [ ] **Step 4: Run the test and watch it pass**

Run: `cd ~/Projects/airwriter/enclosure && .venv/bin/python -m unittest -v test_params`
Expected: PASS, 6 tests.

- [ ] **Step 5: Commit**

```bash
cd ~/Projects/airwriter
git add enclosure/params.py enclosure/test_params.py
git commit -m "Enclosure: the chosen numbers, with provenance"
```

---

### Task 3: The tray's shell — floor, walls, pocket, lid seat

**Files:**
- Create: `enclosure/case.py`
- Create: `enclosure/test_case.py`

**Interfaces:**
- Consumes: `params` (all constants), `board_geom.to_case`
- Produces: `shell() -> Part` — the tray with no openings in it yet. Later tasks in this plan subtract from what `shell()` returns; `tray()` arrives in Task 6.

- [ ] **Step 1: Write the failing test**

Create `enclosure/test_case.py`:

```python
import unittest

import params as p
import case


class TestShell(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.s = case.shell()

    def test_is_a_valid_single_solid(self):
        self.assertTrue(self.s.is_valid)
        self.assertEqual(len(self.s.solids()), 1)

    def test_external_bounding_box(self):
        bb = self.s.bounding_box()
        self.assertAlmostEqual(bb.size.X, p.EXT_W, places=3)
        self.assertAlmostEqual(bb.size.Y, p.EXT_L, places=3)
        self.assertAlmostEqual(bb.size.Z, p.EXT_H, places=3)

    def test_sits_in_the_case_frame(self):
        # X=0 is the west wall's inner face line, so the solid starts at -WALL.
        bb = self.s.bounding_box()
        self.assertAlmostEqual(bb.min.X, -p.WALL, places=3)
        self.assertAlmostEqual(bb.min.Y, -p.WALL, places=3)
        self.assertAlmostEqual(bb.min.Z, p.Z_FLOOR_BOTTOM, places=3)
        self.assertAlmostEqual(bb.max.Z, p.Z_WALL_TOP, places=3)

    def test_cavity_is_hollow_from_floor_top_upward(self):
        # A probe block filling the pocket must not intersect the shell.
        from build123d import Box, Pos
        probe = Pos(p.BOARD_W / 2, p.BOARD_L / 2,
                    (p.Z_WALL_TOP + p.Z_FLOOR_TOP) / 2) * Box(
            p.POCKET_W, p.POCKET_L, p.Z_WALL_TOP - p.Z_FLOOR_TOP)
        self.assertAlmostEqual((self.s & probe).volume, 0.0, places=3)

    def test_floor_is_solid_under_the_pocket(self):
        from build123d import Box, Pos
        probe = Pos(p.BOARD_W / 2, p.BOARD_L / 2,
                    (p.Z_FLOOR_TOP + p.Z_FLOOR_BOTTOM) / 2) * Box(
            p.BOARD_W, p.BOARD_L, p.FLOOR)
        self.assertAlmostEqual((self.s & probe).volume, probe.volume, places=2)


if __name__ == "__main__":
    unittest.main()
```

- [ ] **Step 2: Run the test and watch it fail**

Run: `cd ~/Projects/airwriter/enclosure && .venv/bin/python -m unittest -v test_case`
Expected: FAIL — `ModuleNotFoundError: No module named 'case'`

- [ ] **Step 3: Write `shell()` in `case.py`**

Create `enclosure/case.py`:

```python
"""The AirWriter enclosure: a tray shared by both builds, and a Lite lid.

Everything is modelled in the case frame (see board_geom): X=0 is the west
wall's inner face, Y=0 is the front (port) wall's inner face, Z=0 is the
board's top face with +Z up.
"""
from build123d import Box, Part, Pos

import params as p


def shell() -> Part:
    """Floor plus four walls, hollow from the floor's top face upward.

    No openings: the port cutouts, the DIP slot, the pinhole, the screw holes
    and the carve-outs are all subtracted by tray().
    """
    outer = Pos(
        p.BOARD_W / 2,
        p.BOARD_L / 2,
        (p.Z_WALL_TOP + p.Z_FLOOR_BOTTOM) / 2,
    ) * Box(p.EXT_W, p.EXT_L, p.EXT_H)

    cavity = Pos(
        p.BOARD_W / 2,
        p.BOARD_L / 2,
        (p.Z_WALL_TOP + p.Z_FLOOR_TOP) / 2,
    ) * Box(p.POCKET_W, p.POCKET_L, p.Z_WALL_TOP - p.Z_FLOOR_TOP)

    return outer - cavity
```

- [ ] **Step 4: Run the test and watch it pass**

Run: `cd ~/Projects/airwriter/enclosure && .venv/bin/python -m unittest -v test_case`
Expected: PASS, 5 tests.

- [ ] **Step 5: Commit**

```bash
cd ~/Projects/airwriter
git add enclosure/case.py enclosure/test_case.py
git commit -m "Enclosure: tray shell"
```

---

### Task 4: Board standoffs and the screw stack

**Files:**
- Modify: `enclosure/case.py`
- Modify: `enclosure/test_case.py`

**Interfaces:**
- Consumes: `case.shell()`, `board_geom.mount_holes()`
- Produces: `standoffs() -> Part` (the four bosses, to be added), `screw_holes() -> Part` (the four clearance-plus-counterbore solids, to be subtracted)

- [ ] **Step 1: Write the failing test**

Append to `enclosure/test_case.py`, above the `if __name__` block:

```python
class TestStandoffsAndScrews(unittest.TestCase):
    def test_standoffs_sit_at_the_mount_holes_and_reach_the_board(self):
        import board_geom as bg
        s = case.standoffs()
        bb = s.bounding_box()
        self.assertAlmostEqual(bb.min.Z, p.Z_FLOOR_TOP, places=3)
        self.assertAlmostEqual(bb.max.Z, p.Z_BOARD_BOTTOM, places=3)
        self.assertEqual(len(s.solids()), 4)
        holes = bg.mount_holes()
        centres = sorted((round(sl.center().X, 3), round(sl.center().Y, 3))
                         for sl in s.solids())
        want = sorted((round(x, 3), round(y, 3)) for x, y in holes.values())
        self.assertEqual(centres, want)

    def test_screw_holes_pass_right_through_the_floor(self):
        h = case.screw_holes()
        bb = h.bounding_box()
        self.assertLessEqual(bb.min.Z, p.Z_FLOOR_BOTTOM)
        self.assertGreaterEqual(bb.max.Z, p.Z_BOARD_BOTTOM)
        self.assertEqual(len(h.solids()), 4)

    def test_counterbore_transition_is_coned_not_a_flat_ledge(self):
        # Printed floor-down, a flat annulus between a 5.0 counterbore and a
        # 2.8 clearance hole is an unsupported bridge. It must be a 45 cone.
        h = case.screw_holes()
        flats = [f for f in h.faces()
                 if abs(f.normal_at(f.center()).Z + 1.0) < 1e-6
                 and f.area > 2.0
                 and f.center().Z > p.Z_FLOOR_BOTTOM + 1e-6]
        self.assertEqual(flats, [], "downward flat inside a screw hole")
```

- [ ] **Step 2: Run the test and watch it fail**

Run: `cd ~/Projects/airwriter/enclosure && .venv/bin/python -m unittest -v test_case`
Expected: FAIL — `AttributeError: module 'case' has no attribute 'standoffs'`

- [ ] **Step 3: Implement both**

Add to `enclosure/case.py` — extend the import line to
`from build123d import Box, Cone, Cylinder, Part, Pos` and add:

```python
import board_geom as bg

STANDOFF_D = 6.00
"""Boss diameter under each mounting hole. Wide enough to carry the board and
to leave a printable wall around the 2.80 clearance bore."""


def standoffs() -> Part:
    """Four bosses from the floor's top face up to the board's underside."""
    h = p.Z_BOARD_BOTTOM - p.Z_FLOOR_TOP
    out = None
    for x, y in bg.mount_holes().values():
        boss = Pos(x, y, p.Z_FLOOR_TOP + h / 2) * Cylinder(STANDOFF_D / 2, h)
        out = boss if out is None else out + boss
    return out


def screw_holes() -> Part:
    """Clearance bore up through floor and boss, with a counterbore for the
    screw head in the floor's underside.

    The counterbore opens onto the print's first layer, so its transition to
    the clearance bore is a 45-degree cone rather than a flat annulus -- a
    flat there is an unsupported bridge (see the test)."""
    top = p.Z_BOARD_BOTTOM + 0.01
    bottom = p.Z_FLOOR_BOTTOM - 0.01
    out = None
    for x, y in bg.mount_holes().values():
        bore = Pos(x, y, (top + bottom) / 2) * Cylinder(
            p.SCREW_CLEAR_D / 2, top - bottom)
        cb_top = p.Z_FLOOR_BOTTOM + p.SCREW_HEAD_DEPTH
        cbore = Pos(x, y, (cb_top + bottom) / 2) * Cylinder(
            p.SCREW_HEAD_D / 2, cb_top - bottom)
        # 45-degree run-out from the counterbore up to the clearance bore.
        cone_h = (p.SCREW_HEAD_D - p.SCREW_CLEAR_D) / 2
        cone = Pos(x, y, cb_top + cone_h / 2) * Cone(
            p.SCREW_HEAD_D / 2, p.SCREW_CLEAR_D / 2, cone_h)
        one = bore + cbore + cone
        out = one if out is None else out + one
    return out
```

- [ ] **Step 4: Run the test and watch it pass**

Run: `cd ~/Projects/airwriter/enclosure && .venv/bin/python -m unittest -v test_case`
Expected: PASS, 8 tests.

- [ ] **Step 5: Commit**

```bash
cd ~/Projects/airwriter
git add enclosure/case.py enclosure/test_case.py
git commit -m "Enclosure: board standoffs and the screw stack"
```

---

### Task 5: Port cutouts booleaned from the real connector STEPs

**Files:**
- Modify: `enclosure/case.py`
- Modify: `enclosure/test_case.py`

**Interfaces:**
- Consumes: `board_geom.footprint()`, the `.step` files under `hardware/airwriter/lib/airwriter.3dshapes/`
- Produces: `connector_solid(ref: str) -> Part` — the connector's own geometry, placed in the case frame; `port_cutouts() -> Part` — the offset solids to subtract from the front wall

This is the reason the toolchain is OCCT and not OpenSCAD. J2, J4 and J1 all carry real vendor STEP files, so no barrel diameter, flange size or shell height is ever guessed — the wall opening is the connector's own solid, grown by `PORT_CLEAR`.

- [ ] **Step 1: Write the failing test**

Append to `enclosure/test_case.py`, above the `if __name__` block:

```python
class TestPorts(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tray_shell = case.shell() - case.port_cutouts()

    def test_each_connector_has_real_step_geometry(self):
        for ref in ("J2", "J4", "J1"):
            solid = case.connector_solid(ref)
            self.assertTrue(solid.is_valid, ref)
            self.assertGreater(solid.volume, 100.0, ref)

    def test_minidin_mating_faces_stand_2_75_proud_of_the_board_edge(self):
        # Y=0 is the board's front edge; the footprint origin IS the mating
        # face, so the connector reaches Y = -2.75.
        for ref in ("J2", "J4"):
            bb = case.connector_solid(ref).bounding_box()
            self.assertAlmostEqual(bb.min.Y, -2.75, places=2, msg=ref)

    def test_no_connector_touches_the_tray(self):
        for ref in ("J2", "J4", "J1"):
            hit = self.tray_shell & case.connector_solid(ref)
            self.assertAlmostEqual(hit.volume, 0.0, places=3, msg=ref)

    def test_openings_clear_each_connector_by_at_least_0_30(self):
        from build123d import offset
        for ref in ("J2", "J4", "J1"):
            grown = offset(case.connector_solid(ref), 0.30)
            hit = self.tray_shell & grown
            self.assertAlmostEqual(hit.volume, 0.0, places=3, msg=ref)

    def test_front_wall_still_exists_between_the_two_minidins(self):
        # J2 spans X 4.2..21.8 and J4 23.7..41.3; there must be plastic between.
        from build123d import Box, Pos
        probe = Pos(22.75, -p.WALL / 2, 5.0) * Box(1.0, p.WALL, 2.0)
        self.assertGreater((self.tray_shell & probe).volume, 0.5)
```

- [ ] **Step 2: Run the test and watch it fail**

Run: `cd ~/Projects/airwriter/enclosure && .venv/bin/python -m unittest -v test_case`
Expected: FAIL — `AttributeError: module 'case' has no attribute 'port_cutouts'`

- [ ] **Step 3: Implement**

Add to `enclosure/case.py` — extend the import line to
`from build123d import Box, Cone, Cylinder, Part, Pos, Rot, import_step, offset` and add:

```python
def connector_solid(ref: str) -> Part:
    """A connector's own vendor STEP, placed in the case frame.

    KiCad stores a footprint's model offset/rotation/scale, but every model
    this project touches is placed at offset 0, scale 1 with the part's own
    origin on the footprint origin, so the placement here is the footprint's
    position and rotation and nothing else. If a model is ever given a
    non-zero offset in the board file, this function has to grow to read it.
    """
    fp = bg.footprint(ref)
    if not fp.model or not fp.model.endswith(".step"):
        raise RuntimeError(f"{ref} has no STEP model: {fp.model}")
    part = import_step(fp.model)

    # STEP models are authored in the footprint's own frame: +X right,
    # +Y "into the board" (KiCad's y-down), Z up off the board. The case frame
    # flips Y, so the model turns by (180 - rot) about Z.
    x, y = bg.to_case(fp.x, fp.y)
    return Pos(x, y, 0) * Rot(0, 0, 180.0 - fp.rot) * part


def port_cutouts() -> Part:
    """What the front wall gives up to J2, J4 and J1.

    Each opening is the connector's own solid grown by PORT_CLEAR, so the
    barrel diameters, flanges and shell heights come from the vendor geometry
    rather than from a datasheet reading. The USB-C receptacle sits ~1.5mm
    behind the wall's outer face, so its opening also gets an outside chamfer
    wide enough for a plug's overmould.
    """
    out = None
    for ref in ("J2", "J4", "J1"):
        cut = offset(connector_solid(ref), p.PORT_CLEAR)
        out = cut if out is None else out + cut

    j1 = connector_solid("J1").bounding_box()
    flare = p.PORT_CLEAR + p.USBC_CHAMFER
    chamfer = Pos(
        (j1.min.X + j1.max.X) / 2,
        -p.WALL + p.USBC_CHAMFER / 2,
        (j1.min.Z + j1.max.Z) / 2,
    ) * Cone(
        (j1.size.X + 2 * flare) / 2,
        (j1.size.X + 2 * p.PORT_CLEAR) / 2,
        p.USBC_CHAMFER,
        rotation=(90, 0, 0),
    )
    return out + chamfer
```

- [ ] **Step 4: Run the test and watch it pass**

Run: `cd ~/Projects/airwriter/enclosure && .venv/bin/python -m unittest -v test_case`
Expected: PASS, 13 tests.

If `test_minidin_mating_faces_stand_2_75_proud_of_the_board_edge` fails with the sign of Y reversed, the `180.0 - fp.rot` in `connector_solid` is wrong for this board's rotation convention — try `-fp.rot` and re-read the comment in `board_geom.local_to_board`. Do not "fix" it by editing the expected value; 2.75 proud is measured from the board file.

- [ ] **Step 5: Commit**

```bash
cd ~/Projects/airwriter
git add enclosure/case.py enclosure/test_case.py
git commit -m "Enclosure: port openings cut from the real connector STEPs"
```

---

### Task 6: DIP slot, reset pinhole, carve-outs, and `tray()`

**Files:**
- Modify: `enclosure/case.py`
- Modify: `enclosure/test_case.py`

**Interfaces:**
- Consumes: everything from Tasks 3–5
- Produces: `dip_slot() -> Part`, `reset_pinhole() -> Part`, `carveouts() -> Part`, `tray() -> Part` — the finished, exportable tray

S1 and SW1 have only `.wrl` models, which OCCT cannot import, so these two cutouts are parametric from their footprints' own measured descriptions rather than booleaned from vendor solids. Both descriptions carry traced-from-drawing dimensions; the numbers are in `params.py`.

- [ ] **Step 1: Write the failing test**

Append to `enclosure/test_case.py`, above the `if __name__` block:

```python
class TestSideFeaturesAndTray(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.t = case.tray()

    def test_dip_slot_is_in_the_west_wall_at_the_key_band(self):
        import board_geom as bg
        bb = case.dip_slot().bounding_box()
        self.assertLessEqual(bb.min.X, -p.WALL)
        self.assertGreaterEqual(bb.max.X, 0.0)
        self.assertAlmostEqual(bb.min.Z, p.DIP_SLOT_Z0, places=3)
        self.assertAlmostEqual(bb.max.Z, p.DIP_SLOT_Z1, places=3)
        # Key band: S1's pad columns at footprint-local x = -8.89 .. +8.89.
        fp = bg.footprint("S1")
        lo = bg.to_case(*bg.local_to_board(fp, -8.89, 0.0))[1]
        hi = bg.to_case(*bg.local_to_board(fp, 8.89, 0.0))[1]
        lo, hi = min(lo, hi), max(lo, hi)
        self.assertAlmostEqual(bb.min.Y, lo - p.DIP_SLOT_MARGIN, places=2)
        self.assertAlmostEqual(bb.max.Y, hi + p.DIP_SLOT_MARGIN, places=2)

    def test_reset_pinhole_is_in_the_EAST_wall(self):
        # HANDOFF carries a superseded note putting this on the west wall.
        import board_geom as bg
        bb = case.reset_pinhole().bounding_box()
        self.assertGreaterEqual(bb.max.X, p.BOARD_W + p.WALL)
        self.assertLessEqual(bb.min.X, p.BOARD_W)
        _, cy = bg.to_case(*[bg.footprint("SW1").x, bg.footprint("SW1").y])
        self.assertAlmostEqual((bb.min.Y + bb.max.Y) / 2, cy, places=2)
        self.assertAlmostEqual(bb.size.Z, p.RESET_PINHOLE_D, places=3)

    def test_tray_is_one_valid_watertight_solid(self):
        self.assertTrue(self.t.is_valid)
        self.assertEqual(len(self.t.solids()), 1)

    def test_tray_keeps_its_external_envelope(self):
        bb = self.t.bounding_box()
        self.assertAlmostEqual(bb.size.X, p.EXT_W, places=3)
        self.assertAlmostEqual(bb.size.Y, p.EXT_L, places=3)
        self.assertAlmostEqual(bb.size.Z, p.EXT_H, places=3)

    def test_carveouts_clear_the_named_internal_parts(self):
        import board_geom as bg
        for ref in ("U4", "R3", "U9"):
            fp = bg.footprint(ref)
            x, y = bg.to_case(fp.x, fp.y)
            from build123d import Cylinder, Pos
            probe = Pos(x, y, 1.0) * Cylinder(1.0, 2.0)
            self.assertAlmostEqual((self.t & probe).volume, 0.0, places=3,
                                   msg=ref)
```

- [ ] **Step 2: Run the test and watch it fail**

Run: `cd ~/Projects/airwriter/enclosure && .venv/bin/python -m unittest -v test_case`
Expected: FAIL — `AttributeError: module 'case' has no attribute 'tray'`

- [ ] **Step 3: Implement**

Add to `enclosure/case.py`:

```python
CARVEOUT_R = {"U4": 4.0, "R3": 2.0, "U9": 2.5}
"""Keep-out radius per internal part, from its footprint's courtyard. These
are wall-side reliefs: the parts sit inboard, and the carve-out only matters
where a wall would otherwise crowd them."""


def dip_slot() -> Part:
    """S1's eight piano keys, out through the west wall.

    S1's footprint origin is the centre of its pin field; the pad columns run
    footprint-local x = -8.89 .. +8.89 and the keys align with them. The tips
    reach local y = -7.205 (body 9.61 deep centred on the 7.62 pin rows, keys
    2.40 proud), which lands at KiCad x = 97.80 -- 2.20mm past the board edge,
    so behind a 3.00 wall the keys sit 0.75mm recessed by design.
    """
    fp = bg.footprint("S1")
    ys = [bg.to_case(*bg.local_to_board(fp, lx, 0.0))[1] for lx in (-8.89, 8.89)]
    y0, y1 = min(ys) - p.DIP_SLOT_MARGIN, max(ys) + p.DIP_SLOT_MARGIN
    depth = p.WALL + 1.0  # overshoot inward so the cut is clean
    return Pos(
        -p.WALL / 2 + 0.5,  # spans x = -WALL .. +1.0
        (y0 + y1) / 2,
        (p.DIP_SLOT_Z0 + p.DIP_SLOT_Z1) / 2,
    ) * Box(depth, y1 - y0, p.DIP_SLOT_Z1 - p.DIP_SLOT_Z0)


def reset_pinhole() -> Part:
    """A pinhole through the EAST wall on SW1's actuator axis.

    HANDOFF has a superseded line putting this on the west wall at x=100; the
    live ruling is the east edge, with SW1's plate face at KiCad x ~ 159.4.
    """
    fp = bg.footprint("SW1")
    _, y = bg.to_case(fp.x, fp.y)
    z = (p.DIP_SLOT_Z0 + p.DIP_SLOT_Z1) / 2 - 5.0  # SW1's axis, low on the wall
    length = p.WALL + 2.0
    return Pos(p.BOARD_W + p.WALL - length / 2 + 1.0, y, z) * Cylinder(
        p.RESET_PINHOLE_D / 2, length, rotation=(0, 90, 0))


def carveouts() -> Part:
    """Air around the internal parts the ledger names, from the board's top
    face to the lid underside."""
    h = p.Z_LID_UNDER
    out = None
    for ref, r in CARVEOUT_R.items():
        fp = bg.footprint(ref)
        x, y = bg.to_case(fp.x, fp.y)
        cyl = Pos(x, y, h / 2) * Cylinder(r + p.CARVEOUT_CLEAR, h)
        out = cyl if out is None else out + cyl
    return out


def tray() -> Part:
    """The finished tray, shared by Pro and Lite."""
    body = shell() + standoffs()
    return body - screw_holes() - port_cutouts() - dip_slot() \
        - reset_pinhole() - carveouts()
```

- [ ] **Step 4: Run the test and watch it pass**

Run: `cd ~/Projects/airwriter/enclosure && .venv/bin/python -m unittest -v test_case`
Expected: PASS, 18 tests.

`test_reset_pinhole_is_in_the_EAST_wall` also pins the pinhole's Z. If SW1's real actuator axis is not 5.0 mm below the DIP band's centre, correct the `z` line in `reset_pinhole()` from the footprint's `.wrl` body height and update nothing else — the test asserts the wall and the Y axis, not the Z.

- [ ] **Step 5: Commit**

```bash
cd ~/Projects/airwriter
git add enclosure/case.py enclosure/test_case.py
git commit -m "Enclosure: DIP slot, reset pinhole, carve-outs, finished tray"
```

---

### Task 7: The Lite lid

**Files:**
- Modify: `enclosure/case.py`
- Modify: `enclosure/test_case.py`

**Interfaces:**
- Consumes: `params`, `board_geom.mount_holes()`, `board_geom.footprint("SW3")`
- Produces: `lite_lid() -> Part`

- [ ] **Step 1: Write the failing test**

Append to `enclosure/test_case.py`, above the `if __name__` block:

```python
class TestLiteLid(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.lid = case.lite_lid()

    def test_is_one_valid_solid(self):
        self.assertTrue(self.lid.is_valid)
        self.assertEqual(len(self.lid.solids()), 1)

    def test_plate_matches_the_art_panel_and_seats_flush(self):
        bb = self.lid.bounding_box()
        self.assertAlmostEqual(bb.size.X, p.POCKET_W, places=3)
        self.assertAlmostEqual(bb.size.Y, p.POCKET_L, places=3)
        self.assertAlmostEqual(bb.max.Z, p.Z_WALL_TOP, places=3)

    def test_legs_span_the_cavity_to_the_board_face(self):
        bb = self.lid.bounding_box()
        self.assertAlmostEqual(bb.min.Z, 0.0, places=3)
        self.assertAlmostEqual(bb.size.Z, p.CAVITY + p.PANEL, places=3)

    def test_it_fits_the_tray_with_no_interference(self):
        self.assertAlmostEqual((case.tray() & self.lid).volume, 0.0, places=3)

    def test_plunger_bore_sits_over_SW3(self):
        # SW3's plunger is 3.95 from the footprint origin; HANDOFF puts it at
        # KiCad (131, 91), under the art panel's test dot.
        from build123d import Cylinder, Pos
        import board_geom as bg
        x, y = bg.to_case(131.0, 91.0)
        probe = Pos(x, y, p.Z_WALL_TOP - p.PANEL / 2) * Cylinder(0.4, p.PANEL)
        self.assertAlmostEqual((self.lid & probe).volume, 0.0, places=3)

    def test_insert_bores_open_downward_at_the_board_face(self):
        import board_geom as bg
        from build123d import Cylinder, Pos
        for x, y in bg.mount_holes().values():
            probe = Pos(x, y, p.INSERT_DEPTH / 2) * Cylinder(
                p.INSERT_D / 2 - 0.1, p.INSERT_DEPTH)
            self.assertAlmostEqual((self.lid & probe).volume, 0.0, places=3)
```

- [ ] **Step 2: Run the test and watch it fail**

Run: `cd ~/Projects/airwriter/enclosure && .venv/bin/python -m unittest -v test_case`
Expected: FAIL — `AttributeError: module 'case' has no attribute 'lite_lid'`

- [ ] **Step 3: Implement**

Add to `enclosure/case.py`:

```python
LEG_D = 6.00
"""Lite lid leg diameter -- enough plastic round a 3.60 insert bore."""


def lite_lid() -> Part:
    """The panel stand-in for boards built without the dot-grid art panel.

    A plate the panel's size and thickness, seating in the same place, on four
    legs that reproduce in plastic the 14.34mm the lattice and the panel's
    SMTSO nuts span on a Pro. Each leg takes an M2.5 heat-set insert at its
    BOTTOM, level with the board's top face, so the screw from below engages
    brass after a few millimetres instead of threading printed plastic.
    """
    plate = Pos(
        p.BOARD_W / 2,
        p.BOARD_L / 2,
        p.Z_WALL_TOP - p.PANEL / 2,
    ) * Box(p.POCKET_W, p.POCKET_L, p.PANEL)

    body = plate
    for x, y in bg.mount_holes().values():
        leg = Pos(x, y, p.CAVITY / 2) * Cylinder(LEG_D / 2, p.CAVITY)
        body = body + leg

    for x, y in bg.mount_holes().values():
        bore = Pos(x, y, p.INSERT_DEPTH / 2 - 0.01) * Cylinder(
            p.INSERT_D / 2, p.INSERT_DEPTH + 0.02)
        body = body - bore

    px, py = bg.to_case(131.0, 91.0)
    plunger = Pos(px, py, p.Z_WALL_TOP - p.PANEL / 2) * Cylinder(
        p.PLUNGER_BORE_D / 2, p.PANEL + 0.02)
    return body - plunger
```

- [ ] **Step 4: Run the test and watch it pass**

Run: `cd ~/Projects/airwriter/enclosure && .venv/bin/python -m unittest -v test_case`
Expected: PASS, 24 tests.

- [ ] **Step 5: Commit**

```bash
cd ~/Projects/airwriter
git add enclosure/case.py enclosure/test_case.py
git commit -m "Enclosure: Lite lid"
```

---

### Task 8: Export, printability gate, README

**Files:**
- Create: `enclosure/export.py`
- Create: `enclosure/test_printable.py`
- Create: `enclosure/build.sh`
- Create: `enclosure/README.md`

**Interfaces:**
- Consumes: `case.tray()`, `case.lite_lid()`
- Produces: `out/airwriter-tray.step`, `out/airwriter-tray.stl`, `out/airwriter-lite-lid.step`, `out/airwriter-lite-lid.stl`

- [ ] **Step 1: Write the failing printability test**

Create `enclosure/test_printable.py`:

```python
"""The gate between "the model closes" and "the print will not fail".

Both parts print flat-side-down with no supports. The check that matters is
unsupported DOWNWARD-FACING FLATS: a curved overhang (a port hole's top arc)
prints fine at these diameters, a horizontal ledge in mid-air does not.
"""
import unittest

import case
import params as p

MIN_WALL = 2.80
"""3.00 nominal minus the 0.20 pocket clearance."""


def downward_flats(part, floor_z, min_area=2.0):
    out = []
    for f in part.faces():
        try:
            n = f.normal_at(f.center())
        except Exception:
            continue
        if abs(n.Z + 1.0) < 1e-6 and f.area > min_area and f.center().Z > floor_z + 1e-6:
            out.append(f)
    return out


class TestTray(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.t = case.tray()

    def test_single_watertight_solid(self):
        self.assertTrue(self.t.is_valid)
        self.assertEqual(len(self.t.solids()), 1)

    def test_no_unsupported_downward_flats(self):
        flats = downward_flats(self.t, p.Z_FLOOR_BOTTOM)
        self.assertEqual(
            [(round(f.center().X, 1), round(f.center().Y, 1),
              round(f.center().Z, 1), round(f.area, 1)) for f in flats], [])

    def test_walls_are_at_least_2_80_thick(self):
        # Probe each wall mid-height, away from any opening.
        from build123d import Box, Pos
        probes = [
            ("west", Pos(-p.WALL / 2, 90.0, 12.0), (p.WALL, 4.0, 2.0)),
            ("east", Pos(p.BOARD_W + p.WALL / 2, 90.0, 12.0), (p.WALL, 4.0, 2.0)),
            ("back", Pos(31.0, p.BOARD_L + p.WALL / 2, 12.0), (4.0, p.WALL, 2.0)),
        ]
        for name, pos, size in probes:
            probe = pos * Box(*size)
            got = (self.t & probe).volume
            want = MIN_WALL * size[0] * size[1] * size[2] / p.WALL
            self.assertGreaterEqual(got, want - 0.05, name)


class TestLid(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.lid = case.lite_lid()

    def test_single_watertight_solid(self):
        self.assertTrue(self.lid.is_valid)
        self.assertEqual(len(self.lid.solids()), 1)

    def test_no_unsupported_downward_flats_printed_plate_down(self):
        # Printed plate-down, the plate's own underside is the build surface,
        # so "floor" is the top of the plate's underside: Z_WALL_TOP - PANEL.
        flats = downward_flats(self.lid, 0.0)
        offenders = [f for f in flats
                     if abs(f.center().Z - (p.Z_WALL_TOP - p.PANEL)) > 1e-6]
        self.assertEqual(
            [(round(f.center().X, 1), round(f.center().Y, 1),
              round(f.center().Z, 1)) for f in offenders], [])


if __name__ == "__main__":
    unittest.main()
```

- [ ] **Step 2: Run it and see where the geometry actually stands**

Run: `cd ~/Projects/airwriter/enclosure && .venv/bin/python -m unittest -v test_printable`

Expected: FAIL. Do not guess at the fix — the failure prints the X/Y/Z and area of every offending face. For each one, go back to the function in `case.py` that made it and either cone the transition (as `screw_holes()` already does) or accept it by proving it is on the build plate. The two likely offenders are the top of each Lite-lid leg where it meets the plate (legitimate — it is a fillet candidate, not a bridge, because the leg is *under* the plate and prints plate-down) and any flat left by the USB-C chamfer cone.

- [ ] **Step 3: Fix `case.py` until the gate passes**

Run: `cd ~/Projects/airwriter/enclosure && .venv/bin/python -m unittest -v test_printable`
Expected: PASS, 5 tests.

- [ ] **Step 4: Write the exporter**

Create `enclosure/export.py`:

```python
"""Write the two parts to out/ as STEP (for the KiCad interference check) and
STL (to slice)."""
import pathlib
import sys

from build123d import export_step, export_stl

import case

OUT = pathlib.Path(__file__).resolve().parent / "out"

PARTS = {
    "airwriter-tray": case.tray,
    "airwriter-lite-lid": case.lite_lid,
}


def main() -> int:
    OUT.mkdir(exist_ok=True)
    for name, build in PARTS.items():
        part = build()
        export_step(part, str(OUT / f"{name}.step"))
        export_stl(part, str(OUT / f"{name}.stl"))
        bb = part.bounding_box()
        print(f"{name}: {bb.size.X:.2f} x {bb.size.Y:.2f} x {bb.size.Z:.2f} mm, "
              f"{part.volume / 1000:.1f} cm3")
    return 0


if __name__ == "__main__":
    sys.exit(main())
```

- [ ] **Step 5: Write the driver**

Create `enclosure/build.sh`:

```sh
#!/bin/sh
# The one supported way to build the enclosure. Everything board-derived is
# re-read from hardware/airwriter/airwriter.kicad_pcb on every run.
set -e
cd "$(dirname "$0")"
PY=.venv/bin/python
[ -x "$PY" ] || { echo "run: python3 -m venv .venv && .venv/bin/pip install -r requirements.txt" >&2; exit 1; }
"$PY" -m unittest discover -p 'test_*.py' -v
"$PY" export.py
```

Then: `chmod +x enclosure/build.sh`

- [ ] **Step 6: Run the whole build**

Run: `cd ~/Projects/airwriter && ./enclosure/build.sh`
Expected: all tests pass, then two lines reporting `68.00 x 110.27 x 21.94 mm` for the tray and `62.40 x 104.67 x 15.94 mm` for the lid, and four files in `enclosure/out/`.

- [ ] **Step 7: Write the README**

Create `enclosure/README.md`:

```markdown
# Enclosure (enclosure/out/*.stl)

Two printed parts. The **tray** is shared by both builds. The **Lite lid**
replaces the dot-grid art panel on boards built without it.

    ./build.sh          # runs the tests, then writes out/*.{step,stl}

Design: `docs/superpowers/specs/2026-09-08-airwriter-enclosure-design.md`.
Every board-derived number is read out of `hardware/airwriter/airwriter.kicad_pcb`
at build time by `board_geom.py` — nothing here is retyped, so a footprint move
re-cuts the case on the next run. `params.py` holds what this project chooses,
each constant commented with where it came from.

## Print

FDM, 0.4 mm nozzle, 0.2 mm layers, matte black. **No supports.** Tray floor-down,
lid plate-down. `test_printable.py` fails the build on any unsupported
downward-facing flat, so if it passes, it slices.

## Hardware

| Build | Screws | Into |
|---|---|---|
| Pro | 4 × M2.5 × 20 | the art panel's PEM SMTSO-M25-3ET blind nuts |
| Lite | 4 × M2.5 × 8 | M2.5 brass heat-set inserts in the lid legs |

Press the four inserts into the **bottom** of the lid's legs before assembly.
Rubber feet go on the floor's underside, over the screw counterbores.

## First print, check in this order

1. A real USB-C cable into the chamfered opening. The receptacle sits 1.5 mm
   behind the wall; this is the one clearance the board file could not settle.
2. Both mini-DIN plugs.
3. A pen tip on S1's keys (they sit 0.75 mm recessed by design).
4. A paperclip on the east-wall reset pinhole.
5. The full stack screwed together.

## Interference check

`out/*.step` opens in KiCad's 3D viewer alongside the real board. That check is
why this is build123d and not OpenSCAD.
```

- [ ] **Step 8: Commit**

```bash
cd ~/Projects/airwriter
git add enclosure/export.py enclosure/test_printable.py enclosure/build.sh enclosure/README.md
git commit -m "Enclosure: export, printability gate, README"
```

---

### Task 9: The shop's GLB

**Files:**
- Modify: `enclosure/README.md`
- Create (in the **shop** repo): `~/Projects/shop/public/models/airwriter-case.glb`

**Interfaces:**
- Consumes: `enclosure/out/airwriter-tray.stl`
- Produces: a GLB the shop's product page can use. Wiring it to a product is explicitly out of scope (spec §8).

- [ ] **Step 1: Convert the tray**

Run:

```bash
cd ~/Projects/shop && node scripts/addon-models/stl2glb.mjs \
  ~/Projects/airwriter/enclosure/out/airwriter-tray.stl \
  public/models/airwriter-case.glb
```

If `stl2glb.mjs` takes different arguments, read its header comment and adjust; do not rewrite the converter.

- [ ] **Step 2: Check the result loads and is a sane size**

Run:

```bash
cd ~/Projects/shop && ls -la public/models/airwriter-case.glb && \
  node -e "const b=require('fs').readFileSync('public/models/airwriter-case.glb');console.log('magic',b.toString('utf8',0,4),'bytes',b.length)"
```

Expected: `magic glTF` and a file well under 2 MB.

- [ ] **Step 3: Record the recipe**

Append to `enclosure/README.md`:

```markdown
## Shop model

    cd ~/Projects/shop && node scripts/addon-models/stl2glb.mjs \
      ~/Projects/airwriter/enclosure/out/airwriter-tray.stl \
      public/models/airwriter-case.glb

Nothing on the site references it yet — making the case a purchasable add-on is
a product decision, not part of this design.
```

- [ ] **Step 4: Commit, in both repos**

```bash
cd ~/Projects/airwriter && git add enclosure/README.md && \
  git commit -m "Enclosure: record the shop GLB recipe"
cd ~/Projects/shop && git add public/models/airwriter-case.glb && \
  git commit -m "AirWriter case model"
```

---

## Self-review notes

- **Spec coverage.** §1 parts → Tasks 3–7. §2 stack → Task 2 (`test_z_landmarks`). §3 uniform wall → Task 2 + `test_walls_are_at_least_2_80_thick`. §4 front wall → Task 5; west/east/carve-outs → Task 6; top seat → Task 3 + Task 7. §5 fastening → Task 4 (Pro) and Task 7 (Lite inserts). §6 source layout → Tasks 1, 2, 8. §7 verification → Task 8's gate plus the README's first-print order; the KiCad interference check is manual and the README says so. §8 shop GLB → Task 9. §9 out-of-scope is honoured: no task touches `hardware/`.
- **Open items carried, not resolved.** Insert MPN (`INSERT_D`/`INSERT_DEPTH` are re-cuttable constants), rubber foot, USB-C chamfer (`USBC_CHAMFER`, tuned on the first print). Each is a named constant rather than a buried literal.
- **The two soft numbers**, both flagged at their use site: `DIP_SLOT_Z0/Z1` (from HANDOFF's 8.11–9.01 tip travel, no STEP to boolean against) and `reset_pinhole()`'s Z (SW1 has only a `.wrl`). Everything else is either read from the board or booleaned from vendor geometry.
