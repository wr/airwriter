#!/usr/bin/env python3
"""Rev 2026.11: place the new parts and make the board's own changes.

    <KiCad python> tools_pcb/r13_place.py

Run after r13_sync.py and r13_clean.py, with KiCad closed. Re-runnable: it
sets positions and attributes outright and adds TP9 and the texts only once.

Every resistor sits beside the pin it serves, on a spot found clear of other
courtyards, F.Cu copper (0.25 mm), U7's TASHTALK label, the antenna keepout
and the lit dots' light paths. Wells draws the traces.

D3 stands under the top case's slash: centred at (159.35, 160.38), its leads
along the slash's -77.5 degrees, the cathode (pad 1) to the north toward R17.
Its footprint (r13_mkfp_d3.py) carries the LED 8 mm up on a spacer model, so
the lattice cuts its tube round both. The LED stands 0.27 mm east of the
slash's centre: its courtyard then clears J1's, and a 3 mm face still covers
the slash's middle.
"""
import math
import os

import pcbnew

HERE = os.path.dirname(os.path.abspath(__file__))
BOARD = os.path.join(HERE, '..', 'airwriter.kicad_pcb')

PLACE = {  # ref: (x, y, orientation); pad 1 is west at 0, south at 90
    'R21': (141.30, 151.20, 90),    # IO0 pull-up, on C7's old spot: pad 2 meets BOOT_CFG
    'R22': (126.55, 129.05, 0),     # LT_DE pull-down, north of U7.3
    'R23': (147.70, 127.16, 0),     # IO48 pull-down, beside U9.2
    'R24': (139.00, 152.00, 0),     # IO35 pull-down, south of U1.28
    'R25': (118.42, 152.56, 90),    # IO17 pull-up, beside U2's 3V3 pin
    'R26': (135.75, 129.30, 0),     # IO3 pull-up, north of U1.15
    'R27': (122.59, 130.59, 0),     # 1 k into U7.4 (MCLR)
    'R28': (140.12, 153.80, 0),     # 1 k in DIP3, south of U1, west of SW2's label
    'R29': (112.75, 152.50, 90),    # 10 k, J2.8 to U2.8
    'R30': (109.75, 153.25, 0),     # 10 k, J2.5 to U2.7
    'R31': (134.25, 135.90, 90),    # IO10/T2IN pull-down, west of U1.18
    'R32': (134.25, 139.16, 90),    # revision strap, west of U1.20
    'R33': (134.25, 132.81, 90),    # Lite strap, west of U1.15-17
    'R17': (160.50, 156.30, 90),    # D3's resistor, north of it, out of the antenna keepout
}

SLASH = (159.35, 160.38, -77.5)     # centre and angle of the top case's slash
D3_EAST = 0.27                      # LED axis east of the slash centre, clear of J1
D3_ORIENT = 257.5                   # pad 1 north, pad 2 south, along the slash

FRONT_LEGENDS = ('SERIAL', 'LOCALTALK', 'USB-C', 'PWR')
BACK_TEXT = [  # text, x, y, height
    ('SERIAL', 113.0, 153.8, 1.0),
    ('LOCALTALK', 132.5, 153.8, 1.0),
    ('USB-C', 152.0, 157.0, 1.0),
    ('AirWriter rev 2026.11', 131.0, 106.0, 1.5),
    ('github.com/wr/airwriter', 131.0, 109.0, 1.0),
    ('CERN-OHL-S-2.0', 131.0, 111.0, 1.0),
]
TP9 = (150.0, 142.1)                # GND, west of TP4 (EN) and its label, near TP7/TP8


def at(x, y):
    return pcbnew.VECTOR2I(pcbnew.FromMM(x), pcbnew.FromMM(y))


def place(board, ref, x, y, deg):
    fp = board.FindFootprintByReference(ref)
    if fp.IsFlipped():
        fp.Flip(fp.GetPosition(), False)
    fp.SetOrientationDegrees(deg)
    fp.SetPosition(at(x, y))
    return fp


