#!/usr/bin/env python3
"""Rev 2026.11: draw the new traces, one chosen connection at a time.

    <KiCad python> tools_pcb/r13_route.py [--only NET ...]

Not an autorouter over the board. The long runs are drawn point by point
(HAND), along paths chosen so each is a few straight legs with as few vias as
the board allows. The short stubs round a part's own pins are each listed
with their width, the layers they may use and how far from the straight line
between their ends they may wander; within that corridor a small grid search
(0.1 mm, two layers, 45-degree moves, a cost per bend, per via and per
millimetre on the back so the ground plane stays whole) finds the exact path
clear of every other net's copper, holes, the board edge and the antenna
keepout. Wells reviews the result in KiCad.

Runs after r13_build.sh, with KiCad closed. Each connection starts from the
named pad and ends on any copper of its net it isn't already joined to.
"""
import heapq
import math
import os
import sys
from collections import namedtuple

import pcbnew

HERE = os.path.dirname(os.path.abspath(__file__))
BOARD = os.path.join(HERE, '..', 'airwriter.kicad_pcb')

RES = 0.1                    # grid, mm
CLRS = (0.20, 0.15)          # copper to other nets: try 0.2 first, then the rule's 0.15
EDGE = 0.35                  # copper to the board edge (rule 0.25)
HOLE_CLR = 0.25              # copper to a hole (rule 0.2)
VIA_D, VIA_DRILL = 0.6, 0.3
VIA_COST = 4.0               # mm of track a via is worth avoiding
BEND = 0.6                   # mm per 45 degrees turned: straight runs, few jogs
HINT_W = 0.35                # a hint pulls the path within this of its line
HINT_COST = 0.4              # cost per mm along a hint, against 1.0 elsewhere
BACK = 1.4                   # cost per mm on B.Cu, where the ground plane lives
F, B = pcbnew.F_Cu, pcbnew.B_Cu
Pt = namedtuple('Pt', 'x y')

