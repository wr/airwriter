"""Round 7: find U4's new home in the LED grid, and its bypass caps' with it.

Designer mandate: the regulator and its own input/output caps leave the crowded
north-east strip for a gold-dot cell -- a cell the panel never lights, so a
1.8mm part under it blocks nothing. Which cell is not a guess: this sweeps the
whole grid zone at all four rotations and scores every position that fits, by
the run the mandate names -- /VBUS in from J1 plus /+3V3 out to U1's power pad.

Partners come from the netlist, not from the old block table. U4's pin 1 is
/VBUS with C21 (22u) on it, pin 3 is /+3V3 with C2 and C4 (10u each), and pin 2
-- the SOT-223 tab -- is /GND, not VOUT as the mandate supposed, so it merges
into the GND pour and needs no copper patch of its own. C5, C6, C9 and R3 stay
behind: they are U1's rail caps and its EN network, and they belong at U1's
pads, not at the regulator's.

C2 stays behind too, and that is a judgement call worth stating. C4 is the
LDO's output cap and has to sit at pin 3 or the regulator's control loop is
unstable, so it travels. C2 does double duty -- it is also the ESP32 module's
bulk reservoir, 6mm from U1's power pad -- and taking it 20mm away to follow the
regulator would leave the module with nothing bigger than a 100n nearby.

The geometry is done as exact interval arithmetic, not on a raster. A raster
fine enough not to lie about a 0.4mm slot is a raster too big to sweep, and the
first cut of this script rejected the best cell on quantisation error alone.
For each rotation and each y, the obstacles that span that y turn into forbidden
x-intervals (their courtyard grown by the part's own half-width plus the 0.2mm
packing gap -- a Minkowski sum, so the test is on the origin, not on the body).
What is left is where the part may stand.

Obstacles are real courtyards plus a disc around every dot that still has an
LED: an LED's courtyard is wider than its dot but shorter, so the disc is what
keeps a part from creeping under lit glass.
"""
import json
import sys
sys.path.insert(0, "/Users/wells/Projects/airwriter/hardware/airwriter/tools_pcb")
import pcbnew
import r6_lib as L

LAYOUT = "/Users/wells/Projects/airwriter/hardware/panel/panel_layout.json"
OUT = "/Users/wells/Projects/airwriter/hardware/airwriter/tools_pcb/r7_u4.json"
ORIGIN = (100.0, 60.0)
GRID_SOUTH = 127.6      # the grid zone's boundary; tall parts live south of it
BOARD = (100.0, 60.0, 162.0, 164.27)
EDGE = 0.5              # copper-to-edge rule
GAP = 0.2               # courtyard packing gap
YSTEP = 0.05
DOT_R = 5.6364 / 2
BLOCK = ["U4", "C21", "C4"]

b = L.load()
fps = {f.GetReference(): f for f in b.Footprints()}
lay = json.loads(open(LAYOUT).read())


def probe(ref):
    f = fps[ref]
    p0, r0 = f.GetPosition(), f.GetOrientationDegrees()
    ox, oy = L.mm(p0.x), L.mm(p0.y)
    out = {}
    for rot in (0, 90, 180, 270):
        f.SetOrientationDegrees(rot)
        rects = [(r[0] - ox, r[1] - oy, r[2] - ox, r[3] - oy) for r in L.courtyard_rects(f)]
        pads = {}
        for pad in f.Pads():
            q = pad.GetPosition()
            pads.setdefault(pad.GetNetname(), []).append((L.mm(q.x) - ox, L.mm(q.y) - oy))
        out[rot] = (rects, pads)
    f.SetOrientationDegrees(r0)
    f.SetPosition(p0)
    return out


GEOM = {ref: probe(ref) for ref in BLOCK}
OBST = []


def rebuild_index():
    """Bucket obstacles by whole-mm y bands. A query must read every band its
    part's courtyard spans, not just the band its origin sits in -- U4 reaches
    4.4mm either side of its origin, and looking only at the origin's own band
    was the first cut of this script's one real bug."""
    global BUCKETS
    BUCKETS = {}
    for r in OBST:
        for k in range(int(r[1]) - 1, int(r[3]) + 2):
            BUCKETS.setdefault(k, []).append(r)


def near(ylo, yhi):
    seen, out = set(), []
    for k in range(int(ylo) - 1, int(yhi) + 2):
        for r in BUCKETS.get(k, ()):
            if id(r) not in seen:
                seen.add(id(r))
                out.append(r)
    return out


for f in b.Footprints():
    if f.GetReference() in BLOCK:
        continue
    OBST += (L.courtyard_rects(f) or [L.cy_bbox(f)])
led_at = {(round(L.mm(f.GetPosition().x), 2), round(L.mm(f.GetPosition().y), 2))
          for f in b.Footprints() if "SK6812" in f.GetValue()}
