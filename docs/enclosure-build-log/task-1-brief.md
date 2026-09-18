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
print('valid', b.is_valid())
"
```

Expected: `volume 6000.0`, `size 10.0 20.0 30.0`, `valid True`.

If any name here does not exist, STOP and report the actual build123d 0.11.1 API before continuing — every later task uses `Box`, `bounding_box()`, `.volume`, `.is_valid()`, `export_step`, `export_stl`.

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

