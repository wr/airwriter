#!/usr/bin/env python3
"""Rev 2026.11: SW1's footprint, the BZCN TSB001A3518A side-press switch.

    <KiCad python> tools_pcb/r13_mkfp_sw1.py

Rev A's Panasonic EVQP44B3M sits on two small pads and lifts off the board
unless it is reflowed by hand. The TSB001A3518A (LCSC C2888448), the switch
the Macintosh Mini uses for its front buttons, is held by two plastic pegs in
0.9 mm holes and two 1.5 mm bracket pads besides its two contacts.

Drawn here from the part's land (LCSC's drawing, as the Macintosh Mini's
footprint has it): origin at the body's centre, the button towards -y. The
contacts are numbered so that, turned -90 degrees on the east edge with the
button facing the case's pinhole, pad 1 lands where rev A's EN pad was and
pad 2 on its GND pad; the switch is symmetric, so the numbering is free. The
bracket pads are unnumbered: soldered, on no net. The 3D model is
lib/make_sw1_model.py's. Saved through KiCad's own footprint writer.
"""
import os

import pcbnew

HERE = os.path.dirname(os.path.abspath(__file__))
LIB = os.path.normpath(os.path.join(HERE, '..', 'lib', 'airwriter.pretty'))
NAME = 'SW_BZCN_TSB001A3518A'
MODEL = '${KIPRJMOD}/lib/airwriter.3dshapes/SW_BZCN_TSB001A3518A.step'
CONTACTS = {'1': (2.1, 0.97), '2': (-2.1, 0.97)}       # 1.2 x 1.2
BRACKETS = ((-3.45, -0.97), (3.45, -0.97))             # 1.5 x 1.5, no net
PEGS = ((-1.15, -0.48), (1.15, -0.48))                 # 0.9 mm NPTH
BODY = (-3.85, -1.6, 3.85, 1.48)                       # x0, y0, x1, y1
BUTTON = (-1.5, -2.31, 1.5, -1.6)
COURTYARD = (-4.45, -2.6, 4.45, 1.85)
SILK = [((-3.81, -0.05), (-3.81, 1.09)), ((-3.81, 1.09), (-2.93, 1.09)),
        ((-1.27, 1.09), (1.27, 1.09)),
        ((2.93, 1.09), (3.81, 1.09)), ((3.81, 1.09), (3.81, -0.05)),
        ((-2.54, -1.57), (2.54, -1.57)),
        ((-1.5, -1.57), (-1.5, -2.56)), ((-1.5, -2.56), (1.48, -2.56)), ((1.48, -2.56), (1.48, -1.57))]


def v(x, y):
    return pcbnew.VECTOR2I(pcbnew.FromMM(x), pcbnew.FromMM(y))


def pad(fp, number, at, size, attr):
    p = pcbnew.PAD(fp)
    p.SetNumber(number)
    p.SetPosition(v(*at))
    p.SetSize(v(size, size))
    if attr == 'npth':
        p.SetAttribute(pcbnew.PAD_ATTRIB_NPTH)
        p.SetShape(pcbnew.PAD_SHAPE_CIRCLE)
        p.SetDrillSize(v(size, size))
        p.SetLayerSet(pcbnew.PAD.UnplatedHoleMask())
    else:
        p.SetAttribute(pcbnew.PAD_ATTRIB_SMD)
        p.SetShape(pcbnew.PAD_SHAPE_RECT)
        p.SetLayerSet(pcbnew.PAD.SMDMask())
    fp.Add(p)


def rect(fp, layer, box, width):
    x0, y0, x1, y1 = box
    r = pcbnew.PCB_SHAPE(fp, pcbnew.SHAPE_T_RECT)
    r.SetStart(v(x0, y0))
    r.SetEnd(v(x1, y1))
    r.SetLayer(layer)
    r.SetWidth(pcbnew.FromMM(width))
    fp.Add(r)


def line(fp, layer, a, b, width):
    s = pcbnew.PCB_SHAPE(fp, pcbnew.SHAPE_T_SEGMENT)
    s.SetStart(v(*a))
    s.SetEnd(v(*b))
    s.SetLayer(layer)
    s.SetWidth(pcbnew.FromMM(width))
    fp.Add(s)


def main():
    board = pcbnew.LoadBoard(os.path.join(HERE, '..', 'airwriter.kicad_pcb'))
    fp = pcbnew.FOOTPRINT(board)
    fp.SetFPIDAsString('airwriter:' + NAME)
    fp.SetLibDescription('BZCN TSB001A3518A side-press SMD tactile switch, 7.7 x 3.55 x 3.5 mm, '
                         '1.8 N (LCSC C2888448): two 0.9 mm locating pegs, two bracket pads; '
                         'button 3.0 x 1.5 mm, its axis 1.9 mm above the board')
    fp.SetKeywords('switch tactile side SMD TSB001 C2888448')
    fp.SetAttributes(pcbnew.FP_SMD)
    fp.Reference().SetText('REF**')
    fp.Reference().SetLayer(pcbnew.F_SilkS)
    fp.Reference().SetPosition(v(0, -3.6))
    fp.Value().SetText(NAME)
    fp.Value().SetLayer(pcbnew.F_Fab)
    fp.Value().SetPosition(v(0, 3.0))
    for number, at in CONTACTS.items():
        pad(fp, number, at, 1.2, 'smd')
    for at in BRACKETS:
        pad(fp, '', at, 1.5, 'smd')
    for at in PEGS:
        pad(fp, '', at, 0.9, 'npth')
    for a, b in SILK:
        line(fp, pcbnew.F_SilkS, a, b, 0.2)
    rect(fp, pcbnew.F_Fab, BODY, 0.1)
    rect(fp, pcbnew.F_Fab, BUTTON, 0.1)
    rect(fp, pcbnew.F_CrtYd, COURTYARD, 0.05)
    m = pcbnew.FP_3DMODEL()
    m.m_Filename = MODEL
    fp.Models().push_back(m)
    pcbnew.FootprintSave(LIB, fp)
    print('wrote', os.path.join(LIB, NAME + '.kicad_mod'))


if __name__ == '__main__':
    main()