lit = 0
for d in lay["dots"]:
    bx, by = ORIGIN[0] + d["x"], ORIGIN[1] + d["y"]
    if (round(bx, 2), round(by, 2)) in led_at:
        lit += 1
        OBST.append((bx - DOT_R, by - DOT_R, bx + DOT_R, by + DOT_R))
assert lit == 35, lit
rebuild_index()


def free_intervals(ref, rot, y, ylo, yhi, xlo, xhi):
    """x-intervals where `ref` at rotation `rot` may stand with its origin at y."""
    spans = []
    for cx0, cy0, cx1, cy1 in GEOM[ref][rot][0]:
        lo, hi = xlo - cx0, xhi - cx1
        if y + cy0 - GAP < ylo or y + cy1 + GAP > yhi:
            return []
        blocked = []
        for ox0, oy0, ox1, oy1 in near(y + cy0 - GAP, y + cy1 + GAP):
            if y + cy1 + GAP <= oy0 or y + cy0 - GAP >= oy1:
                continue
            blocked.append((ox0 - cx1 - GAP, ox1 - cx0 + GAP))
        blocked.sort()
        out, cur = [], lo
        for a, bb in blocked:
            if a > cur:
                out.append((cur, min(a, hi)))
            cur = max(cur, bb)
            if cur >= hi:
                break
        if cur < hi:
            out.append((cur, hi))
        out = [(a, bb) for a, bb in out if bb - a >= 1e-9]
        if not spans:
            spans = out
        else:  # multi-rect courtyard: every rect must be clear at once
            spans = [(max(a, c), min(bb, d)) for a, bb in spans for c, d in out
                     if min(bb, d) - max(a, c) >= 1e-9]
        if not spans:
            return []
    return spans


def padsof(ref, net):
    return [(L.mm(p.GetPosition().x), L.mm(p.GetPosition().y))
            for p in fps[ref].Pads() if p.GetNetname() == net]


def dist(a, c):
    return min(((p[0] - q[0]) ** 2 + (p[1] - q[1]) ** 2) ** 0.5 for p in a for q in c)


def padat(ref, rot, px, py, net):
    return [(px + dx, py + dy) for dx, dy in GEOM[ref][rot][1][net]]


J1_VBUS = padsof("J1", "/VBUS")
U1_3V3 = padsof("U1", "/+3V3")
cur = dist(J1_VBUS, padsof("U4", "/VBUS")) + dist(padsof("U4", "/+3V3"), U1_3V3)
print(f"U4 where it stands today: {cur:.2f}mm of /VBUS-in + /+3V3-out run")


def sweep(ref, ylo, yhi, xlo, xhi, score, rots=(0, 90, 180, 270), inset=0.0):
    best, n = None, 0
    y = ylo
    while y <= yhi:
        for rot in rots:
            for a, c in free_intervals(ref, rot, y, ylo, yhi, xlo, xhi):
                n += 1
                for x in (a + inset, c - inset, (a + c) / 2):
                    if a - 1e-9 <= x <= c + 1e-9:
                        s = score(rot, x, y)
                        if best is None or s < best[0]:
                            best = (s, rot, x, y)
        y += YSTEP
    return best, n


# U4's pose and its caps are chosen together, not one after the other. Picking
# the pose that minimises the run alone wins ~1mm of copper and then leaves the
# caps 9-10mm from the pins they decouple, which is the wrong trade: an LDO with
# its output cap 9mm away is a stability question, not a layout preference. So
# every pose that fits is scored on the run PLUS its caps' reach, weighted --
# C4 heavily (it sets the regulator's loop), C21 lightly (bulk on the input).
W_C4, W_C21 = 3.0, 1.0
CAP_WINDOW = 7.0


def u4score(rot, x, y):
    return (dist(J1_VBUS, padat("U4", rot, x, y, "/VBUS"))
            + dist(padat("U4", rot, x, y, "/+3V3"), U1_3V3))


def poses(ref, ylo, yhi, xlo, xhi, rots, step):
    y = ylo
    while y <= yhi:
        for rot in rots:
            for a, c in free_intervals(ref, rot, y, ylo, yhi, xlo, xhi):
                x = a
                while x <= c + 1e-9:
                    yield rot, x, y
                    x += step
                if c - a > 1e-9:
                    yield rot, c, y
        y += step


ZONE = (BOARD[1] + EDGE, GRID_SOUTH, BOARD[0] + EDGE, BOARD[2] - EDGE)
cands = [(u4score(*p), p) for p in poses("U4", *ZONE, rots=(0, 90, 180, 270), step=0.25)]
cands.sort(key=lambda t: t[0])
print(f"{len(cands)} legal U4 poses in the grid zone; best run alone "
      f"{cands[0][0]:.2f}mm at {cands[0][1]}")

