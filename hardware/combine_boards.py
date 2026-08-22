#!/usr/bin/env python3
"""Merge the main board and the art panel into one 2-design snap-apart file.

    python3 hardware/combine_boards.py

Layout: panel sits ABOVE the main board (both 62mm wide), butted FLUSH so
one straight V-score at y=60 separates them (the art panel's seam edge sands
smoother than mouse-bite nubs). The corner-radius voids at both ends are
routed; PCBWay handles route + V-cut panels as "2 designs". Output:
hardware/combined/combined.kicad_pcb.

Re-run after any change to either source board — the output is derived,
never hand-edited. Pure text/s-expression merge: copied panel copper keeps
its stored zone fills but loses net assignments (net 0), which is fine for
gerbers and means NEVER refill zones in the combined file.
"""
import copy as _copy
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, 'airwriter', 'tools_sch'))
from sexp import parse, serialize, Sym, unquote, quote

MAIN = os.path.join(HERE, 'airwriter', 'airwriter.kicad_pcb')
PANEL = os.path.join(HERE, 'panel', 'panel.kicad_pcb')
OUT = os.path.join(HERE, 'combined', 'combined.kicad_pcb')

DX, DY = 100.0, -44.27          # panel translate: (0,0)-(62,104.27) -> (100,-44.27)-(162,60)
VCUT_Y = 60.0
COPY_TAGS = {'footprint', 'gr_line', 'gr_arc', 'gr_rect', 'gr_circle', 'gr_poly',
             'gr_curve', 'gr_text', 'gr_text_box', 'segment', 'arc', 'via', 'zone',
             'dimension', 'image'}
COORD_TAGS = {'at', 'start', 'end', 'mid', 'center', 'xy'}


def is_tag(n, t):
    return isinstance(n, list) and n and str(n[0]) == t


def translate(node, dx, dy, *, top_footprint):
    """Shift absolute coordinates. Footprint children are relative to the
    footprint's own (at), so for footprints only that one anchor moves."""
    if not isinstance(node, list):
        return
    if str(node[0]) in COORD_TAGS:
        try:
            node[1] = Sym(f'{float(str(node[1])) + dx:g}')
            node[2] = Sym(f'{float(str(node[2])) + dy:g}')
        except (ValueError, IndexError):
            pass
        return
    for c in node[1:]:
        if isinstance(c, list):
            if top_footprint:
                if str(c[0]) == 'at':
                    translate(c, dx, dy, top_footprint=False)
                continue  # nothing else in a footprint is absolute
            translate(c, dx, dy, top_footprint=False)


def strip_nets(node):
    if not isinstance(node, list):
        return
    for c in node[1:]:
        if is_tag(c, 'net'):
            del c[1:]
            c.append(Sym('0'))
        elif is_tag(c, 'net_name'):
            del c[1:]
            c.append(quote(''))
        elif isinstance(c, list):
            strip_nets(c)


def edge_line(x1, y1, x2, y2):
    return [Sym('gr_line'),
            [Sym('start'), Sym(f'{x1:g}'), Sym(f'{y1:g}')],
            [Sym('end'), Sym(f'{x2:g}'), Sym(f'{y2:g}')],
            [Sym('stroke'), [Sym('width'), Sym('0.05')], [Sym('type'), Sym('default')]],
            [Sym('layer'), quote('Edge.Cuts')]]


def find_edge(items, x1, y1, x2, y2):
    for n in items:
        if not is_tag(n, 'gr_line'):
            continue
        s = next(c for c in n if is_tag(c, 'start'))
        e = next(c for c in n if is_tag(c, 'end'))
        pts = {(round(float(str(s[1])), 3), round(float(str(s[2])), 3)),
               (round(float(str(e[1])), 3), round(float(str(e[2])), 3))}
        lay = next((c for c in n if is_tag(c, 'layer')), None)
        if lay and unquote(str(lay[1])) == 'Edge.Cuts' and \
                pts == {(x1, y1), (x2, y2)}:
            return n
    raise SystemExit(f'edge line ({x1},{y1})-({x2},{y2}) not found')


def main():
    base = parse(open(MAIN).read())[0]
    panel = parse(open(PANEL).read())[0]

    copied = []
    for n in panel[1:]:
        if isinstance(n, list) and str(n[0]) in COPY_TAGS:
            n = _copy.deepcopy(n)
            strip_nets(n)
            translate(n, DX, DY, top_footprint=is_tag(n, 'footprint'))
            copied.append(n)

    # No outline surgery: both closed outlines stay intact, butted flush at
    # y=60. One V-score line spans the full panel width, plus a label PCBWay's
    # CAM can read. The four 2mm corner radii at the seam leave small voids
    # that get routed; the score crosses them.
    new_edges = [edge_line(99.95, VCUT_Y, 162.05, VCUT_Y)]
    label = parse('(gr_text "V-CUT" (at 166 60 0) (layer "Edge.Cuts")'
                  ' (effects (font (size 2 2) (thickness 0.3))))')[0]
    bites = [label]

    base.extend(copied + new_edges + bites)

    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    open(OUT, 'w').write(serialize(base))
    print(f'wrote {OUT}: +{len(copied)} panel items, V-cut seam at y={VCUT_Y}')
    print('overall: 62.1 x 208.6 mm, 2 designs, route + V-cut. NEVER refill zones here.')


if __name__ == '__main__':
    main()
