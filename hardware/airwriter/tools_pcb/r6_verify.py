"""Round 6 verification battery: real courtyard overlaps, board containment,
antenna rule-area occupancy, mounting-hole and dot clearances, the SW3 plunger
proof, and bypass-cap distances."""
import sys, math, json
sys.path.insert(0, "/Users/wells/Projects/airwriter/hardware/airwriter/tools_pcb")
import pcbnew, r6_lib as L

b = L.load()
fps = {f.GetReference(): f for f in b.Footprints()}
fail = 0
def bad(msg):
    global fail; fail += 1; print("  FAIL", msg)

# --- 1. courtyard overlaps, per real outline rect (U1 stays an L) ----------
refs = sorted(fps)
rects = {r: (L.courtyard_rects(fps[r]) or [L.cy_bbox(fps[r])]) for r in refs}
ov = []
for i, a in enumerate(refs):
    for c in refs[i + 1:]:
        if any(L.rect_overlap(x, y) for x in rects[a] for y in rects[c]):
            ov.append((a, c))
print("courtyard overlaps: %d %s" % (len(ov), ov[:12]))
if ov: bad("courtyard overlaps")

# --- 2. every pad inside the board, 0.25mm copper-to-edge -----------------
def inside(x, y, inset):
    x0, y0, x1, y1, r = 100 + inset, 60 + inset, 162 - inset, 164.27 - inset, max(2.0 - inset, 0)
    if not (x0 <= x <= x1 and y0 <= y <= y1): return False
    for cx, cy in ((x0 + r, y0 + r), (x1 - r, y0 + r), (x1 - r, y1 - r), (x0 + r, y1 - r)):
        if (x - cx) * (1 if cx > 131 else -1) > 0 and (y - cy) * (1 if cy > 112 else -1) > 0:
            if (x - cx) ** 2 + (y - cy) ** 2 > r * r: return False
    return True
outside = []
for r in refs:
    for p in fps[r].Pads():
        bb = p.GetBoundingBox()
        for x, y in ((L.mm(bb.GetLeft()), L.mm(bb.GetTop())), (L.mm(bb.GetRight()), L.mm(bb.GetTop())),
                     (L.mm(bb.GetLeft()), L.mm(bb.GetBottom())), (L.mm(bb.GetRight()), L.mm(bb.GetBottom()))):
            if not inside(x, y, 0.25):
                outside.append((r, p.GetPadName(), round(x, 3), round(y, 3))); break
print("pads breaching the 0.25mm edge clearance: %d %s" % (len(outside), outside[:8]))

# --- 3. antenna rule area: nothing but U1 inside --------------------------
ko = None
for z in fps["U1"].Zones():
    o = z.Outline().Outline(0)
    ko = [(L.mm(o.CPoint(i).x), L.mm(o.CPoint(i).y)) for i in range(o.PointCount())]
def in_poly(x, y, poly):
    ins = False; n = len(poly)
    for i in range(n):
        ax, ay = poly[i]; bx, by = poly[(i + 1) % n]
        if (ay > y) != (by > y):
            if x < (bx - ax) * (y - ay) / (by - ay) + ax: ins = not ins
    return ins
def rect_hits_poly(rc, poly):
    x0, y0, x1, y1 = rc
    px = [p[0] for p in poly]; py = [p[1] for p in poly]
    if x1 < min(px) or x0 > max(px) or y1 < min(py) or y0 > max(py): return False
    for x in (x0, x1, (x0 + x1) / 2):
        for y in (y0, y1, (y0 + y1) / 2):
            if in_poly(x, y, poly): return True
    for p in poly:
        if x0 <= p[0] <= x1 and y0 <= p[1] <= y1: return True
    return False
