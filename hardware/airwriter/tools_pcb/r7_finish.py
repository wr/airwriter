"""Round 7 step 3: refill the pour, and settle the one silk collision left.

The pour has to be refilled by hand after a placement change made outside the
GUI. Deleting the old route left the stored fill polygons full of the clearance
cut-outs the vanished traces used to need -- the top-view render showed the dead
route as a ghost snaking through the LED field, which is a stale fill, not
copper.

U5's reference moved with it and landed on U1's south pad row. There is no room
for it on silk: U5 has 0.22mm to U1's courtyard, 0.20mm to J1's, 1.10mm to J4's
and 0.93mm to H4. It goes to F.Fab, which is where this board already puts the
designators it cannot fit -- every LED's, and the passives' -- rather than
printing a label that names the wrong part.
"""
import sys
sys.path.insert(0, "/Users/wells/Projects/airwriter/hardware/airwriter/tools_pcb")
import pcbnew
import r6_lib as L

b = L.load()
fps = {f.GetReference(): f for f in b.Footprints()}

u5 = fps["U5"]
ref = u5.Reference()
p = u5.GetPosition()
print(f"U5 reference was on {b.GetLayerName(ref.GetLayer())} at "
      f"({L.mm(ref.GetPosition().x):.3f}, {L.mm(ref.GetPosition().y):.3f})")
ref.SetLayer(pcbnew.F_Fab)
ref.SetPosition(pcbnew.VECTOR2I(p.x, p.y))
print(f"U5 reference now on F.Fab at ({L.mm(p.x):.3f}, {L.mm(p.y):.3f}), on its own body")

# Every designator must still name the part it sits on -- round 6's rule. D3's
# is 2.99mm out and was placed by hand in "Adjust silkscreen labels by hand";
# it is not this round's and is left where the designer put it.
HAND_PLACED = {"D3"}
far = []
for f in b.Footprints():
    r = f.Reference()
    if not r.IsVisible():
        continue
    cy = L.cy_bbox(f)
    q = (L.mm(r.GetPosition().x), L.mm(r.GetPosition().y))
    d = max(cy[0] - q[0], q[0] - cy[2], cy[1] - q[1], q[1] - cy[3], 0.0)
    if d > 2.5:
        far.append((f.GetReference(), round(d, 2)))
stray = [t for t in far if t[0] not in HAND_PLACED]
assert not stray, stray
print(f"designators more than 2.5mm from their own part: {far or 'none'} "
      f"({len(list(b.Footprints()))} checked, {sorted(HAND_PLACED)} hand-placed and allowed)")

filler = pcbnew.ZONE_FILLER(b)
filler.Fill(b.Zones())
print(f"refilled {len(list(b.Zones()))} zones")

pcbnew.SaveBoard(L.BOARD, b)
print("saved")
