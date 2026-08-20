"""Dump board geometry to JSON for the placement solver.

Courtyard and pad extents are probed EMPIRICALLY at each rotation rather than
derived from a rotation matrix -- KiCad's Y-down frame has burned this project
before, and several footprints here (S1, SW3, SW1, U1) have origins that are not
at their body centre.
"""
import sys, json
sys.path.insert(0, "/Users/wells/Projects/airwriter/hardware/airwriter/tools_pcb")
import pcbnew, r6_lib as L

OUT = "/Users/wells/Projects/airwriter/hardware/airwriter/tools_pcb/r6_geom.json"

MOVABLE = ["C1", "C3", "C7", "C10", "C11", "C12", "C13", "C14", "C16", "C17",
           "C18", "C22", "R9", "R10", "R11", "R12", "R13", "R14", "R15", "R16"]

b = L.load()
fps = {f.GetReference(): f for f in b.Footprints()}

def probe(fp):
    """Per-rotation courtyard rects and pad bbox, as offsets from the origin."""
    p0 = fp.GetPosition(); r0 = fp.GetOrientationDegrees()
    ox, oy = L.mm(p0.x), L.mm(p0.y)
    out = {}
    for rot in (0, 90, 180, 270):
        fp.SetOrientationDegrees(rot)
        rects = L.courtyard_rects(fp) or [L.cy_bbox(fp)]
        pb = L.pads_bbox(fp)
        out[str(rot)] = dict(
            cy=[[r[0] - ox, r[1] - oy, r[2] - ox, r[3] - oy] for r in rects],
            pads=[pb[0] - ox, pb[1] - oy, pb[2] - ox, pb[3] - oy] if pb else None)
    fp.SetOrientationDegrees(r0); fp.SetPosition(p0)
    return out

data = dict(movable=MOVABLE, parts={}, nets={})
for ref, fp in fps.items():
    p = fp.GetPosition()
    d = dict(x=L.mm(p.x), y=L.mm(p.y), rot=fp.GetOrientationDegrees(),
             movable=ref in MOVABLE, rots=probe(fp), pads={})
    for pad in fp.Pads():
        n = pad.GetNetname()
        if not n: continue
        pp = pad.GetPosition()
        d["pads"].setdefault(n, []).append([L.mm(pp.x), L.mm(pp.y)])
        data["nets"].setdefault(n, []).append(ref)
    data["parts"][ref] = d

# antenna keepout polygon (absolute)
ko = None
for z in fps["U1"].Zones():
    o = z.Outline().Outline(0)
    ko = [[L.mm(o.CPoint(i).x), L.mm(o.CPoint(i).y)] for i in range(o.PointCount())]
data["keepout"] = ko
data["board"] = dict(x0=100.0, y0=60.0, x1=162.0, y1=164.27, radius=2.0,
                     band_y0=127.6, grid=dict(x0=102.8, y0=62.8, x1=159.2, y1=127.6))
json.dump(data, open(OUT, "w"), indent=0)
print("wrote", OUT, "parts:", len(data["parts"]), "keepout pts:", len(ko))
