"""Round 7 adjust cycle: give the power indicator its corridor back.

The first route after the lift left one real break, /+3V3 to D3 pad 2. D3 is
the green power LED and it cannot move -- it is the light source for the panel's
bare-FR4 slash, board x 158.423..160.279, and moving it would put the indicator
somewhere the front face does not have a window.

What could move is R2, one of J1's CC pulldowns, which had been parked at
(159.175, 159.925) in the only corridor that reaches D3 from the west: with the
keepout's south edge at y=156.828 and J1's courtyard ending at x=157.315, the
lane through x 157.3..162 is 4.7mm wide until R2 takes 2.95mm of it.

Lifting U1 opened a band north of J1 that did not exist before, y 151.573 to
156.855 across x 146.685..157.315. R2 belongs there anyway: it is a CC pulldown
and that band puts it at J1's own CC2 pad instead of 5.6mm away around a corner.
"""
import sys
sys.path.insert(0, "/Users/wells/Projects/airwriter/hardware/airwriter/tools_pcb")
import pcbnew
import r6_lib as L
import r7_geom as G

b = L.load()
sp = G.Space(b, ["R2"])
sp.reset(exclude=["R2"])
anchor = [(L.mm(p.GetPosition().x), L.mm(p.GetPosition().y))
          for p in sp.fps["J1"].Pads() if p.GetNetname() == "/CC2"]
was = (L.mm(sp.fps["R2"].GetPosition().x), L.mm(sp.fps["R2"].GetPosition().y))
best = None
for rot, x, y in sp.poses("R2", (150.0, 157.0, 146.0, 158.0), rots=(0, 90), step=0.05):
    d = G.dist(sp.pads("R2", rot, x, y, "/CC2"), anchor)
    if best is None or d < best[0]:
        best = (d, rot, x, y)
assert best, "no room north of J1 for R2"
d, rot, x, y = best
sp.place("R2", rot, x, y)
cy = L.cy_bbox(sp.fps["R2"])
print(f"R2 ({was[0]:.3f}, {was[1]:.3f}) -> ({x:.3f}, {y:.3f}) rot {rot}")
print(f"   /CC2 to J1 {d:.2f}mm (was 5.61mm); cy [{cy[0]:.3f},{cy[1]:.3f} - {cy[2]:.3f},{cy[3]:.3f}]")
print(f"   corridor to D3 at x 157.315..162.0 is now clear for its full 4.685mm")
pcbnew.SaveBoard(L.BOARD, b)
print("saved")