# Drawn by hand: the long runs, where the path is the design (U3's J4 lines
# down beside U3 and under U8; the drivers' inputs and receiver 1's output on
# the back to the module's west pins the DIP switches freed, IO47, IO21 and
# IO13, which leaves IO35 and IO37 to an octal-PSRAM module), and the ground
# vias the new parts' pads need.
# (net, layer or 'via', points, width)
HAND = [
    ('/J4_HSKO', F, [(120.05, 135.16), (122.95, 135.16), (122.95, 142.2), (124.6, 143.85), (124.6, 151.25)], 0.2),
    ('/J4_HSKO', 'via', [(124.6, 151.25)], None),
    ('/J4_HSKO', B, [(124.6, 151.25), (128.6, 151.25), (132.7, 155.35), (132.7, 157.4), (133.8, 158.5)], 0.2),
    ('/J4_HSKI', F, [(120.05, 136.43), (122.35, 136.43), (122.35, 142.6), (123.95, 144.2), (123.95, 152.4)], 0.2),
    ('/J4_HSKI', 'via', [(123.95, 152.4)], None),
    ('/J4_HSKI', B, [(123.95, 152.4), (124.6, 153.05), (128.2, 153.05), (130.35, 155.2), (130.35, 157.67),
                     (131.2, 158.52)], 0.2),
    ('/J4_HSKI_RX', F, [(120.05, 137.7), (119.0, 137.7), (118.0, 138.7)], 0.2),
    ('/J4_HSKI_RX', 'via', [(118.0, 138.7)], None),
    ('/J4_HSKI_RX', B, [(118.0, 138.7), (122.75, 143.45), (135.0, 143.45)], 0.2),
    ('/J4_HSKI_RX', 'via', [(135.0, 143.45)], None),
    ('/J4_HSKI_RX', F, [(135.0, 143.45), (135.48, 142.97), (136.75, 142.97)], 0.2),          # IO21
    # U3.10 from IO47 along rev A's back run below J4_HSKI_RX's; R24 below U3
    ('/HSKI_DRV', F, [(120.05, 140.24), (118.6, 140.24), (118.6, 142.0), (118.94, 142.34), (119.75, 142.34)], 0.2),
    ('/HSKI_DRV', 'via', [(119.75, 142.34)], None),
    ('/HSKI_DRV', F, [(119.75, 142.34), (120.2, 142.79)], 0.2),
    ('/HSKI_DRV', B, [(119.75, 142.34), (122.12, 144.71), (135.0, 144.71)], 0.2),
    ('/HSKI_DRV', 'via', [(135.0, 144.71)], None),
    ('/HSKI_DRV', F, [(135.0, 144.71), (135.47, 144.24), (136.75, 144.24)], 0.2),          # IO47
    # U3.11 from IO13: one straight run on the back between U7's two rows of pins,
    # up just west of the module's pin (BTN_TEST passes on the front)
    ('/J4_HSKO_DRV', F, [(120.05, 138.97), (121.4, 138.97)], 0.2),
    ('/J4_HSKO_DRV', 'via', [(121.4, 138.97)], None),
    ('/J4_HSKO_DRV', B, [(121.4, 138.97), (134.65, 138.97), (135.65, 139.97)], 0.2),
    ('/J4_HSKO_DRV', 'via', [(135.65, 139.97)], None),
    ('/J4_HSKO_DRV', F, [(135.65, 139.97), (136.11, 140.43), (136.75, 140.43)], 0.2),
    ('/UART_TX', B, [(136.4, 147.537), (136.4, 147.75)], 0.2),          # R25 off UART_TX's back run
    ('/UART_TX', 'via', [(136.4, 147.75)], None),
    ('/UART_TX', F, [(136.4, 147.75), (136.0, 148.15), (136.0, 150.175)], 0.2),
    ('/GND', 'via', [(118.05, 143.25)], None),                          # R24
    ('/GND', F, [(118.05, 143.25), (118.9, 143.25)], 0.2),
    ('/GND', 'via', [(140.62, 128.25)], None),                          # R32
    ('/GND', F, [(140.62, 128.25), (140.62, 129.05)], 0.2),
    ('/GND', 'via', [(143.68, 128.25)], None),                          # R33
    ('/GND', F, [(143.68, 128.25), (143.68, 129.05)], 0.2),
    ('/GND', 'via', [(150.0, 140.6)], None),                            # TP9's patch of the back pour
    ('/GND', B, [(150.0, 140.6), (150.0, 142.1)], 0.2),
]

# Found by the grid search: the short stubs round their own pins.
# net, start pad, width, layers, corridor margin (mm)
CONNECTIONS = [
    ('/BOARD_REV', ('R32', '1'), 0.2, (F, B), 3.0),
    ('/BOARD_LITE', ('R33', '1'), 0.2, (F, B), 3.0),
    ('/LT_DE', ('R22', '1'), 0.2, (F, B), 2.5),
    ('/LED_DATA', ('R23', '1'), 0.2, (F, B), 2.0),
    ('/PR_TXD_N', ('R30', '1'), 0.2, (F, B), 3.0),
    ('/PR_TXD_N_IN', ('R30', '2'), 0.2, (F, B), 3.0),
    ('/PR_TXD_P', ('R29', '1'), 0.2, (F, B), 3.0),
    ('/PR_TXD_P_IN', ('R29', '2'), 0.2, (F, B), 3.0),
    ('/+3V3', ('D3', '2'), 0.3, (F, B), 3.0),
    ('Net-(D3-K)', ('D3', '1'), 0.2, (F,), 2.0),
    ('/+3V3', ('R25', '1'), 0.3, (F, B), 4.0),
    ('/+3V3', ('R21', '1'), 0.3, (F, B), 4.0),
]


def nm(v):
    return int(round(v * 1e6))


def mm(v):
    return v / 1e6


def seg_dist(px, py, ax, ay, bx, by):
    dx, dy = bx - ax, by - ay
    L2 = dx * dx + dy * dy
    u = 0.0 if L2 == 0 else max(0.0, min(1.0, ((px - ax) * dx + (py - ay) * dy) / L2))
    return math.hypot(ax + u * dx - px, ay + u * dy - py)


