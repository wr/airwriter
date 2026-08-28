#!/usr/bin/env python3
"""Place ONE board-only stub test point (TXD0/RXD0) in the U1 pocket.
Usage: r10_place_uart_tp.py <ref> <label> <net> <srcx> <srcy>
Reload between calls so the second TP avoids the first."""
import sys, math
sys.path.insert(0,'tools_pcb')
from r10fin import Model, validate, mm, um
import pcbnew
from pcbnew import VECTOR2I

ref,lab,net,sx,sy = sys.argv[1],sys.argv[2],sys.argv[3],float(sys.argv[4]),float(sys.argv[5])
BOARD='airwriter.kicad_pcb'
TPLIB='/Applications/KiCad/KiCad.app/Contents/SharedSupport/footprints/TestPoint.pretty'
M=Model(BOARD); b=M.b
FCu=b.GetLayerID('F.Cu'); FSilk=b.GetLayerID('F.SilkS'); FFab=b.GetLayerID('F.Fab')
nc=b.FindNet(net).GetNetCode(); S=VECTOR2I(mm(sx),mm(sy))

pads=[(p.GetPosition().x,p.GetPosition().y) for f in b.GetFootprints() for p in f.Pads()]
silktext=[]
for d in b.GetDrawings():
    if isinstance(d,pcbnew.PCB_TEXT) and d.GetLayer()==FSilk:
        bb=d.GetBoundingBox(); silktext.append((bb.GetLeft(),bb.GetTop(),bb.GetRight(),bb.GetBottom()))
for f in b.GetFootprints():
    for t in [f.Reference(),f.Value()]:
        if t.IsVisible() and t.GetLayer()==FSilk:
            bb=t.GetBoundingBox(); silktext.append((bb.GetLeft(),bb.GetTop(),bb.GetRight(),bb.GetBottom()))
    for g in f.GraphicalItems():
        if g.GetLayer()==FSilk and isinstance(g,(pcbnew.PCB_TEXT,pcbnew.PCB_SHAPE)):
            bb=g.GetBoundingBox(); silktext.append((bb.GetLeft(),bb.GetTop(),bb.GetRight(),bb.GetBottom()))
EDGE=(100.6,161.4,60.6,163.7)
def label_clear(cx,cy,halfw,selfref):
    if cx-mm(halfw)<mm(EDGE[0]) or cx+mm(halfw)>mm(EDGE[1]) or cy-mm(0.6)<mm(EDGE[2]) or cy+mm(0.6)>mm(EDGE[3]): return False
    lx0,lx1,ly0,ly1=cx-mm(halfw+0.3),cx+mm(halfw+0.3),cy-mm(0.8),cy+mm(0.8)
    for tx0,ty0,tx1,ty1 in silktext:
        if lx0<tx1 and lx1>tx0 and ly0<ty1 and ly1>ty0: return False
    for px,py in pads:
        if abs(px-cx)<mm(halfw+0.5) and abs(py-cy)<mm(1.1): return False
    samples=[VECTOR2I(int(cx+mm(ex)),int(cy+mm(ey))) for ex in (-halfw,-halfw/2,0,halfw/2,halfw) for ey in (-0.5,0.0,0.5)]
    for r,cyp in M.cyards:
        if r==selfref: continue
        if any(cyp.Collide(s,0) for s in samples): return False
    return True
def pick_label(x,y,lab,ref,wide=False):
    halfw=0.6*len(lab); W=2.0+halfw
    offs=[(0,1.9),(0,-1.9),(W,0),(-W,0),(0,2.7),(0,-2.7),(W,1.9),(-W,1.9),(W,-1.9),(-W,-1.9)]
    if wide:  # expand search outward for a nearby readable label spot
        for r in [3.2,3.8,4.4,5.0]:
            for a in range(0,360,30):
                offs.append((r*math.cos(math.radians(a)),r*math.sin(math.radians(a))))
    for ox,oy in offs:
        if label_clear(mm(x+ox),mm(y+oy),halfw,ref): return (ox,oy)
    return None
def seg_clear(A,B,clr=mm(0.25)):
    d=math.hypot(B.x-A.x,B.y-A.y)/1e6; n=max(1,int(d/0.1))
    return all(M.copper_clear(VECTOR2I(int(A.x+(B.x-A.x)*i/n),int(A.y+(B.y-A.y)*i/n)),nc,clr) for i in range(1,n+1))
def route(P):
    """return list of corner points (excl S, incl P) for a clear straight or L stub, else None."""
    if seg_clear(S,P): return [P]
    # L-stub: try corners on a coarse grid between S and P
    for cx10 in range(min(int(S.x/1e5),int(P.x/1e5))-4, max(int(S.x/1e5),int(P.x/1e5))+5,3):
        for cy10 in range(int(S.y/1e5), int(P.y/1e5)+5,3):
            C=VECTOR2I(int(cx10*1e5),int(cy10*1e5))
            if seg_clear(S,C) and seg_clear(C,P): return [C,P]
    return None

# search: nearest validate-ok TP spot reachable by a clean straight/L stub
cands=[]
for xi in range(1440,1580):
    for yi in range(1495,1615):
        x,y=xi/10.0,yi/10.0; P=VECTOR2I(mm(x),mm(y))
        if not validate(M,P,nc,need_onnet=False)[0]: continue
        d=math.hypot(S.x-P.x,S.y-P.y)/1e6
        if d>7: continue
        cands.append((d,x,y,P))
cands.sort()
chosen=None
for d,x,y,P in cands:
    path=route(P)
    if path is None: continue
    off=pick_label(x,y,lab,ref,wide=True)   # label optional, wide search
    chosen=(x,y,off,round(d,2),um(M.nearest_other(P,nc)),path); break
assert chosen, f"{ref} {lab}: no clean stub-TP spot"
x,y,off,stub,mgn,path=chosen
print(f"{ref} {lab} TP({x},{y}) reach={stub}mm margin={mgn} label={off} segs={len(path)}")

# place
fp=pcbnew.FootprintLoad(TPLIB,'TestPoint_Pad_D1.0mm')
fp.SetReference(ref); fp.SetValue(lab); fp.SetPosition(VECTOR2I(mm(x),mm(y)))
b.Add(fp)
for pad in fp.Pads(): pad.SetNet(b.FindNet(net))
fp.Reference().SetLayer(FFab); fp.Reference().SetTextSize(VECTOR2I(mm(1.0),mm(1.0)))
for g in list(fp.GraphicalItems()):
    if isinstance(g,pcbnew.PCB_SHAPE) and g.GetLayer()==FSilk: fp.Remove(g)
if off is not None:
    txt=pcbnew.PCB_TEXT(fp); txt.SetText(lab); txt.SetLayer(FSilk)
    txt.SetPosition(VECTOR2I(mm(x+off[0]),mm(y+off[1]))); txt.SetTextSize(VECTOR2I(mm(1.0),mm(1.0))); txt.SetTextThickness(mm(0.15))
    fp.Add(txt)
else:
    print(f"  WARNING: no silk-label spot for {ref} {lab}; F.Fab ref only")
prev=S
for pt in path:
    tr=pcbnew.PCB_TRACK(b); tr.SetStart(VECTOR2I(prev.x,prev.y)); tr.SetEnd(VECTOR2I(pt.x,pt.y))
    tr.SetWidth(mm(0.2)); tr.SetLayer(FCu); tr.SetNet(b.FindNet(net)); b.Add(tr); prev=pt
pcbnew.ZONE_FILLER(b).Fill(b.Zones())
b.Save(BOARD); print("saved",ref)
