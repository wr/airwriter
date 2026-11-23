#!/usr/bin/env python3
# SPDX-License-Identifier: CERN-OHL-S-2.0
"""W-1176: remove the copper the two-way ports draw again.

    <KiCad python> tools_pcb/r15_clean.py

Run after r13_sync.py has taken the schematic (the order is in r15_route.py),
with KiCad closed. Both sockets get the same block of parts and the same
copper, so nothing of the old port wiring is kept:

  1. every track and via on a net of the two blocks, on the four handshake
     lines to the sockets, or on a net the schematic no longer has, goes
     (WIPE);
  2. the 3.3 V and ground copper that fed the old parts, named piece by
     piece, goes (DROP). A piece that is not where it is named stops the
     script: the board is not the one this was written against.

r15_place.py sets the parts and r15_route.py draws every run again. A second
run finds nothing to remove and says so.
"""
import os

import pcbnew

from r13_edit import Board, near, mm

HERE = os.path.dirname(os.path.abspath(__file__))
BOARD = os.environ.get('R15_BOARD') or os.path.join(HERE, '..', 'airwriter.kicad_pcb')
F, B = 'F', 'B'

WIPE = {
    # J2's block
    '/UART_RX', '/UART_TX', '/J2_DE', '/LT2_DATA', '/LT2_DE', '/LT2_MCLR', '/LT2_CTS',
    '/LT2_ICSPCLK', '/LT2_ICSPDAT', '/LT2_UART_TX', '/LT2_UART_RX',
    '/PR_RXD_P', '/PR_RXD_N', '/PR_TXD_P', '/PR_TXD_N', '/PR_TXD_P_IN', '/PR_TXD_N_IN',
    # J4's block
    '/J4_RX', '/J4_TX', '/J4_DE', '/LT_DATA', '/LT_DE', '/LT_RESET', '/LT_CTS',
    '/LT_ICSPCLK', '/LT_ICSPDAT', '/LT_UART_TX', '/LT_UART_RX',
    '/LT_TXD_P', '/LT_TXD_N', '/LT_RXD_P', '/LT_RXD_N', '/LT_PORT_RXD_P', '/LT_PORT_RXD_N',
    # the handshake lines from U3 to the sockets
    '/PR_HSKI', '/PR_HSKO', '/J4_HSKI', '/J4_HSKO',
}