class Grid:
    """Cells over a corridor: per layer, blocked for this net (0/1), plus the
    cells inside the start pad and inside the copper it should reach."""

    def __init__(self, board, net, start, width, layers, margin, target_pts):
        self.board, self.net, self.hw, self.layers = board, net, width / 2, layers
        xs = [start.x] + [p[0] for p in target_pts]
        ys = [start.y] + [p[1] for p in target_pts]
        self.x0, self.y0 = min(xs) - margin, min(ys) - margin
        self.nx = int((max(xs) + margin - self.x0) / RES) + 1
        self.ny = int((max(ys) + margin - self.y0) / RES) + 1
        n = self.nx * self.ny
        self.blocked = {l: bytearray(n) for l in (F, B)}
        self.via_blocked = bytearray(n)

    def xy(self, i, j):
        return self.x0 + i * RES, self.y0 + j * RES

    def cells_near(self, x0, y0, x1, y1):
        i0 = max(0, int((x0 - self.x0) / RES)); i1 = min(self.nx - 1, int((x1 - self.x0) / RES) + 1)
        j0 = max(0, int((y0 - self.y0) / RES)); j1 = min(self.ny - 1, int((y1 - self.y0) / RES) + 1)
        for j in range(j0, j1 + 1):
            for i in range(i0, i1 + 1):
                yield i, j

    def block(self, layer, test, bbox, grow):
        x0, y0, x1, y1 = bbox
        arr = self.blocked[layer] if layer is not None else None
        for i, j in self.cells_near(x0 - grow, y0 - grow, x1 + grow, y1 + grow):
            x, y = self.xy(i, j)
            if test(x, y):
                k = j * self.nx + i
                if arr is None:
                    self.blocked[F][k] = self.blocked[B][k] = 1
                else:
                    arr[k] = 1

    def block_via(self, test, bbox, grow):
        x0, y0, x1, y1 = bbox
        for i, j in self.cells_near(x0 - grow, y0 - grow, x1 + grow, y1 + grow):
            x, y = self.xy(i, j)
            if test(x, y):
                self.via_blocked[j * self.nx + i] = 1


def bbox_mm(item):
    bb = item.GetBoundingBox()
    return mm(bb.GetLeft()), mm(bb.GetTop()), mm(bb.GetRight()), mm(bb.GetBottom())


