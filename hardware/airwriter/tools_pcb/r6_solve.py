"""Round 6 step 2: solve placement for the 40 parts the structural moves displaced.

Rasterised free-space search.  Obstacles are real courtyard rectangles (U1's
L-shape stays two rectangles, never a bounding box), inflated by the courtyard
gap; a candidate is legal only if every one of its own courtyard rectangles is
free, its pads clear the board edge, and it misses the antenna rule area.
Blocks are placed in function order so same-block parts land together.
"""
import json, math, sys
import numpy as np

HERE = "/Users/wells/Projects/airwriter/hardware/airwriter/tools_pcb/"
G = json.load(open(HERE + "r6_geom.json"))
STEP = 0.1
GAP = 0.20          # minimum courtyard-to-courtyard gap
EDGE_PAD = 0.30     # minimum pad-copper to board-edge
B = G["board"]

# ---- placement order, block by block -------------------------------------
ORDER = [
    "C16", "R15", "R16", "R14",                  # U7 / PIC block
    "C14", "R10", "R11",                         # U8 / LocalTalk A-B
    "C18", "R12", "R13",                         # U6 / LocalTalk Y-Z
    "C17", "C10", "C11", "C12", "C13", "R9",     # U3 / RS-232 block
    "C3", "C7", "R6",                             # U2 block, boot cap, TX bias
    "C22", "C1",                                 # VBUS bulk
    "R1", "R2",                                  # 5.1k CC pulldowns at J1
]
EXPLICIT = {"C3": "U2@/+3V3", "C17": "U3@/+3V3", "C18": "U6@/+3V3",
            "C16": "U7@/+3V3", "C14": "U8@/+3V3",
            "C10": "U3", "C11": "U3", "C12": "U3", "C13": "U3",
            "C1": "U4@/VBUS", "C22": "U4@/VBUS", "R6": "U2@/+3V3"}
SKIP_NETS = {"/GND", "/+3V3", "/VBUS"}
ROTS = {r: (0, 90, 180, 270) for r in ("U2", "U3", "U4", "U5", "U6", "U7", "U8")}

# ---- raster --------------------------------------------------------------
X0, X1 = 99.6, 162.4
Y0, Y1 = 126.2, 164.8
NX = int(round((X1 - X0) / STEP))
NY = int(round((Y1 - Y0) / STEP))
xs = X0 + (np.arange(NX) + 0.5) * STEP
ys = Y0 + (np.arange(NY) + 0.5) * STEP
XX, YY = np.meshgrid(xs, ys, indexing="ij")

def rounded_rect_inside(x, y, inset):
    """Point inside the board outline shrunk by `inset` (2mm corner radii)."""
    x0, y0, x1, y1, r = B["x0"] + inset, B["y0"] + inset, B["x1"] - inset, B["y1"] - inset, B["radius"]
    ok = (x >= x0) & (x <= x1) & (y >= y0) & (y <= y1)
    rr = max(r - inset, 0.0)
    for cx, cy in ((x0 + rr, y0 + rr), (x1 - rr, y0 + rr), (x1 - rr, y1 - rr), (x0 + rr, y1 - rr)):
        near = ((x - cx) * np.sign(cx - (x0 + x1) / 2) > 0) & ((y - cy) * np.sign(cy - (y0 + y1) / 2) > 0)
        ok &= ~(near & ((x - cx) ** 2 + (y - cy) ** 2 > rr ** 2))
    return ok

def poly_inside(px, py, poly):
    inside = np.zeros(px.shape, bool)
    n = len(poly)
    for i in range(n):
        ax, ay = poly[i]; bx, by = poly[(i + 1) % n]
        cond = ((ay > py) != (by > py))
        with np.errstate(divide="ignore", invalid="ignore"):
            xint = (bx - ax) * (py - ay) / (by - ay + 1e-12) + ax
        inside ^= cond & (px < xint)
    return inside

blocked = ~rounded_rect_inside(XX, YY, 0.0)
blocked |= (YY < B["band_y0"])                       # keep out of the LED grid zone
ko = G["keepout"]
kx = [p[0] for p in ko]; ky = [p[1] for p in ko]
# inflate the rule area by GAP by testing the point against a slightly grown poly
blocked |= poly_inside(XX, YY, ko)
blocked |= ((XX >= min(kx) - GAP) & (XX <= max(kx) + GAP) &
            (YY >= min(ky) - GAP) & (YY <= max(ky) + GAP) &
            poly_inside(np.clip(XX, min(kx), max(kx)), np.clip(YY, min(ky), max(ky)), ko))

def mark(rects, grow):
    for x0, y0, x1, y1 in rects:
        i0 = max(0, int(math.floor((x0 - grow - X0) / STEP)))
        i1 = min(NX, int(math.ceil((x1 + grow - X0) / STEP)))
        j0 = max(0, int(math.floor((y0 - grow - Y0) / STEP)))
        j1 = min(NY, int(math.ceil((y1 + grow - Y0) / STEP)))
        if i1 > i0 and j1 > j0:
            blocked[i0:i1, j0:j1] = True

