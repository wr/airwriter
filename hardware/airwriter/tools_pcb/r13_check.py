#!/usr/bin/env python3
"""Rev 2026.11: board checks the DRC's own rules don't make (KiCad python).

    <KiCad python> tools_pcb/r13_check.py

- Every pad of a part that sits inside the board is at least 0.30 mm from the
  outline. The board's DRC rule is 0.25, but PCBWay's routed edge wanders
  ±0.2 mm, and its stated minimum is 0.30. Parts made to meet the edge (the
  ports) are left out.
- No front silk text runs off the board.
- D3 has a cathode mark outside its spacer: the square pad is hidden once the
  spacer is on, and PCBWay fits D3 by hand.
- SW1 has its RESET label beside it.
- Every label on both sides reads with the sockets at the top (r15_silk.py): the
  board's own frame turned half a turn, so a label's angle is 180, or 270 if it
  runs up the board. The top carries the OSHW logo, the back the Wells Workshop
  nameplate.
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
AT_EDGE = ('J1', 'J2', 'J4')
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
    sw1 = b.FindFootprintByReference('SW1').GetPosition()
    if not any(t.GetLayer() == pcbnew.F_SilkS and t.GetShownText(False) == 'RESET'
               and math.dist((mm(t.GetPosition().x), mm(t.GetPosition().y)), (mm(sw1.x), mm(sw1.y))) < 4.0
               for t in texts):
        bad.append('no RESET label beside SW1')
    fields = [f for fp in b.GetFootprints() for f in fp.GetFields()
              if f.GetName() not in ('Reference', 'Value')]      # those two are in texts
    for t in texts + fields:
        if t.GetLayer() in (pcbnew.F_SilkS, pcbnew.B_SilkS) and t.IsVisible() and t.GetText() \
                and round(t.GetDrawRotation().AsDegrees()) % 360 not in (180, 270):
            bad.append(f'"{t.GetShownText(False)}" reads with the sockets at the bottom '
                       f'({t.GetDrawRotation().AsDegrees():.0f} degrees)')
    logo = b.FindFootprintByReference('LOGO1')
    if not logo or logo.IsFlipped():
        bad.append('the OSHW logo is missing from the top')
    back = [d for d in b.GetDrawings() if d.GetLayer() == pcbnew.B_SilkS and d.GetClass() == 'PCB_SHAPE']
    if not any(d.GetShape() == pcbnew.SHAPE_T_RECT for d in back) \
            or sum(d.GetShape() == pcbnew.SHAPE_T_POLY for d in back) < 3:
        bad.append('the Wells Workshop nameplate is missing from the back')
    print('\n'.join(bad) if bad else 'ok: pads clear of the edge, silk on the board, D3 and SW1 marked, '
          'labels turned, the logo on top and the nameplate on the back')
    sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()
