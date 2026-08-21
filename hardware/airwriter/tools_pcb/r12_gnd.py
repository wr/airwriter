#!/usr/bin/env python3
"""GND connectivity analyzer: connected components of GND copper (zone-fill
islands + pads + tracks + vias), handling thermal spokes via the filled polygon.
Reports the main ground plane, stranded islands, and stranded GND pads."""
import pcbnew
from pcbnew import VECTOR2I
MM=1e6; U=lambda v:round(v/MM,3); mm=lambda v:int(round(v*MM))

class DSU:
    def __init__(s,n): s.p=list(range(n))
    def find(s,x):
        while s.p[x]!=x: s.p[x]=s.p[s.p[x]]; x=s.p[x]
        return x
    def union(s,a,b): s.p[s.find(a)]=s.find(b)

def load(path='airwriter.kicad_pcb'):
    return pcbnew.LoadBoard(path)

def analyze(b, verbose=True):
    FCu=b.GetLayerID('F.Cu'); BCu=b.GetLayerID('B.Cu')
    gnc=b.FindNet('/GND').GetNetCode()
    # island nodes: (layer, SHAPE_POLY_SET(one outline), bbox, area)
    islands=[]
    for z in b.Zones():
        if z.GetNetname()!='/GND': continue
        for L in (FCu,BCu):
            if not z.IsOnLayer(L): continue
            fp=z.GetFilledPolysList(L)
            for i in range(fp.OutlineCount()):
                ps=pcbnew.SHAPE_POLY_SET(); ps.AddOutline(fp.Outline(i))
                bb=ps.BBox(); area=ps.Area()/1e12
                islands.append([L,ps,(bb.GetLeft(),bb.GetTop(),bb.GetRight(),bb.GetBottom()),area])
    nI=len(islands)
    dsu=DSU(nI)
    CLR=int(0.002*MM)  # 2um: touching/abutting counts as connected

    def overlap_islands(shape, bb, layers):
        hit=[]
        L,T,R,B=bb
        for idx,(il,ips,ib,ia) in enumerate(islands):
            if il not in layers: continue
            if R<ib[0]-CLR or L>ib[2]+CLR or B<ib[1]-CLR or T>ib[3]+CLR: continue
            if ips.Collide(shape,CLR): hit.append(idx)
        return hit

    def sbbox(shape):
        bb=shape.BBox(); return (bb.GetLeft(),bb.GetTop(),bb.GetRight(),bb.GetBottom())

    # tracks + vias
    for t in b.GetTracks():
        if t.GetNetCode()!=gnc: continue
        if t.Type()==pcbnew.PCB_VIA_T:
            sh=t.GetEffectiveShape(FCu); bb=sbbox(sh)
            fh=overlap_islands(sh,bb,{FCu}); bh=overlap_islands(sh,bb,{BCu})
            allh=fh+bh
            for k in range(1,len(allh)): dsu.union(allh[0],allh[k])
        else:
            L=t.GetLayer(); sh=t.GetEffectiveShape(L); bb=sbbox(sh)
            hit=overlap_islands(sh,bb,{L})
            for k in range(1,len(hit)): dsu.union(hit[0],hit[k])

    # pads: record (ref/pad, component-islands)
    padinfo=[]  # (ref,padnum,pos, [island idxs])
    for f in b.GetFootprints():
        for p in f.Pads():
            if p.GetNetCode()!=gnc: continue
            layers=set()
            if p.IsOnLayer(FCu): layers.add(FCu)
            if p.IsOnLayer(BCu): layers.add(BCu)
            if not layers: continue
            sh=p.GetEffectiveShape(FCu if FCu in layers else BCu); bb=sbbox(sh)
            hit=overlap_islands(sh,bb,layers)
            for k in range(1,len(hit)): dsu.union(hit[0],hit[k])
            padinfo.append((f.GetReference(),p.GetNumber(),p.GetPosition(),hit))

    # components
    from collections import defaultdict
    comp=defaultdict(list)
    for idx in range(nI): comp[dsu.find(idx)].append(idx)
    # main = component containing the largest-area island
    biggest=max(range(nI), key=lambda i:islands[i][3])
    main_root=dsu.find(biggest)
    ncomp=len(comp)
    # stranded pads
    stranded=[]; floating=[]
    for ref,num,pos,hit in padinfo:
        if not hit: floating.append((ref,num,U(pos.x),U(pos.y))); continue
        roots=set(dsu.find(h) for h in hit)
        if main_root not in roots:
            stranded.append((ref,num,U(pos.x),U(pos.y)))
    # islands not in main
    stranded_islands=[i for i in range(nI) if dsu.find(i)!=main_root]
    if verbose:
        print(f"GND islands: {nI} (F+B), components: {ncomp}, main-plane islands: {len(comp[main_root])}")
        print(f"stranded islands (not on main): {len(stranded_islands)}")
        print(f"total GND pads: {len(padinfo)}, STRANDED pads: {len(stranded)}, floating(no island): {len(floating)}")
        for s in sorted(stranded): print("  STRANDED", s)
        for s in floating: print("  FLOATING", s)
    return dict(nI=nI, ncomp=ncomp, main_root=main_root, dsu=dsu, islands=islands,
               comp=comp, stranded=stranded, floating=floating,
               stranded_islands=stranded_islands, padinfo=padinfo, FCu=FCu, BCu=BCu, gnc=gnc)

if __name__=='__main__':
    analyze(load())
