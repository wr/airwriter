"""The board's parts as boxes the light-guide lattice must keep clear of.

KiCad's own STEP export places every 3D model exactly where pcbnew does, so it
is the authority for where each body is. kicad-cli writes it to
out/components.step (gitignored), redone whenever the board file is newer.
Each part becomes a "stepped envelope": the XY footprint of each BAND-tall
slice and that slice's top above the board face. The lattice voids each
band's footprint from below the board up to the band's top -- its vertical
shadow, because the lattice is lowered onto the board from above. One box per
part would do for flat parts, but it would turn SW3 (a 6 mm body under a
3.5 mm plunger reaching 15 mm) into a 15 mm block and cut the test cell's
walls away."""
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
    """out/components.step, re-exported with kicad-cli when the board is newer."""
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


def _bands(shape) -> list[Band]:
    bb = shape.bounding_box()
    x0, x1, y0, y1, _, z1 = _to_case_box(bb)
    if z1 <= STEPPED_ABOVE:
        return [(x0, x1, y0, y1, z1)]
    cx, cy = (bb.min.X + bb.max.X) / 2, (bb.min.Y + bb.max.Y) / 2
    w, l = bb.size.X + 2, bb.size.Y + 2
    out = []
    z = 0.0
    while z < z1 - 1e-6:
        top = min(z + BAND, z1)
        slab = Pos(cx, cy, p.PCB + (z + top) / 2) * Box(w, l, top - z)
        # Per solid: a STEP part is a Compound, and Compound & Box comes back
        # empty in build123d 0.11 where Solid & Box does not.
        boxes = []
        for solid in shape.solids():
            sl = solid & slab
            if sl is not None and sl.volume > 1e-6:
                boxes.append(_to_case_box(sl.bounding_box()))
        if boxes:
            out.append((min(b[0] for b in boxes), max(b[1] for b in boxes),
                        min(b[2] for b in boxes), max(b[3] for b in boxes), top))
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
        yard = bg.courtyard(ref)
        if top is None or yard is None:
            raise RuntimeError(f"{ref} ({lib}) has no 3D model and no NO_MODEL height")
        env[ref] = [(*yard, top)]
    for ref, top in TOP_OVERRIDE.items():
        bands = env[ref]
        env[ref] = [(min(b[0] for b in bands), max(b[1] for b in bands),
                     min(b[2] for b in bands), max(b[3] for b in bands), top)]
    _ENV = env
    return env
