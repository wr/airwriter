"""Round 7 gate: every geometric claim this round makes, checked on the board.

Courtyards are decomposed into rectangles, never bounding-boxed -- U1's is a
genuine L and its bbox throws false overlaps against everything near the notch.
Run it after any placement change; it writes nothing.
"""
import json
import os
import sys
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import pcbnew
import r6_lib as L

LAYOUT = os.path.join(HERE, "..", "..", "panel", "panel_layout.json")
ORIGIN = (100.0, 60.0)
DOT_R = 5.6364 / 2
BOARD = (100.0, 60.0, 162.0, 164.27)

b = L.load()
fps = {f.GetReference(): f for f in b.Footprints()}
lay = json.loads(open(LAYOUT).read())
fail = []


def check(ok, msg):
    print(("  ok   " if ok else "  FAIL ") + msg)
    if not ok:
        fail.append(msg)


print("courtyard overlaps (real polygons, decomposed)")
refs = sorted(fps)
rects = {r: (L.courtyard_rects(fps[r]) or [L.cy_bbox(fps[r])]) for r in refs}
worst, over = None, []
for i, a in enumerate(refs):
    for c in refs[i + 1:]:
        for ra in rects[a]:
            for rc in rects[c]:
                g = max(rc[0] - ra[2], ra[0] - rc[2], rc[1] - ra[3], ra[1] - rc[3])
                if g < 0:
                    over.append((a, c, g))
                elif worst is None or g < worst[0]:
                    worst = (g, a, c)
pairs = sorted({(a, c) for a, c, _ in over})
check(not over, f"{len(pairs)} overlapping pairs in {len(refs)} footprints"
                + (f" -- {pairs}" if pairs else ""))
print(f"       tightest legal pair: {worst[1]}/{worst[2]} at {worst[0]:.3f}mm")

print("mounting holes")
holes = {r: (L.mm(fps[r].GetPosition().x), L.mm(fps[r].GetPosition().y)) for r in
         ("H1", "H2", "H3", "H4")}
for r, (x, y) in holes.items():
    d = [p for p in fps[r].Pads()][0]
    cy = L.cy_bbox(fps[r])
    inset = min(cy[0] - BOARD[0], cy[1] - BOARD[1], BOARD[2] - cy[2], BOARD[3] - cy[3])
    check(d.GetAttribute() == pcbnew.PAD_ATTRIB_NPTH and abs(L.mm(d.GetDrillSizeX()) - 2.75) < 1e-6,
          f"{r} ({x:.3f}, {y:.3f}) non-plated, drill {L.mm(d.GetDrillSizeX()):.2f}mm")
    check(inset > 0.5, f"{r} courtyard {inset:.3f}mm inside the outline")
# convex hull area -- listing the holes in reference order traces a bowtie and
# under-reports the span by an order of magnitude
pts = sorted(holes.values())
def _hull(ps):
    def half(ps):
        out = []
        for p in ps:
            while len(out) > 1 and ((out[-1][0] - out[-2][0]) * (p[1] - out[-2][1])
                                    - (out[-1][1] - out[-2][1]) * (p[0] - out[-2][0])) <= 0:
                out.pop()
            out.append(p)
        return out
    return half(pts)[:-1] + half(pts[::-1])[:-1]
h = _hull(pts)
area = abs(sum(h[i][0] * h[(i + 1) % len(h)][1] - h[(i + 1) % len(h)][0] * h[i][1]
               for i in range(len(h)))) / 2
print(f"       support hull {area:.0f}mm2 over {len(h)} corners")

print("the grid: light, and what sits under it")
led_at = {(L.mm(f.GetPosition().x), L.mm(f.GetPosition().y)): f.GetReference()
          for f in b.Footprints() if "SK6812" in f.GetValue()}
check(len(led_at) == 37, f"{len(led_at)} LEDs placed")
worst_err, unlit = 0.0, []
for d in lay["dots"]:
    if not d["led"]:
        continue
    bx, by = ORIGIN[0] + d["x"], ORIGIN[1] + d["y"]
    hit = [k for k in led_at if abs(k[0] - bx) < 0.01 and abs(k[1] - by) < 0.01]
    if not hit:
        unlit.append((d["r"], d["c"]))
        continue
    worst_err = max(worst_err, abs(hit[0][0] - bx), abs(hit[0][1] - by))
