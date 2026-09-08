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

_IDENTITY_OFFSET = (0.0, 0.0, 0.0)
_IDENTITY_SCALE = (1.0, 1.0, 1.0)
_IDENTITY_ROTATE = (0.0, 0.0, 0.0)


@dataclass(frozen=True)
class Footprint:
    ref: str
    x: float
    y: float
    rot: float
    lib: str
    model: str | None
    model_offset: tuple[float, float, float] = _IDENTITY_OFFSET
    model_scale: tuple[float, float, float] = _IDENTITY_SCALE
    model_rotate: tuple[float, float, float] = _IDENTITY_ROTATE


def _text() -> str:
    return BOARD_PCB.read_text()


def _blocks(src: str, opener: str):
    """Yield every top-level `(<opener> ...)` block, brackets matched, strings skipped.

    The opener is matched at a word boundary rather than requiring a literal
    trailing space: some block kinds (e.g. `gr_line`, `gr_arc`) put their
    first argument on the next line, so `(gr_line\\n` is as valid a start as
    `(footprint "..."`.
    """
    needle = re.compile(r"\n\t\(" + re.escape(opener) + r"(?=[\s)])")
    i = 0
    while True:
        m = needle.search(src, i)
        if not m:
            return
        i = m.start()
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
_XYZ = re.compile(r"\(xyz ([-\d.]+) ([-\d.]+) ([-\d.]+)\)")


def _model_sub_block(blk: str, name: str) -> tuple[float, float, float] | None:
    """Extract the (xyz x y z) triple out of a (model ...)'s (offset|scale|rotate ...) sub-block."""
    m = re.search(r"\(" + name + r"\s*\n?\s*" + _XYZ.pattern, blk)
    if not m:
        return None
    return (float(m.group(1)), float(m.group(2)), float(m.group(3)))


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
        model_offset = _IDENTITY_OFFSET
        model_scale = _IDENTITY_SCALE
        model_rotate = _IDENTITY_ROTATE
        if model:
            path = model.group(1).replace("${KIPRJMOD}", str(BOARD_PCB.parent))
            # The model's own sub-block, from "(model ..." to its closing paren,
            # so offset/scale/rotate from unrelated blocks never leak in.
            model_blk = blk[model.start() :]
            model_offset = _model_sub_block(model_blk, "offset") or _IDENTITY_OFFSET
            model_scale = _model_sub_block(model_blk, "scale") or _IDENTITY_SCALE
            model_rotate = _model_sub_block(model_blk, "rotate") or _IDENTITY_ROTATE
        out[ref.group(1)] = Footprint(
            ref=ref.group(1),
            x=float(at.group(1)),
            y=float(at.group(2)),
            rot=float(at.group(3) or 0.0),
            lib=_LIB.search(blk).group(1),
            model=path,
            model_offset=model_offset,
            model_scale=model_scale,
            model_rotate=model_rotate,
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
