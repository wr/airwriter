#!/usr/bin/env python3
# SPDX-License-Identifier: CERN-OHL-S-2.0
"""W-1176: place the parts of the two-way ports.

    <KiCad python> tools_pcb/r15_place.py

Run after r13_sync.py has taken the schematic and r15_clean.py has removed the
copper to the old pads, with KiCad closed. Re-runnable: it sets positions and
orientations outright. r15_route.py draws the traces.

Each socket's THVD1452 keeps its place above the socket, logic pins toward the
PIC that shares its data and enable lines. U10, J2's PIC, stands in U8's old
spot with pins 2 and 3 facing U2, and the resistors that join the ESP32's
lines to those two nets sit in the gap between them. J4's stand where the
nets they join already run.
"""
import os

import pcbnew

from r13_edit import Board, nm

HERE = os.path.dirname(os.path.abspath(__file__))
BOARD = os.environ.get('R15_BOARD') or os.path.join(HERE, '..', 'airwriter.kicad_pcb')

PLACE = {  # ref: (x, y, orientation); a 0603's pad 1 is west at 0, south at 90, north at -90
    # J2
    'U10': (124.70, 148.80, 0),      # J2's PIC, on U8's old spot: pins 2 and 3 face U2, J4's handshakes pass under it
    'C68': (122.03, 144.50, -90),    # U10's bypass, pad 1 on the 3.3 V run to pin 1
    'R37': (118.63, 146.875, -90),   # 2.2 k, U2's RO (north) to LT2_DATA (south, on the U2.4-U10.2 run)
    'R38': (120.18, 146.875, -90),   # 220 R, IO17 (north) to LT2_DATA
    'R36': (118.63, 149.925, -90),   # 10 k, LT2_DE (north, on the U2.3-U10.3 run) to GND
    'R39': (120.18, 149.925, 90),    # 220 R, IO7 (south) to LT2_DE
    'C3': (112.90, 150.80, 180),     # U2's bypass, below its VCC pin
    'R29': (112.75, 153.10, 90),     # 0.6 mm south, for C3
    'R40': (117.05, 152.25, -90),    # 4.7 k, 3.3 V (C3's old pad) to TxD+ on its way to J2.6
    'R41': (119.75, 152.90, 0),      # 4.7 k, TxD- on its way to J2.3, to GND
    # J4
    'U6': (132.45, 148.40, 0),       # over J4's centre line; U7's two lines and RO come in under it
    'C14': (134.20, 145.80, 0),      # U6's bypass, pad 1 on the 3.3 V run into pin 10
    'R42': (130.15, 145.00, 0),      # 2.2 k, RO (west) to LT_DATA (east), below U7's label; LT_DE passes between its pads
    'R43': (134.12, 135.775, -90),    # 220 R, IO9 (north) to LT_DATA, between U7 and the module with a via each end
    'R44': (135.80, 128.60, 90),     # 220 R, IO3 (south) to LT_DE, north of the module's pin and west of dot (7,4)'s light:
                                     # IO3 has no other way out
    'R10': (128.75, 153.07, -90),    # 15 k, U6's B to J4.5
    'R11': (130.30, 152.00, -90),    # 15 k, U6's A to J4.8; B passes between its pads
    'R46': (131.85, 152.05, -90),    # 4.7 k, TxD- to GND
    'R12': (133.40, 152.05, -90),    # 10 R, TxD- to J4.3
    'R13': (134.95, 152.05, -90),    # 10 R, TxD+ to J4.6
    'R45': (136.50, 152.05, -90),    # 4.7 k, 3.3 V to TxD+
    # U10's programming lines and U7's reset
    'R35': (134.12, 139.625, -90),   # 100 R, IO12 (north) to U10's ICSPDAT, below R43 with a via each end
    'R34': (127.10, 145.00, 0),      # 100 R, IO11 (west) to U10's ICSPCLK, below U7's label
    'R27': (122.60, 130.60, 0),      # 1 k, LT_RESET (west) to U7.4, north-west of U7: R43 and R35 have its old place
}


def main():
    e = Board(BOARD)
    for ref, (x, y, deg) in PLACE.items():
        e.place(ref, x, y, deg)
    for ref in list(PLACE) + ['U2']:                 # references on the fab layer, like the rest of the board
        e.b.FindFootprintByReference(ref).Reference().SetLayer(pcbnew.F_Fab)
    # U7's TASHTALK label and reference stay where rev 2026.11 has them: nothing else stands in the label's box
    u7 = e.b.FindFootprintByReference('U7')
    label = [f for f in u7.GetFields() if f.GetName() == 'Name'][0]
    label.SetPosition(pcbnew.VECTOR2I(nm(127.9), nm(143.0)))
    label.SetTextAngleDegrees(0)
    u7.Reference().SetPosition(pcbnew.VECTOR2I(nm(127.9), nm(136.9)))
    u7.Reference().SetTextAngleDegrees(90)
    e.save()
    print(f'{len(PLACE)} parts placed')


if __name__ == '__main__':
    main()
