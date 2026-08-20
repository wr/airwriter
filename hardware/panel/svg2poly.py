#!/usr/bin/env python3
"""Flatten SVG fills to polygons (mm). Outlines + holes, evenodd."""
import json, sys
from svgelements import SVG, Path, Shape

TOL = 0.05  # mm sampling tolerance at target scale

# svgelements has no scipy available in this environment, so Segment.length()
# falls back to a recursive subdivider with a default error of 1e-12 - absurd
# precision for picking a sample count, and slow enough to make real glyph
# paths (dozens of curve segments per subpath) take minutes. A much looser
# error bound is plenty here since the result only sets sampling density.
_LEN_ERROR = 1e-3
_LEN_MIN_DEPTH = 3

def _subpaths_to_points(path, scale):
    for sub in path.as_subpaths():
        pts = []
        # Sample each segment with its own point(t), not Path.point(t) on the
        # whole subpath: Path.point() re-derives cumulative arc length across
        # every prior segment on each call, which is O(n) per sample and O(n^2)
        # over a subpath - the actual cause of the multi-minute hang on real
        # (curve-heavy) glyph paths. Segment.point(t) is a direct polynomial
        # evaluation and stays O(1) per sample.
        for segment in Path(sub):
            seg_len = segment.length(error=_LEN_ERROR, min_depth=_LEN_MIN_DEPTH)
            if seg_len == 0:
                continue
            n = max(3, int(seg_len * scale / TOL / 4))
            for i in range(n):
                p = segment.point(i / n)
                pts.append([round(p.real * scale, 4), round(p.imag * scale, 4)])
        if pts:
            yield pts

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
            if len(p) == 0:
                continue
            rings.extend(_subpaths_to_points(p, scale))
    if not rings:
        raise ValueError(
            f"{svg_path}: no fillable path geometry found. If this SVG uses "
            "<mask> elements to clip strokes, svgelements may not resolve them "
            "into flat paths here — ask Wells for a flattened re-export "
            "(outline strokes, no masks) rather than implementing SVG masks."
        )
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
