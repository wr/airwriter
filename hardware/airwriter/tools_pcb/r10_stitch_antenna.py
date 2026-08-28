#!/usr/bin/env python3
"""Restore the antenna GND stitch grid: 5mm grid west of the WROOM antenna
keepout + a tight west-edge perimeter ring at the antenna base.
GND vias 0.4mm drill / 0.6mm pad (matches existing board stitch vias).
Clearance-checked on BOTH layers, in both GND pours, outside the keepout.
Adds vias only; strips nothing. Refills zones after."""
import sys
sys.path.insert(0,'tools_pcb')
from r10fin import Model, mm, um
import pcbnew
from pcbnew import VECTOR2I

BOARD='airwriter.kicad_pcb'
M=Model(BOARD); b=M.b
gnc=b.FindNet('/GND').GetNetCode(); GND=b.FindNet('/GND')
VR=mm(0.3); CLRV=mm(0.2)
DRILL=mm(0.4); PAD=mm(0.6)
exist=[(t.GetPosition().x,t.GetPosition().y) for t in b.GetTracks() if t.Type()==pcbnew.PCB_VIA_T]
# all drilled holes (PTH pads + vias): (x, y, hole_radius_nm) for hole-to-hole spacing
H2H=mm(0.25)+mm(0.1)   # min_hole_to_hole 0.25 + safety
holes=[]
for t in b.GetTracks():
    if t.Type()==pcbnew.PCB_VIA_T:
        p=t.GetPosition(); holes.append((p.x,p.y,t.GetDrill()//2))
for f in b.GetFootprints():
    for pad in f.Pads():
        dr=pad.GetDrillSize().x
        if dr>0:
            p=pad.GetPosition(); holes.append((p.x,p.y,dr//2))
import math
def hole_ok(P):
    for hx,hy,hr in holes:
        if math.hypot(hx-P.x,hy-P.y) < VR + hr + H2H:
            return False
    return True

def ok_via(P,mindist=2.0):
    if not M.copper_clear(P,gnc,VR+CLRV,'F'): return False
    if not M.copper_clear(P,gnc,VR+CLRV,'B'): return False
    if not M.in_gnd(P,'F') or not M.in_gnd(P,'B'): return False
    if not M.keepout_clear(P,VR): return False
    if not hole_ok(P): return False
    x,y=um(P.x),um(P.y)
    if x<100.9 or x>161.1 or y<60.9 or y>163.4: return False
    for ex,ey in exist:
        if abs(ex-P.x)<mm(mindist) and abs(ey-P.y)<mm(mindist): return False
    return True

new=[]
# 5mm grid, antenna base region west of keepout (keepout x>=156)
x=137.0
while x<=155.5:
    y=108.0
    while y<=156.0:
        P=VECTOR2I(mm(x),mm(y))
        if ok_via(P): new.append((x,y)); exist.append((P.x,P.y))
        y+=5.0
    x+=5.0
# tight west-edge perimeter ring at the antenna base + one south
for y in [128.5,133.5,138.5,143.5,148.5,153.5]:
    for x in [155.4,154.6]:
        P=VECTOR2I(mm(x),mm(y))
        if ok_via(P,1.8): new.append((x,y)); exist.append((P.x,P.y)); break
for x in [157.5,159.5,161.0]:
    P=VECTOR2I(mm(x),mm(155.5))
    if ok_via(P,1.8): new.append((x,155.5)); exist.append((P.x,P.y))

before=sum(1 for t in b.GetTracks() if t.Type()==pcbnew.PCB_VIA_T and t.GetNetname()=='/GND')
for x,y in new:
    v=pcbnew.PCB_VIA(b)
    v.SetPosition(VECTOR2I(mm(x),mm(y)))
    v.SetDrill(DRILL); v.SetWidth(PAD)
    v.SetNet(GND)
    v.SetLayerPair(b.GetLayerID('F.Cu'),b.GetLayerID('B.Cu'))
    b.Add(v)
print(f"added {len(new)} GND stitch vias (GND vias {before} -> {before+len(new)})")

pcbnew.ZONE_FILLER(b).Fill(b.Zones())
b.Save(BOARD)
print("refilled + saved")
