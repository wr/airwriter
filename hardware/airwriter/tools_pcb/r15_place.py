#!/usr/bin/env python3
# SPDX-License-Identifier: CERN-OHL-S-2.0
"""W-1176: place the parts of the two-way ports.

    <KiCad python> tools_pcb/r15_place.py

Run after r13_sync.py and r15_clean.py (the order is in r15_route.py), with
KiCad closed. Re-runnable: it sets positions and orientations outright.

Each socket has the same block of fourteen parts, J4's 19.5 mm east of J2's,
part for part (BLOCK, measured from the socket's centre line):

  - the THVD1452 over the centre line, bus pins east;
  - its PIC to the west, pins 2 and 3 facing it, clear of the LC's case leg
    at H3 on J2's side, with its bypass below pin 1;
  - a row of three above the transceiver: the 220 R from the GPIO to DE, the
    2.2 k from RO to the data net, the 220 R from the GPIO to the data net;
  - a row of three above the PIC: the two 100 R programming resistors over
    the pins they serve, and DE's 10 k pull-down;
  - a row below the transceiver, over the socket's pins: the two 15 k receive
    legs, the 4.7 k bias pair, and the transceiver's bypass between them.

J2's two receive legs are turned half a turn from J4's: the schematic gives
their pads the other way round, and the copper is the same.

R24 turns to stand between the handshake lines that now pass below U3. The
OSHW logo and the WW mark move up the left column, clear of J2's PIC.
"""
import os

import pcbnew

from r13_edit import Board, nm, mm

HERE = os.path.dirname(os.path.abspath(__file__))
BOARD = os.environ.get('R15_BOARD') or os.path.join(HERE, '..', 'airwriter.kicad_pcb')

J2_X, J4_X = 113.0, 132.5       # the sockets' centre lines
# (J2's part, J4's part): (x from the centre line, y, orientation). A 0603's pad 1 is west at 0, south at 90,
# north at -90; a list gives J2's and J4's orientation where they differ.
BLOCK = {
    ('U2', 'U6'): (0.0, 148.40, 0),          # logic pins west, bus pins east
    ('U10', 'U7'): (-7.4, 145.06, 180),      # pins 1 to 4 east, 1 at the bottom
    ('R39', 'R44'): (-2.4, 145.05, -90),     # 220 R, GPIO (north) to DE
    ('R37', 'R42'): (-0.85, 145.05, 90),     # 2.2 k, RO (south) to the data net
    ('R38', 'R43'): (0.7, 145.05, -90),      # 220 R, GPIO (north) to the data net
    ('R34', 'R15'): (-8.05, 140.75, -90),    # 100 R, GPIO (north) to ICSPCLK, over pin 6's run
    ('R35', 'R16'): (-6.45, 140.75, -90),    # 100 R, GPIO (north) to ICSPDAT
    ('R36', 'R22'): (-3.05, 140.75, 90),     # 10 k, DE (south) to GND
    ('C68', 'C16'): (-5.2, 148.58, 0),       # the PIC's bypass, 3.3 V (west) below pin 1
    ('R30', 'R10'): (-3.75, 153.07, (90, -90)),   # 15 k, the transceiver's B (north) to the socket's pin 5
    ('R29', 'R11'): (-2.2, 152.05, (90, -90)),    # 15 k, A (north) to pin 8; B passes between its pads
    ('R41', 'R46'): (0.7, 152.05, -90),      # 4.7 k, Z (north) to GND
    ('C3', 'C14'): (2.25, 152.05, -90),      # the transceiver's bypass, 3.3 V north
    ('R40', 'R45'): (3.8, 152.05, -90),      # 4.7 k, 3.3 V (north) to Y
}
OTHER = {
    'R24': (119.575, 143.90, -90),           # HSKI_DRV (north) to GND, between J2's and J4's handshake lines
}
LOGO_AT = (105.3, 134.2)                     # LOGO1, the OSHW mark
WW_BOX = (101.5, 131.0, 109.1, 138.0)        # where r13_place.py drew the WW mark
WW_UP = 7.55                                 # mm it moves north, to sit above the OSHW mark


def place_list():
    out = {}
    for (a, b), (dx, y, deg) in BLOCK.items():
        da, db = deg if isinstance(deg, tuple) else (deg, deg)
        out[a] = (J2_X + dx, y, da)
        out[b] = (J4_X + dx, y, db)
    out.update(OTHER)
    return out


def main():
    e = Board(BOARD)
    place = place_list()
    for ref, (x, y, deg) in place.items():
        e.place(ref, x, y, deg)
        e.b.FindFootprintByReference(ref).Reference().SetLayer(pcbnew.F_Fab)   # references on the fab layer,
                                                                               # like the rest of the board
    logo = e.b.FindFootprintByReference('LOGO1')
    logo.SetPosition(pcbnew.VECTOR2I(nm(LOGO_AT[0]), nm(LOGO_AT[1])))
    moved = 0
    for d in e.b.GetDrawings():
        if d.GetClass() != 'PCB_SHAPE' or d.GetLayer() != pcbnew.F_SilkS:
            continue
        bb = d.GetBoundingBox()
        if (mm(bb.GetLeft()) >= WW_BOX[0] and mm(bb.GetTop()) >= WW_BOX[1]
                and mm(bb.GetRight()) <= WW_BOX[2] and mm(bb.GetBottom()) <= WW_BOX[3]):
            d.Move(pcbnew.VECTOR2I(0, -nm(WW_UP)))
            moved += 1
    if moved == 0:                           # a second run: the mark must already be up
        up = (WW_BOX[0], WW_BOX[1] - WW_UP, WW_BOX[2], WW_BOX[3] - WW_UP)
        moved_already = sum(1 for d in e.b.GetDrawings()
                            if d.GetClass() == 'PCB_SHAPE' and d.GetLayer() == pcbnew.F_SilkS
                            and mm(d.GetBoundingBox().GetLeft()) >= up[0] - 0.01
                            and mm(d.GetBoundingBox().GetTop()) >= up[1] - 0.01
                            and mm(d.GetBoundingBox().GetRight()) <= up[2] + 0.01
                            and mm(d.GetBoundingBox().GetBottom()) <= up[3] + 0.01)
        # None at all is right once r15_silk.py has moved the mark to the back's nameplate.
        nameplate = any(d.GetLayer() == pcbnew.B_SilkS and d.GetClass() == 'PCB_SHAPE'
                        for d in e.b.GetDrawings())
        if moved_already != 2 and not (moved_already == 0 and nameplate):
            raise SystemExit(f'the WW mark is in neither place ({moved_already} pieces up)')
    elif moved != 2:
        raise SystemExit(f'{moved} pieces of the WW mark found, not 2')
    e.save()
    print(f'{len(place)} parts placed, the OSHW logo and {moved} pieces of the WW mark moved')


if __name__ == '__main__':
    main()
