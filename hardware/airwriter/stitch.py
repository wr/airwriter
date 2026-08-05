"""Add GND stitching vias so the F.Cu pour fragments all reach the B.Cu plane.

    /Applications/KiCad/KiCad.app/Contents/Frameworks/Python.framework/Versions/3.9/bin/python3 \
        stitch.py

Run this after every route.py pass -- Freerouting does not stitch, so a fresh route
leaves the front pour cut into islands that touch GND pads but not each other. The
pass walks a 2mm grid, keeps a point only when the whole via footprint sits inside
both the F.Cu and B.Cu GND fills and clears every pad, track and via by 0.35mm, and
thins the result to one via per 3mm. Refills the zones and saves.
"""
import pcbnew, math

import os
HERE = os.path.dirname(os.path.abspath(__file__))
BOARD = os.path.join(HERE, 'airwriter.kicad_pcb')
B = pcbnew.LoadBoard(BOARD)
IU = pcbnew.PCB_IU_PER_MM if hasattr(pcbnew, 'PCB_IU_PER_MM') else 1000000
mm = lambda v: int(round(v * IU))

F, BK = pcbnew.F_Cu, pcbnew.B_Cu
gnd = [z for z in B.Zones() if z.GetNetname() == '/GND']
fz = [z for z in gnd if z.IsOnLayer(F)]
bz = [z for z in gnd if z.IsOnLayer(BK)]

def fill(zs, layer):
    out = pcbnew.SHAPE_POLY_SET()
    for z in zs:
        p = z.GetFilledPolysList(layer)
        for i in range(p.OutlineCount()):
            out.AddOutline(p.Outline(i))
    return out

ffill, bfill = fill(fz, F), fill(bz, BK)

# obstacles: every pad, track, via, plus board edge
obs = []
for fp in B.GetFootprints():
    for p in fp.Pads():
        c = p.GetCenter(); r = max(p.GetSize().x, p.GetSize().y)/2 + p.GetDrillSize().x/2
        obs.append((c.x, c.y, r))
for t in B.GetTracks():
    if isinstance(t, pcbnew.PCB_VIA):
        c = t.GetPosition(); obs.append((c.x, c.y, t.GetWidth()/2))
    else:
        s, e = t.GetStart(), t.GetEnd(); w = t.GetWidth()/2
        n = max(1, int(math.hypot(e.x-s.x, e.y-s.y) / mm(0.4)))
        for i in range(n+1):
            obs.append((s.x + (e.x-s.x)*i/n, s.y + (e.y-s.y)*i/n, w))

VIA_D, VIA_DRILL, CLR = mm(0.6), mm(0.3), mm(0.35)
NEED = VIA_D/2 + CLR

def ok(x, y):
    if not ffill.Contains(pcbnew.VECTOR2I(x, y)): return False
    if not bfill.Contains(pcbnew.VECTOR2I(x, y)): return False
    for ox, oy, r in obs:
        if math.hypot(x-ox, y-oy) < r + NEED: return False
    # the whole via footprint, not just its centre, must sit inside both fills
    for k in range(8):
        a = k * math.pi / 4
        px = int(x + NEED * math.cos(a)); py = int(y + NEED * math.sin(a))
        if not ffill.Contains(pcbnew.VECTOR2I(px, py)): return False
        if not bfill.Contains(pcbnew.VECTOR2I(px, py)): return False
    return True

added = []
step = mm(2.0)
x0, y0, x1, y1 = mm(101), mm(61), mm(151), mm(125)
x = x0
while x <= x1:
    y = y0
    while y <= y1:
        if ok(x, y):
            if all(math.hypot(x-ax, y-ay) > mm(3.0) for ax, ay in added):
                added.append((x, y))
        y += step
    x += step

net = B.FindNet('/GND')
for x, y in added:
    v = pcbnew.PCB_VIA(B)
    v.SetPosition(pcbnew.VECTOR2I(x, y))
    v.SetWidth(VIA_D); v.SetDrill(VIA_DRILL)
    v.SetLayerPair(F, BK); v.SetNet(net)
    B.Add(v)

pcbnew.ZONE_FILLER(B).Fill(B.Zones())
B.Save(BOARD)
print('stitching vias added:', len(added))
