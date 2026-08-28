"""Tie the front ground pour to the back ground plane with stitching vias.

    /Applications/KiCad/KiCad.app/Contents/Frameworks/Python.framework/Versions/3.9/bin/python3 \
        stitch.py

Run after every route.py pass. Freerouting does not stitch, so a fresh route leaves
the front pour cut into fragments by the traces crossing it. Each fragment still
touches GND pads, but on a two layer board the return current for anything fast has
no way down to the back plane except through whatever routing vias happen to be
nearby.

Two passes, in this order, because they do different jobs:

  Targeted. One via per front-pour fragment, at the point inside that fragment with
  the most room around it. This is the pass that does the connectivity work -- a
  blanket grid mostly drops vias into fragments that were already stitched and skips
  the small ones that actually needed it. Measured on rev A: no stitching left 14 of
  the 17 fragments unjoined, a blanket 2mm grid of 82 vias left 9, and 8 targeted
  vias left 8. The ones that remain are slivers too small to take a 0.6mm via with
  clearance; they are tied to GND through their own pads and are harmless.

  Grid. A 5mm lattice over what is left, for return-path continuity under the
  2.4GHz module rather than for connectivity. Cheap -- 0.3mm is the same drill tool
  the board already uses, and PCBWay does not charge by the hole.

Vias accumulate if you run this twice, and pcbnew's Remove() on a via does not
survive a Save, so there is no clean undo. Re-stitch by re-routing from scratch:
strip every segment, via and arc from the .kicad_pcb, run route.py, then run this.
"""
import argparse
import math
import os

import pcbnew

HERE = os.path.dirname(os.path.abspath(__file__))
BOARD = os.path.join(HERE, 'airwriter.kicad_pcb')

IU = 1000000
mm = lambda v: int(round(v * IU))
F, BK = pcbnew.F_Cu, pcbnew.B_Cu

VIA_DIAMETER = mm(0.6)
VIA_DRILL = mm(0.3)          # PCBWay's standard floor; do not go below
CLEARANCE = mm(0.35)
GRID_STEP = mm(5.0)
GRID_SPACING = mm(4.5)
NEED = VIA_DIAMETER / 2 + CLEARANCE


def gnd_zones(board):
    return [z for z in board.Zones() if z.GetNetname() == '/GND']


def filled(board, layer):
    """Union of the GND fill on one layer."""
    out = pcbnew.SHAPE_POLY_SET()
    for z in gnd_zones(board):
        if not z.IsOnLayer(layer):
            continue
        polys = z.GetFilledPolysList(layer)
        for i in range(polys.OutlineCount()):
            out.AddOutline(polys.Outline(i))
    return out


def fragments(board):
    """Each disjoint piece of the front GND fill, as its own polygon set."""
    out = []
    for z in gnd_zones(board):
        if not z.IsOnLayer(F):
            continue
        polys = z.GetFilledPolysList(F)
        for i in range(polys.OutlineCount()):
            sp = pcbnew.SHAPE_POLY_SET()
            sp.AddOutline(polys.Outline(i))
            out.append(sp)
    return out


def obstacles(board):
    """Every pad, track and via, as circles to keep clear of."""
    out = []
    for fp in board.GetFootprints():
        for pad in fp.Pads():
            c = pad.GetCenter()
            r = max(pad.GetSize().x, pad.GetSize().y) / 2 + pad.GetDrillSize().x / 2
            out.append((c.x, c.y, r))
    for t in board.GetTracks():
        if isinstance(t, pcbnew.PCB_VIA):
            c = t.GetPosition()
            out.append((c.x, c.y, t.GetWidth() / 2))
        else:
            s, e = t.GetStart(), t.GetEnd()
            half = t.GetWidth() / 2
            n = max(1, int(math.hypot(e.x - s.x, e.y - s.y) / mm(0.4)))
            for i in range(n + 1):
                out.append((s.x + (e.x - s.x) * i / n, s.y + (e.y - s.y) * i / n, half))
    return out


def place(board, points):
    net = board.FindNet('/GND')
    for x, y in points:
        v = pcbnew.PCB_VIA(board)
        v.SetPosition(pcbnew.VECTOR2I(int(x), int(y)))
        v.SetWidth(VIA_DIAMETER)
        v.SetDrill(VIA_DRILL)
        v.SetLayerPair(F, BK)
        v.SetNet(net)
        board.Add(v)


def main():
    argparse.ArgumentParser().parse_args()   # so --help works, no options of its own

    board = pcbnew.LoadBoard(BOARD)
    back = filled(board, BK)
    obs = obstacles(board)

    def room(sp, x, y):
        """Clearance to the nearest obstacle, or -1 if the via would not fit here."""
        v = pcbnew.VECTOR2I(int(x), int(y))
        if not sp.Contains(v) or not back.Contains(v):
            return -1
        for k in range(16):
            a = k * math.pi / 8
            p = pcbnew.VECTOR2I(int(x + NEED * math.cos(a)), int(y + NEED * math.sin(a)))
            if not sp.Contains(p) or not back.Contains(p):
                return -1
        return min(math.hypot(x - ox, y - oy) - r for ox, oy, r in obs)

    # pass 1 -- one via per fragment, at its roomiest interior point
    targeted = []
    pieces = fragments(board)
    for sp in pieces:
        bb = sp.BBox()
        best, best_room = None, NEED
        x = bb.GetLeft()
        while x <= bb.GetRight():
            y = bb.GetTop()
            while y <= bb.GetBottom():
                d = room(sp, x, y)
                if d > best_room:
                    best_room, best = d, (x, y)
                y += mm(0.5)
            x += mm(0.5)
        if best:
            targeted.append(best)
    print('front pour fragments: %d, targeted vias: %d' % (len(pieces), len(targeted)))

    # pass 2 -- coarse lattice over the rest, for return-path continuity.
    # Both passes read the same pre-existing fill, so they are computed before any
    # via is added; placing them afterwards keeps pcbnew out of freed fill memory.
    # Box covering the ESP32-S3-WROOM module footprint and the ground it returns
    # through. Round 7 lifted the module 4.556mm north for the corner mount, so
    # its centre is (149.25, 139.794) and its courtyard y 130.299..149.289, so
    # the box's north edge follows it up. Its south edge stays at 163: the
    # module moved, the connector row it returns through did not. The antenna
    # rule area (now x156-162, y127.6-154.544, notched for H4) bars vias itself,
    # so the box may safely overlap it.
    front = filled(board, F)
    obs = obstacles(board)
    grid = []
    x = mm(134)
    while x <= mm(161):
        y = mm(106)
        while y <= mm(163):
            v = pcbnew.VECTOR2I(int(x), int(y))
            fits = front.Contains(v) and back.Contains(v)
            if fits:
                for k in range(8):
                    a = k * math.pi / 4
                    p = pcbnew.VECTOR2I(int(x + NEED * math.cos(a)), int(y + NEED * math.sin(a)))
                    if not front.Contains(p) or not back.Contains(p):
                        fits = False
                        break
            if fits and all(math.hypot(x - ox, y - oy) >= r + NEED for ox, oy, r in obs) \
                    and all(math.hypot(x - ax, y - ay) > GRID_SPACING
                            for ax, ay in targeted + grid):
                grid.append((x, y))
            y += GRID_STEP
        x += GRID_STEP
    print('grid vias:', len(grid))

    place(board, targeted)
    place(board, grid)
    pcbnew.ZONE_FILLER(board).Fill(board.Zones())
    board.Save(BOARD)
    print('total stitching vias:', len(targeted) + len(grid))


main()
