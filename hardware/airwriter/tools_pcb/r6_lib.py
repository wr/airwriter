"""Shared geometry helpers for round 6. Absolute paths only."""
import pcbnew, math, json, sys

BOARD = "/Users/wells/Projects/airwriter/hardware/airwriter/airwriter.kicad_pcb"

def load():
    return pcbnew.LoadBoard(BOARD)

def mm(v): return v / 1e6
def nm(v): return int(round(v * 1e6))

def decompose(pts):
    """Split a rectilinear polygon into non-overlapping rectangles.

    U1's courtyard is a genuine L (module body + antenna area); its bounding box
    covers the notch and throws false overlaps against every neighbour there.
    Vertical-slab sweep: for each x-slab between consecutive vertex x values,
    ray-cast a vertical line at the slab midpoint to get the interior y spans.
    """
    xs = sorted(set(round(p[0], 6) for p in pts))
    if len(xs) < 2:
        return []
    n = len(pts)
    out = []
    for i in range(len(xs) - 1):
        xm = (xs[i] + xs[i + 1]) / 2.0
        ys = []
        for j in range(n):
            ax, ay = pts[j]; bx, by = pts[(j + 1) % n]
            if (ax > xm) != (bx > xm):
                ys.append(ay + (by - ay) * (xm - ax) / (bx - ax))
        ys.sort()
        for k in range(0, len(ys) - 1, 2):
            out.append((xs[i], ys[k], xs[i + 1], ys[k + 1]))
    return out

def courtyard_rects(fp):
    """F.Courtyard as a list of (x1,y1,x2,y2) mm rects -- never a bounding box."""
    try:
        poly = fp.GetCourtyard(pcbnew.F_Cu)
    except Exception:
        poly = None
    rects = []
    if poly is not None and poly.OutlineCount() > 0:
        for i in range(poly.OutlineCount()):
            o = poly.Outline(i)
            pts = [(mm(o.CPoint(j).x), mm(o.CPoint(j).y)) for j in range(o.PointCount())]
            if len(pts) <= 4:
                xs = [p[0] for p in pts]; ys = [p[1] for p in pts]
                rects.append((min(xs), min(ys), max(xs), max(ys)))
            else:
                rects += decompose(pts)
    return rects

def courtyard_polys(fp):
    """Return list of point-lists (mm) for each F.Courtyard outline."""
    out = []
    try:
        poly = fp.GetCourtyard(pcbnew.F_Cu)
    except Exception:
        return out
    if poly is None: return out
    for i in range(poly.OutlineCount()):
        o = poly.Outline(i)
        pts = [(mm(o.CPoint(j).x), mm(o.CPoint(j).y)) for j in range(o.PointCount())]
        out.append(pts)
    return out

def cy_bbox(fp):
    rs = courtyard_rects(fp)
    if not rs:
        bb = fp.GetBoundingBox(False, False)
        return (mm(bb.GetLeft()), mm(bb.GetTop()), mm(bb.GetRight()), mm(bb.GetBottom()))
    return (min(r[0] for r in rs), min(r[1] for r in rs),
            max(r[2] for r in rs), max(r[3] for r in rs))

def rect_overlap(a, b, margin=0.0):
    return not (a[2] + margin <= b[0] or b[2] + margin <= a[0] or
                a[3] + margin <= b[1] or b[3] + margin <= a[1])

def fp_overlap(fa, fb, margin=0.0):
    """True courtyard overlap using per-outline rect decomposition (handles U1's L)."""
    ra = courtyard_rects(fa) or [cy_bbox(fa)]
    rb = courtyard_rects(fb) or [cy_bbox(fb)]
    for x in ra:
        for y in rb:
            if rect_overlap(x, y, margin):
                return True
    return False

def pads_bbox(fp):
    xs=[];ys=[]
    for p in fp.Pads():
        bb = p.GetBoundingBox()
        xs += [mm(bb.GetLeft()), mm(bb.GetRight())]
        ys += [mm(bb.GetTop()), mm(bb.GetBottom())]
    if not xs: return None
    return (min(xs), min(ys), max(xs), max(ys))

def inventory(b):
    out = []
    for fp in b.Footprints():
        r = fp.GetReference()
        p = fp.GetPosition()
        cy = cy_bbox(fp)
        out.append(dict(ref=r, val=fp.GetValue(),
                        x=round(mm(p.x),4), y=round(mm(p.y),4),
                        rot=fp.GetOrientationDegrees(),
                        lib=str(fp.GetFPID().GetUniStringLibId()),
                        cy=[round(v,4) for v in cy],
                        w=round(cy[2]-cy[0],3), h=round(cy[3]-cy[1],3),
                        npads=len(list(fp.Pads()))))
    return out
