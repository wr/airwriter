#!/usr/bin/env python3
"""Bridge the last GND fragments (in the LocalTalk B.Cu desert) on the BACK:
a via from the F.Cu fragment to a clear B.Cu spot, then a B.Cu GND track through
clear back space to the main B.Cu flood. Local 0.12mm clearance (rule area
'GND_STITCH_012'). B.Cu track sits in the GND-flood region (invisible)."""
import sys, math
sys.path.insert(0,'tools_pcb')
import r12_gnd as G
from r10fin import Model, mm, um
import pcbnew
from pcbnew import VECTOR2I
from collections import deque

b=pcbnew.LoadBoard('airwriter.kicad_pcb')
r=G.analyze(b, verbose=False)
FCu=r['FCu']; BCu=r['BCu']; islands=r['islands']; mainr=r['main_root']; dsu=r['dsu']; gnc=r['gnc']; GND=b.FindNet('/GND')
main_B=[islands[i][1] for i in r['comp'][mainr] if islands[i][0]==BCu]
def omb(P): return any(ps.Contains(P) for ps in main_B)
M=Model('airwriter.kicad_pcb')
DRILL=mm(0.3); PAD=mm(0.5); VR=mm(0.25); CLR=mm(0.12)
holes=[(t.GetPosition().x,t.GetPosition().y,t.GetDrill()//2) for t in b.GetTracks() if t.Type()==pcbnew.PCB_VIA_T]
for f in b.GetFootprints():
    for p in f.Pads():
        if p.GetDrillSize().x>0: pp=p.GetPosition(); holes.append((pp.x,pp.y,p.GetDrillSize().x//2))
def hole_ok(P): return all(math.hypot(hx-P.x,hy-P.y)>=VR+hr+mm(0.26) for hx,hy,hr in holes)
def via_ok(P):   # via lands on F.Cu fragment (caller ensures) + B.Cu clear + hole ok
    return (M.copper_clear(P,gnc,VR+CLR,'F') and M.copper_clear(P,gnc,VR+CLR,'B')
            and M.keepout_clear(P,VR) and hole_ok(P))
STEP=0.2
def bpass(x,y):  # B.Cu cell for a 0.2mm GND track at 0.12 clearance
    if x<100.75 or x>161.25 or y<60.75 or y>163.55: return False
    return M.copper_clear(VECTOR2I(mm(x),mm(y)),gnc,mm(0.1+0.12),'B')

def rule_area(box,m=0.6):
    z=pcbnew.ZONE(b); z.SetIsRuleArea(True); z.SetZoneName("GND_STITCH_012")
    ls=pcbnew.LSET(); ls.AddLayer(FCu); ls.AddLayer(BCu); z.SetLayerSet(ls)
    for fn in ('SetDoNotAllowTracks','SetDoNotAllowVias','SetDoNotAllowPads','SetDoNotAllowZoneFills','SetDoNotAllowFootprints'):
        getattr(z,fn)(False)
    poly=pcbnew.SHAPE_LINE_CHAIN()
    for cx,cy in [(box[0]-m,box[1]-m),(box[2]+m,box[1]-m),(box[2]+m,box[3]+m),(box[0]-m,box[3]+m)]:
        poly.Append(mm(cx),mm(cy))
    poly.SetClosed(True); z.AddPolygon(poly); b.Add(z)

added=[]; failed=[]
for rt in list(r['comp'].keys()):
    if rt==mainr: continue
    pads=[f"{ref}.{num}" for ref,num,pos,hit in r['padinfo'] if hit and dsu.find(hit[0])==rt]
    if not pads: continue
    frag=[islands[i][1] for i in r['comp'][rt] if islands[i][0]==FCu]
    # candidate via spots: on-fragment, B.Cu clear + F.Cu clear(of other) + hole ok
    vias=[]
    for ps in frag:
        bb=ps.BBox(); x=bb.GetLeft()
        while x<=bb.GetRight():
            y=bb.GetTop()
            while y<=bb.GetBottom():
                P=VECTOR2I(int(x),int(y))
                if ps.Contains(P) and via_ok(P): vias.append((um(P.x),um(P.y)))
                y+=mm(0.2)
            x+=mm(0.2)
    # for each candidate via, BFS on B.Cu to main-B flood
    best=None
    for vx,vy in vias:
        if omb(VECTOR2I(mm(vx),mm(vy))):   # already over main-B (shouldn't happen here)
            best=((vx,vy),[]); break
        seen={(round(vx,1),round(vy,1)):None}; q=deque([(round(vx,1),round(vy,1))])
        goal=None
        while q:
            cx,cy=q.popleft()
            if omb(VECTOR2I(mm(cx),mm(cy))) and (cx,cy)!=(round(vx,1),round(vy,1)): goal=(cx,cy); break
            if abs(cx-vx)>12 or abs(cy-vy)>12: continue
            for dx in(-STEP,0,STEP):
                for dy in(-STEP,0,STEP):
                    if dx==0 and dy==0: continue
                    nx,ny=round(cx+dx,1),round(cy+dy,1)
                    if (nx,ny) in seen: continue
                    if bpass(nx,ny): seen[(nx,ny)]=(cx,cy); q.append((nx,ny))
        if goal:
            path=[goal]; cur=seen[goal]
            while cur is not None: path.append(cur); cur=seen[cur]
            path.reverse()
            if best is None or len(path)<len(best[1]): best=((vx,vy),path)
    if best is None:
        failed.append(pads); print(f"{pads}: no B.Cu bridge"); continue
    (vx,vy),path=best
    # place via + B.Cu track along path (simplify collinear)
    v=pcbnew.PCB_VIA(b); v.SetPosition(VECTOR2I(mm(vx),mm(vy))); v.SetDrill(DRILL); v.SetWidth(PAD)
    v.SetNet(GND); v.SetLayerPair(FCu,BCu); b.Add(v); holes.append((mm(vx),mm(vy),VR))
    box=[vx,vy,vx,vy]
    simp=[path[0]]
    for i in range(1,len(path)-1):
        ax,ay=simp[-1]; bx,by=path[i]; cxx,cyy=path[i+1]
        if (bx-ax)*(cyy-ay)!=(cxx-ax)*(by-ay): simp.append(path[i])
    simp.append(path[-1])
    for i in range(len(simp)-1):
        a,c=simp[i],simp[i+1]
        tr=pcbnew.PCB_TRACK(b); tr.SetStart(VECTOR2I(mm(a[0]),mm(a[1]))); tr.SetEnd(VECTOR2I(mm(c[0]),mm(c[1])))
        tr.SetWidth(mm(0.2)); tr.SetLayer(BCu); tr.SetNet(GND); b.Add(tr)
        box[0]=min(box[0],a[0],c[0]);box[1]=min(box[1],a[1],c[1]);box[2]=max(box[2],a[0],c[0]);box[3]=max(box[3],a[1],c[1])
    L=sum(math.hypot(simp[i+1][0]-simp[i][0],simp[i+1][1]-simp[i][1]) for i in range(len(simp)-1))
    rule_area(box)
    print(f"{pads}: F->B via @({vx},{vy}) + B.Cu GND track {round(L,2)}mm -> main flood")
    added.append(pads)

print("stitched:",len(added),"failed:",len(failed))
if failed: print("FAILED:",failed)
pcbnew.ZONE_FILLER(b).Fill(b.Zones()); b.Save('airwriter.kicad_pcb'); print("saved")
