#!/usr/bin/env python3
"""Flatten fill-filtered SVG <path> art to polygons in absolute mm."""
import json, re, sys
from io import StringIO
from svgelements import SVG, Path, Line, Close

TOL = 0.05  # mm sampling tolerance

# svgelements yields the CONTENT of <mask> definitions as ordinary elements
# during traversal - a mask's inner rect (which defines what to keep, not
# what to paint) inherits the mask's own fill="white" default and otherwise
# looks exactly like a real painted shape, so it leaks into fill-filtered
# output as a solid plate. Figma's masks here are always a single rect used
# to inset-crop a masked path (e.g. a label cell's rule bar clipped to the
# cell's own bounds), so: strip the <mask> definitions out of the raw SVG
# text before parsing (their content must never become an element), and
# separately read each mask's own rect bbox from the raw text so any element
# that still carries mask="url(#id)" can be clipped to it after flattening.
_MASK_DEF_RE = re.compile(r'<mask\b[^>]*\bid="([^"]+)"[^>]*>(.*?)</mask>', re.S)
_MASK_URL_RE = re.compile(r'url\(#([^)]+)\)')

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
        # over a subpath - the actual cause of a multi-minute hang on real
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
    x, y = pt; inside = False
    for (x1, y1), (x2, y2) in zip(outer, outer[1:] + outer[:1]):
        if (y1 > y) != (y2 > y) and x < (x2 - x1) * (y - y1) / (y2 - y1) + x1:
            inside = not inside
    return inside

def _interior_point(ring):
    # A point guaranteed to sit strictly inside ring's own interior: the
    # centroid of the ear triangle at ring's extreme (lowest-y) vertex, which
    # is always a convex corner regardless of winding. Deliberately not a
    # ring vertex itself - a sampled vertex can sit exactly on another ring's
    # edge and give a false containment reading.
    i = min(range(len(ring)), key=lambda k: (ring[k][1], ring[k][0]))
    a, b, c = ring[i - 1], ring[i], ring[(i + 1) % len(ring)]
    return ((a[0] + b[0] + c[0]) / 3, (a[1] + b[1] + c[1]) / 3)

def _mask_rect_bboxes(svg_text):
    # Parse each <mask id=...>...</mask> block's own content (before it gets
    # stripped out of the document) into an axis-aligned rect bbox, raw SVG
    # units. Raises if a mask's content isn't a single simple rectangle -
    # this module doesn't implement general SVG masking, only this one
    # inset-crop pattern.
    boxes = {}
    for mask_id, content in _MASK_DEF_RE.findall(svg_text):
        frag = SVG.parse(
            StringIO(f'<svg xmlns="http://www.w3.org/2000/svg">{content}</svg>'),
            reify=True,
        )
        seen = {}
        for el in frag.elements():
            if isinstance(el, Path) and len(el) > 0:
                seen[tuple(round(v, 6) for v in el.bbox())] = el
        paths = list(seen.values())
        if len(paths) != 1:
            raise ValueError(
                f"mask #{mask_id}: expected exactly one shape in its content, "
                f"found {len(paths)} - svg2poly only supports single-rect "
                "masks; ask Wells for a flattened re-export without this mask."
            )
        corners = []
        for seg in paths[0]:
            if isinstance(seg, (Line, Close)):
                corners.append((seg.end.x, seg.end.y))
            elif type(seg).__name__ == "Move":
                continue
            else:
                raise ValueError(
                    f"mask #{mask_id}: content has a curved segment - "
                    "svg2poly only supports single-rect masks; ask Wells for "
                    "a flattened re-export without this mask."
                )
        xs = [c[0] for c in corners]
        ys = [c[1] for c in corners]
        bbox = (min(xs), min(ys), max(xs), max(ys))
        bbox_area = (bbox[2] - bbox[0]) * (bbox[3] - bbox[1])
        ring_area = abs(_area(corners))
        if bbox_area <= 0 or abs(ring_area - bbox_area) > 1e-3 * bbox_area:
            raise ValueError(
                f"mask #{mask_id}: content is not an axis-aligned rectangle - "
                "svg2poly only supports single-rect masks; ask Wells for a "
                "flattened re-export without this mask."
            )
        boxes[mask_id] = bbox
    return boxes

def _sh_clip_edge(points, inside, isect):
    # One Sutherland-Hodgman clip pass against a single half-plane.
    if not points:
        return []
    out = []
    prev = points[-1]
    prev_in = inside(prev)
    for cur in points:
        cur_in = inside(cur)
        if cur_in:
            if not prev_in:
                out.append(isect(prev, cur))
            out.append(cur)
        elif prev_in:
            out.append(isect(prev, cur))
        prev, prev_in = cur, cur_in
    return out

