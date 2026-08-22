#!/usr/bin/env python3
"""Merge the main board and the art panel into one 2-design snap-apart file.

    python3 hardware/combine_boards.py

Layout: panel sits ABOVE the main board (both 62mm wide), 2mm routed gap,
three 5mm tabs bridging the main board's top edge (y=60) to the panel's
bottom edge (y=58), each tab lined with 0.5mm mouse-bite drills on both
break lines. Output: hardware/combined/combined.kicad_pcb.

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

DX, DY = 100.0, -46.27          # panel translate: (0,0)-(62,104.27) -> (100,-46.27)-(162,58)
MAIN_EDGE_Y, PANEL_EDGE_Y = 60.0, 58.0
TABS = [(107.5, 112.5), (128.5, 133.5), (149.5, 154.5)]   # x spans of the 3 tabs
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


def bite(x, y):
    """One 0.5mm NPTH mouse-bite hole as a bare-pad footprint."""
    return parse(
        f'(footprint "combined:MouseBite" (layer "F.Cu") (at {x:g} {y:g})'
        f' (attr exclude_from_pos_files exclude_from_bom)'
        f' (pad "" np_thru_hole circle (at 0 0) (size 0.5 0.5) (drill 0.5)'
        f' (layers "F.Cu" "B.Cu" "F.Mask" "B.Mask")))')[0]


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


def split_edge(y, x_lo, x_hi):
    """Return the straight edge at height y broken around the tab spans."""
    out, cur = [], x_lo
    for a, b in TABS:
        out.append(edge_line(cur, y, a, y))
        cur = b
    out.append(edge_line(cur, y, x_hi, y))
    return out


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

    # outline surgery: break the two facing edges around the tabs
    main_edge = find_edge(base[1:], 102.0, 60.0, 160.0, 60.0)
    base.remove(main_edge)
    # panel bottom edge was (2,104.27)-(60,104.27); translated to y=58, x 102..160
    panel_edge = find_edge(copied, 102.0, 58.0, 160.0, 58.0)
    copied.remove(panel_edge)

    new_edges = split_edge(MAIN_EDGE_Y, 102.0, 160.0)
    new_edges += split_edge(PANEL_EDGE_Y, 102.0, 160.0)
    for a, b in TABS:                       # tab side walls across the gap
        new_edges.append(edge_line(a, PANEL_EDGE_Y, a, MAIN_EDGE_Y))
        new_edges.append(edge_line(b, PANEL_EDGE_Y, b, MAIN_EDGE_Y))

    bites = []
    for a, b in TABS:
        for i in range(4):
            x = a + 1.0 + i * (b - a - 2.0) / 3.0
            bites.append(bite(x, MAIN_EDGE_Y))
            bites.append(bite(x, PANEL_EDGE_Y))

    base.extend(copied + new_edges + bites)

    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    open(OUT, 'w').write(serialize(base))
    print(f'wrote {OUT}: +{len(copied)} panel items, {len(new_edges)} edge segments, '
          f'{len(bites)} mouse bites')
    print('overall: 62.1 x 210.6 mm, 2 designs, 3 tabs. NEVER refill zones here.')


if __name__ == '__main__':
    main()
