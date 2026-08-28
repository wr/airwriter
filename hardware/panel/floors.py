#!/usr/bin/env python3
"""Advisory fab-floor gap/pinch check over the flattened *.polys.json classes.

Warns (never fails) when two same-class polys, or a poly's own hole and its
outline, come closer together than the fab can reliably resolve but not close
enough to read as a deliberate touch (a hole and outline that literally share
an edge by design, e.g. two glyphs whose serifs kiss). Anything in between is
worth a human's eyes at quote time - it's not a hard defect, since it hasn't
been checked against any particular fab's actual capability line.

System python, run between the svg2poly steps and emit_board in generate.sh.
"""
import json, pathlib, sys

IGNORE = 0.02   # below this: a deliberate touching/shared edge, not a defect
WARN = 0.101    # above this: comfortable clearance, nothing to say
BUCKET = 0.5    # mm - well above WARN, so a 3x3 bucket search never misses a pair

def _min_dist(points):
    # points: [(x, y, tag), ...]. Nearest distance between two DIFFERENT tags,
    # via a spatial hash so this stays fast over the whole panel's worth of
    # sampled points instead of comparing every pair against every other.
    buckets = {}
    for i, (x, y, tag) in enumerate(points):
        buckets.setdefault((int(x // BUCKET), int(y // BUCKET)), []).append(i)
    best = None
    for i, (x, y, tag) in enumerate(points):
        bx, by = int(x // BUCKET), int(y // BUCKET)
        for dx in (-1, 0, 1):
            for dy in (-1, 0, 1):
                for j in buckets.get((bx + dx, by + dy), ()):
                    if j <= i:
                        continue
                    x2, y2, tag2 = points[j]
                    if tag2 == tag:
                        continue
                    d = ((x - x2) ** 2 + (y - y2) ** 2) ** 0.5
                    if best is None or d < best[0]:
                        best = (d, (x + x2) / 2, (y + y2) / 2)
    return best

def class_gap(polys):
    """(a) min gap between distinct polys in one class. (dist, x, y) or None."""
    pts = [(x, y, i) for i, p in enumerate(polys) for x, y in p["outline"]]
    return _min_dist(pts)

def poly_pinch(poly):
    """(b) min hole-edge to outline-edge distance within one poly. (dist, x, y) or None."""
    if not poly["holes"]:
        return None
    pts = [(x, y, "outline") for x, y in poly["outline"]]
    for h in poly["holes"]:
        pts += [(x, y, "hole") for x, y in h]
    return _min_dist(pts)

def _banded(label, dist_xy):
    # Round to the same 3 decimals the warning prints before banding -
    # otherwise a measurement that reads as "0.101mm" on screen can fail an
    # unrounded "<= 0.101" comparison by a fraction of a micron.
    if dist_xy and IGNORE <= round(dist_xy[0], 3) <= WARN:
        d, x, y = dist_xy
        return f"{label} {d:.3f}mm at x {x:.2f}, y {y:.2f}"
    return None

def check_floors(classes):
    """classes: {name: polys}. Returns a list of warning strings; never raises."""
    warnings = []
    for name, polys in classes.items():
        w = _banded(f"{name} inter-poly gap", class_gap(polys))
        if w:
            warnings.append(w)
        for p in polys:
            w = _banded(f"{name} pinch", poly_pinch(p))
            if w:
                warnings.append(w)
    return warnings

def knockout_glyphs(silk, layout):
    """Silk polys that sit fully inside a gold-family (G/T) dot's disc -
    Wells's black-letters-on-gold mask-knockout design (a label drawn as a
    hole in the dot's kept mask instead of separate silkscreen). Mirrors the
    same "fully inside a gold dot" test emit_board.py's
    split_mask_knockouts() applies at emission time, from JSON alone."""
    r = layout["grid"]["dot_dia"] / 2
    dots = [d for d in layout["dots"] if d["mat"] in ("G", "T")]
    return [p for p in silk
            if any(all(((x - d["x"]) ** 2 + (y - d["y"]) ** 2) ** 0.5 < r
                       for x, y in p["outline"])
                   for d in dots)]

def check_mask_ridges(glyphs):
    """Same nearest-neighbour check as check_floors, but for mask-knockout
    glyphs: the solid (kept) mask forming a letter stroke must still clear
    the fab's practical minimum, whether that stroke sits between two
    letters (a "gap" in copper/silk terms) or around a letter's own counter
    (a "pinch") - both are just "mask ridge" width here."""
    warnings = []
    w = _banded("mask ridge", class_gap(glyphs))
    if w:
        warnings.append(w)
    for p in glyphs:
        w = _banded("mask ridge", poly_pinch(p))
        if w:
            warnings.append(w)
    return warnings

if __name__ == "__main__":
    here = pathlib.Path(__file__).parent
    classes = {name: json.loads((here / f"{name}.polys.json").read_text())
               for name in ("gold", "silk", "fr4")}
    layout = json.loads((here / "panel_layout.json").read_text())
    glyphs = knockout_glyphs(classes["silk"], layout)
    glyph_ids = {id(p) for p in glyphs}
    classes["silk"] = [p for p in classes["silk"] if id(p) not in glyph_ids]
    warnings = check_floors(classes) + check_mask_ridges(glyphs)
    for w in warnings:
        print(f"WARNING floors: {w}", file=sys.stderr)
    print(f"floors: {len(warnings)} warning(s)" if warnings else "floors: clean")
