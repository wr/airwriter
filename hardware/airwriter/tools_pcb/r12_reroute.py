#!/usr/bin/env python3
"""Local connectivity-preserving 'jog': push the portion of a signal trace that
runs near a reserve point away from it, keeping both endpoints anchored (so the
net stays connected). Used to open clean GND-via space for a stranded fragment.
Then drop the GND stitch via. DRC-verify externally per fragment."""
import sys, math
sys.path.insert(0,'tools_pcb')
from r10fin import Model, mm, um
import pcbnew
from pcbnew import VECTOR2I

def jog_segment(b, seg, P, reserve_mm, push_mm, span_mm):
    """Replace the part of straight track `seg` within span of the foot-of-perp
    from P with a detour pushed `push_mm` further from P. Endpoints preserved."""
    S=seg.GetStart(); E=seg.GetEnd()
    sx,sy,ex,ey=S.x,S.y,E.x,E.y
    dx,dy=ex-sx,ey-sy
    L=math.hypot(dx,dy)
    if L<1: return None
    ux,uy=dx/L,dy/L                      # unit along seg
    # foot of perpendicular from P
    t=((P.x-sx)*ux+(P.y-sy)*uy)
    fx,fy=sx+ux*t, sy+uy*t
    # perpendicular direction pointing AWAY from P
    px,py=fx-P.x, fy-P.y
    pl=math.hypot(px,py)
    if pl<1: return None
    px,py=px/pl,py/pl
    span=mm(span_mm); push=mm(push_mm)
    # jog anchor points along the segment, clamped inside it
    t0=max(0.0, t-span); t1=min(L, t+span)
    A=(sx+ux*t0, sy+uy*t0)               # jog start (on original line)
    Bp=(sx+ux*t1, sy+uy*t1)              # jog end (on original line)
    Ao=(A[0]+px*push, A[1]+py*push)      # pushed
    Bo=(Bp[0]+px*push, Bp[1]+py*push)
    net=seg.GetNet(); layer=seg.GetLayer(); w=seg.GetWidth()
    pts=[(sx,sy),A,Ao,Bo,Bp,(ex,ey)]
    b.Remove(seg)
    newtracks=[]
    for i in range(len(pts)-1):
        a,c=pts[i],pts[i+1]
        if abs(a[0]-c[0])<1 and abs(a[1]-c[1])<1: continue
        tr=pcbnew.PCB_TRACK(b); tr.SetStart(VECTOR2I(int(a[0]),int(a[1]))); tr.SetEnd(VECTOR2I(int(c[0]),int(c[1])))
        tr.SetWidth(w); tr.SetLayer(layer); tr.SetNet(net); b.Add(tr); newtracks.append(tr)
    return newtracks

def find_seg(b, net_name, layer_name, near_xy, tol=0.4):
    L=b.GetLayerID(layer_name); nx,ny=mm(near_xy[0]),mm(near_xy[1])
    best=None
    for t in b.GetTracks():
        if t.Type()==pcbnew.PCB_VIA_T: continue
        if t.GetNetname()!=net_name or t.GetLayer()!=L: continue
        sh=t.GetEffectiveShape(L)
        if sh.Collide(VECTOR2I(nx,ny),mm(tol)):
            # distance from near point to seg midpoint (pick closest)
            m=((t.GetStart().x+t.GetEnd().x)/2,(t.GetStart().y+t.GetEnd().y)/2)
            d=math.hypot(m[0]-nx,m[1]-ny)
            if best is None or d<best[0]: best=(d,t)
    return best[1] if best else None
