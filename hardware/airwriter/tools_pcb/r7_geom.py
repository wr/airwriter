"""Exact placement geometry for round 7: where may this part stand?

A raster fine enough not to lie about a 0.4mm slot is a raster too big to sweep,
and the first cut of the U4 search rejected the right cell on quantisation error
alone. So this is interval arithmetic instead. For one rotation and one y, every
obstacle that spans that y becomes a forbidden x-interval -- its courtyard grown
by the part's own half-width plus the packing gap, a Minkowski sum, so the test
is on the part's origin rather than on its body. What is left is where it may go.

Obstacles are real courtyard rectangles (decomposed, never bounding-boxed),
a disc around every dot that still carries an LED (an LED's courtyard is wider
than its dot but shorter, so the disc is what stops a part creeping under lit
glass), and every rule area that bars footprints -- the WROOM antenna keepout
bars them and is owned by U1's footprint, so it is invisible to both
board.Zones() and to any check that only walks courtyards. Leaving it out let a
sweep park two passives inside the antenna keepout, which DRC would have caught
and a reader would not.
"""
import json
import pcbnew
import r6_lib as L

LAYOUT = "/Users/wells/Projects/airwriter/hardware/panel/panel_layout.json"
ORIGIN = (100.0, 60.0)
BOARD = (100.0, 60.0, 162.0, 164.27)
GRID_SOUTH = 127.6      # grid zone above, lower band below; tall parts stay below
EDGE = 0.5              # copper-to-edge rule
GAP = 0.2               # courtyard packing gap
DOT_R = 5.6364 / 2


class Space:
    def __init__(self, board, movable, gap=GAP):
        self.b = board
        self.gap = gap
        self.fps = {f.GetReference(): f for f in board.Footprints()}
        self.movable = set(movable)
        self.geom = {r: self._probe(r) for r in self.movable}
        self.lay = json.loads(open(LAYOUT).read())
        self.led_at = {(round(L.mm(f.GetPosition().x), 2), round(L.mm(f.GetPosition().y), 2))
                       for f in board.Footprints() if "SK6812" in f.GetValue()}
        self.reset()

    def _rule_areas(self):
        out = [z for z in self.b.Zones() if z.GetIsRuleArea()]
        for f in self.b.Footprints():
            out += [z for z in f.Zones() if z.GetIsRuleArea()]
        return [z for z in out if z.GetDoNotAllowFootprints()]

    def _probe(self, ref):
        f = self.fps[ref]
        p0, r0 = f.GetPosition(), f.GetOrientationDegrees()
        ox, oy = L.mm(p0.x), L.mm(p0.y)
        out = {}
        for rot in (0, 90, 180, 270):
            f.SetOrientationDegrees(rot)
            rects = [(r[0] - ox, r[1] - oy, r[2] - ox, r[3] - oy)
                     for r in (L.courtyard_rects(f) or [L.cy_bbox(f)])]
            pads = {}
            for pad in f.Pads():
                q = pad.GetPosition()
                pads.setdefault(pad.GetNetname(), []).append((L.mm(q.x) - ox, L.mm(q.y) - oy))
            out[rot] = (rects, pads)
        f.SetOrientationDegrees(r0)
        f.SetPosition(p0)
        return out

    def reset(self, exclude=()):
        """Rebuild the obstacle set from the live board, minus `exclude`."""
        skip = set(exclude)
        self.obst = []
        for f in self.b.Footprints():
            if f.GetReference() in skip:
                continue
            self.obst += (L.courtyard_rects(f) or [L.cy_bbox(f)])
        for z in self._rule_areas():
            o = z.Outline().Outline(0)
            pts = [(L.mm(o.CPoint(i).x), L.mm(o.CPoint(i).y)) for i in range(o.PointCount())]
            self.obst += L.decompose(pts) or [(min(p[0] for p in pts), min(p[1] for p in pts),
                                               max(p[0] for p in pts), max(p[1] for p in pts))]
        for d in self.lay["dots"]:
            bx, by = ORIGIN[0] + d["x"], ORIGIN[1] + d["y"]
            if (round(bx, 2), round(by, 2)) in self.led_at:
                self.obst.append((bx - DOT_R, by - DOT_R, bx + DOT_R, by + DOT_R))
        self._index()

    def add(self, ref, rot, x, y):
        self.obst += [(x + r[0], y + r[1], x + r[2], y + r[3])
                      for r in self.geom[ref][rot][0]]
        self._index()

    def _index(self):
        # Bucket by whole-mm y band. A query must read every band its part's
        # courtyard spans, not just the band its origin sits in -- U4 reaches
        # 4.4mm either side of its own origin.
        self.buckets = {}
        for r in self.obst:
            for k in range(int(r[1]) - 1, int(r[3]) + 2):
                self.buckets.setdefault(k, []).append(r)

    def _near(self, ylo, yhi):
        seen, out = set(), []
        for k in range(int(ylo) - 1, int(yhi) + 2):
            for r in self.buckets.get(k, ()):
                if id(r) not in seen:
                    seen.add(id(r))
                    out.append(r)
        return out

    def free_intervals(self, ref, rot, y, win):
        """x-intervals where `ref` at `rot` may stand with its origin at `y`."""
        ylo, yhi, xlo, xhi = win
        g = self.gap
        spans = None
        for cx0, cy0, cx1, cy1 in self.geom[ref][rot][0]:
            # The window is the region the courtyard must lie INSIDE -- a board
            # edge or a reserved channel, not a neighbour. The 0.2mm packing gap
            # is for real obstacles; charging it against a soft boundary too
            # cost an 0805 its slot in a 2.09mm band that genuinely fits it.
            if y + cy0 < ylo or y + cy1 > yhi:
                return []
            lo, hi = xlo - cx0, xhi - cx1
            blocked = sorted((ox0 - cx1 - g, ox1 - cx0 + g)
                             for ox0, oy0, ox1, oy1 in self._near(y + cy0 - g, y + cy1 + g)
                             if not (y + cy1 + g <= oy0 or y + cy0 - g >= oy1))
            out, cur = [], lo
            for a, c in blocked:
                if a > cur:
                    out.append((cur, min(a, hi)))
                cur = max(cur, c)
                if cur >= hi:
                    break
            if cur < hi:
                out.append((cur, hi))
            out = [(a, c) for a, c in out if c - a >= 1e-9]
            if spans is None:
                spans = out
            else:   # a multi-rect courtyard has to be clear in every rect at once
                spans = [(max(a, p), min(c, q)) for a, c in spans for p, q in out
                         if min(c, q) - max(a, p) >= 1e-9]
            if not spans:
                return []
        return spans or []

    def poses(self, ref, win, rots=(0, 90, 180, 270), step=0.1):
        y = win[0]
        while y <= win[1]:
            for rot in rots:
                for a, c in self.free_intervals(ref, rot, y, win):
                    x = a
                    while x < c:
                        yield rot, x, y
                        x += step
                    yield rot, c, y
            y += step

    def pads(self, ref, rot, x, y, net=None):
        d = self.geom[ref][rot][1]
        keys = [net] if net else list(d)
        return [(x + dx, y + dy) for k in keys for dx, dy in d.get(k, ())]

    def place(self, ref, rot, x, y):
        f = self.fps[ref]
        f.SetOrientationDegrees(rot)
        f.SetPosition(pcbnew.VECTOR2I(L.nm(x), L.nm(y)))


def dist(a, c):
    return min(((p[0] - q[0]) ** 2 + (p[1] - q[1]) ** 2) ** 0.5 for p in a for q in c)