def build(board, net, start_pad, width, layers, margin, CLR=0.2, probe=False):
    """probe: build the grid round the start pad even with nothing to reach
    (for checking how far a pin's copper can get)."""
    netcode = board.FindNet(net).GetNetCode()
    sp = start_pad.GetCenter()
    start = Pt(mm(sp.x), mm(sp.y))
    # what the route should reach: this net's copper not already joined to the
    # start pad (its tracks, vias and other pads)
    joined = joined_items(board, start_pad)
    targets = []
    for fp in board.GetFootprints():
        for p in fp.Pads():
            if p.GetNetCode() == netcode and p.m_Uuid.AsString() not in joined:
                targets.append(p)
    for t in board.GetTracks():
        if t.GetNetCode() == netcode and t.m_Uuid.AsString() not in joined:
            targets.append(t)
    if not targets and not probe:
        return None, None
    tpts = [(start.x, start.y)] if probe else []                                  # each target's point nearest the start
    for t in targets:
        if isinstance(t, pcbnew.PCB_TRACK) and not isinstance(t, pcbnew.PCB_VIA):
            s_, e_ = t.GetStart(), t.GetEnd()
            ax, ay, bx, by = mm(s_.x), mm(s_.y), mm(e_.x), mm(e_.y)
            dx, dy = bx - ax, by - ay
            L2 = dx * dx + dy * dy
            u = 0.0 if L2 == 0 else max(0.0, min(1.0, ((start.x - ax) * dx + (start.y - ay) * dy) / L2))
            tpts.append((ax + u * dx, ay + u * dy))
        else:
            c = t.GetPosition()
            tpts.append((mm(c.x), mm(c.y)))
    near = min(tpts, key=lambda p: math.hypot(p[0] - start.x, p[1] - start.y))
    g = Grid(board, net, start, width, layers, margin, [near])
    gx0, gy0 = g.x0 - 1, g.y0 - 1
    gx1, gy1 = g.x0 + g.nx * RES + 1, g.y0 + g.ny * RES + 1

    def inside(bb):
        return bb[2] >= gx0 and bb[0] <= gx1 and bb[3] >= gy0 and bb[1] <= gy1

    hw = width / 2
    grow = hw + CLR
    vgrow = VIA_D / 2 + CLR
    # other nets' pads, tracks and vias
    for fp in board.GetFootprints():
        for p in fp.Pads():
            bb = bbox_mm(p)
            if not inside(bb):
                continue
            if p.GetDrillSize().x > 0:          # any hole: keep copper and vias off it
                c = p.GetPosition(); hx, hy = mm(c.x), mm(c.y)
                hr = mm(max(p.GetDrillSize().x, p.GetDrillSize().y)) / 2
                if p.GetNetCode() != netcode or p.GetAttribute() == pcbnew.PAD_ATTRIB_NPTH:
                    g.block(None, lambda x, y, hx=hx, hy=hy, r=hr + HOLE_CLR + hw:
                            math.hypot(x - hx, y - hy) < r, bb, grow)
                g.block_via(lambda x, y, hx=hx, hy=hy, r=hr + 0.25 + VIA_DRILL / 2:
                            math.hypot(x - hx, y - hy) < r, bb, 1.0)
            if p.GetNetCode() == netcode and p.GetAttribute() != pcbnew.PAD_ATTRIB_NPTH:
                for layer in (F, B):           # own pads: copper may run in, vias may not
                    if p.IsOnLayer(layer):
                        shape = p.GetEffectiveShape(layer)
                        g.block_via(lambda x, y, s=shape: s.Collide(pcbnew.VECTOR2I(nm(x), nm(y)), nm(VIA_D / 2)),
                                    bb, VIA_D / 2)
                continue
            for layer in (F, B):
                if not p.IsOnLayer(layer):
                    continue
                shape = p.GetEffectiveShape(layer)
                g.block(layer, lambda x, y, s=shape: s.Collide(pcbnew.VECTOR2I(nm(x), nm(y)), nm(grow)),
                        bb, grow)
                g.block_via(lambda x, y, s=shape: s.Collide(pcbnew.VECTOR2I(nm(x), nm(y)), nm(vgrow)),
                            bb, vgrow)
    for t in board.GetTracks():
        bb = bbox_mm(t)
        if not inside(bb) or t.GetNetCode() == netcode:
            continue
        if isinstance(t, pcbnew.PCB_VIA):
            c = t.GetPosition(); vx, vy = mm(c.x), mm(c.y); vr = mm(t.GetWidth(F)) / 2
            g.block(None, lambda x, y, vx=vx, vy=vy, r=vr + grow: math.hypot(x - vx, y - vy) < r, bb, grow)
            g.block_via(lambda x, y, vx=vx, vy=vy, r=max(vr + vgrow, mm(t.GetDrillValue()) / 2 + 0.25 + VIA_DRILL / 2):
                        math.hypot(x - vx, y - vy) < r, bb, vgrow)
        else:
            s, e = t.GetStart(), t.GetEnd()
            ax, ay, bx, by = mm(s.x), mm(s.y), mm(e.x), mm(e.y)
            r = mm(t.GetWidth()) / 2
            g.block(t.GetLayer(), lambda x, y, a=(ax, ay, bx, by), r=r + grow: seg_dist(x, y, *a) < r,
                    bb, grow)
            g.block_via(lambda x, y, a=(ax, ay, bx, by), r=r + vgrow, l=t.GetLayer():
                        seg_dist(x, y, *a) < r, bb, vgrow)
    # keepouts inside footprints (the WROOM antenna's): no copper, no vias
    for fp in board.GetFootprints():
        for z in fp.Zones():
            if z.GetIsRuleArea() and (z.GetDoNotAllowTracks() or z.GetDoNotAllowVias()):
                bb = bbox_mm(z)
                if inside(bb):
                    g.block(None, lambda x, y, b=bb: b[0] - grow < x < b[2] + grow and b[1] - grow < y < b[3] + grow, bb, grow)
                    g.block_via(lambda x, y, b=bb: b[0] - vgrow < x < b[2] + vgrow and b[1] - vgrow < y < b[3] + vgrow, bb, vgrow)
    # the board edge
    ebb = board.GetBoardEdgesBoundingBox()
    ex0, ey0, ex1, ey1 = mm(ebb.GetLeft()), mm(ebb.GetTop()), mm(ebb.GetRight()), mm(ebb.GetBottom())
    for j in range(g.ny):
        for i in range(g.nx):
            x, y = g.xy(i, j)
            if not (ex0 + EDGE + hw < x < ex1 - EDGE - hw and ey0 + EDGE + hw < y < ey1 - EDGE - hw):
                g.blocked[F][j * g.nx + i] = g.blocked[B][j * g.nx + i] = 1
                g.via_blocked[j * g.nx + i] = 1
    for l in (F, B):
        if l not in layers:
            g.blocked[l] = bytearray(b'\x01' * (g.nx * g.ny))
    # start and target cells
    src = set()
    for layer in layers:
        if start_pad.IsOnLayer(layer):
            shape = start_pad.GetEffectiveShape(layer)
            for i, j in g.cells_near(*bbox_mm(start_pad)):
                x, y = g.xy(i, j)
                if shape.Collide(pcbnew.VECTOR2I(nm(x), nm(y)), 0):
                    src.add((i, j, layer))
    goal = {}
    for t in targets:
        for layer in layers:
            if not t.IsOnLayer(layer):
                continue
            if isinstance(t, pcbnew.PAD):
                shape = t.GetEffectiveShape(layer)
                hit = lambda x, y, s=shape: s.Collide(pcbnew.VECTOR2I(nm(x), nm(y)), 0)
            elif isinstance(t, pcbnew.PCB_VIA):
                c = t.GetPosition()
                hit = lambda x, y, c=(mm(c.x), mm(c.y)), r=mm(t.GetWidth(F)) / 2 - 0.05: math.hypot(x - c[0], y - c[1]) < r
            else:
                s, e = t.GetStart(), t.GetEnd()
                hit = lambda x, y, a=(mm(s.x), mm(s.y), mm(e.x), mm(e.y)), r=mm(t.GetWidth()) / 2 - 0.02: seg_dist(x, y, *a) < r
            for i, j in g.cells_near(*bbox_mm(t)):
                x, y = g.xy(i, j)
                if hit(x, y):
                    goal[(i, j, layer)] = t
    for (i, j, l) in list(src) + list(goal):
        g.blocked[l][j * g.nx + i] = 0
    return g, (src, goal)