def _clip_to_rect(ring, rect):
    # Sutherland-Hodgman polygon clip against an axis-aligned rectangle:
    # clip the ring against each of the rect's 4 half-planes in turn.
    xmin, ymin, xmax, ymax = rect
    def isect_x(a, b, x):
        t = (x - a[0]) / (b[0] - a[0])
        return [x, a[1] + t * (b[1] - a[1])]
    def isect_y(a, b, y):
        t = (y - a[1]) / (b[1] - a[1])
        return [a[0] + t * (b[0] - a[0]), y]
    pts = list(ring)
    pts = _sh_clip_edge(pts, lambda p: p[0] >= xmin, lambda a, b: isect_x(a, b, xmin))
    pts = _sh_clip_edge(pts, lambda p: p[0] <= xmax, lambda a, b: isect_x(a, b, xmax))
    pts = _sh_clip_edge(pts, lambda p: p[1] >= ymin, lambda a, b: isect_y(a, b, ymin))
    pts = _sh_clip_edge(pts, lambda p: p[1] <= ymax, lambda a, b: isect_y(a, b, ymax))
    return pts

def flatten(svg_path, scale, fills):
    fills = {f.lower() for f in fills}
    with open(svg_path) as f:
        svg_text = f.read()
    mask_boxes_raw = _mask_rect_bboxes(svg_text)
    stripped = _MASK_DEF_RE.sub("", svg_text)
    svg = SVG.parse(StringIO(stripped), reify=True)
    vb = svg.viewbox
    rings = []
    for el in svg.elements():
        if not isinstance(el, Path):
            continue
        fill = str(getattr(el, "fill", "")).lower()
        if fill not in fills:
            continue
        bb = el.bbox()
        if bb and vb and (bb[2]-bb[0]) * (bb[3]-bb[1]) >= 0.9 * vb.width * vb.height:
            continue                       # background-sized shape
        if len(el) == 0:
            continue
        el_rings = list(_subpaths_to_points(el, scale))
        mask_url = el.values.get("mask") if hasattr(el, "values") else None
        if mask_url:
            m = _MASK_URL_RE.search(mask_url)
            mask_id = m.group(1) if m else None
            if mask_id and mask_id in mask_boxes_raw:
                rx0, ry0, rx1, ry1 = mask_boxes_raw[mask_id]
                rect_mm = (rx0 * scale, ry0 * scale, rx1 * scale, ry1 * scale)
                el_rings = [c for c in (_clip_to_rect(r, rect_mm) for r in el_rings)
                            if len(c) >= 3]
        rings.extend(el_rings)
    return _classify(rings)

def _classify(rings):
    # evenodd nesting can be more than two levels deep: a plate, a letter-shaped
    # hole cut into it, and a counter island re-solidified inside that hole (the
    # bowl of an "a"/"e"/"p"). A flat "biggest ring wins, everything else is its
    # hole" pass puts the hole and its island in as siblings, which loses the
    # island. Classify by containment depth instead: depth counts how many other
    # rings contain this ring's interior point. Even depth (0, 2, 4, ...) is
    # filled material and becomes its own top-level poly's outline - this is what
    # turns a counter island back into a solid shape instead of void space. Odd
    # depth is a gap and becomes a hole of its immediate (tightest-area) parent.
    n = len(rings)
    interior = [_interior_point(r) for r in rings]
    areas = [abs(_area(r)) for r in rings]
    depth = [0] * n
    parent = [None] * n
    for i in range(n):
        best_area = None
        for j in range(n):
            # A container must have strictly larger area than what it
            # contains. Without this, two unrelated failure modes both look
            # like containment: (a) a ring's own nested descendant can swallow
            # the ring's interior-point test (e.g. a donut's outer boundary,
            # tested near its middle, geometrically also sits inside its own
            # smaller inner hole ring) and (b) two adjacent same-fill shapes
            # that merely share a boundary by design (a label cell's rule bar
            # flush against its background rectangle's edge) can trade false
            # positives depending on exactly where the test point lands. Real
            # ancestors are always larger than what they contain, so this one
            # filter rules out both without needing a cleverer test point.
            if i == j or areas[j] <= areas[i] or not _contains(rings[j], interior[i]):
                continue
            depth[i] += 1
            if best_area is None or areas[j] < best_area:
                best_area, parent[i] = areas[j], j
    polys = []
    poly_for_ring = {}
    for i in sorted(range(n), key=lambda k: depth[k]):
        if depth[i] % 2 == 0:
            poly = dict(outline=rings[i], holes=[])
            polys.append(poly)
            poly_for_ring[i] = poly
        else:
            poly_for_ring[parent[i]]["holes"].append(rings[i])
    return polys

if __name__ == "__main__":
    inp, scale, fills, outp = sys.argv[1], float(sys.argv[2]), sys.argv[3], sys.argv[4]
    with open(outp, "w") as f:
        json.dump(flatten(inp, scale, set(fills.split(","))), f)
    print("wrote", outp)
