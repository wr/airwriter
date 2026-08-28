"""Re-home everything the lift displaced, in the order the circuit cares about.

The lift closes the north-east strip to 5.18mm between the grid-zone boundary
and U1's new courtyard top. A SOT-223 needs 7.19mm at every rotation, and
r7_u4home.py finds ZERO legal poses for U4 anywhere in the lower band, so the
regulator leaves for the LED grid and its output cap goes with it -- C4 sets the
AP7361C's control loop and cannot be left 40mm behind.

What stays is re-fitted by what each part is for, not by what fits first:

  C9  U1's rail 100n, the one that has to be at the module's power pad
  C2  the module's 10u bulk reservoir -- second, not fourth. The band the lift
      leaves is one part tall, so everything spreads sideways and whatever goes
      late ends up 10mm west. Ordering by "what does the ESP32 need nearest"
      rather than by package size is the whole of the fix.
  C6  the EN cap, at U1 pad 3
  C5  supplementary rail, at pad 2
  R3  the EN pull-up -- it can sit anywhere on its two nets, so it goes last
  C21 VBUS bulk; it follows U4 if the grid takes it, otherwise it stays on the
      VBUS plane in the strip, which the LED field decouples all over anyway

U5 is re-swept too: the lift and H4's move both freed the pocket, and moving the
ESD clamp SOUTH now shortens its reach to J1's data pins instead of lengthening
it. That is the opposite of the earlier pass, where U1 was in the way.
"""
import sys
sys.path.insert(0, "/Users/wells/Projects/airwriter/hardware/airwriter/tools_pcb")
import pcbnew
import r6_lib as L
import r7_geom as G

MOVE = ["U4", "C4", "C21", "C9", "C6", "C5", "C2", "R3", "U5"]
U4_POSE = (90, 147.394, 108.250)          # from r7_u4home.py; the only window with room
POCKET = (151.8, 162.0, 140.0, 148.0)     # where U5 may sit, between J4 and J1

b = L.load()
sp = G.Space(b, MOVE)

# The north-east band is whatever the lift left: from 0.4mm inside the grid
# margin (where this column has sat since round 6) down to U1's pad row.
#
# NOT down to U1's courtyard. Fourteen signals leave that north row, and a
# solver told only to minimise anchor distance packs every cap hard against it,
# which is precisely how round 6 lost /HSKI_DRV on the south side. So the band
# stops CHANNEL short of the pads and the escape lane is reserved before
# anything is placed, not recovered afterwards.
CHANNEL = 1.0
_north_pads = [p for p in sp.fps["U1"].Pads()
               if L.mm(p.GetPosition().y) < L.mm(sp.fps["U1"].GetPosition().y)]
_pad_top = min(L.mm(p.GetBoundingBox().GetTop()) for p in _north_pads)
STRIP = (127.2, round(_pad_top - CHANNEL, 3), 136.0, 155.95)
print(f"U1's north pad row starts at y={_pad_top:.3f}; reserving a {CHANNEL}mm escape "
      f"channel leaves a band y {STRIP[0]}..{STRIP[1]} = {STRIP[1] - STRIP[0]:.3f}mm tall")


def pad(ref, net):
    return [(L.mm(p.GetPosition().x), L.mm(p.GetPosition().y))
            for p in sp.fps[ref].Pads() if p.GetNetname() == net]


def u1pad(n):
    return [(L.mm(p.GetPosition().x), L.mm(p.GetPosition().y))
            for p in sp.fps["U1"].Pads() if p.GetPadName() == str(n)]


sp.reset(exclude=MOVE)

# --- the regulator and its output cap, into the grid cell
rot, ux, uy = U4_POSE
sp.place("U4", rot, ux, uy)
sp.add("U4", rot, ux, uy)
print(f"U4 -> ({ux:.3f}, {uy:.3f}) rot {rot}")

