#!/usr/bin/env python3
# SPDX-License-Identifier: CERN-OHL-S-2.0
"""W-1185: place and route each socket's port sense.

    kicad-cli sch export netlist --format kicadxml -o net.xml airwriter.kicad_sch
    <KiCad python> tools_pcb/r13_sync.py net.xml
    <KiCad python> tools_pcb/r16_sense.py

Run after r15_shell.py and a fresh r13_sync.py (which brings in Q1, Q2 and
R47-R56), with KiCad closed. Re-runnable: it sets the parts' places outright and
redraws only its own copper.

Each socket's transistor and four divider resistors sit beside it (J2's west of
the socket, between it and the board edge; J4's in the gap between the two
sockets, the transistor just above), and tap the socket's pins 8 and 5 on the
back. The two 4.7 k series resistors sit by the module's IO39 and IO38, between
its pads' vias and the collector lines' vias; the test pads TP11 and TP10 stay on
the GPIO side, now J2_SENSE and J4_SENSE. Each socket's collector line crosses the
board on the back to its series resistor, both through the one gap past J4's
handshake runs, so J2 takes IO39 and J4 IO38: the other way round they'd cross.
SW2's BOOT label and the back's 38/39/40 move clear of the new pads and vias.
Every run is drawn point by point.
"""
import os

import pcbnew

from r13_edit import Board, nm, mm

HERE = os.path.dirname(os.path.abspath(__file__))
BOARD = os.environ.get('R15_BOARD') or os.path.join(HERE, '..', 'airwriter.kicad_pcb')
F, B = pcbnew.F_Cu, pcbnew.B_Cu

PLACE = {   # ref: (x, y, orientation); a 0402 at 90 has pad 1 below, at 270 above
    # J2's, west of the socket. Q1 at 90: bottom row 1 GND, 2 SNS_P, 3 SNS; top row 6 SNS, 5 GND, 4 SNS_N.
    'Q1': (102.0, 155.4, 90),
    'R48': (101.0, 158.0, 270),   # 6.8 k, SNS_P (above) to GND
    'R47': (102.25, 158.0, 90),   # 22 k, SNS_P (above) to PR_TXD_P
    'R50': (103.5, 158.0, 90),    # 6.8 k, GND (above) to SNS_N
    'R49': (103.5, 160.1, 90),    # 15 k, SNS_N (above) to PR_TXD_N
    # J4's: Q2 above the gap between the sockets, its resistors down the gap.
    'Q2': (119.85, 152.55, 90),
    'R52': (122.75, 156.1, 270),  # 22 k, LT_PORT_RXD_P (above) to SNS_P
    'R53': (122.75, 158.0, 270),  # 6.8 k, SNS_P (above) to GND
    'R54': (122.75, 159.9, 270),  # 15 k, LT_PORT_RXD_N (above) to SNS_N
    'R55': (122.75, 161.8, 270),  # 6.8 k, SNS_N (above) to GND
    # The series resistors, between IO39's and IO38's vias and the collector lines' vias.
    'R51': (144.35, 151.2, 90),   # 4.7 k, J2_SENSE (IO39, above) to J2_SNS
    'R56': (143.08, 151.2, 90),   # 4.7 k, J4_SENSE (IO38, above) to J4_SNS
}
RENAME = {'/SPARE_IO39': '/J2_SENSE', '/SPARE_IO38': '/J4_SENSE'}
OWN = ('/J2_SNS', '/J2_SNS_P', '/J2_SNS_N', '/J4_SNS', '/J4_SNS_P', '/J4_SNS_N')