check(not unlit, f"led:true dots with no LED under them: {sorted(unlit)}")
check(worst_err < 0.005, f"worst LED-to-dot error {worst_err:.4f}mm over 37 dots")
gold = {(d["r"], d["c"]) for d in lay["dots"] if d["mat"] in ("G", "T")}
check({(0, 0), (7, 0), (0, 6)} <= gold,
      "panel corners (0,0), (7,0) and (0,6) are gold, so nothing is lit over "
      "the two upper holes or under the DIP switch")

# The north-east strip already reached into the bottom dot row before this
# round -- r6 lifted U4 and its EN column 0.70mm into the grid margin to open
# U1's pad escape. Recorded by PART, not by cell, since the rebalanced panel
# changed which dots are lit under them.
PREEXISTING_PARTS = {"R3", "U4"}
blockers = []
for d in lay["dots"]:
    bx, by = ORIGIN[0] + d["x"], ORIGIN[1] + d["y"]
    if not any(abs(k[0] - bx) < 0.01 and abs(k[1] - by) < 0.01 for k in led_at):
        continue           # unlit or gold: nothing to shadow
    disc = (bx - DOT_R, by - DOT_R, bx + DOT_R, by + DOT_R)
    for r in refs:
        if r in led_at.values() or fps[r].GetReference().startswith("C") and \
                abs(L.mm(fps[r].GetPosition().x) - bx) < 0.01:
            continue
        for ra in rects[r]:
            if not (disc[0] >= ra[2] or ra[0] >= disc[2] or disc[1] >= ra[3] or ra[1] >= disc[3]):
                blockers.append((r, d["r"], d["c"]))
found = set(blockers)
new = {t for t in found if t[0] not in PREEXISTING_PARTS}
check(not new, f"parts newly under a lit dot: {sorted(new)}")
for r, dr, dc in sorted(found - new):
    bx = ORIGIN[0] + next(d["x"] for d in lay["dots"] if (d["r"], d["c"]) == (dr, dc))
    by = ORIGIN[1] + next(d["y"] for d in lay["dots"] if (d["r"], d["c"]) == (dr, dc))
    depth = max(min(ra[3], by + DOT_R) - max(ra[1], by - DOT_R) for ra in rects[r])
    print(f"       carried over: {r} reaches {depth:.3f}mm into dot ({dr},{dc})'s disc")

print("SW3 plunger")
sw3 = fps["SW3"]
plunger = next(((L.mm(i.GetCenter().x), L.mm(i.GetCenter().y)) for i in sw3.GraphicalItems()
                if i.GetClass() == "PCB_SHAPE" and i.GetLayer() == pcbnew.F_Fab
                and i.GetShape() == pcbnew.SHAPE_T_CIRCLE), None)
check(plunger and abs(plunger[0] - 131) < 1e-4 and abs(plunger[1] - 91) < 1e-4,
      f"plunger at {plunger}, wants (131, 91)")

print("the west control edge")
if "S1" not in fps:
    print("       no DIP switches (rev 2026.11): nothing on the west edge")
