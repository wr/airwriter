#!/usr/bin/env python3
"""Place 6 board-only test points (clearance-validated) + LED_DATA5 stub.
Refs TP1..TP6 on F.Fab; net-name label on F.Silkscreen, placed clear of pads,
courtyards and the board edge. Idempotent: run against a clean board file."""
import sys, math
sys.path.insert(0,'tools_pcb')
from r10fin import Model, validate, mm, um
import pcbnew
from pcbnew import VECTOR2I

BOARD='airwriter.kicad_pcb'
TPLIB='/Applications/KiCad/KiCad.app/Contents/SharedSupport/footprints/TestPoint.pretty'
FPNAME='TestPoint_Pad_D1.0mm'

M=Model(BOARD); b=M.b
FCu=b.GetLayerID('F.Cu'); FSilk=b.GetLayerID('F.SilkS'); FFab=b.GetLayerID('F.Fab')

pads=[(p.GetPosition().x,p.GetPosition().y) for f in b.GetFootprints() for p in f.Pads()]
# existing F.Silkscreen text bounding boxes (board texts + footprint silk texts)
silktext=[]
for d in b.GetDrawings():
    if isinstance(d,pcbnew.PCB_TEXT) and d.GetLayer()==b.GetLayerID('F.SilkS'):
        bb=d.GetBoundingBox(); silktext.append((bb.GetLeft(),bb.GetTop(),bb.GetRight(),bb.GetBottom()))
for f in b.GetFootprints():
    for t in [f.Reference(),f.Value()]:
        if t.IsVisible() and t.GetLayer()==b.GetLayerID('F.SilkS'):
            bb=t.GetBoundingBox(); silktext.append((bb.GetLeft(),bb.GetTop(),bb.GetRight(),bb.GetBottom()))
    for g in f.GraphicalItems():
        if g.GetLayer()==b.GetLayerID('F.SilkS') and isinstance(g,(pcbnew.PCB_TEXT,pcbnew.PCB_SHAPE)):
            bb=g.GetBoundingBox(); silktext.append((bb.GetLeft(),bb.GetTop(),bb.GetRight(),bb.GetBottom()))
EDGE=(100.6,161.4,60.6,163.7)
def label_clear(cx,cy,halfw,selfref):
    if cx-mm(halfw)<mm(EDGE[0]) or cx+mm(halfw)>mm(EDGE[1]) or cy-mm(0.6)<mm(EDGE[2]) or cy+mm(0.6)>mm(EDGE[3]):
        return False
    lx0,lx1,ly0,ly1=cx-mm(halfw+0.3),cx+mm(halfw+0.3),cy-mm(0.8),cy+mm(0.8)
    for tx0,ty0,tx1,ty1 in silktext:
        if lx0<tx1 and lx1>tx0 and ly0<ty1 and ly1>ty0: return False
    for px,py in pads:
        if abs(px-cx)<mm(halfw+0.5) and abs(py-cy)<mm(1.1): return False
    samples=[VECTOR2I(int(cx+mm(ex)),int(cy+mm(ey)))
             for ex in (-halfw,-halfw/2,0,halfw/2,halfw) for ey in (-0.5,0.0,0.5)]
    for ref,cy2 in M.cyards:
        if ref==selfref: continue
        for S in samples:
            if cy2.Collide(S,0): return False
    return True
def nearest_cy_dir(x,y):
    best=None
    for ref,cy in M.cyards:
        bb=cy.BBox(); ccx=um((bb.GetLeft()+bb.GetRight())/2); ccy=um((bb.GetTop()+bb.GetBottom())/2)
        d=math.hypot(ccx-x,ccy-y)
        if d<0.1: continue
        if best is None or d<best[0]: best=(d,ccx,ccy)
    if not best: return (0,1)
    return (x-best[1],y-best[2])
def pick_label(x,y,lab,ref):
    halfw=0.6*len(lab); W=2.0+halfw
    cand=[(0,1.9),(0,-1.9),(W,0),(-W,0),(0,2.7),(0,-2.7),(W,1.9),(-W,1.9),(W,-1.9),(-W,-1.9)]
    dx,dy=nearest_cy_dir(x,y); nrm=math.hypot(dx,dy) or 1; ux,uy=dx/nrm,dy/nrm
    cand.sort(key=lambda o:-(o[0]*ux+o[1]*uy))
    for ox,oy in cand:
        if label_clear(mm(x+ox),mm(y+oy),halfw,ref): return (ox,oy)
    return None

def search_gnd(xr,yr,ref):
    gnc=b.FindNet('/GND').GetNetCode(); best=None
    for xi in range(xr[0],xr[1],2):
        for yi in range(yr[0],yr[1],2):
            x,y=xi/10.0,yi/10.0
            if not validate(M,VECTOR2I(mm(x),mm(y)),gnc,need_gnd=True,extra=0.1)[0]: continue
            if pick_label(x,y,'GND',ref) is None: continue
            d=M.nearest_other(VECTOR2I(mm(x),mm(y)),gnc)
            if best is None or d>best[0]: best=(d,x,y)
    return best

