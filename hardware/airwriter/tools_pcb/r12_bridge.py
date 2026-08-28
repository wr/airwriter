#!/usr/bin/env python3
"""Bridge GND fragments that sit over B.Cu dot-windows (no back copper) to the
main plane with a short F.Cu GND track routed through clear space (BFS around
the isolating traces). GND-to-GND, so it never shorts a signal."""
import sys, math
sys.path.insert(0,'tools_pcb')
import r12_gnd as G
from r10fin import Model, mm, um
import pcbnew
from pcbnew import VECTOR2I
from collections import deque

b=pcbnew.LoadBoard('airwriter.kicad_pcb')
r=G.analyze(b, verbose=False)
FCu=r['FCu']; BCu=r['BCu']; islands=r['islands']; main=r['main_root']; dsu=r['dsu']; gnc=r['gnc']; GNDnet=b.FindNet('/GND')
main_B=[islands[i][1] for i in r['comp'][main] if islands[i][0]==BCu]
def over_main_B(P): return any(ps.Contains(P) for ps in main_B)
M=Model('airwriter.kicad_pcb')
holes=[(t.GetPosition().x,t.GetPosition().y,t.GetDrill()//2) for t in b.GetTracks() if t.Type()==pcbnew.PCB_VIA_T]
for f in b.GetFootprints():
    for p in f.Pads():
        if p.GetDrillSize().x>0: pp=p.GetPosition(); holes.append((pp.x,pp.y,p.GetDrillSize().x//2))
def via_fits(P,vr):
    return (M.copper_clear(P,gnc,vr+mm(0.155),'F') and M.copper_clear(P,gnc,vr+mm(0.155),'B')
            and M.keepout_clear(P,vr) and all(math.hypot(hx-P.x,hy-P.y)>=vr+hr+mm(0.26) for hx,hy,hr in holes))
def via_here(P):  # (drill,pad,radius) of largest via to main-B at P, or None
    if not over_main_B(P): return None
    for drill,pad,vr in [(mm(0.4),mm(0.6),mm(0.3)),(mm(0.3),mm(0.5),mm(0.25))]:
        if via_fits(P,vr): return (drill,pad,vr)
    return None
STEP=0.2
def passable(x,y):  # F.Cu cell a 0.15mm GND track can occupy; edge- and clearance-aware
    if x<100.75 or x>161.25 or y<60.75 or y>163.55: return False  # stay off board edge
    return M.copper_clear(VECTOR2I(mm(x),mm(y)), gnc, mm(0.075+0.15), 'F')

def bridge(frag_polys):
    # start cells: interior points of the fragment
    starts=[]
    for ps in frag_polys:
        bb=ps.BBox()
        y=bb.GetTop()
        while y<=bb.GetBottom():
            x=bb.GetLeft()
            while x<=bb.GetRight():
                P=VECTOR2I(int(x),int(y))
                if ps.Contains(P): starts.append((round(um(P.x),1),round(um(P.y),1)))
                x+=mm(STEP)
            y+=mm(STEP)
    if not starts: return None, None
    # bounded BFS box around fragment
    xs=[s[0] for s in starts]; ys=[s[1] for s in starts]
    box=(min(xs)-22,max(xs)+22,min(ys)-22,max(ys)+22)
    startset=set(starts)
    seen={}; q=deque()
    for s in starts: seen[s]=None; q.append(s)
    goal=None; goalsz=None
    while q:
        cx,cy=q.popleft()
        if (cx,cy) not in startset:
            sz=via_here(VECTOR2I(mm(cx),mm(cy)))
            if sz: goal=(cx,cy); goalsz=sz; break
        for dx,dy in ((-STEP,0),(STEP,0),(0,-STEP),(0,STEP)):  # orthogonal only (uniform track clearance)
                nx,ny=round(cx+dx,1),round(cy+dy,1)
                if not(box[0]<nx<box[1] and box[2]<ny<box[3]): continue
                if (nx,ny) in seen: continue
                if passable(nx,ny):
                    seen[(nx,ny)]=(cx,cy); q.append((nx,ny))
    if goal is None: return None, None
    # reconstruct + simplify collinear
    path=[goal]; cur=seen[goal]
    while cur is not None: path.append(cur); cur=seen[cur]
    path.reverse()
    simp=[path[0]]
    for i in range(1,len(path)-1):
        ax,ay=simp[-1]; bx,by=path[i]; cxx,cyy=path[i+1]
        if (bx-ax)*(cyy-ay)!=(cxx-ax)*(by-ay): simp.append(path[i])
    simp.append(path[-1])
    return simp, goalsz

stranded=[root for root in r['comp'] if root!=main and any(hit and dsu.find(hit[0])==root for _,_,_,hit in r['padinfo'])]
done=0; fail=[]
for root in stranded:
    frag=[islands[i][1] for i in r['comp'][root] if islands[i][0]==FCu]
    pads=[f"{ref}.{num}" for ref,num,pos,hit in r['padinfo'] if hit and dsu.find(hit[0])==root]
    path,sz=bridge(frag)
    if path and len(path)>=2:
        Ltot=sum(math.hypot(path[i+1][0]-path[i][0],path[i+1][1]-path[i][1]) for i in range(len(path)-1))
        if Ltot>8.0:
            print(f"SKIP {pads}: only a long {round(Ltot,1)}mm bridge found (>8mm) — reporting instead")
            fail.append(pads); continue
        for i in range(len(path)-1):
            a,c=path[i],path[i+1]
            tr=pcbnew.PCB_TRACK(b); tr.SetStart(VECTOR2I(mm(a[0]),mm(a[1]))); tr.SetEnd(VECTOR2I(mm(c[0]),mm(c[1])))
            tr.SetWidth(mm(0.15)); tr.SetLayer(FCu); tr.SetNet(GNDnet); b.Add(tr)
        drill,pad,vr=sz; gp=path[-1]
        v=pcbnew.PCB_VIA(b); v.SetPosition(VECTOR2I(mm(gp[0]),mm(gp[1]))); v.SetDrill(drill); v.SetWidth(pad)
        v.SetNet(GNDnet); v.SetLayerPair(FCu,BCu); b.Add(v); holes.append((mm(gp[0]),mm(gp[1]),vr))
        L=sum(math.hypot(path[i+1][0]-path[i][0],path[i+1][1]-path[i][1]) for i in range(len(path)-1))
        print(f"bridged {pads}: F.Cu GND track {round(L,2)}mm -> via(pad {um(pad)}) to main-B at {gp}")
        done+=1
    else:
        print(f"FAILED {pads}: no clear F.Cu corridor to an over-main-B via spot")
        fail.append(pads)
if done:
    pcbnew.ZONE_FILLER(b).Fill(b.Zones()); b.Save('airwriter.kicad_pcb'); print("saved",done,"bridges")
print("remaining failed:",fail)
