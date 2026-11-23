#!/usr/bin/env python3
# SPDX-License-Identifier: CERN-OHL-S-2.0
"""W-1176: turn the board's printing to read with the sockets at the top, and put the
Wells Workshop nameplate on the back.

    <KiCad python> tools_pcb/r15_silk.py

Run last, after r15_route.py, with KiCad closed. A second run finds the nameplate
and stops.

AirWriter stands with its sockets at the back, so seen from above, and through the
LC's clear lid, the board is its own frame turned half a turn. Every label on both
sides turns half a turn about its own centre, and the OSHW logo with them, so the
board reads right side up with its sockets at the top: from the front, and from the
back once it is turned over left to right.

The WW mark leaves the front, where a frosted dot of the LC's lid covered it, for a
nameplate on the back. The back's three lines (the board's name, the repository and
the licence) go into it, beside the Wells Workshop mark and under the name in its own
lettering (art/wells-workshop.svg), left-aligned, in a thin frame with corners
rounded 0.35 mm. The plate sits at the north edge, centred on the board, in the one
stretch wide enough with no via in it: a via under the art would print as a dot. Its
top line runs just south of the vias at y 73.29.
"""
import os
import re

import pcbnew

from r13_edit import Board, nm, mm

HERE = os.path.dirname(os.path.abspath(__file__))
BOARD = os.environ.get('R15_BOARD') or os.path.join(HERE, '..', 'airwriter.kicad_pcb')
ART = os.path.join(HERE, '..', 'art', 'wells-workshop.svg')

LINES = ('AirWriter rev 2026.11', 'github.com/wr/airwriter', 'CERN-OHL-S-2.0')
SIZES = (1.1, 0.9, 0.9)          # the lines' text heights, mm (PCBWay prints 0.8 and up)
LINE_GAP = 0.65                  # between the lines
NAME_H, NAME_GAP = 2.0, 0.7      # the name's height (to its descender), and the gap below it
MARK_H, MARK_GAP = 5.0, 2.4      # the mark's height, and the gap between it and the text
PAD, RADIUS, FRAME_W = 1.9, 0.35, 0.2
CENTRE_X = 131.0                 # the board's centre line
FRAME_TOP = 72.75                # board y of the frame's top (north of it, vias at y 73.29)
STROKE = 0.15

HALF_TURN = pcbnew.EDA_ANGLE(180.0, pcbnew.DEGREES_T)
F, B = pcbnew.F_SilkS, pcbnew.B_SilkS


def svg_paths(path):
    """Each <path> as a list of closed outlines in SVG units, curves flattened."""
    src = open(path).read()
    out = []
    for d in re.findall(r'<path fill="[^"]+" d="([^"]+)"', src):
        toks = re.findall(r'[A-Za-z]|-?(?:\d+\.?\d*|\.\d+)(?:e-?\d+)?', d)
        i, cmd, x, y, sx, sy, ctrl = 0, None, 0.0, 0.0, 0.0, 0.0, None
        rings, cur = [], []

        def num():
            nonlocal i
            i += 1
            return float(toks[i - 1])

        def quad(qx, qy, ex, ey, steps=8):
            for k in range(1, steps + 1):
                t = k / steps
                cur.append(((1 - t) ** 2 * x + 2 * (1 - t) * t * qx + t * t * ex,
                            (1 - t) ** 2 * y + 2 * (1 - t) * t * qy + t * t * ey))

        while i < len(toks):
            if toks[i].isalpha():
                cmd = toks[i]
                i += 1
                if cmd in 'Zz':
                    rings.append(cur)
                    cur, x, y, ctrl = [], sx, sy, None
                    continue
            ox, oy = (x, y) if cmd.islower() else (0.0, 0.0)
            c = cmd.upper()
            if c == 'M':
                if cur:
                    rings.append(cur)
                x, y = ox + num(), oy + num()
                sx, sy, cur, ctrl = x, y, [(x, y)], None
                cmd = 'l' if cmd.islower() else 'L'
            elif c == 'L':
                x, y = ox + num(), oy + num()
                cur.append((x, y))
                ctrl = None
            elif c == 'H':
                x = ox + num()
                cur.append((x, y))
                ctrl = None
            elif c == 'V':
                y = oy + num()
                cur.append((x, y))
                ctrl = None
            elif c in 'QT':
                if c == 'Q':
                    qx, qy = ox + num(), oy + num()
                else:
                    qx, qy = (2 * x - ctrl[0], 2 * y - ctrl[1]) if ctrl else (x, y)
                ex, ey = ox + num(), oy + num()
                quad(qx, qy, ex, ey)
                ctrl, x, y = (qx, qy), ex, ey
            else:
                raise SystemExit(f'{path}: no support for the path command {cmd}')
        if cur:
            rings.append(cur)
        out.append(rings)
    return out