# (net, layer, a, b) tracks and (net, 'via', p) vias
DROP = [
    # 3.3 V: U3's feed under its body to the old U2, U8, C3 and C18. U3.16 keeps its own feed from C17.
    ('/+3V3', F, (120.05, 132.62), (118.8, 132.62)),
    ('/+3V3', F, (118.8, 132.62), (117.3, 134.12)),
    ('/+3V3', F, (117.3, 134.12), (117.3, 144.24)),
    ('/+3V3', F, (117.404, 144.2405), (121.2371, 144.2405)),
    ('/+3V3', F, (115.4, 146.2445), (117.404, 144.2405)),
    ('/+3V3', F, (115.4, 151.575), (115.4, 146.2445)),
    ('/+3V3', F, (115.4, 151.575), (116.85, 151.575)),
    ('/+3V3', F, (116.85, 151.575), (116.9, 151.625)),
    ('/+3V3', F, (117.175, 150.105), (116.9, 150.38)),
    ('/+3V3', F, (116.9, 150.38), (116.9, 151.625)),
    ('/+3V3', F, (122.3509, 145.3543), (121.2371, 144.2405)),
    ('/+3V3', F, (122.3509, 150.105), (122.3509, 145.3543)),
    ('/+3V3', F, (120.925, 150.105), (122.3509, 150.105)),
    ('/+3V3', F, (120.925, 150.105), (121.55, 150.73)),
    ('/+3V3', F, (121.55, 150.73), (121.55, 151.62)),
    # 3.3 V: from U7's socket pin 1 to C16, U6, C14, R25 and, by the back, R21
    ('/+3V3', F, (132.5, 132.3), (132.5, 130.8)),
    ('/+3V3', F, (131.71, 133.09), (132.5, 132.3)),
    ('/+3V3', F, (132.9267, 134.3067), (131.71, 133.09)),
    ('/+3V3', F, (132.9267, 146.295), (132.9267, 134.3067)),
    ('/+3V3', F, (133.275, 146.295), (132.9267, 146.295)),
    ('/+3V3', F, (132.9267, 146.295), (132.7178, 146.295)),
    ('/+3V3', F, (132.7178, 146.295), (131.8927, 147.1201)),
    ('/+3V3', F, (131.8927, 147.1201), (131.8927, 151.2177)),
    ('/+3V3', F, (131.8927, 151.2177), (132.3, 151.625)),
    ('/+3V3', F, (132.1, 150.825), (131.8927, 150.825)),
    ('/+3V3', F, (132.125, 144.6), (132.927, 144.6)),
    ('/+3V3', F, (136.0, 151.825), (135.6, 151.625)),
    ('/+3V3', F, (135.6, 151.625), (134.4, 151.625)),
    ('/+3V3', B, (134.4, 151.625), (132.9, 151.625)),
    ('/+3V3', B, (139.2, 151.5), (134.6, 151.5)),
    ('/+3V3', B, (134.6, 151.5), (134.4, 151.625)),
    ('/+3V3', B, (132.9, 151.625), (132.1, 150.825)),
    ('/+3V3', 'via', (134.4, 151.625)),
    ('/+3V3', 'via', (132.1, 150.825)),
    # ground: U7's and U6's runs, and vias that new pads or runs land on
    ('/GND', F, (129.17, 133.09), (130.2295, 134.1495)),
    ('/GND', F, (130.2295, 134.1495), (130.2295, 148.5189)),
    ('/GND', F, (130.23, 144.6), (130.475, 144.6)),
    ('/GND', F, (130.2295, 148.5189), (128.6434, 150.105)),
    ('/GND', F, (128.6434, 150.105), (128.325, 150.105)),
    ('/GND', F, (128.325, 150.105), (125.875, 150.105)),
    ('/GND', F, (118.05, 143.25), (118.9, 143.25)),
    ('/GND', 'via', (118.05, 143.25)),
    ('/GND', 'via', (111.45, 152.6)),
    ('/GND', 'via', (112.9201, 143.6353)),
    ('/GND', 'via', (117.611, 145.401)),
    ('/GND', 'via', (120.925, 146.74)),
    ('/GND', 'via', (133.3, 150.97)),
    ('/GND', 'via', (134.9829, 152.9795)),
    # R24 moves: its end of HSKI_DRV
    ('/HSKI_DRV', F, (119.75, 142.34), (120.2, 142.79)),
]


def main():
    e = Board(BOARD)
    if any(t.GetNetname() == '/LT2_DATA' for t in e.b.GetTracks()):      # only r15_route.py draws this net
        print('already drawn: nothing removed')
        return
    padded = {p.GetNetname() for fp in e.b.GetFootprints() for p in fp.Pads()}
    wiped = 0
    for t in e.b.GetTracks():
        if t.GetNetname() in WIPE or t.GetNetname() not in padded:
            e.drop(t)
            wiped += 1
    named = 0
    tracks = [t for t in e.b.GetTracks() if t not in e.dead]
    for item in DROP:
        if item[1] == 'via':                # by place alone: KiCad gives a free via the net of a pad put on it
            hit = [t for t in tracks if isinstance(t, pcbnew.PCB_VIA)
                   and near((mm(t.GetPosition().x), mm(t.GetPosition().y)), item[2], 0.01)]
        else:
            layer = pcbnew.F_Cu if item[1] == F else pcbnew.B_Cu
            hit = []
            for t in tracks:
                if isinstance(t, pcbnew.PCB_VIA) or t.GetNetname() != item[0] or t.GetLayer() != layer:
                    continue
                s, c = (mm(t.GetStart().x), mm(t.GetStart().y)), (mm(t.GetEnd().x), mm(t.GetEnd().y))
                if (near(s, item[2]) and near(c, item[3])) or (near(s, item[3]) and near(c, item[2])):
                    hit.append(t)
        if len(hit) == 1:
            e.drop(hit[0])
            named += 1
        elif not (wiped == 0 and not hit):       # a clean board has none of them; anything else is the wrong board
            raise SystemExit(f'missing {item}')
    if not (wiped or named):
        print('nothing to remove')
        return
    e.save()
    print(f'{wiped} items on the ports\' nets removed, {named} of {len(DROP)} named items removed')


if __name__ == '__main__':
    main()
