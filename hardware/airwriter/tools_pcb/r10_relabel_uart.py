#!/usr/bin/env python3
"""Add F.Silkscreen labels to TP7/TP8 where they fit (mutual + full obstacle aware).
Single LoadBoard. Tries full net name then TX/RX; skips if genuinely no room."""
import sys, math
sys.path.insert(0,'tools_pcb')
from r10fin import Model, mm, um
import pcbnew
from pcbnew import VECTOR2I
BOARD='airwriter.kicad_pcb'
M=Model(BOARD); b=M.b
FSilk=b.GetLayerID('F.SilkS')
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
def clear(cx,cy,halfw,selfref):
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
def pick(x,y,text,ref):
    halfw=0.6*len(text); W=2.0+halfw
    offs=[(0,1.9),(0,-1.9),(W,0),(-W,0),(0,2.7),(0,-2.7),(W,1.9),(-W,1.9),(W,-1.9),(-W,-1.9)]
    for r in [3.0,3.5,4.0,4.5,5.0,5.5,6.0]:
        for a in range(0,360,20): offs.append((r*math.cos(math.radians(a)),r*math.sin(math.radians(a))))
    for ox,oy in offs:
        if clear(mm(x+ox),mm(y+oy),halfw,ref): return (ox,oy),halfw
    return None,None

ref=sys.argv[1]; full=sys.argv[2]
f=b.FindFootprintByReference(ref); x,y=um(f.GetX()),um(f.GetY())
done=False
for text in [full, full[:2]]:
    off,halfw=pick(x,y,text,ref)
    if off:
        cx,cy=x+off[0],y+off[1]
        txt=pcbnew.PCB_TEXT(f); txt.SetText(text); txt.SetLayer(FSilk)
        txt.SetPosition(VECTOR2I(mm(cx),mm(cy))); txt.SetTextSize(VECTOR2I(mm(1.0),mm(1.0))); txt.SetTextThickness(mm(0.15))
        f.Add(txt)
        print(f"{ref}: '{text}' at ({round(cx,2)},{round(cy,2)}) off({round(off[0],2)},{round(off[1],2)})")
        done=True; break
if not done: print(f"{ref}: no silk-label room (F.Fab value '{full}' identifies it)")
pcbnew.ZONE_FILLER(b).Fill(b.Zones())
b.Save(BOARD); print("saved "+ref)