DIRS = [(1, 0), (1, 1), (0, 1), (-1, 1), (-1, 0), (-1, -1), (0, -1), (1, -1)]


def hint_cells(g, hint):
    """Grid cells within HINT_W of a hint polyline [(x, y, layer), ...]; a
    change of layer between two points marks where a via is meant."""
    cells = set()
    for (ax, ay, la), (bx, by, lb) in zip(hint, hint[1:]):
        if la != lb:
            continue
        for i, j in g.cells_near(min(ax, bx) - HINT_W, min(ay, by) - HINT_W,
                                 max(ax, bx) + HINT_W, max(ay, by) + HINT_W):
            x, y = g.xy(i, j)
            if seg_dist(x, y, ax, ay, bx, by) <= HINT_W:
                cells.add((i, j, la))
    return cells


def search(g, src, goal, hint=None):
    gi0 = min(i for i, _, _ in goal); gi1 = max(i for i, _, _ in goal)
    gj0 = min(j for _, j, _ in goal); gj1 = max(j for _, j, _ in goal)

    def h(i, j):              # distance to the targets' box: cheap and never too high
        return math.hypot(max(gi0 - i, 0, i - gi1), max(gj0 - j, 0, j - gj1)) * RES
    openq, best, came = [], {}, {}
    for (i, j, l) in src:
        for d in range(8):
            s = (i, j, l, d)
            best[s] = 0.0
            heapq.heappush(openq, (h(i, j), 0.0, s))
    while openq:
        f, c, s = heapq.heappop(openq)
        if c > best.get(s, 1e18):
            continue
        i, j, l, d = s
        if (i, j, l) in goal:
            path = [s]
            while s in came:
                s = came[s]; path.append(s)
            return path[::-1]
        moves = []
        for nd, (di, dj) in enumerate(DIRS):
            turn = min((nd - d) % 8, (d - nd) % 8)
            if turn > 2:
                continue
            ni, nj = i + di, j + dj
            if not (0 <= ni < g.nx and 0 <= nj < g.ny) or g.blocked[l][nj * g.nx + ni]:
                continue
            if di and dj and (g.blocked[l][j * g.nx + ni] and g.blocked[l][nj * g.nx + i]):
                continue
            step = RES * (1.4142 if di and dj else 1.0) * (BACK if l == B else 1.0)
            if hint and (ni, nj, l) in hint:
                step *= HINT_COST
            moves.append(((ni, nj, l, nd), step + BEND * turn))
        if not g.via_blocked[j * g.nx + i]:
            ol = B if l == F else F
            if not g.blocked[ol][j * g.nx + i] and ol in g.layers:
                moves.append(((i, j, ol, d), VIA_COST))
        for ns, cost in moves:
            nc = c + cost
            if nc < best.get(ns, 1e18):
                best[ns] = nc
                came[ns] = s
                heapq.heappush(openq, (nc + h(ns[0], ns[1]), nc, ns))
    return None


