"""Where does U4 go once the lift evicts it? Search the whole board, not a guess.

Lifting U1 for the corner mount closes the north-east strip: the band between
the grid-zone boundary and U1's new courtyard top is 5.18mm, and the SOT-223's
courtyard is 7.19mm at every rotation that matters. So the regulator has to
leave, and the honest question is where the board still has room -- the lower
band as well as the LED grid, since the lift also freed the pocket H4 vacated.

Scored on the run the mandate has always named, /VBUS in from J1 plus /+3V3 out
to U1's power pad, jointly with the output cap's reach: an LDO whose output cap
is 8mm away is a stability question, not a layout preference, so C4's distance
is weighted heavily and U4's pose is not chosen without it.

Reports; writes nothing. r7_lift_fit.py applies the chosen answer.
"""
import sys
sys.path.insert(0, "/Users/wells/Projects/airwriter/hardware/airwriter/tools_pcb")
import r6_lib as L
import r7_geom as G

MOVE = ["U4", "C4", "C21", "C9"]
W_C4 = 3.0
STEP = 0.25

b = L.load()
sp = G.Space(b, MOVE)
sp.reset(exclude=MOVE)

# Two windows, reported separately so the trade is visible rather than averaged:
# the LED grid (parts under ~4mm tall only -- the SOT-223 is 1.8mm) and the
# lower band, where U1's lift and H4's move both freed space.
WINDOWS = {
    "grid zone": (G.BOARD[1] + G.EDGE, G.GRID_SOUTH, G.BOARD[0] + G.EDGE, G.BOARD[2] - G.EDGE),
    "lower band": (G.GRID_SOUTH, G.BOARD[3] - G.EDGE, G.BOARD[0] + G.EDGE, G.BOARD[2] - G.EDGE),
}


def padsof(ref, net):
    return [(L.mm(p.GetPosition().x), L.mm(p.GetPosition().y))
            for p in sp.fps[ref].Pads() if p.GetNetname() == net]


J1_VBUS = padsof("J1", "/VBUS")
U1_3V3 = padsof("U1", "/+3V3")
print(f"U1's /+3V3 pad is at {U1_3V3[0]} after the lift")
print(f"U4 stands at {(L.mm(sp.fps['U4'].GetPosition().x), L.mm(sp.fps['U4'].GetPosition().y))}, "
      f"which the lift has made illegal")


def run(rot, x, y):
    return (G.dist(J1_VBUS, sp.pads("U4", rot, x, y, "/VBUS"))
            + G.dist(sp.pads("U4", rot, x, y, "/+3V3"), U1_3V3))


base = list(sp.obst)
results = []
for name, win in WINDOWS.items():
    cands = sorted((run(*p), p) for p in sp.poses("U4", win, step=STEP))
    print(f"\n{name}: {len(cands)} legal U4 poses"
          + ("" if cands else "  -- nowhere for the regulator here"))
    if not cands:
        continue
    best = None
    for r, (rot, px, py) in cands[:300]:
        if best and r >= best[0]:
            break
        sp.obst[:] = base
        sp._index()
        sp.add("U4", rot, px, py)
        anchor = sp.pads("U4", rot, px, py, "/+3V3")
        cb = None
        for r2, qx, qy in sp.poses("C4", (max(win[0], py - 7), min(win[1], py + 7),
                                          max(win[2], px - 7), min(win[3], px + 7)),
                                   rots=(0, 90), step=STEP):
            d = G.dist(sp.pads("C4", r2, qx, qy, "/+3V3"), anchor)
            if cb is None or d < cb[0]:
                cb = (d, r2, qx, qy)
        if cb is None:
            continue
        total = r + W_C4 * cb[0]
        if best is None or total < best[0]:
            best = (total, r, rot, px, py, cb)
    sp.obst[:] = base
    sp._index()
    if not best:
        print("  no pose here takes its output cap")
        continue
    total, r, rot, px, py, (d4, r4, x4, y4) = best
    print(f"  U4 ({px:.3f}, {py:.3f}) rot {rot}   run {r:.2f}mm   C4 "
          f"({x4:.3f}, {y4:.3f}) rot {r4} at {d4:.2f}mm from the output pad")
    results.append((name, total, r, d4, (rot, px, py), (r4, x4, y4)))

print()
if results:
    results.sort(key=lambda t: t[1])
    name = results[0][0]
    print(f"BEST: {name} -- run {results[0][2]:.2f}mm, output cap {results[0][3]:.2f}mm")
    for n, t, r, d, u, c in results:
        print(f"   {n:11s} run {r:6.2f}mm   C4 {d:5.2f}mm   U4 {u}   C4 {c}")