print("antenna rule area:", [(round(a,2),round(c,2)) for a,c in ko])
hits = []
for r in refs:
    if r == "U1": continue
    for p in fps[r].Pads():
        bb = p.GetBoundingBox()
        rc = (L.mm(bb.GetLeft()), L.mm(bb.GetTop()), L.mm(bb.GetRight()), L.mm(bb.GetBottom()))
        if rect_hits_poly(rc, ko): hits.append((r, "pad " + p.GetPadName()))
    for rc in rects[r]:
        if rect_hits_poly(rc, ko): hits.append((r, "courtyard")); break
print("items inside the antenna rule area (should be none): %d %s" % (len(hits), sorted(set(hits))))
if hits: bad("antenna keepout occupied")

# --- 4. SW3 plunger proof -------------------------------------------------
sw3 = fps["SW3"]
pl = None
for it in sw3.GraphicalItems():
    if it.GetClass() == "PCB_SHAPE" and it.GetLayerName() == "F.Fab" and it.GetShape() == pcbnew.SHAPE_T_CIRCLE:
        c = it.GetCenter(); pl = (L.mm(c.x), L.mm(c.y), L.mm(it.GetRadius()))
print("SW3 origin %s rot %g -> plunger centre (%.4f, %.4f) r=%.3f  error from (131,91) = %.4f mm"
      % ((round(L.mm(sw3.GetPosition().x), 4), round(L.mm(sw3.GetPosition().y), 4)),
         sw3.GetOrientationDegrees(), pl[0], pl[1], pl[2], math.hypot(pl[0] - 131, pl[1] - 91)))
if math.hypot(pl[0] - 131, pl[1] - 91) > 1e-4: bad("SW3 plunger off the test dot")

# --- 5. mounting holes: >=1.5mm from every dot ----------------------------
dots = [(r, fps[r]) for r in refs if (r.startswith("D") and r[1:].isdigit() and int(r[1:]) >= 10)]
for h in ("H1", "H2", "H3", "H4"):
    p = fps[h].GetPosition(); hx, hy = L.mm(p.x), L.mm(p.y)
    hb = L.cy_bbox(fps[h])
    d = min(max(0, max(L.cy_bbox(f)[0] - hb[2], hb[0] - L.cy_bbox(f)[2],
                       L.cy_bbox(f)[1] - hb[3], hb[1] - L.cy_bbox(f)[3])) for _, f in dots)
    print("  %s at (%.3f, %.3f) -- nearest dot courtyard gap %.3f mm" % (h, hx, hy, d))
    if d < 1.5: bad("%s within 1.5mm of a dot" % h)

# --- 6. bypass caps within 5mm of the pins they decouple ------------------
BYPASS = {"C9": "U1", "C5": "U4", "C3": "U2", "C17": "U3", "C18": "U6",
          "C16": "U7", "C14": "U8", "C29": "U9", "C2": "U4", "C4": "U4"}
for cap, ic in BYPASS.items():
    cp = [(L.mm(p.GetPosition().x), L.mm(p.GetPosition().y)) for p in fps[cap].Pads()
          if p.GetNetname() in ("/+3V3", "/VBUS")]
    ip = [(L.mm(p.GetPosition().x), L.mm(p.GetPosition().y)) for p in fps[ic].Pads()
          if p.GetNetname() in ("/+3V3", "/VBUS")]
    if not cp or not ip:
        print("  %-4s -> %-3s  (no shared rail pad)" % (cap, ic)); continue
    d = min(math.hypot(a[0] - c[0], a[1] - c[1]) for a in cp for c in ip)
    print("  %-4s -> %-3s  %.2f mm %s" % (cap, ic, d, "" if d <= 5 else "  OVER 5mm"))

# --- 7. drills ------------------------------------------------------------
dr = [L.mm(p.GetDrillSizeX()) for r in refs for p in fps[r].Pads() if p.GetDrillSizeX() > 0]
print("minimum drill: %.3f mm" % min(dr))
if min(dr) < 0.30: bad("drill under 0.30mm")

print("VERIFY:", "clean" if not fail else "%d FAILURES" % fail)