def joined_items(board, pad):
    """UUIDs of the copper already joined to `pad` through tracks and vias
    (geometric: a track end inside a pad or another item's end)."""
    netcode = pad.GetNetCode()
    items = [t for t in board.GetTracks() if t.GetNetCode() == netcode]
    pads = [p for fp in board.GetFootprints() for p in fp.Pads() if p.GetNetCode() == netcode]
    seen = {pad.m_Uuid.AsString()}
    frontier = [pad]
    def touches(a, b):
        for la in (F, B):
            if not (a.IsOnLayer(la) and b.IsOnLayer(la)):
                continue
            sa, sb = a.GetEffectiveShape(la), b.GetEffectiveShape(la)
            if sa.Collide(sb, 0):
                return True
        return False
    while frontier:
        cur = frontier.pop()
        for o in items + pads:
            u = o.m_Uuid.AsString()
            if u not in seen and touches(cur, o):
                seen.add(u); frontier.append(o)
    return seen


def lay(board, g, path, net, width, start_pad, goal):
    """Turn the cell path into straight runs and vias on the board."""
    netinfo = board.FindNet(net)
    pts = []          # (x, y, layer)
    for (i, j, l, d) in path:
        x, y = g.xy(i, j)
        pts.append((round(x, 4), round(y, 4), l))
    # the pad's centre ahead of the first cell, and the target's own point after
    # the last, so a via the search placed stays where the search placed it
    sp = start_pad.GetCenter()
    pts.insert(0, (mm(sp.x), mm(sp.y), pts[0][2]))
    end = goal[(path[-1][0], path[-1][1], path[-1][2])]
    ex, ey, el = pts[-1]
    if isinstance(end, pcbnew.PAD) or isinstance(end, pcbnew.PCB_VIA):
        c = end.GetPosition(); pts.append((mm(c.x), mm(c.y), el))
    else:                                      # onto the track's centreline
        s, e = end.GetStart(), end.GetEnd()
        ax, ay, bx, by = mm(s.x), mm(s.y), mm(e.x), mm(e.y)
        dx, dy = bx - ax, by - ay
        L2 = dx * dx + dy * dy
        u = 0.0 if L2 == 0 else max(0.0, min(1.0, ((ex - ax) * dx + (ey - ay) * dy) / L2))
        pts.append((ax + u * dx, ay + u * dy, el))
    # collapse runs of the same direction on the same layer
    out = [pts[0]]
    for k in range(1, len(pts) - 1):
        a, b_, c = out[-1], pts[k], pts[k + 1]
        if a[2] != b_[2] or b_[2] != c[2]:
            out.append(b_); continue
        v1 = (b_[0] - a[0], b_[1] - a[1]); v2 = (c[0] - b_[0], c[1] - b_[1])
        if abs(v1[0] * v2[1] - v1[1] * v2[0]) > 1e-9 or (v1[0] * v2[0] + v1[1] * v2[1]) < 0:
            out.append(b_)
    out.append(pts[-1])
    n_tracks = n_vias = 0
    for a, b_ in zip(out, out[1:]):
        if a[2] != b_[2]:
            v = pcbnew.PCB_VIA(board)
            v.SetPosition(pcbnew.VECTOR2I(nm(a[0]), nm(a[1])))
            v.SetWidth(nm(VIA_D)); v.SetDrill(nm(VIA_DRILL))
            v.SetLayerPair(F, B); v.SetNet(netinfo)
            board.Add(v); n_vias += 1
            continue
        if math.hypot(b_[0] - a[0], b_[1] - a[1]) < 0.001:
            continue
        t = pcbnew.PCB_TRACK(board)
        t.SetStart(pcbnew.VECTOR2I(nm(a[0]), nm(a[1]))); t.SetEnd(pcbnew.VECTOR2I(nm(b_[0]), nm(b_[1])))
        t.SetWidth(nm(width)); t.SetLayer(a[2]); t.SetNet(netinfo)
        board.Add(t); n_tracks += 1
    length = sum(math.hypot(b_[0] - a[0], b_[1] - a[1]) for a, b_ in zip(out, out[1:]))
    return n_tracks, n_vias, length