else:
    s1 = L.cy_bbox(fps["S1"])
    # S1's F.Fab carries the body outline AND the eight key tabs, and the tabs
    # deliberately hang 2.25mm past the board edge, so the leftmost F.Fab ink is not
    # the body face. The body's own west wall is the one long vertical run.
    walls = [L.mm(i.GetStart().x) for i in fps["S1"].GraphicalItems()
             if i.GetClass() == "PCB_SHAPE" and i.GetLayer() == pcbnew.F_Fab
             and abs(L.mm(i.GetStart().x) - L.mm(i.GetEnd().x)) < 1e-6
             and abs(L.mm(i.GetStart().y) - L.mm(i.GetEnd().y)) > 15.0]
    bx0 = min(walls)
    keys = min(min(L.mm(i.GetStart().x), L.mm(i.GetEnd().x)) for i in fps["S1"].GraphicalItems()
               if i.GetClass() == "PCB_SHAPE" and i.GetLayer() == pcbnew.F_Fab)
    check(abs(bx0 - 100.0) <= 0.3, f"S1 body face at x={bx0:.3f} (wants x<=100.3); "
                                   f"key tips reach x={keys:.3f}, outboard by design")
    sw1 = L.cy_bbox(fps["SW1"])
    check(abs(sw1[0] + 0.195 - 100.0) < 0.02, f"SW1 courtyard west edge {sw1[0]:.3f} "
          f"(actuator tip at {sw1[0] + 0.195:.3f}, wants 100.000)")
    print(f"       S1 reaches {max(0.0, 127.6 - s1[1]):.3f}mm into the grid zone (top at "
          f"y={s1[1]:.3f}) -- legal because Frame 9b's (7,0) is gold, but the light "
          f"lattice has to be carved around a 9.41mm part there")

print("the ESD clamp")
# pair the pads per net -- a /USB_DP pad next to a /USB_DM pad is not a short
# hop for either signal, and mixing them flatters the number
worst_net = None
for net in ("/USB_DP", "/USB_DM"):
    u5 = [(L.mm(p.GetPosition().x), L.mm(p.GetPosition().y))
          for p in fps["U5"].Pads() if p.GetNetname() == net]
    j1 = [(L.mm(p.GetPosition().x), L.mm(p.GetPosition().y))
          for p in fps["J1"].Pads() if p.GetNetname() == net]
    d = min(((a[0] - c[0]) ** 2 + (a[1] - c[1]) ** 2) ** 0.5 for a in u5 for c in j1)
    print(f"       {net} U5 to J1: {d:.2f}mm")
    if worst_net is None or d > worst_net:
        worst_net = d
check(worst_net <= 7.0, f"U5 {worst_net:.2f}mm from J1 on its worse data net (limit 7.0)")

print("pads and drills")
mind = min(L.mm(p.GetDrillSizeX()) for f in b.Footprints() for p in f.Pads()
           if p.GetDrillSizeX() > 0)
check(mind >= 0.30, f"minimum drill {mind:.3f}mm")
breach = []
for f in b.Footprints():
    for p in f.Pads():
        bb = p.GetBoundingBox()
        if (L.mm(bb.GetLeft()) < BOARD[0] + 0.25 or L.mm(bb.GetRight()) > BOARD[2] - 0.25
                or L.mm(bb.GetTop()) < BOARD[1] + 0.25 or L.mm(bb.GetBottom()) > BOARD[3] - 0.25):
            breach.append(f"{f.GetReference()}.{p.GetPadName()}")
check(not breach, f"pads within 0.25mm of the outline: {breach}")

print("antenna rule area")
# The antenna keepout is a zone owned by U1's footprint, in absolute
# coordinates -- board.Zones() does not list footprint zones.
ra = [z for z in b.Zones() if z.GetIsRuleArea()]
for f in b.Footprints():
    ra += [z for z in f.Zones() if z.GetIsRuleArea()]
for z in ra:
    o = z.Outline().Outline(0)
    pts = [(L.mm(o.CPoint(i).x), L.mm(o.CPoint(i).y)) for i in range(o.PointCount())]
    # The keepout is an L: it is notched to clear J1 on purpose, so its bounding
    # box says J1 is inside and the polygon says it is not. Decompose, as
    # everywhere else in this project.
    zr = L.decompose(pts)
    inside = sorted({r for r in refs if r != "U1" for x in rects[r] for q in zr
                     if not (q[0] >= x[2] or x[0] >= q[2] or q[1] >= x[3] or x[1] >= q[3])})
    check(not inside, f"{z.GetZoneName() or 'rule area'}, {len(zr)} rects spanning "
                      f"{tuple(round(v, 2) for v in (min(p[0] for p in pts), min(p[1] for p in pts), max(p[0] for p in pts), max(p[1] for p in pts)))}, "
                      f"holds nothing but U1 (found {inside})")

print()
print("FAILURES: " + (", ".join(fail) if fail else "none"))
sys.exit(1 if fail else 0)