base = [r for r in OBST]
best = None
for run, (rot, px, py) in cands[:400]:
    if best and run + 0 >= best[0]:
        break
    OBST[:] = base + [(px + r[0], py + r[1], px + r[2], py + r[3])
                      for r in GEOM["U4"][rot][0]]
    rebuild_index()
    caps, total, ok = {}, run, True
    for ref, net, w in (("C4", "/+3V3", W_C4), ("C21", "/VBUS", W_C21)):
        anchor = padat("U4", rot, px, py, net)
        cb = None
        for r2, qx, qy in poses(ref, max(ZONE[0], py - CAP_WINDOW), min(ZONE[1], py + CAP_WINDOW),
                                max(ZONE[2], px - CAP_WINDOW), min(ZONE[3], px + CAP_WINDOW),
                                rots=(0, 90), step=0.25):
            d = dist(padat(ref, r2, qx, qy, net), anchor)
            if cb is None or d < cb[0]:
                cb = (d, r2, qx, qy)
        if cb is None:
            ok = False
            break
        caps[ref] = cb
        total += w * cb[0]
        OBST.extend((cb[2] + r[0], cb[3] + r[1], cb[2] + r[2], cb[3] + r[3])
                    for r in GEOM[ref][cb[1]][0])
        rebuild_index()
    if ok and (best is None or total < best[0]):
        best = (total, run, rot, px, py, dict(caps))
OBST[:] = base
rebuild_index()

assert best, "no U4 pose in the grid zone takes its caps"
total, score, rot, px, py, caps = best
print(f"chosen U4 ({px:.3f}, {py:.3f}) rot {rot}")
print(f"   /VBUS in {dist(J1_VBUS, padat('U4', rot, px, py, '/VBUS')):.2f}mm + "
      f"/+3V3 out {dist(padat('U4', rot, px, py, '/+3V3'), U1_3V3):.2f}mm = {score:.2f}mm "
      f"({score - cur:+.2f}mm against today)")
cell = min(lay["dots"], key=lambda d: (ORIGIN[0] + d["x"] - px) ** 2 + (ORIGIN[1] + d["y"] - py) ** 2)
print(f"   lands on panel cell ({cell['r']},{cell['c']}) mat {cell['mat']} led={cell['led']} "
      f"at board ({ORIGIN[0] + cell['x']:.3f}, {ORIGIN[1] + cell['y']:.3f})")
placed = {"U4": [rot, round(px, 4), round(py, 4)]}
for ref, (d, r2, qx, qy) in caps.items():
    placed[ref] = [r2, round(qx, 4), round(qy, 4)]
    print(f"   {ref} ({qx:.3f}, {qy:.3f}) rot {r2}  {d:.2f}mm to U4's pad "
          f"(was {dist(padsof(ref, '/+3V3' if ref != 'C21' else '/VBUS'), padsof('U4', '/+3V3' if ref != 'C21' else '/VBUS')):.2f}mm)")
json.dump(placed, open(OUT, "w"), indent=1)
print("wrote", OUT)

# Independent check: brute force every chosen pose against every obstacle the
# sweep was meant to respect. The interval sweep is fast but easy to get subtly
# wrong (it was, once), so nothing leaves this script unverified.
CHECK = []
for f in b.Footprints():
    if f.GetReference() in BLOCK:
        continue
    CHECK += [(f.GetReference(), r) for r in (L.courtyard_rects(f) or [L.cy_bbox(f)])]
for d in lay["dots"]:
    bx, by = ORIGIN[0] + d["x"], ORIGIN[1] + d["y"]
    if (round(bx, 2), round(by, 2)) in led_at:
        CHECK.append((f"lit dot ({d['r']},{d['c']})",
                      (bx - DOT_R, by - DOT_R, bx + DOT_R, by + DOT_R)))
worst = None
for ref, (r2, qx, qy) in placed.items():
    for cx0, cy0, cx1, cy1 in GEOM[ref][r2][0]:
        a = (qx + cx0, qy + cy0, qx + cx1, qy + cy1)
        assert (BOARD[0] + EDGE <= a[0] and a[2] <= BOARD[2] - EDGE
                and BOARD[1] + EDGE <= a[1] and a[3] <= GRID_SOUTH), (ref, a)
        for name, o in CHECK:
            gap = max(o[0] - a[2], a[0] - o[2], o[1] - a[3], a[1] - o[3])
            assert gap >= GAP - 1e-9, f"{ref} vs {name}: {gap:.4f}mm"
            if worst is None or gap < worst[0]:
                worst = (gap, ref, name)
print(f"brute-force check clean against {len(CHECK)} obstacles; "
      f"tightest {worst[1]} to {worst[2]} at {worst[0]:.3f}mm")
