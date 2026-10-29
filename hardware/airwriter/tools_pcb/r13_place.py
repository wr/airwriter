#!/usr/bin/env python3
"""Rev 2026.11: place the new parts and make the board's own changes.

    <KiCad python> tools_pcb/r13_place.py

Run after r13_sync.py and r13_clean.py, with KiCad closed. Re-runnable: it
sets positions and attributes outright and adds TP9 and the texts only once.

Every resistor sits beside the pin it serves, on a spot found clear of other
courtyards, F.Cu copper (0.25 mm), U7's TASHTALK label, the antenna keepout
and the lit dots' light paths. Rev A parts that move to make room move in
r13_reroute.py, after r13_clean.py has seen the board as rev A left it;
r13_route.py draws the new traces.

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
    'R24': (119.725, 143.25, 180),  # IO47 pull-down, just below U3 where HSKI_DRV drops to the back
    'R25': (136.00, 151.00, 90),    # IO17 pull-up, east of U6, off UART_TX's back run by a via
    'R27': (134.12, 136.00, -90),   # 1 k in LT_RESET's back run, east of U7
    'R29': (112.75, 152.50, 90),    # 10 k, J2.8 to U2.8
    'R30': (109.75, 153.25, 0),     # 10 k, J2.5 to U2.7
    'R32': (141.45, 129.05, 180),   # revision strap, north of U1, beside IO16 (pin 9)
    'R33': (144.50, 129.05, 180),   # Lite strap, north of U1, above IO15 (pin 8)
    'SW1': (157.60, 99.45, -90),    # reset, button east at x 159.9 on rev A's pinhole axis;
                                    # pads 1 and 2 over rev A's EN and GND pads
    'R17': (160.20, 155.60, 0),     # D3's resistor, north of it: clear of the keepout and the lattice's D3 tube,
                                    # pad 2 0.58 mm in from the edge (PCBWay's minimum is 0.30), pad 1 on D3-K's run
}

SLASH = (159.35, 160.38, -77.5)     # centre and angle of the top case's slash
D3_EAST = 0.27                      # LED axis east of the slash centre, clear of J1
D3_ORIENT = 257.5                   # pad 1 north, pad 2 south, along the slash
D3_K = 1.8                          # the cathode's K, this far past pad 1 along the leads: outside the spacer

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
RESET_LABEL = (154.75, 99.45)       # beside SW1's back, on its axis
# Spare GPIOs on bare pads on the back, below the module's south pins, for
# trying a new idea on a built board with a few wires. On the back, the front
# keeps only a short stub and a via per pin, so the front pour still reaches C1
# and C22. IO41 has no way down past USB D- on the back; IO42 has C1 over it.
SPARE_PADS = [('TP10', '/SPARE_IO38', (143.08, 151.20)), ('TP11', '/SPARE_IO39', (144.35, 151.20)),
              ('TP12', '/SPARE_IO40', (145.62, 151.20))]


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
    # an LED's designator lives on F.Fab, as rev A's do; on the silk it ran off the edge
    fp.Reference().SetLayer(pcbnew.F_Fab)
    fp.Reference().SetTextSize(pcbnew.VECTOR2I(pcbnew.FromMM(1.0), pcbnew.FromMM(1.0)))
    fp.Reference().SetTextThickness(pcbnew.FromMM(0.15))
    fp.Reference().SetTextAngleDegrees(0)
    fp.Reference().SetPosition(at(cx, cy))
    # The spacer hides the square pad once it is on, and PCBWay fits D3 by hand:
    # a K beside the cathode, past pad 1 on the side away from pad 2.
    k = [d for d in board.GetDrawings() if d.GetClass() == 'PCB_TEXT'
         and d.GetLayer() == pcbnew.F_SilkS and d.GetText() == 'K']
    if not k:
        k = [pcbnew.PCB_TEXT(board)]
        k[0].SetText('K')
        k[0].SetLayer(pcbnew.F_SilkS)
        k[0].SetTextSize(pcbnew.VECTOR2I(pcbnew.FromMM(0.8), pcbnew.FromMM(0.8)))
        k[0].SetTextThickness(pcbnew.FromMM(0.15))
        board.Add(k[0])
    k[0].SetPosition(at(cx + (1.27 + D3_K) * ux, cy + (1.27 + D3_K) * uy))
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


def spare_pads(board):
    """Bare 1 mm round pads on the back, each with its pin number beside it,
    built here rather than copied from TP7: copying a footprint and removing its
    label leaves SWIG's lists opaque for the next."""
    for ref, net, xy in SPARE_PADS:
        if board.FindFootprintByReference(ref):
            continue
        tp = pcbnew.FOOTPRINT(board)
        tp.SetFPIDAsString('SparePad_D1.0mm')     # drawn here; no library to match
        tp.SetAttributes(pcbnew.FP_SMD | pcbnew.FP_BOARD_ONLY
                         | pcbnew.FP_EXCLUDE_FROM_POS_FILES | pcbnew.FP_EXCLUDE_FROM_BOM)
        tp.Reference().SetText(ref)
        tp.Reference().SetLayer(pcbnew.F_Fab)
        tp.Value().SetText(net.lstrip('/'))
        tp.Value().SetLayer(pcbnew.F_Fab)
        tp.Value().SetVisible(False)
        pad = pcbnew.PAD(tp)
        pad.SetNumber('1')
        pad.SetAttribute(pcbnew.PAD_ATTRIB_SMD)
        pad.SetShape(pcbnew.PAD_SHAPE_CIRCLE)
        pad.SetSize(pcbnew.VECTOR2I(pcbnew.FromMM(1.0), pcbnew.FromMM(1.0)))
        layers = pcbnew.LSET()
        layers.AddLayer(pcbnew.F_Cu)
        layers.AddLayer(pcbnew.F_Mask)
        pad.SetLayerSet(layers)                    # bare copper, no paste
        tp.Add(pad)
        label = pcbnew.PCB_TEXT(tp)
        label.SetText(net[-2:])                    # 38, 39, 40
        label.SetLayer(pcbnew.F_SilkS)
        label.SetTextSize(pcbnew.VECTOR2I(pcbnew.FromMM(0.8), pcbnew.FromMM(0.8)))
        label.SetTextThickness(pcbnew.FromMM(0.15))
        label.SetPosition(at(0, 2.3 if net.endswith('39') else 1.3))   # staggered: 1.27 apart
        tp.Add(label)
        tp.SetPosition(at(*xy))
        tp.Reference().SetPosition(at(*xy))
        tp.Flip(tp.GetPosition(), False)           # to the back: pad, label and all
        board.Add(tp)
        pad.SetNet(board.FindNet(net))


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
    # Rev A's RESET was a field of the EVQP44B3M's footprint and went with it:
    # the same knockout label, on the board, west of the new switch
    if not any(d.GetClass() == 'PCB_TEXT' and d.GetText() == 'RESET' for d in board.GetDrawings()):
        t = pcbnew.PCB_TEXT(board)
        t.SetText('RESET')
        t.SetLayer(pcbnew.F_SilkS)
        t.SetIsKnockout(True)
        t.SetTextSize(pcbnew.VECTOR2I(pcbnew.FromMM(1.0), pcbnew.FromMM(1.0)))
        t.SetTextThickness(pcbnew.FromMM(0.15))
        t.SetTextAngleDegrees(90)
        t.SetPosition(at(*RESET_LABEL))
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
    spare_pads(board)
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