TRACKS = [   # (net, layer, [points][, width]); 0.2 mm unless given
    # J2
    ('/J2_SNS', F, [(101.35, 154.562), (101.35, 155.4), (102.65, 155.4), (102.65, 156.238)]),  # collectors
    ('/J2_SNS', F, [(101.35, 154.562), (101.35, 153.3)]),
    ('/GND', F, [(102.0, 154.562), (102.0, 153.55), (102.25, 153.3)]),
    ('/J2_SNS_P', F, [(102.0, 156.238), (102.0, 156.95), (102.25, 157.2), (102.25, 157.49)]),
    ('/J2_SNS_P', F, [(101.0, 157.49), (102.25, 157.49)]),
    ('/J2_SNS_N', F, [(102.65, 154.562), (104.15, 154.562), (104.15, 159.05), (103.5, 159.05)]),
    ('/J2_SNS_N', F, [(103.5, 158.51), (103.5, 159.59)]),
    ('/GND', F, [(103.5, 157.49), (103.5, 156.75)]),
    ('/GND', F, [(101.0, 158.51), (101.0, 159.3)]),
    ('/PR_TXD_P', F, [(102.25, 158.51), (102.25, 159.3)]),
    ('/PR_TXD_P', B, [(102.25, 159.3), (104.38, 157.17), (108.45, 157.17), (109.6, 156.02)]),
    ('/PR_TXD_N', F, [(103.5, 160.61), (103.5, 161.4)]),
    ('/PR_TXD_N', B, [(103.5, 161.4), (104.55, 160.35), (105.23, 159.67), (108.73, 159.67), (109.6, 158.8), (109.6, 158.52)]),  # between J2.5 and its shell
    # J4
    ('/J4_SNS', F, [(120.5, 153.388), (120.5, 152.55), (119.2, 152.55), (119.2, 151.712)]),  # collectors
    ('/J4_SNS', F, [(119.2, 151.712), (119.2, 150.6)]),
    ('/J4_SNS_N', F, [(120.5, 151.712), (121.3, 151.712), (121.3, 154.4), (123.85, 154.4), (123.85, 160.85), (122.75, 160.85)]),
    ('/J4_SNS_N', F, [(122.75, 160.41), (122.75, 161.29)]),
    ('/J4_SNS_P', F, [(119.85, 153.388), (119.85, 154.9), (123.45, 154.9), (123.45, 157.05), (122.75, 157.05)]),
    ('/J4_SNS_P', F, [(122.75, 156.61), (122.75, 157.49)]),
    ('/LT_PORT_RXD_P', F, [(122.75, 155.59), (121.45, 155.59)]),
    ('/LT_PORT_RXD_P', B, [(121.45, 155.59), (128.67, 155.59), (129.1, 156.02)]),
    ('/LT_PORT_RXD_N', F, [(122.75, 159.39), (121.45, 159.39)]),
    ('/LT_PORT_RXD_N', B, [(121.45, 159.39), (128.23, 159.39), (129.1, 158.52)]),
    ('/GND', F, [(122.75, 158.51), (121.45, 158.3)]),
    ('/GND', F, [(122.75, 162.31), (121.6, 162.6)]),
    # the series resistors
    ('/J2_SENSE', F, [(144.35, 149.9), (144.35, 150.69)]),
    ('/J2_SNS', F, [(144.35, 151.71), (144.35, 152.45)]),
    ('/J4_SENSE', F, [(143.08, 149.9), (143.08, 150.69)]),
    ('/J4_SNS', F, [(143.08, 151.71), (143.08, 152.45)]),
    # The collector lines across the back, J4's in the gap's north lane (over C16's ground via at
    # (128.8, 149.95)), J2's in its south lane, J2's ending east of J4's so they never cross.
    ('/J4_SNS', B, [(119.2, 150.6), (120.435, 149.365), (133.415, 149.365), (136.45, 152.4), (143.08, 152.4), (143.08, 152.45)]),
    ('/J2_SNS', B, [(101.35, 153.3), (102.05, 154.0), (112.5, 154.0), (114.8, 151.7), (120.5, 151.7), (121.6, 150.6),
                    (128.9, 150.6), (131.35, 153.05), (143.85, 153.05), (144.35, 152.55), (144.35, 152.45)]),
    # the front pour east of the series resistors keeps its ground through its own via
]
VIAS = [      # (net, point)
    ('/J2_SNS', (101.35, 153.3)), ('/GND', (102.25, 153.3)), ('/GND', (103.5, 156.75)),
    ('/GND', (101.0, 159.3)), ('/PR_TXD_P', (102.25, 159.3)), ('/PR_TXD_N', (103.5, 161.4)),
    ('/J4_SNS', (119.2, 150.6)), ('/LT_PORT_RXD_P', (121.45, 155.59)), ('/LT_PORT_RXD_N', (121.45, 159.39)),
    ('/GND', (121.45, 158.3)), ('/GND', (121.6, 162.6)),
    ('/J2_SNS', (144.35, 152.45)), ('/J4_SNS', (143.08, 152.45)), ('/GND', (145.4, 152.3)),
]
BOOT_AT = (143.1, 154.4)     # SW2's BOOT label, moved down off R51's and R56's pads
TP_LABEL_DY = 1.55           # the back's 38/39/40, moved down off the collector lines' vias


