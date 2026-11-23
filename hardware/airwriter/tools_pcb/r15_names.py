#!/usr/bin/env python3
# SPDX-License-Identifier: CERN-OHL-S-2.0
"""W-1176: name the sockets Port 1 and Port 2.

    <KiCad python> tools_pcb/r15_names.py

Run after r15_silk.py, with KiCad closed. Re-runnable: it sets the names outright.

Since W-1176 either socket takes a serial printer or LocalTalk, so neither is named
for one job. J2 is Port 1 and J4 Port 2: the back's labels under the sockets, SERIAL
and LOCALTALK, become PORT 1 and PORT 2, in the capitals of the back's other labels,
and the sockets' values follow the schematic's (Mini-DIN-8 Port 1 and Port 2).
"""
import os

import pcbnew

from r13_edit import Board

HERE = os.path.dirname(os.path.abspath(__file__))
BOARD = os.environ.get('R15_BOARD') or os.path.join(HERE, '..', 'airwriter.kicad_pcb')

LABELS = {'SERIAL': 'PORT 1', 'LOCALTALK': 'PORT 2'}           # the back's labels under J2 and J4
VALUES = {'J2': 'Mini-DIN-8 Port 1', 'J4': 'Mini-DIN-8 Port 2'}


def main():
    e = Board(BOARD)
    texts = [d for d in e.b.GetDrawings()
             if d.GetClass() == 'PCB_TEXT' and d.GetLayer() == pcbnew.B_SilkS]
    named = 0
    for d in texts:
        if d.GetText() in LABELS:
            d.SetText(LABELS[d.GetText()])
        if d.GetText() in LABELS.values():
            named += 1
    if named != len(LABELS):
        raise SystemExit(f'{named} socket labels on the back, not {len(LABELS)}')
    for ref, value in VALUES.items():
        e.b.FindFootprintByReference(ref).SetValue(value)
    e.save()
    print(f'{named} socket labels and {len(VALUES)} socket values named Port 1 and Port 2')


if __name__ == '__main__':
    main()