def search_onnet(net,ref,lab):
    nc=b.FindNet(net).GetNetCode(); best=None
    for P in M.net_track_points(nc,'F'):
        x,y=um(P.x),um(P.y)
        if not validate(M,P,nc,need_gnd=False,need_onnet=True)[0]: continue
        if pick_label(x,y,lab,ref) is None: continue
        d=M.nearest_other(P,nc)
        if best is None or d>best[0]: best=(d,x,y)
    return best

# GND SE (east/lower) and GND SW (west/lower)
g3=search_gnd((1400,1615),(1440,1620),'TP3'); print("GND-E ->",(g3[1],g3[2]),"m",um(g3[0]))
g4=search_gnd((1010,1300),(1500,1620),'TP4'); print("GND-W ->",(g4[1],g4[2]),"m",um(g4[0]))
v1=search_onnet('/VBUS','TP1','VBUS'); print("VBUS ->",(v1[1],v1[2]),"m",um(v1[0]))
v2=search_onnet('/+3V3','TP2','3V3'); print("3V3  ->",(v2[1],v2[2]),"m",um(v2[0]))
v5=search_onnet('/EN','TP5','EN'); print("EN   ->",(v5[1],v5[2]),"m",um(v5[0]))

# LED_DATA5 stub TP
lnc=b.FindNet('/LED_DATA5').GetNetCode()
srcs=[VECTOR2I(mm(150.1),mm(125.643)),VECTOR2I(mm(153.639),mm(124.068))]
bestL=None
for xi in range(1485,1560):
    for yi in range(1200,1320):
        x,y=xi/10.0,yi/10.0; P=VECTOR2I(mm(x),mm(y))
        if not validate(M,P,lnc,need_onnet=False,extra=0.05)[0]: continue
        if pick_label(x,y,'LD5','TP6') is None: continue
        s=min(srcs,key=lambda q:math.hypot(q.x-P.x,q.y-P.y))
        d=math.hypot(s.x-P.x,s.y-P.y)/1e6
        if d>3.5: continue
        n=max(1,int(d/0.15)); good=all(
            M.copper_clear(VECTOR2I(int(s.x+(P.x-s.x)*i/n),int(s.y+(P.y-s.y)*i/n)),lnc,mm(0.25))
            for i in range(n+1))
        if good:
            mg=M.nearest_other(P,lnc)
            if bestL is None or mg>bestL[0]: bestL=(mg,x,y,s)
led=(bestL[1],bestL[2]); ledsrc=bestL[3]
print("LED_DATA5 ->",led,"stub from",(um(ledsrc.x),um(ledsrc.y)),"m",um(bestL[0]))

# (ref,label,net,x,y,stub_src)
TPS=[
 ('TP1','VBUS','/VBUS',v1[1],v1[2],None),
 ('TP2','3V3','/+3V3',v2[1],v2[2],None),
 ('TP3','GND','/GND',g3[1],g3[2],None),
 ('TP4','GND','/GND',g4[1],g4[2],None),
 ('TP5','EN','/EN',v5[1],v5[2],None),
 ('TP6','LD5','/LED_DATA5',led[0],led[1],ledsrc),
]

# validate placement + label feasibility before mutating
labels={}
for ref,lab,net,x,y,src in TPS:
    nc=b.FindNet(net).GetNetCode()
    need_gnd=(net=='/GND'); onnet=(src is None and not need_gnd)
    ok,why=validate(M,VECTOR2I(mm(x),mm(y)),nc,need_gnd=need_gnd,need_onnet=onnet)
    assert ok, f"{ref} {lab} pad invalid: {why}"
    off=pick_label(x,y,lab,ref); assert off is not None, f"{ref} {lab} no silk spot"
    labels[ref]=off
    print(f"OK {ref} {lab} ({x},{y}) net {net} label{off}")

for ref,lab,net,x,y,src in TPS:
    fp=pcbnew.FootprintLoad(TPLIB,FPNAME)
    fp.SetReference(ref); fp.SetValue(lab)
    fp.SetPosition(VECTOR2I(mm(x),mm(y)))
    b.Add(fp)
    net_obj=b.FindNet(net)
    for pad in fp.Pads(): pad.SetNet(net_obj)
    fp.Reference().SetLayer(FFab); fp.Reference().SetTextSize(VECTOR2I(mm(1.0),mm(1.0)))
    for g in list(fp.GraphicalItems()):
        if isinstance(g,pcbnew.PCB_SHAPE) and g.GetLayer()==FSilk: fp.Remove(g)
    ox,oy=labels[ref]
    txt=pcbnew.PCB_TEXT(fp); txt.SetText(lab); txt.SetLayer(FSilk)
    txt.SetPosition(VECTOR2I(mm(x+ox),mm(y+oy)))
    txt.SetTextSize(VECTOR2I(mm(1.0),mm(1.0))); txt.SetTextThickness(mm(0.15))
    fp.Add(txt)
    if src is not None:
        tr=pcbnew.PCB_TRACK(b); tr.SetStart(VECTOR2I(src.x,src.y)); tr.SetEnd(VECTOR2I(mm(x),mm(y)))
        tr.SetWidth(mm(0.2)); tr.SetLayer(FCu); tr.SetNet(net_obj); b.Add(tr)

b.Save(BOARD); print("SAVED",len(TPS),"test points")