def d3(board):
    cx, cy, ang = SLASH
    cx += D3_EAST
    ux, uy = math.cos(math.radians(ang)), math.sin(math.radians(ang))
    # pad 1 (the footprint's origin) half a pitch north-east of the centre
    fp = place(board, 'D3', cx + 1.27 * ux, cy + 1.27 * uy, D3_ORIENT)
    for pad in fp.Pads():
        c = pad.GetCenter()
        print(f'  D3.{pad.GetNumber()} {pad.GetNetname()} at '
              f'({pcbnew.ToMM(c.x):.3f}, {pcbnew.ToMM(c.y):.3f})')


def tp9(board):
    if board.FindFootprintByReference('TP9'):
        return
    tp = board.FindFootprintByReference('TP7').Duplicate(False).Cast()
    tp.SetReference('TP9')
    tp.SetValue('GND')
    tp.SetPosition(at(*TP9))
    for item in tp.GraphicalItems():
        if item.GetClass() == 'PCB_TEXT' and item.GetText() == 'TX':
            item.SetText('GND')               # a letter wider than TX: clear the pad
            item.Move(pcbnew.VECTOR2I(pcbnew.FromMM(-0.4), 0))
    for pad in tp.Pads():
        pad.SetNet(board.FindNet('/GND'))
    board.Add(tp)


def texts(board):
    for d in list(board.GetDrawings()):
        if d.GetClass() == 'PCB_TEXT' and d.GetLayer() == pcbnew.F_SilkS \
                and d.GetText() in FRONT_LEGENDS:
            board.Delete(d)
    have = {d.GetText() for d in board.GetDrawings()
            if d.GetClass() == 'PCB_TEXT' and d.GetLayer() == pcbnew.B_SilkS}
    for text, x, y, h in BACK_TEXT:
        if text in have:
            continue
        t = pcbnew.PCB_TEXT(board)
        t.SetText(text)
        t.SetLayer(pcbnew.B_SilkS)
        t.SetMirrored(True)
        t.SetTextSize(pcbnew.VECTOR2I(pcbnew.FromMM(h), pcbnew.FromMM(h)))
        t.SetTextThickness(pcbnew.FromMM(0.15 if h <= 1.0 else 0.22))
        t.SetPosition(at(x, y))
        board.Add(t)


def main():
    board = pcbnew.LoadBoard(BOARD)
    for ref, (x, y, deg) in PLACE.items():
        fp = place(board, ref, x, y, deg)
        # designators on F.Fab, as every R and C on rev A
        fp.Reference().SetLayer(pcbnew.F_Fab)
        fp.Reference().SetTextSize(pcbnew.VECTOR2I(pcbnew.FromMM(1.0), pcbnew.FromMM(1.0)))
        fp.Reference().SetTextThickness(pcbnew.FromMM(0.15))
        fp.Reference().SetPosition(fp.GetPosition())
    d3(board)
    board.FindFootprintByReference('R9').SetDNP(False)
    board.FindFootprintByReference('J2').SetExcludedFromPosFiles(False)
    tp9(board)
    texts(board)
    for ref in ('R21', 'R24', 'R29', 'R30'):
        fp = board.FindFootprintByReference(ref)
        print(' ', ref, [(p.GetNumber(), p.GetNetname(), round(pcbnew.ToMM(p.GetCenter().x), 3),
                          round(pcbnew.ToMM(p.GetCenter().y), 3)) for p in fp.Pads()])
    # Persist the pours: DRC refills in memory only, and r12_gnd.py reads the
    # fill on disk.
    pcbnew.ZONE_FILLER(board).Fill(board.Zones())
    board.Save(BOARD)
    print('placed', len(PLACE), 'parts, D3, TP9, texts; zones refilled')


if __name__ == '__main__':
    main()
