#!/usr/bin/env python3
"""Stitch every stranded GND component to the main ground plane: one GND via
per stranded component, placed on its F.Cu island over the main B.Cu flood.
0.4mm drill / 0.6mm pad. Clearance + hole-to-hole + antenna-keepout aware."""
import sys, math
sys.path.insert(0,'tools_pcb')
import r12_gnd as G
from r10fin import Model, mm, um
import pcbnew
from pcbnew import VECTOR2I

b=pcbnew.LoadBoard('airwriter.kicad_pcb')
r=G.analyze(b, verbose=False)
FCu=r['FCu']; BCu=r['BCu']; dsu=r['dsu']; main=r['main_root']; islands=r['islands']
GNDnet=b.FindNet('/GND')

# main-plane island polygons per layer (a via bridges to main where main has
# copper on the layer OPPOSITE the stranded island the via sits on)
main_F=[islands[i][1] for i in r['comp'][main] if islands[i][0]==FCu]
main_B=[islands[i][1] for i in r['comp'][main] if islands[i][0]==BCu]
def in_any(P,polys): return any(ps.Contains(P) for ps in polys)

# clearance model (reuses filled zones already in the file)
M=Model('airwriter.kicad_pcb')
# via sizes to try, largest first: (drill, pad, radius). 0.3/0.5 = 0.1 annular fallback.
VIA_SIZES=[(mm(0.4),mm(0.6),mm(0.3)),(mm(0.3),mm(0.5),mm(0.25))]
CLRV=mm(0.155)
# hole list
holes=[(t.GetPosition().x,t.GetPosition().y,t.GetDrill()//2) for t in b.GetTracks() if t.Type()==pcbnew.PCB_VIA_T]
for f in b.GetFootprints():
    for p in f.Pads():
        if p.GetDrillSize().x>0: pp=p.GetPosition(); holes.append((pp.x,pp.y,p.GetDrillSize().x//2))
H2H=mm(0.25)+mm(0.01)
def hole_ok(P,vr): return all(math.hypot(hx-P.x,hy-P.y)>=vr+hr+H2H for hx,hy,hr in holes)

def fits(P,vr):
    if not M.copper_clear(P,r['gnc'],vr+CLRV,'F'): return False
    if not M.copper_clear(P,r['gnc'],vr+CLRV,'B'): return False
    if not M.keepout_clear(P,vr): return False
    if not hole_ok(P,vr): return False
    x,y=um(P.x),um(P.y)
    if x<100.9 or x>161.1 or y<60.9 or y>163.4: return False
    return True
def clr_ok(P):
    """returns (drill,pad,radius) of the largest via that fits at P, or None."""
    for drill,pad,vr in VIA_SIZES:
        if fits(P,vr): return (drill,pad,vr)
    return None
def bridges_main(P, stranded_layer):
    # via at P: stranded copper on stranded_layer, need MAIN copper on the other layer
    if stranded_layer==FCu: return in_any(P, main_B)
    else: return in_any(P, main_F)

# stranded components (root != main)
from collections import defaultdict
comp=r['comp']
stranded_roots=[root for root in comp if root!=main]
print(f"stranded components to stitch: {len(stranded_roots)}")

added=[]; failed=[]
for root in stranded_roots:
    comp_isl=[(islands[i][0],islands[i][1]) for i in comp[root]]  # (layer,poly)
    placed=None; sz=None
    for L,ps in comp_isl:
        bb=ps.BBox()
        x0,y0,x1,y1=bb.GetLeft(),bb.GetTop(),bb.GetRight(),bb.GetBottom()
        step=mm(0.2)
        yy=y0
        while yy<=y1 and not placed:
            xx=x0
            while xx<=x1:
                P=VECTOR2I(int(xx),int(yy))
                if ps.Contains(P) and bridges_main(P,L):
                    s=clr_ok(P)
                    if s: placed=P; sz=s; break
                xx+=step
            yy+=step
        if placed: break
    if placed:
        drill,pad,vr=sz
        v=pcbnew.PCB_VIA(b); v.SetPosition(placed); v.SetDrill(drill); v.SetWidth(pad)
        v.SetNet(GNDnet); v.SetLayerPair(FCu,BCu); b.Add(v)
        holes.append((placed.x,placed.y,vr))
        added.append((um(placed.x),um(placed.y),um(pad)))
    else:
        # describe the component for reporting
        pads=[f"{ref}.{num}" for ref,num,pos,hit in r['padinfo'] if hit and dsu.find(hit[0])==root]
        failed.append((root,pads,[(um(islands[i][2][0]),um(islands[i][2][1])) for i in comp[root]]))

print(f"vias added: {len(added)}")
for a in added: print("  via",a)
if failed:
    print(f"FAILED to stitch {len(failed)} components:")
    for root,pads,geo in failed: print("  comp",root,"pads",pads,"island-bbox-corners",geo[:3])

if added:
    pcbnew.ZONE_FILLER(b).Fill(b.Zones())
    b.Save('airwriter.kicad_pcb')
    print("refilled + saved", len(added), "vias")
if failed:
    print("REMAINING unstitched:", len(failed))