def draw_hand():
    """The HAND runs, once: a board that already has them is left alone."""
    from r13_edit import Board
    e = Board(BOARD)
    if any(t.GetNetname() == '/J4_HSKO' for t in e.b.GetTracks()):
        print('hand runs: already drawn')
        return
    for net, layer, pts, width in HAND:
        if layer == 'via':
            e.add_via(net, pts[0])
        else:
            e.draw(net, pts, layer, width)
    e.save()
    print(f'hand runs: {len(HAND)} drawn')


def main():
    only = sys.argv[sys.argv.index('--only') + 1:] if '--only' in sys.argv else None
    if not only:
        draw_hand()
    done, failed = [], []
    todo = list(CONNECTIONS)
    # a second pass gives anything blocked in the first a wider corridor
    for pass_no in (1, 2):
        failed = route_all(todo, only, done, extra=0.0 if pass_no == 1 else 3.0)
        todo = failed
        if not todo:
            break
    board = pcbnew.LoadBoard(BOARD)
    pcbnew.ZONE_FILLER(board).Fill(board.Zones())
    board.Save(BOARD)
    print(f'{len(done)} connections drawn, {len(todo)} not; zones refilled')


def route_all(conns, only, done, extra):
    failed = []
    for conn in conns:
        net, (ref, num), width, layers, margin = conn[:5]
        hint = conn[5] if len(conn) > 5 else None
        margin += extra
        if only and net not in only:
            continue
        board = pcbnew.LoadBoard(BOARD)           # fresh each time: SWIG lists go stale
        pad = [p for p in board.FindFootprintByReference(ref).Pads() if p.GetNumber() == num][0]
        for clr in CLRS:
            g, ends = build(board, net, pad, width, layers, margin, clr)
            if g is None:
                break
            path = search(g, *ends, hint=hint_cells(g, hint) if hint else None)
            if path is not None:
                break
        if g is None:
            print(f'{net} from {ref}.{num}: already joined')
            continue
        if path is None:
            print(f'{net} from {ref}.{num}: NO PATH within {margin} mm')
            failed.append(conn)
            continue
        nt, nv, length = lay(board, g, path, net, width, pad, ends[1])
        board.Save(BOARD)
        done.append(net)
        print(f'{net} from {ref}.{num}: {nt} tracks, {nv} vias, {length:.1f} mm, clearance {clr}')
    return failed


if __name__ == '__main__':
    main()
