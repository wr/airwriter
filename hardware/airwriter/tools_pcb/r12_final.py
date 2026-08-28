#!/usr/bin/env python3
"""Tie the last 5 stranded GND fragments to the main plane using a LOCAL 0.12mm
clearance relaxation (scoped by named rule areas 'GND_STITCH_012'; global stays
0.15). On-fragment via where the fragment sits over the main B.Cu flood
(U5/C5); an F.Cu GND bridge track to a nearby over-main-B via spot otherwise
(C3/C14/C22). Smallest legal via 0.3mm drill / 0.5mm pad."""
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
def via_fits(P):
    return (omb(P) and M.copper_clear(P,gnc,VR+CLR,'F') and M.copper_clear(P,gnc,VR+CLR,'B')
            and M.keepout_clear(P,VR) and hole_ok(P))
STEP=0.15
def passable(x,y):
    if x<100.75 or x>161.25 or y<60.75 or y>163.55: return False
    return M.copper_clear(VECTOR2I(mm(x),mm(y)), gnc, mm(0.075+0.12), 'F')

def best_onfrag(frag):
    best=None
    for ps in frag:
        bb=ps.BBox(); x=bb.GetLeft()
        while x<=bb.GetRight():
            y=bb.GetTop()
            while y<=bb.GetBottom():
                P=VECTOR2I(int(x),int(y))
                if ps.Contains(P) and via_fits(P):
                    return P     # first fit is enough
                y+=mm(0.1)
            x+=mm(0.1)
    return None

def bridge(frag):
    starts=[]
    for ps in frag:
        bb=ps.BBox(); y=bb.GetTop()
        while y<=bb.GetBottom():
            x=bb.GetLeft()
            while x<=bb.GetRight():
                P=VECTOR2I(int(x),int(y))
                if ps.Contains(P): starts.append((round(um(P.x),1),round(um(P.y),1)))
                x+=mm(STEP)
            y+=mm(STEP)
    if not starts: return None
    xs=[s[0] for s in starts]; ys=[s[1] for s in starts]
    box=(min(xs)-20,max(xs)+20,min(ys)-20,max(ys)+20)
    seen={s:None for s in starts}; q=deque(starts); sset=set(starts); goal=None
    while q:
        cx,cy=q.popleft()
        if (cx,cy) not in sset and via_fits(VECTOR2I(mm(cx),mm(cy))): goal=(cx,cy); break
        for dx in (-STEP,0,STEP):
            for dy in (-STEP,0,STEP):
                if dx==0 and dy==0: continue
                nx,ny=round(cx+dx,1),round(cy+dy,1)
                if not(box[0]<nx<box[1] and box[2]<ny<box[3]) or (nx,ny) in seen: continue
                if passable(nx,ny): seen[(nx,ny)]=(cx,cy); q.append((nx,ny))
    if goal is None: return None
    path=[goal]; cur=seen[goal]
    while cur is not None: path.append(cur); cur=seen[cur]
    path.reverse()
    simp=[path[0]]
    for i in range(1,len(path)-1):
        ax,ay=simp[-1]; bx,by=path[i]; cxx,cyy=path[i+1]
        if (bx-ax)*(cyy-ay)!=(cxx-ax)*(by-ay): simp.append(path[i])
    simp.append(path[-1])
    return simp

def add_via(P):
    v=pcbnew.PCB_VIA(b); v.SetPosition(P); v.SetDrill(DRILL); v.SetWidth(PAD)
    v.SetNet(GND); v.SetLayerPair(FCu,BCu); b.Add(v); holes.append((P.x,P.y,VR))
    return v

added=[]; failed=[]
for rt in list(r['comp'].keys()):
    if rt==mainr: continue
    pads=[f"{ref}.{num}" for ref,num,pos,hit in r['padinfo'] if hit and dsu.find(hit[0])==rt]
    if not pads: continue
    frag=[islands[i][1] for i in r['comp'][rt] if islands[i][0]==FCu]
    box=[9e9,9e9,-9e9,-9e9]   # accumulate added-copper bbox (for rule area)
    def acc(x,y): box[0]=min(box[0],x); box[1]=min(box[1],y); box[2]=max(box[2],x); box[3]=max(box[3],y)
    P=best_onfrag(frag)
    if P is not None:
        add_via(P); acc(um(P.x),um(P.y))
        print(f"{pads}: on-fragment via @({um(P.x)},{um(P.y)})")
    else:
        path=bridge(frag)
        if not path:
            failed.append(pads); print(f"{pads}: FAILED even at 0.12"); continue
        for i in range(len(path)-1):
            a,c=path[i],path[i+1]
            tr=pcbnew.PCB_TRACK(b); tr.SetStart(VECTOR2I(mm(a[0]),mm(a[1]))); tr.SetEnd(VECTOR2I(mm(c[0]),mm(c[1])))
            tr.SetWidth(mm(0.15)); tr.SetLayer(FCu); tr.SetNet(GND); b.Add(tr)
            acc(a[0],a[1]); acc(c[0],c[1])
        gp=path[-1]; add_via(VECTOR2I(mm(gp[0]),mm(gp[1])))
        L=sum(math.hypot(path[i+1][0]-path[i][0],path[i+1][1]-path[i][1]) for i in range(len(path)-1))
        print(f"{pads}: bridge {round(L,2)}mm -> via @({gp[0]},{gp[1]})")
    # named rule area covering the added copper (+0.5mm margin)
    z=pcbnew.ZONE(b); z.SetIsRuleArea(True); z.SetZoneName("GND_STITCH_012")
    ls=pcbnew.LSET(); ls.AddLayer(FCu); ls.AddLayer(BCu); z.SetLayerSet(ls)
    for fn in ('SetDoNotAllowTracks','SetDoNotAllowVias','SetDoNotAllowPads','SetDoNotAllowZoneFills','SetDoNotAllowFootprints'):
        getattr(z,fn)(False)
    m=0.6
    poly=pcbnew.SHAPE_LINE_CHAIN()
    for cx,cy in [(box[0]-m,box[1]-m),(box[2]+m,box[1]-m),(box[2]+m,box[3]+m),(box[0]-m,box[3]+m)]:
        poly.Append(mm(cx),mm(cy))
    poly.SetClosed(True); z.AddPolygon(poly); b.Add(z)
    added.append(pads)

print("stitched:",len(added),"failed:",len(failed))
if failed: print("FAILED:",failed)
pcbnew.ZONE_FILLER(b).Fill(b.Zones()); b.Save('airwriter.kicad_pcb'); print("saved")
