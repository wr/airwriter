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