def main():
    e = Board(BOARD)
    b = e.b
    for old, new in RENAME.items():
        n_old, n_new = b.FindNet(old), b.FindNet(new)
        if n_old is None:
            continue
        for t in b.GetTracks():
            if t.GetNetname() == old:
                t.SetNet(n_new)
        for fp in b.GetFootprints():
            for p in fp.Pads():
                if p.GetNetname() == old:
                    p.SetNet(n_new)
    mine = {(round(a[0], 2), round(a[1], 2), round(c[0], 2), round(c[1], 2))
            for _, _, pts, *_ in TRACKS for a, c in zip(pts, pts[1:])}
    vias = {(round(p[0], 2), round(p[1], 2)) for _, p in VIAS}
    for t in list(b.GetTracks()):
        s = (round(mm(t.GetStart().x), 2), round(mm(t.GetStart().y), 2))
        f = (round(mm(t.GetEnd().x), 2), round(mm(t.GetEnd().y), 2))
        if isinstance(t, pcbnew.PCB_VIA):
            if t.GetNetname() in OWN or s in vias:
                e.drop(t)
        elif t.GetNetname() in OWN or s + f in mine or f + s in mine:
            e.drop(t)
    for ref, (x, y, deg) in PLACE.items():
        e.place(ref, x, y, deg)
        fp = b.FindFootprintByReference(ref)
        fp.Reference().SetLayer(pcbnew.F_Fab)
        if not fp.HasField('MPN'):                   # the schematic's is blank; r13_sync skips blanks
            fp.SetField('MPN', '')
            fp.GetField('MPN').SetVisible(False)
            fp.GetField('MPN').SetLayer(pcbnew.F_Fab)
    (boot,) = [g for g in b.FindFootprintByReference('SW2').GraphicalItems()
               if g.GetClass() == 'PCB_TEXT' and g.GetText() == 'BOOT']
    boot.SetPosition(pcbnew.VECTOR2I(nm(BOOT_AT[0]), nm(BOOT_AT[1])))
    for ref, label, y in (('TP10', '38', 152.5), ('TP11', '39', 153.5), ('TP12', '40', 152.5)):
        fp = b.FindFootprintByReference(ref)
        (t,) = [t for t in [fp.Reference(), fp.Value()] + list(fp.GraphicalItems())
                if t.GetClass() in ('PCB_FIELD', 'PCB_TEXT') and t.GetText() == label]
        t.SetPosition(pcbnew.VECTOR2I(t.GetPosition().x, nm(y + TP_LABEL_DY)))   # set outright: re-runnable
    b.FindFootprintByReference('TP10').SetValue('J4_SENSE')   # their fab values named the old nets
    b.FindFootprintByReference('TP11').SetValue('J2_SENSE')
    for net, layer, pts, *w in TRACKS:
        e.draw(net, pts, layer, *w)
    for net, p in VIAS:
        e.add_via(net, p)
    e.save()
    for ref in PLACE:
        fp = b.FindFootprintByReference(ref)
        print(ref, [(p.GetNumber(), round(mm(p.GetPosition().x), 3), round(mm(p.GetPosition().y), 3), p.GetNetname())
                    for p in fp.Pads()])


if __name__ == '__main__':
    main()
