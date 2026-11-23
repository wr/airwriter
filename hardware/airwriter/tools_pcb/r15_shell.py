#!/usr/bin/env python3
# SPDX-License-Identifier: CERN-OHL-S-2.0
"""W-1176: J2's shell goes straight to ground, as J4's does.

    kicad-cli sch export netlist --format kicadxml -o net.xml airwriter.kicad_sch
    <KiCad python> tools_pcb/r13_sync.py net.xml
    <KiCad python> tools_pcb/r15_shell.py

Run after r15_names.py, with KiCad closed. Re-runnable: a second run finds nothing
to remove.

Rev A gave the printer port's shell a tie that could be lifted: R8 (0 R) with C15
(1 nF) beside it, net /SHELL, in case a mains-powered printer's own ground put
noise on the link. It was never lifted, and since W-1176 either socket takes a
printer, so the two sockets are made the same: the schematic drops R8 and C15 and
puts J2's shell on GND, r13_sync.py removes their footprints and moves J2's shell
pads, and the copper on them, to GND, and this removes that copper: the run to
where R8 and C15 stood, and the back runs joining the three shell pads. The ground
pours then reach J2's shell pads as they reach J4's, with no tracks.
"""
import os

import pcbnew

from r13_edit import Board, mm, near

HERE = os.path.dirname(os.path.abspath(__file__))
BOARD = os.environ.get('R15_BOARD') or os.path.join(HERE, '..', 'airwriter.kicad_pcb')
DEAD = '/SHELL'
RUNS = [                                     # rev A's /SHELL copper, (start, end), mm
    ((103.0, 158.125), (103.0, 159.625)),    # F.Cu, R8.1 to C15.1
    ((104.355, 159.625), (103.0, 159.625)),  # F.Cu, on to J2's west shell pad
    ((106.25, 161.52), (104.355, 159.625)),
    ((106.25, 161.52), (112.2, 161.52)),     # B.Cu, the three shell pads joined
    ((112.2, 161.52), (113.0, 162.32)),
    ((113.0, 162.32), (118.95, 162.32)),
    ((118.95, 162.32), (119.75, 161.52)),
]


def is_run(t):
    if isinstance(t, pcbnew.PCB_VIA) or t.GetNetname() not in ('/GND', DEAD):
        return False
    s, e = (mm(t.GetStart().x), mm(t.GetStart().y)), (mm(t.GetEnd().x), mm(t.GetEnd().y))
    return any((near(s, a) and near(e, c)) or (near(s, c) and near(e, a)) for a, c in RUNS)


def main():
    e = Board(BOARD)
    for ref in ('R8', 'C15'):
        if e.b.FindFootprintByReference(ref):
            raise SystemExit(f'{ref} is still on the board: run r13_sync.py with a fresh netlist first')
    shell = [p for p in e.b.FindFootprintByReference('J2').Pads() if p.GetNumber() == '9']
    if len(shell) != 3 or any(p.GetNetname() != '/GND' for p in shell):
        raise SystemExit("J2's shell pads aren't all on /GND: run r13_sync.py with a fresh netlist first")
    dead = [t for t in e.b.GetTracks() if is_run(t)]
    if len(dead) not in (0, len(RUNS)):
        raise SystemExit(f'{len(dead)} of the shell\'s {len(RUNS)} runs found: the board has been edited')
    e.drop(*dead)
    e.save()
    print(f'{len(dead)} shell runs removed; J2\'s shell is on the ground pours')


if __name__ == '__main__':
    main()
