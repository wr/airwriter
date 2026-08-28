#!/usr/bin/env python3
"""Search a clearance-clean C67 site next to U4.1 (VBUS). Report only."""
import pcbnew, math
from pcbnew import VECTOR2I
b=pcbnew.LoadBoard('airwriter.kicad_pcb')
mm=lambda v:int(round(v*1e6)); U=lambda v:round(v/1e6,3)
FCu=b.GetLayerID('F.Cu')
c67=b.FindFootprintByReference('C67')
c67pads=set(id(p) for p in c67.Pads())
U41=VECTOR2I(mm(116.65),mm(113.9))

# obstacles: F.Cu tracks+vias (all) + pads not belonging to C67
obst=[]  # (netcode, shape, bbox)
def bx(it):
    bb=it.GetBoundingBox(); return (bb.GetLeft(),bb.GetTop(),bb.GetRight(),bb.GetBottom())
for t in b.GetTracks():
    if t.IsOnLayer(FCu): obst.append((t.GetNetCode(),t.GetEffectiveShape(FCu),bx(t)))
for f in b.GetFootprints():
    for p in f.Pads():
        if id(p) in c67pads: continue
        if p.IsOnLayer(FCu): obst.append((p.GetNetCode(),p.GetEffectiveShape(FCu),bx(p)))
cyards=[(f.GetReference(),f.GetCourtyard(FCu)) for f in b.GetFootprints()
        if f.GetReference()!='C67' and f.GetCourtyard(FCu).OutlineCount()>0]
keepouts=[z.Outline() for f in b.GetFootprints() for z in f.Zones() if z.GetIsRuleArea()]

def pad_clear(shape, netcode, clr=mm(0.2)):
    bb=shape.BBox(); L,T,R,B=bb.GetLeft(),bb.GetTop(),bb.GetRight(),bb.GetBottom()
    for nc,sh,ob in obst:
        if nc==netcode: continue
        if R+clr<ob[0] or L-clr>ob[2] or B+clr<ob[1] or T-clr>ob[3]: continue
        if shape.Collide(sh,clr): return False
    return True

VBUS=b.FindNet('/VBUS').GetNetCode(); GND=b.FindNet('/GND').GetNetCode()
best=None
for cx10 in range(1120,1230):
    for cy10 in range(1075,1140):
        cx,cy=cx10/10.0,cy10/10.0
        for rot in (0,90,180,270):
            c67.SetOrientationDegrees(rot); c67.SetPosition(VECTOR2I(mm(cx),mm(cy)))
            pads={p.GetNumber():p for p in c67.Pads()}
            p2=pads['2'].GetPosition()  # VBUS
            d=math.hypot(p2.x-U41.x,p2.y-U41.y)/1e6
            if d>3.0: continue
            ok=True
            for num,pad in pads.items():
                net=VBUS if num=='2' else GND
                if not pad_clear(pad.GetEffectiveShape(FCu),net): ok=False;break
            if not ok: continue
            # courtyard
            cyc=c67.GetCourtyard(FCu); bad=False
            for ref,cyp in cyards:
                if cyp.Collide(cyc.BBox().Centre(),0): pass
                # sample C67 courtyard vertices vs others
            # sample-based courtyard overlap
            sb=cyc.BBox()
            samples=[VECTOR2I(x,y) for x in (sb.GetLeft(),(sb.GetLeft()+sb.GetRight())//2,sb.GetRight())
                     for y in (sb.GetTop(),(sb.GetTop()+sb.GetBottom())//2,sb.GetBottom())]
            for ref,cyp in cyards:
                if any(cyp.Collide(s,0) for s in samples): bad=True;break
            if bad: continue
            # keepout for both pad centers
            if any(ol.Collide(pads[n].GetPosition(),mm(0.3)) for ol in keepouts for n in pads): continue
            score=d
            if best is None or score<best[0]:
                best=(d,cx,cy,rot,U(p2.x),U(p2.y),U(pads['1'].GetPosition().x),U(pads['1'].GetPosition().y))
if best:
    print("C67 site: center(%.1f,%.1f) rot%d  VBUSpad2(%.3f,%.3f) d2U4.1=%.2fmm  GNDpad1(%.3f,%.3f)"%(
        best[1],best[2],best[3],best[4],best[5],best[0],best[6],best[7]))
else:
    print("NO clean C67 site within 3mm of U4.1")
