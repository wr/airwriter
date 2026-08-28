"""Round 6 step 1: set the structural anchors.

U1 flush (antenna tip at the x=162 edge), keepout narrowed + notched around J1,
S1 and SW1 onto the west control edge, SW3's plunger centred on the panel test
dot, H4 relocated.  Absolute board path only -- a relative path silently wrote a
stray file at the repo root in an earlier round.
"""
import sys
sys.path.insert(0, "/Users/wells/Projects/airwriter/hardware/airwriter/tools_pcb")
import pcbnew, r6_lib as L

b = L.load()
fps = {f.GetReference(): f for f in b.Footprints()}

def V(x, y):
    return pcbnew.VECTOR2I(L.nm(x), L.nm(y))

def place(ref, x, y, rot=None):
    f = fps[ref]
    if rot is not None:
        f.SetOrientationDegrees(rot)
    f.SetPosition(V(x, y))
    print("  %-4s -> (%.4f, %.4f) rot %g" % (ref, x, y, f.GetOrientationDegrees()))

print("tracks on board:", b.GetTracks().GetCount() if hasattr(b.GetTracks(), "GetCount") else len(list(b.GetTracks())))

# ---- SW3: put the PLUNGER (not the origin) on the panel test dot ----------
# The stock SW_PUSH_6mm_H13mm footprint's origin is pin 1; its plunger is the
# F.Fab circle at local (3.25, 2.25).  Find that circle on the live instance and
# solve for the origin that lands it on (131, 91), for whichever rotation we pick.
def plunger_centre(fp):
    best = None
    for it in fp.GraphicalItems():
        if it.GetClass() != "PCB_SHAPE":
            continue
        if it.GetLayerName() != "F.Fab":
            continue
        if it.GetShape() != pcbnew.SHAPE_T_CIRCLE:
            continue
        c = it.GetCenter()
        r = it.GetRadius()
        if best is None or r > best[1]:
            best = ((L.mm(c.x), L.mm(c.y)), r)
    assert best, "no F.Fab circle (plunger) found"
    return best[0], L.mm(best[1])

sw3 = fps["SW3"]
print("SW3 before: origin", (L.mm(sw3.GetPosition().x), L.mm(sw3.GetPosition().y)),
      "rot", sw3.GetOrientationDegrees(), "plunger", plunger_centre(sw3))

TARGET = (131.0, 91.0)
best = None
for rot in (0, 90, 180, 270):
    sw3.SetOrientationDegrees(rot)
    sw3.SetPosition(V(*TARGET))                     # probe
    (px, py), rad = plunger_centre(sw3)
    off = (px - TARGET[0], py - TARGET[1])          # plunger offset from origin
    org = (TARGET[0] - off[0], TARGET[1] - off[1])
    sw3.SetPosition(V(*org))
    (px, py), rad = plunger_centre(sw3)
    cy = L.cy_bbox(sw3)
    print("  rot %3d -> origin (%.4f, %.4f) plunger (%.4f, %.4f) r=%.3f cy=%s"
          % (rot, org[0], org[1], px, py, rad, [round(v, 3) for v in cy]))
    best = best or {}
    best[rot] = (org, (px, py), cy)

# rot 270 is the deliberate choice: it turns the footprint's 9.49mm axis onto Y,
# which is where the neighbouring grid cells are empty.  At rot 0/180 the 9.49mm
# axis lands on X and the courtyard clears D30 by only 0.065mm.
SW3_ROT, SW3_ORG = 270, best[270][0]
sw3.SetOrientationDegrees(SW3_ROT)
sw3.SetPosition(V(*SW3_ORG))
pc, _ = plunger_centre(sw3)
assert abs(pc[0] - TARGET[0]) < 1e-4 and abs(pc[1] - TARGET[1]) < 1e-4, pc
print("  SW3  -> origin (%.4f, %.4f) rot %d; plunger PROVEN at (%.4f, %.4f)"
      % (SW3_ORG[0], SW3_ORG[1], SW3_ROT, pc[0], pc[1]))

# ---- structural anchors --------------------------------------------------
place("U1", 149.25, 144.35, -90)    # antenna tip flush at x=162; 1.65mm north
                                    # of centre. 1.05mm of that clears J4's courtyard;
                                    # the last 0.6mm balances the escape corridors on
                                    # U1's two pad rows -- south of the module only
                                    # 0.72mm of copper separated it from J4, and seven
                                    # signals have to leave that row westward.
place("S1", 105.005, 138.785, 90)   # DP-08RP: body face 0.2mm off the x=100
                                    # edge, keys overhanging 2.2mm past it
place("SW1", 103.55, 152.70, 180)   # actuator tip flush at x=100
place("H4", 143.70, 159.70, 0)      # relocated clear of U1's new body

# ---- antenna keepout: narrowed, notched clear of J1's fixed corner --------
# Ruled extent: x156..162, module width +5mm/side laterally (29.5mm), centred on
# the module -- so it tracks U1's 1.05mm shift to y130.2..159.7.  Notched around
# J1's fixed corner: J1 is edge-fixed and its shield pads are copper.
KEEPOUT = [(156.0, 129.6), (162.0, 129.6), (162.0, 159.1),
           (157.6, 159.1), (157.6, 156.6), (156.0, 156.6)]
zone = None
for z in fps["U1"].Zones():
    zone = z
assert zone is not None, "U1 has no child zone"
print("keepout zone name:", zone.GetZoneName(), "isRuleArea:", zone.GetIsRuleArea())
poly = zone.Outline()
poly.RemoveAllContours()
poly.NewOutline()
for x, y in KEEPOUT:
    poly.Append(L.nm(x), L.nm(y))
zone.HatchBorder()
print("keepout ->", KEEPOUT)

pcbnew.SaveBoard(L.BOARD, b)
print("saved")
