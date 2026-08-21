#!/usr/bin/env python3
"""Iteratively remove dangling track tails on /VBUS: a segment whose endpoint is
shared by no other track/via/pad of the same net (a free end) is an orphan tail."""
import pcbnew
b=pcbnew.LoadBoard('airwriter.kicad_pcb')
U=lambda v:round(v/1e6,3)
Q=lambda p:(round(p.x/1000)*1000, round(p.y/1000)*1000)  # 1um quantize

def endpoints_of_net(nc, exclude=None):
    """count of copper items incident at each quantized point for net nc."""
    from collections import Counter
    c=Counter()
    for t in b.GetTracks():
        if t.GetNetCode()!=nc: continue
        if t is exclude: continue
        if t.Type()==pcbnew.PCB_VIA_T:
            c[Q(t.GetPosition())]+=1
        else:
            c[Q(t.GetStart())]+=1; c[Q(t.GetEnd())]+=1
    for f in b.GetFootprints():
        for pad in f.Pads():
            if pad.GetNetCode()==nc:
                c[Q(pad.GetPosition())]+=1
    return c

total=0
for netname in ['/VBUS']:
    nc=b.FindNet(netname).GetNetCode()
    while True:
        # count incidences without removing yet
        from collections import Counter
        inc=Counter()
        segs=[]
        for t in b.GetTracks():
            if t.GetNetCode()!=nc: continue
            if t.Type()==pcbnew.PCB_VIA_T:
                inc[Q(t.GetPosition())]+=1
            else:
                segs.append(t); inc[Q(t.GetStart())]+=1; inc[Q(t.GetEnd())]+=1
        for f in b.GetFootprints():
            for pad in f.Pads():
                if pad.GetNetCode()==nc: inc[Q(pad.GetPosition())]+=1
        # a segment endpoint incident-count==1 means free end (only this seg touches it)
        todel=[t for t in segs if inc[Q(t.GetStart())]==1 or inc[Q(t.GetEnd())]==1]
        if not todel: break
        for t in todel:
            b.Remove(t); total+=1
            print(f"  removed {netname} dangling ({U(t.GetStart().x)},{U(t.GetStart().y)})-({U(t.GetEnd().x)},{U(t.GetEnd().y)})")
print("total dangling removed:",total)
pcbnew.ZONE_FILLER(b).Fill(b.Zones())
b.Save('airwriter.kicad_pcb'); print("saved")
