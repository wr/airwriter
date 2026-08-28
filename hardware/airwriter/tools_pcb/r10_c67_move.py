#!/usr/bin/env python3
"""Relocate C67 to U4: delete old VBUS stub near U1, move C67 beside U4.1,
add a GND stitch via at its GND pad. VBUS pad lands on the existing VBUS trunk."""
import pcbnew, math
from pcbnew import VECTOR2I
b=pcbnew.LoadBoard('airwriter.kicad_pcb')
mm=lambda v:int(round(v*1e6)); U=lambda v:round(v/1e6,3)
FCu=b.GetLayerID('F.Cu'); BCu=b.GetLayerID('B.Cu')

# NOTE: b.Remove() corrupts later footprint/net lookups in-process (handoff trap),
# so do ALL footprint/net work first and delete the stub LAST.
def near(p,x,y,tol=0.01): return abs(U(p.x)-x)<tol and abs(U(p.y)-y)<tol

# 2) move C67
c67=b.FindFootprintByReference('C67')
GNDnet=b.FindNet('/GND')
c67.SetOrientationDegrees(270); c67.SetPosition(VECTOR2I(mm(114.3),mm(112.9)))
pads={p.GetNumber():p for p in c67.Pads()}
print("C67 -> (114.3,112.9) rot270; pad2 VBUS",U(pads['2'].GetPosition().x),U(pads['2'].GetPosition().y),
      "pad1 GND",U(pads['1'].GetPosition().x),U(pads['1'].GetPosition().y))

# 3) GND stitch via at pad1 into pour (0.4/0.6), hole-to-hole safe
g1=pads['1'].GetPosition()
# nudge via off the pad centre slightly toward open pour (north) if needed; place at pad1
holes=[(t.GetPosition().x,t.GetPosition().y,t.GetDrill()//2) for t in b.GetTracks() if t.Type()==pcbnew.PCB_VIA_T]
for f in b.GetFootprints():
    for p in f.Pads():
        if p.GetDrillSize().x>0: pp=p.GetPosition(); holes.append((pp.x,pp.y,p.GetDrillSize().x//2))
VR=mm(0.3); H2H=mm(0.35)
def hole_ok(P):
    return all(math.hypot(hx-P.x,hy-P.y)>=VR+hr+H2H for hx,hy,hr in holes)
via_pos=None
for dx,dy in [(0,0),(0,-0.7),(0,0.7),(-0.7,0),(0.7,0),(0,-1.0)]:
    P=VECTOR2I(g1.x+mm(dx),g1.y+mm(dy))
    if hole_ok(P): via_pos=P; break
v=pcbnew.PCB_VIA(b); v.SetPosition(via_pos); v.SetDrill(mm(0.4)); v.SetWidth(mm(0.6))
v.SetNet(GNDnet); v.SetLayerPair(FCu,BCu); b.Add(v)
print("GND via at",U(via_pos.x),U(via_pos.y))

# LAST mutation: delete old C67 VBUS stub (149.9,153.35)-(149.9,154.514)
removed=0
for t in list(b.GetTracks()):
    if t.Type()==pcbnew.PCB_VIA_T: continue
    s,e=t.GetStart(),t.GetEnd()
    if (near(s,149.9,153.35) and near(e,149.9,154.514)) or (near(e,149.9,153.35) and near(s,149.9,154.514)):
        b.Remove(t); removed+=1
print("deleted old VBUS stub segments:",removed)

pcbnew.ZONE_FILLER(b).Fill(b.Zones())
b.Save('airwriter.kicad_pcb'); print("saved")