MOV = set(G["movable"])
for ref, p in G["parts"].items():
    if ref in MOV:
        continue
    r = p["rots"][str(int(p["rot"]) % 360)]["cy"]
    mark([[p["x"] + a, p["y"] + b, p["x"] + c, p["y"] + d] for a, b, c, d in r], GAP)

EDGEMASK = ~rounded_rect_inside(XX, YY, EDGE_PAD)   # too close to the edge for copper

def integral(arr):
    return np.cumsum(np.cumsum(arr.astype(np.int32), 0), 1)

IE = integral(EDGEMASK)
II = integral(blocked)

def _free(I, x0, y0, x1, y1):
    i0 = int(math.floor((x0 - X0) / STEP)); i1 = int(math.ceil((x1 - X0) / STEP))
    j0 = int(math.floor((y0 - Y0) / STEP)); j1 = int(math.ceil((y1 - Y0) / STEP))
    if i0 < 0 or j0 < 0 or i1 > NX or j1 > NY or i1 <= i0 or j1 <= j0:
        return False
    a = I[i1 - 1, j1 - 1]
    b = I[i0 - 1, j1 - 1] if i0 else 0
    c = I[i1 - 1, j0 - 1] if j0 else 0
    d = I[i0 - 1, j0 - 1] if (i0 and j0) else 0
    return (a - b - c + d) == 0

# ---- anchors -------------------------------------------------------------
placed = {}   # ref -> (x, y, rot)
def part_pads(ref):
    p = G["parts"][ref]
    if ref in placed:
        x, y, rot = placed[ref]
        dx, dy = x - p["x"], y - p["y"]
        return {n: [[a + dx, b + dy] for a, b in v] for n, v in p["pads"].items()}
    if ref in MOV:
        return None                      # not placed yet -- unusable as an anchor
    return p["pads"]

def anchor(ref):
    p = G["parts"][ref]
    pts = []
    for n in p["pads"]:
        if n in SKIP_NETS or n.startswith("unconnected-"):
            continue
        for other in G["nets"].get(n, []):
            if other == ref:
                continue
            op = part_pads(other)
            if op and n in op:
                pts += op[n]
    if not pts and ref in EXPLICIT:
        tgt = EXPLICIT[ref]
        if "@" in tgt:
            t, net = tgt.split("@")
            op = part_pads(t)
            pts = op.get(net, []) if op else []
        else:
            op = part_pads(tgt)
            if op:
                pts = [q for v in op.values() for q in v]
    if not pts:
        q = G["parts"].get(EXPLICIT.get(ref, "U1"))
        pts = [[q["x"], q["y"]]]
    return (sum(q[0] for q in pts) / len(pts), sum(q[1] for q in pts) / len(pts))

# ---- search --------------------------------------------------------------
report = []
for ref in ORDER:
    p = G["parts"][ref]
    ax, ay = anchor(ref)
    best = None
    for rot in ROTS.get(ref, (0, 90)):
        r = p["rots"][str(rot)]
        cy = r["cy"]; pads = r["pads"]
        cx0 = min(c[0] for c in cy); cy0 = min(c[1] for c in cy)
        cx1 = max(c[2] for c in cy); cy1 = max(c[3] for c in cy)
        # candidate origins on the raster; vectorised coarse filter first
        ox = np.arange(X0 - cx0, X1 - cx1, STEP)
        oy = np.arange(Y0 - cy0, Y1 - cy1, STEP)
        if len(ox) == 0 or len(oy) == 0:
            continue
        OX, OY = np.meshgrid(ox, oy, indexing="ij")
        cost = (OX + (cx0 + cx1) / 2 - ax) ** 2 + (OY + (cy0 + cy1) / 2 - ay) ** 2
        flat = cost.ravel(); order = np.argsort(flat)
        fx = OX.ravel(); fy = OY.ravel()
        for k in order:
            x, y = float(fx[k]), float(fy[k])
            if pads and not _free(IE, x + pads[0], y + pads[1], x + pads[2], y + pads[3]):
                continue
            if all(_free(II, x + a, y + b, x + c, y + d) for a, b, c, d in cy):
                cand = (float(flat[k]), x, y, rot)
                if best is None or cand[0] < best[0]:
                    best = cand
                break
    if best is None:
        print("FAILED", ref); report.append((ref, None)); continue
    _, x, y, rot = best
    placed[ref] = (x, y, rot)
    r = p["rots"][str(rot)]["cy"]
    mark([[x + a, y + b, x + c, y + d] for a, b, c, d in r], GAP)
    II = integral(blocked)
    d = math.hypot(x + sum(c[0] + c[2] for c in r) / (2 * len(r)) - ax,
                   y + sum(c[1] + c[3] for c in r) / (2 * len(r)) - ay)
    report.append((ref, (x, y, rot, d)))
    print("  %-4s -> (%8.3f, %8.3f) rot %3d   anchor (%.2f, %.2f) dist %.2fmm"
          % (ref, x, y, rot, ax, ay, d))

fails = [r for r, v in report if v is None]
json.dump({r: v[:3] for r, v in report if v}, open(HERE + "r6_placement.json", "w"), indent=1)
print("placed %d / %d ; failures: %s" % (len(placed), len(ORDER), fails or "none"))