def area(ring):
    return sum(ring[k][0] * ring[k - 1][1] - ring[k - 1][0] * ring[k][1]
               for k in range(len(ring))) / 2


def inside(pt, ring):
    x, y = pt
    hit = False
    for k in range(len(ring)):
        (xa, ya), (xb, yb) = ring[k - 1], ring[k]
        if (ya > y) != (yb > y) and x < xa + (y - ya) * (xb - xa) / (yb - ya):
            hit = not hit
    return hit


def bbox(paths):
    pts = [p for rings in paths for ring in rings for p in ring]
    return (min(p[0] for p in pts), min(p[1] for p in pts),
            max(p[0] for p in pts), max(p[1] for p in pts))


def width(paths, height):
    x0, y0, x1, y1 = bbox(paths)
    return height * (x1 - x0) / (y1 - y0)


class Back:
    """Draws on the back silk as seen with the board turned over and its sockets at
    the top: view x is board x, view y is board y running north (y down the page is
    board y decreasing)."""

    def __init__(self, board):
        self.b = board
        self.added = []

    def art(self, paths, left, top, height):
        """The art's top-left at view (left, top), in board mm, scaled to the height.
        Outlines wound against their host are its holes (the counters of e, o, p)."""
        x0, y0, x1, y1 = bbox(paths)
        s = height / (y1 - y0)

        def chain(ring):
            ch = pcbnew.SHAPE_LINE_CHAIN()
            for x, y in ring:
                ch.Append(nm(left + s * (x - x0)), nm(top - s * (y - y0)))
            ch.SetClosed(True)
            return ch

        for rings in paths:
            hosts = []
            for ring in sorted(rings, key=lambda r: -abs(area(r))):
                host = next((h for h in hosts if inside(ring[0], h[0])
                             and (area(ring) > 0) != (area(h[0]) > 0)), None)
                if host:
                    host[1].append(ring)
                else:
                    hosts.append((ring, []))
            for outline, holes in hosts:
                poly = pcbnew.SHAPE_POLY_SET()
                poly.AddOutline(chain(outline))
                for h in holes:
                    poly.AddHole(chain(h))
                poly.Fracture()
                shape = pcbnew.PCB_SHAPE(self.b, pcbnew.SHAPE_T_POLY)
                shape.SetPolyShape(poly)
                shape.SetFilled(True)
                shape.SetWidth(0)
                shape.SetLayer(B)
                self.b.Add(shape)
                self.added.append(shape)

    def text(self, s, left, top, h):
        """A line left-aligned at view x `left`, its top at board y `top`."""
        t = pcbnew.PCB_TEXT(self.b)
        t.SetText(s)
        t.SetLayer(B)
        t.SetMirrored(True)
        t.SetKeepUpright(False)
        t.SetTextSize(pcbnew.VECTOR2I(nm(h), nm(h)))
        t.SetTextThickness(nm(STROKE))
        t.SetTextAngleDegrees(180)
        w = mm(t.GetBoundingBox().GetWidth())
        t.SetPosition(pcbnew.VECTOR2I(nm(left + w / 2), nm(top - h / 2)))
        self.b.Add(t)
        self.added.append(t)

    @staticmethod
    def text_width(s, h):
        t = pcbnew.PCB_TEXT(None)
        t.SetText(s)
        t.SetTextSize(pcbnew.VECTOR2I(nm(h), nm(h)))
        t.SetTextThickness(nm(STROKE))
        return mm(t.GetBoundingBox().GetWidth())

    def frame(self, x0, y0, x1, y1):
        r = pcbnew.PCB_SHAPE(self.b, pcbnew.SHAPE_T_RECT)
        r.SetLayer(B)
        r.SetWidth(nm(FRAME_W))
        r.SetStart(pcbnew.VECTOR2I(nm(x0), nm(y0)))
        r.SetEnd(pcbnew.VECTOR2I(nm(x1), nm(y1)))
        r.SetCornerRadius(nm(RADIUS))
        self.b.Add(r)
        self.added.append(r)


