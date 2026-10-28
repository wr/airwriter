#!/usr/bin/env python3
"""Rev 2026.11: board checks the DRC's own rules don't make (KiCad python).

    <KiCad python> tools_pcb/r13_check.py

- Every pad of a part that sits inside the board is at least 0.30 mm from the
  outline. The board's DRC rule is 0.25, but PCBWay's routed edge wanders
  ±0.2 mm, and its stated minimum is 0.30. Parts made to meet the edge (the
  ports, S1, SW1) are left out.
- No front silk text runs off the board.
- D3 has a cathode mark outside its spacer: the square pad is hidden once the
  spacer is on, and PCBWay fits D3 by hand.
Writes nothing.
"""
import math
import os
import sys

import pcbnew

HERE = os.path.dirname(os.path.abspath(__file__))
BOARD = os.path.join(HERE, '..', 'airwriter.kicad_pcb')
OUTLINE = (100.0, 60.0, 162.0, 164.27)
PAD_EDGE = 0.30
AT_EDGE = ('J1', 'J2', 'J4', 'S1', 'SW1')
SPACER_R = 2.0


def mm(v):
    return pcbnew.ToMM(v)


def main():
    b = pcbnew.LoadBoard(BOARD)
    x0, y0, x1, y1 = OUTLINE
    bad = []
    for fp in b.GetFootprints():
        ref = fp.GetReference()
        if ref in AT_EDGE:
            continue
        for p in fp.Pads():
            bb = p.GetBoundingBox()
            d = min(mm(bb.GetLeft()) - x0, x1 - mm(bb.GetRight()),
                    mm(bb.GetTop()) - y0, y1 - mm(bb.GetBottom()))
            if d < PAD_EDGE:
                bad.append(f'{ref}.{p.GetNumber()} is {d:.3f} mm from the edge')
    texts = [d for d in b.GetDrawings() if d.GetClass() == 'PCB_TEXT']
    for fp in b.GetFootprints():
        texts += [t for t in fp.GraphicalItems() if t.GetClass() == 'PCB_TEXT']
        texts += [fp.Reference(), fp.Value()]
    for t in texts:
        if t.GetLayer() != pcbnew.F_SilkS or not t.IsVisible():
            continue
        bb = t.GetBoundingBox()
        if mm(bb.GetLeft()) < x0 or mm(bb.GetRight()) > x1 or mm(bb.GetTop()) < y0 or mm(bb.GetBottom()) > y1:
            bad.append(f'front silk "{t.GetShownText(False)}" runs off the board')
    d3 = b.FindFootprintByReference('D3')
    k = {p.GetNumber(): p.GetCenter() for p in d3.Pads()}
    axis = ((mm(k['1'].x) + mm(k['2'].x)) / 2, (mm(k['1'].y) + mm(k['2'].y)) / 2)
    marks = [t for t in texts if t.GetLayer() == pcbnew.F_SilkS and t.IsVisible()
             and t.GetShownText(False) == 'K'
             and math.dist((mm(t.GetPosition().x), mm(t.GetPosition().y)), axis) > SPACER_R + 0.3
             and math.dist((mm(t.GetPosition().x), mm(t.GetPosition().y)),
                           (mm(k['1'].x), mm(k['1'].y))) < 2.5]
    if not marks:
        bad.append('D3 has no K mark beside pad 1, outside its spacer')
    print('\n'.join(bad) if bad else 'ok: pads clear of the edge, silk on the board, D3 marked')
    sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()
