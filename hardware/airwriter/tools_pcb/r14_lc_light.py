#!/usr/bin/env python3
# SPDX-License-Identifier: CERN-OHL-S-2.0
"""Rev 2026.11: JP1, the solder jumper that gives AirWriter LC its one light (KiCad python).

    <KiCad python> tools_pcb/r14_lc_light.py

The LC fits one LED of the 37, D26, the one under the test button. The chain
starts at D10, so with D10-D25 left off nothing reaches D26 through it. JP1
joins U9's output after R20 (LED_DATA5) straight to D26's input (LED_CH16):
open on AirWriter, bridged with solder on the LC.

Places JP1 in the square between D26, C21's dot, D20 and the dot below D26,
clear of the LC frame's tube round D26, labels it LC on the silk, and draws its
two runs with r13_route's grid search: LED_DATA5 east and down to TP5 on the
back, LED_CH16 under D26's VBUS and BTN_TEST runs to D26's input. LED_DATA5's
front run closes off two patches of the front ground pour, the ones D26, C46 and
C21 reach ground through, so each gets a via to the back plane. Runs after
r13_sync.py has brought JP1 in from the schematic, with KiCad closed. A board
that already has JP1's runs keeps them.
"""
import os
import sys

import pcbnew

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import r13_route as R    # noqa: E402

BOARD = R.BOARD
JP1 = (136.25, 104.5, 180)          # pad 1 (LED_DATA5) east, pad 2 (LED_CH16) west
LABEL = (136.25, 102.75)            # LC, just north of the pads
MARGIN = 5.0                        # corridor round each run's straight line, mm
RUNS = [('/LED_CH16', '2'), ('/LED_DATA5', '1')]
STITCH = [(132.75, 97.6),           # D26.2 and C46.2's patch, north of D26's GND pad
          (139.45, 101.5)]          # C21.2's patch, south of its GND pad


def at(x, y):
    return pcbnew.VECTOR2I(pcbnew.FromMM(x), pcbnew.FromMM(y))


def place(board):
    fp = board.FindFootprintByReference('JP1')
    if fp is None:
        sys.exit('no JP1 on the board: run r13_sync.py first')
    fp.SetPosition(at(JP1[0], JP1[1]))
    fp.SetOrientationDegrees(JP1[2])
    # the symbol is in the schematic's BOM (konnect places it so); make_fab.sh
    # leaves solder jumpers off PCBWay's
    fp.SetExcludedFromBOM(False)
    # a designator lives on F.Fab, as the passives' do
    fp.Reference().SetLayer(pcbnew.F_Fab)
    fp.Reference().SetTextAngleDegrees(0)
    fp.Reference().SetPosition(at(JP1[0], JP1[1] + 1.9))
    if not any(d.GetClass() == 'PCB_TEXT' and d.GetText() == 'LC' for d in board.GetDrawings()):
        t = pcbnew.PCB_TEXT(board)
        t.SetText('LC')
        t.SetLayer(pcbnew.F_SilkS)
        t.SetTextSize(pcbnew.VECTOR2I(pcbnew.FromMM(0.8), pcbnew.FromMM(0.8)))
        t.SetTextThickness(pcbnew.FromMM(0.15))
        t.SetPosition(at(*LABEL))
        board.Add(t)
    return fp


def main():
    board = pcbnew.LoadBoard(BOARD)
    fp = place(board)
    pads = {p.GetNumber(): p for p in fp.Pads()}
    for net, num in RUNS:
        pad = pads[num]
        if R.joined_items(board, pad) - {pad.m_Uuid.AsString()}:
            print(f'{net} from JP1.{num}: already routed')
            continue
        for clr in R.CLRS:
            g, ends = R.build(board, net, pad, 0.2, (R.F, R.B), MARGIN, clr)
            path = R.search(g, *ends)
            if path is not None:
                break
        if path is None:
            sys.exit(f'{net} from JP1.{num}: NO PATH within {MARGIN} mm')
        nt, nv, length = R.lay(board, g, path, net, 0.2, pad, ends[1])
        print(f'{net} from JP1.{num}: {nt} tracks, {nv} vias, {length:.1f} mm, clearance {clr}')
    gnd = board.FindNet('/GND')
    for x, y in STITCH:
        if any(t.Type() == pcbnew.PCB_VIA_T and t.GetPosition() == at(x, y) for t in board.GetTracks()):
            continue
        v = pcbnew.PCB_VIA(board)
        v.SetPosition(at(x, y))
        v.SetWidth(pcbnew.FromMM(R.VIA_D)); v.SetDrill(pcbnew.FromMM(R.VIA_DRILL))
        v.SetLayerPair(R.F, R.B); v.SetNet(gnd)
        board.Add(v)
        print(f'GND via at ({x}, {y})')
    pcbnew.ZONE_FILLER(board).Fill(board.Zones())
    board.Save(BOARD)
    print('JP1 placed and routed; zones refilled')


if __name__ == '__main__':
    main()
