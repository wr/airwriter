"""Round 7 step 2: re-home the satellites the new mounting holes displaced.

H3 landed on the pocket C3 and R9 were sharing. Both are satellites -- a bypass
cap and a series resistor, free to sit anywhere their own net partners allow --
so they move rather than the hole.

Each is scored by where its own pads land, not by where its body lands: a
resistor whose centre is close but whose pads face the wrong way makes the
router cross its own nets, which is the defect round 6 found in R15/R16. /GND is
skipped in the scoring -- it is a pour, reachable from anywhere.
"""
import sys
sys.path.insert(0, "/Users/wells/Projects/airwriter/hardware/airwriter/tools_pcb")
import pcbnew
import r6_lib as L
import r7_geom as G

MOVE = ["C3", "R9"]
# The lower band, west of U2 and north of J2 -- the strip these two have always
# lived in. Constraining the window keeps the search from parking a printer-side
# passive on the far side of the board just because the net is long.
WINDOW = (135.0, 154.4, 100.5, 122.0)

b = L.load()
sp = G.Space(b, MOVE)
partners = {}
for f in b.Footprints():
    for p in f.Pads():
        n = p.GetNetname()
        if n and n != "/GND":
            partners.setdefault(n, []).append(
                (f.GetReference(), L.mm(p.GetPosition().x), L.mm(p.GetPosition().y)))


def reach(ref, rot, x, y):
    """Total distance from each pad to the nearest other part on its own net."""
    total = 0.0
    for net, offs in sp.geom[ref][rot][1].items():
        if net == "/GND" or net not in partners:
            continue
        others = [(px, py) for r, px, py in partners[net] if r != ref]
        if not others:
            continue
        for dx, dy in offs:
            total += G.dist([(x + dx, y + dy)], others)
    return total


sp.reset(exclude=MOVE)
for ref in MOVE:
    was = (L.mm(sp.fps[ref].GetPosition().x), L.mm(sp.fps[ref].GetPosition().y))
    best = None
    for rot, x, y in sp.poses(ref, WINDOW, rots=(0, 90), step=0.1):
        s = reach(ref, rot, x, y)
        if best is None or s < best[0]:
            best = (s, rot, x, y)
    assert best, f"nowhere for {ref} in {WINDOW}"
    s, rot, x, y = best
    sp.place(ref, rot, x, y)
    sp.add(ref, rot, x, y)
    cy = L.cy_bbox(sp.fps[ref])
    print(f"  {ref} ({was[0]:.3f}, {was[1]:.3f}) -> ({x:.3f}, {y:.3f}) rot {rot}   "
          f"pad reach {s:.2f}mm   cy [{cy[0]:.3f},{cy[1]:.3f} - {cy[2]:.3f},{cy[3]:.3f}]")
    for net, offs in sp.geom[ref][rot][1].items():
        if net == "/GND" or net not in partners:
            continue
        others = [(px, py) for r, px, py in partners[net] if r != ref]
        for dx, dy in offs:
            near = min(partners[net], key=lambda t: (t[1] - x - dx) ** 2 + (t[2] - y - dy) ** 2
                       if t[0] != ref else 1e9)
            print(f"       {net:14s} -> {near[0]} at "
                  f"{G.dist([(x + dx, y + dy)], others):.2f}mm")

pcbnew.SaveBoard(L.BOARD, b)
print("saved")