GRID = (G.BOARD[1] + G.EDGE, G.GRID_SOUTH, G.BOARD[0] + G.EDGE, G.BOARD[2] - G.EDGE)

PLAN = [
    ("C4", "/+3V3", sp.pads("U4", rot, ux, uy, "/+3V3"), STRIP, True),
    ("C9", "/+3V3", u1pad(2), STRIP, False),
    ("C2", "/+3V3", u1pad(2), STRIP, False),
    ("C6", "/EN", u1pad(3), STRIP, False),
    ("C5", "/+3V3", u1pad(2), STRIP, False),
    ("R3", "/EN", u1pad(3), STRIP, False),
    ("C21", "/VBUS", sp.pads("U4", rot, ux, uy, "/VBUS"), STRIP, True),
]

# Fallbacks, in order of preference. The band the lift leaves is one part tall
# and only ~8mm of it is usable -- dot (7,4)'s lit envelope blocks its west end
# and dot (7,6)'s its east -- so it seats three parts, not five. C5 (a
# supplementary 100n, redundant with C9 on the same rail) and R3 (the EN
# pull-up, free to sit anywhere on its two nets) are the two that can travel,
# and they say so by being last in PLAN.
LOWER = (G.GRID_SOUTH, G.BOARD[3] - G.EDGE, G.BOARD[0] + G.EDGE, G.BOARD[2] - G.EDGE)
NEAR_U4 = (max(GRID[0], uy - 8), min(GRID[1], uy + 8),
           max(GRID[2], ux - 8), min(GRID[3], ux + 8))

for ref, net, anchor, win, near_u4 in PLAN:
    windows = [NEAR_U4, win] if near_u4 else [win, LOWER, GRID]
    best = None
    for w in windows:
        for r2, qx, qy in sp.poses(ref, w, rots=(0, 90), step=0.05):
            d = G.dist(sp.pads(ref, r2, qx, qy, net), anchor)
            if best is None or d < best[0]:
                best = (d, r2, qx, qy, w)
        if best:
            break
    assert best, f"nowhere for {ref}"
    d, r2, qx, qy, w = best
    was = (L.mm(sp.fps[ref].GetPosition().x), L.mm(sp.fps[ref].GetPosition().y))
    sp.place(ref, r2, qx, qy)
    sp.add(ref, r2, qx, qy)
    where = {id(NEAR_U4): "grid, with U4", id(win): "north-east strip",
             id(LOWER): "lower band", id(GRID): "LED grid"}[id(w)]
    print(f"  {ref:4s} ({was[0]:8.3f},{was[1]:8.3f}) -> ({qx:8.3f},{qy:8.3f}) rot {r2:3d}"
          f"   {net:7s} {d:5.2f}mm   [{where}]")

# --- the ESD clamp, now that the pocket is clear
j1dp, j1dm = pad("J1", "/USB_DP"), pad("J1", "/USB_DM")
best = None
for r2, qx, qy in sp.poses("U5", POCKET, rots=(0, 180), step=0.05):
    dp = G.dist(sp.pads("U5", r2, qx, qy, "/USB_DP"), j1dp)
    dm = G.dist(sp.pads("U5", r2, qx, qy, "/USB_DM"), j1dm)
    s = max(dp, dm)
    if best is None or s < best[0]:
        best = (s, dp, dm, r2, qx, qy)
assert best, "nowhere for U5"
s, dp, dm, r2, qx, qy = best
was = (L.mm(sp.fps["U5"].GetPosition().x), L.mm(sp.fps["U5"].GetPosition().y))
sp.place("U5", r2, qx, qy)
print(f"  U5   ({was[0]:8.3f},{was[1]:8.3f}) -> ({qx:8.3f},{qy:8.3f}) rot {r2:3d}"
      f"   /USB_DP {dp:.2f}mm  /USB_DM {dm:.2f}mm  (limit 7.0)")

pcbnew.SaveBoard(L.BOARD, b)
print("saved")
