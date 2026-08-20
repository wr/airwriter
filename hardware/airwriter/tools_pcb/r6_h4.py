"""Check H4's position against every legal alternative. Reports, never writes.

H4 is an index post, not the panel's only support, so the measure is support
spread: the area of the convex hull of all four holes.  (Maximising the shortest
distance to the other three was tried first and is wrong -- it parks H4 6.6mm
from H1 and grows the hull barely at all.)  Satellites are ignored here, since
the solver fits those around H4 afterwards.
"""
import sys, math
sys.path.insert(0, "/Users/wells/Projects/airwriter/hardware/airwriter/tools_pcb")
import pcbnew, r6_lib as L

GAP, DOT_GAP, STEP = 0.20, 1.50, 0.10

b = L.load()
fps = {f.GetReference(): f for f in b.Footprints()}
h4 = fps["H4"]
cy0 = L.cy_bbox(h4)
p0 = h4.GetPosition()
off = (cy0[0] - L.mm(p0.x), cy0[1] - L.mm(p0.y), cy0[2] - L.mm(p0.x), cy0[3] - L.mm(p0.y))
W, H = off[2] - off[0], off[3] - off[1]
print("H4 courtyard %.3f x %.3f" % (W, H))

def is_dot(r):
    return (r.startswith("D") and r[1:].isdigit() and int(r[1:]) >= 10) or \
           (r.startswith("C") and r[1:].isdigit() and 30 <= int(r[1:]) <= 66)

obst = []
for r, f in fps.items():
    if r == "H4" or r in L.MOVABLE:
        continue
    g = DOT_GAP if is_dot(r) else GAP
    for rc in (L.courtyard_rects(f) or [L.cy_bbox(f)]):
        obst.append((rc[0] - g, rc[1] - g, rc[2] + g, rc[3] + g))

# the antenna rule area bars footprints too
for z in fps["U1"].Zones():
    o = z.Outline().Outline(0)
    pts = [(L.mm(o.CPoint(i).x), L.mm(o.CPoint(i).y)) for i in range(o.PointCount())]
    obst.append((min(p[0] for p in pts) - GAP, min(p[1] for p in pts) - GAP,
                 max(p[0] for p in pts) + GAP, max(p[1] for p in pts) + GAP))

holes = [(L.mm(fps[h].GetPosition().x), L.mm(fps[h].GetPosition().y)) for h in ("H1", "H2", "H3")]

def hull_area(pts):
    p = sorted(set(pts))
    def half(seq):
        out = []
        for q in seq:
            while len(out) > 1 and ((out[-1][0] - out[-2][0]) * (q[1] - out[-2][1])
                                    - (out[-1][1] - out[-2][1]) * (q[0] - out[-2][0])) <= 0:
                out.pop()
            out.append(q)
        return out[:-1]
    h = half(p) + half(reversed(p))
    a = 0.0
    for i in range(len(h)):
        x1, y1 = h[i]; x2, y2 = h[(i + 1) % len(h)]
        a += x1 * y2 - x2 * y1
    return abs(a) / 2.0

# A mounting hole needs material around it for the case's screw boss, so its
# courtyard is held 1mm inside the outline rather than merely on the board.
EDGE = 1.0

def inside(x, y):
    if not (100 + EDGE <= x <= 162 - EDGE and 60 + EDGE <= y <= 164.27 - EDGE):
        return False
    r = 2.0 - EDGE
    for cx, cy in ((102, 62), (160, 62), (160, 162.27), (102, 162.27)):
        if (x - cx) * (1 if cx > 131 else -1) > 0 and (y - cy) * (1 if cy > 112 else -1) > 0:
            if (x - cx) ** 2 + (y - cy) ** 2 > r * r:
                return False
    return True

best = None
y = 127.6 - off[1]
while y + off[3] <= 164.27:
    x = 100 - off[0]
    while x + off[2] <= 162:
        r = (x + off[0], y + off[1], x + off[2], y + off[3])
        if all(inside(a, c) for a in (r[0], r[2]) for c in (r[1], r[3])) and \
           not any(L.rect_overlap(r, o) for o in obst):
            score = hull_area(holes + [(x, y)])
            if best is None or score > best[0]:
                best = (score, x, y)
        x += STEP
    y += STEP

assert best, "no legal H4 position"
score, x, y = best
cur = (L.mm(p0.x), L.mm(p0.y))
print("three holes alone:      hull %7.1f mm2" % hull_area(holes))
print("H4 as placed (%.3f, %.3f): hull %7.1f mm2   H1 %.1f  H2 %.1f  H3 %.1f mm"
      % (cur[0], cur[1], hull_area(holes + [cur]),
         *[math.hypot(cur[0] - hx, cur[1] - hy) for hx, hy in holes]))
print("best legal alternative (%.3f, %.3f): hull %7.1f mm2" % (x, y, score))
print("VERDICT:", "as-placed is best" if hull_area(holes + [cur]) >= score - 1e-6
      else "an alternative beats it by %.1f mm2" % (score - hull_area(holes + [cur])))
