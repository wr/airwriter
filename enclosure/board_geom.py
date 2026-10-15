"""Read the AirWriter main board's geometry straight out of the KiCad file.

Nothing in this project retypes a board dimension. If a footprint moves, the
case re-cuts itself on the next build instead of quietly mismatching.

The parser is deliberately small: KiCad's s-expression board file is read as
text, top-level `(footprint ...)` blocks are bracket-matched, and only the
fields this project needs are pulled out. It does NOT need pcbnew, which only
exists inside KiCad's own bundled Python.
"""
from __future__ import annotations

import json
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
    layer: str = "F.Cu"
    model_offset: tuple[float, float, float] = _IDENTITY_OFFSET
    model_scale: tuple[float, float, float] = _IDENTITY_SCALE
    model_rotate: tuple[float, float, float] = _IDENTITY_ROTATE


def _text(path: pathlib.Path | None = None) -> str:
    # `path` defaults dynamically to the CURRENT module-level BOARD_PCB
    # (looked up here, not bound as a default at def-time) so that tests
    # which monkeypatch board_geom.BOARD_PCB to a scratch file still take
    # effect on every no-argument caller.
    return (path or BOARD_PCB).read_text()


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


def outline(path: pathlib.Path | None = None) -> tuple[float, float, float, float]:
    """The Edge.Cuts bounding box in KiCad mm: (min_x, min_y, max_x, max_y).

    `path` defaults to the main board, but takes any KiCad board file --
    used by test_board_geom.py to read the art panel's own outline off
    hardware/panel/panel.kicad_pcb without a second parser."""
    path = path or BOARD_PCB
    src = _text(path)
    pts: list[tuple[float, float]] = []
    for kind in ("line", "rect", "arc", "poly"):
        for blk in _blocks(src, "gr_" + kind):
            if '"Edge.Cuts"' in blk:
                pts += [(float(a), float(b)) for a, b in _COORD.findall(blk)]
    if not pts:
        raise RuntimeError(f"no Edge.Cuts geometry in {path}")
    xs = [p[0] for p in pts]
    ys = [p[1] for p in pts]
    return min(xs), min(ys), max(xs), max(ys)


_ARC_PT = re.compile(r"\((start|mid|end) ([-\d.]+) ([-\d.]+)\)")


def _circumradius(p1: tuple[float, float], p2: tuple[float, float],
                   p3: tuple[float, float],
                   path: pathlib.Path | None = None) -> float:
    """The radius of the circle through three points, via Heron's formula on
    the triangle they form: R = (a*b*c) / (4*area)."""
    path = path or BOARD_PCB
    a = math.dist(p2, p3)
    b = math.dist(p1, p3)
    c = math.dist(p1, p2)
    s = (a + b + c) / 2
    area = math.sqrt(max(s * (s - a) * (s - b) * (s - c), 0.0))
    if area < 1e-9:
        raise RuntimeError(f"degenerate Edge.Cuts arc (collinear points) in {path}")
    return (a * b * c) / (4 * area)


def corner_radius(tol: float = 1e-3, path: pathlib.Path | None = None) -> float:
    """The board's own corner radius, derived from its Edge.Cuts arcs rather
    than assumed.

    Each Edge.Cuts corner is one `gr_arc`; its start/mid/end are three
    points on the circle it traces, which is enough to recover the radius
    without knowing the centre. Every arc found must agree to within `tol`
    mm -- a board with mismatched corner radii needs a human to look at it,
    not an average that quietly picks a number nobody chose.

    `path` defaults to the main board; see `outline()`."""
    path = path or BOARD_PCB
    src = _text(path)
    radii: list[float] = []
    for blk in _blocks(src, "gr_arc"):
        if '"Edge.Cuts"' not in blk:
            continue
        pts = {kind: (float(x), float(y)) for kind, x, y in _ARC_PT.findall(blk)}
        if not {"start", "mid", "end"} <= pts.keys():
            continue
        radii.append(_circumradius(pts["start"], pts["mid"], pts["end"], path))
    if not radii:
        raise RuntimeError(f"no Edge.Cuts arcs in {path}")
    r0 = radii[0]
    for r in radii[1:]:
        if abs(r - r0) > tol:
            raise ValueError(
                f"Edge.Cuts corner radii disagree in {path}: {radii} "
                f"(tolerance {tol}mm) -- a board with mixed corner radii "
                "needs a human, not an averaged number.")
    return round(sum(radii) / len(radii), 6)


def to_case(kx: float, ky: float) -> tuple[float, float]:
    """KiCad mm -> case-frame X, Y. Y flips, so the frame is right-handed."""
    return round(kx - ORIGIN_X, 6), round(ORIGIN_Y - ky, 6)


_REF = re.compile(r'\(property "Reference" "([^"]+)"')
_AT = re.compile(r"\n\t\t\(at ([-\d.]+) ([-\d.]+)(?: ([-\d.]+))?\)")
_LIB = re.compile(r'\(footprint "([^"]+)"')
_FP_LAYER = re.compile(r'\(footprint "[^"]+"\s*\n\s*\(layer "([^"]+)"')
_MODEL = re.compile(r'\(model "([^"]+)"')
_XYZ = re.compile(r"\(xyz ([-\d.]+) ([-\d.]+) ([-\d.]+)\)")
_PAD_AT = re.compile(r'\(pad "[^"]+" thru_hole \w+\s*\(at ([-\d.]+) ([-\d.]+)')


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
        layer_m = _FP_LAYER.search(blk)
        out[ref.group(1)] = Footprint(
            ref=ref.group(1),
            x=float(at.group(1)),
            y=float(at.group(2)),
            rot=float(at.group(3) or 0.0),
            lib=_LIB.search(blk).group(1),
            model=path,
            layer=layer_m.group(1) if layer_m else "F.Cu",
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


def pad_positions(ref: str) -> list[tuple[float, float]]:
    """Every through-hole pad's LOCAL (footprint-relative, unrotated) x, y for
    a footprint, read straight out of the board file.

    This is deliberately independent of the STEP-placement machinery in
    case.connector_solid(): it exists so a placement-registration check can
    verify the connector lands where the board's own copper says it should,
    without trusting the same transform it is meant to be checking. Pad
    positions are given in the footprint's local frame (before fp.rot is
    applied) -- feed them through local_to_board() then to_case() to reach
    the case frame, same as mount_holes() does for H1-H4.
    """
    src = _text()
    for blk in _blocks(src, "footprint"):
        m = _REF.search(blk)
        if m and m.group(1) == ref:
            return [(float(x), float(y)) for x, y in _PAD_AT.findall(blk)]
    raise KeyError(f"{ref} is not a footprint on {BOARD_PCB.name}")


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
        if m.group(1) == "fp_circle":
            got = {k: (float(a), float(b)) for k, a, b in _GR_PT.findall(sub)}
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