def turn(item):
    """Half a turn about the item's own centre."""
    if hasattr(item, 'SetKeepUpright'):
        item.SetKeepUpright(False)
    item.Rotate(item.GetBoundingBox().GetCenter(), HALF_TURN)


def main():
    e = Board(BOARD)
    b = e.b
    drawings = list(b.GetDrawings())
    if any(d.GetLayer() == B and d.GetClass() == 'PCB_SHAPE' for d in drawings):
        print('the nameplate is already on the back')
        return

    # Gather everything before changing anything: the board's lists go stale under edits.
    ww = [d for d in drawings if d.GetClass() == 'PCB_SHAPE' and d.GetLayer() == F
          and d.GetShape() == pcbnew.SHAPE_T_POLY]
    if len(ww) != 2:
        raise SystemExit(f'{len(ww)} pieces of the WW mark on the front, not 2')
    texts = [d for d in drawings if d.GetClass() == 'PCB_TEXT' and d.GetLayer() in (F, B)]
    old_lines = [d for d in texts if d.GetText() in LINES and d.GetLayer() == B]
    if sorted(d.GetText() for d in old_lines) != sorted(LINES):
        raise SystemExit('the back is missing one of its three lines: ' + ', '.join(LINES))
    logos, fp_texts = [], []
    for fp in list(b.GetFootprints()):
        if fp.GetReference().startswith('LOGO'):
            logos.append(fp)
            continue
        for f in list(fp.GetFields()):
            if f.GetLayer() in (F, B) and f.IsVisible() and f.GetText():
                fp_texts.append(f)
        for g in list(fp.GraphicalItems()):
            if g.GetClass() == 'PCB_TEXT' and g.GetLayer() in (F, B):
                fp_texts.append(g)

    turned = 0
    for item in [d for d in texts if d not in old_lines] + fp_texts + logos:
        turn(item)
        turned += 1
    for d in ww + old_lines:
        b.Remove(d)

    paths = svg_paths(ART)
    mark, name = paths[:2], paths[2:]
    col = NAME_H + NAME_GAP + sum(SIZES) + LINE_GAP * (len(SIZES) - 1)
    text_w = max([width(name, NAME_H)] + [Back.text_width(s, h) for s, h in zip(LINES, SIZES)])
    mark_w = width(mark, MARK_H)
    left = CENTRE_X - (mark_w + MARK_GAP + text_w) / 2
    top = FRAME_TOP - PAD
    back = Back(b)
    back.art(mark, left, top - (col - MARK_H) / 2, MARK_H)
    text_left = left + mark_w + MARK_GAP
    back.art(name, text_left, top, NAME_H)
    y = top - NAME_H - NAME_GAP
    for s, h in zip(LINES, SIZES):
        back.text(s, text_left, y, h)
        y -= h + LINE_GAP
    x0, x1 = left - PAD, text_left + text_w + PAD
    y0, y1 = FRAME_TOP, top - col - PAD
    back.frame(x0, y0, x1, y1)

    # Nothing with a hole, and no bare copper, inside the frame.
    def within(p):
        return x0 < mm(p.x) < x1 and y1 < mm(p.y) < y0
    vias = [t for t in b.GetTracks() if t.GetClass() == 'PCB_VIA' and within(t.GetPosition())]
    pads = [f'{fp.GetReference()}.{p.GetNumber()}' for fp in b.GetFootprints() for p in fp.Pads()
            if within(p.GetPosition()) and (p.IsOnLayer(pcbnew.B_Cu) or p.HasHole())]
    if vias or pads:
        raise SystemExit(f'the nameplate covers {len(vias)} vias and pads {pads}')
    e.save()
    print(f'{turned} labels and logos turned; the WW mark and the back\'s three lines '
          f'became a {x1 - x0:.1f} x {y0 - y1:.1f} mm nameplate, {y1 - 59.95:.2f} mm from the north edge')


if __name__ == '__main__':
    main()
