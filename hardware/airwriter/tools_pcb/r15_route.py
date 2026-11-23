#!/usr/bin/env python3
# SPDX-License-Identifier: CERN-OHL-S-2.0
"""W-1176: draw the traces of the two-way ports.

    <KiCad python> tools_pcb/r15_route.py

The board is built in this order from rev 2026.11's board before W-1176 (the
one r14_lc_light.py left, commit 7ba7f07), with KiCad closed:

    kicad-cli sch export netlist --format kicadxml -o net.xml airwriter.kicad_sch
    <KiCad python> tools_pcb/r13_sync.py net.xml
    <KiCad python> tools_pcb/r15_clean.py
    <KiCad python> tools_pcb/r15_place.py
    <KiCad python> tools_pcb/r15_route.py
    <KiCad python> tools_pcb/r15_silk.py

R15_BOARD=<path> runs the whole chain, r13_sync.py too, on another copy of
the board. Every run is drawn point by point. A board that already has LT2_DATA drawn is
left alone.

The two blocks. block() draws one socket's copper from its centre line, so
J4's is J2's 19.5 mm east, track for track:

  - DE and the data net leave the transceiver west and turn north to the
    PIC's pins 3 and 2; DE goes on up to its pull-down, and the 220 R from
    the GPIO hangs on it. RO goes north from its pad, the data net north
    under the transceiver, and the 2.2 k stands between them.
  - A, B and Z leave under the transceiver to the south. B passes between
    the pads of A's resistor, so each reaches its socket pin in the socket's
    order; Z goes down between pins 8 and 7 and along between the two rows
    to pin 3; Y goes straight down to pin 6.
  - 3.3 V comes in at a via above the bypass, goes up the transceiver's east
    side to its pin 10, and along the back to a via at the PIC's bypass. The
    back run passes north of the transceiver's ground via, so pin 5 reaches
    the bypass and the socket's ground pin across the back plane, not round
    the run's ends.
  - Each line from the ESP32 ends at the same place in each block (PORTS).
    J2's arrive on the back, so each has a via there; J4's arrive on the
    front, but for LT_CTS, which keeps rev A's run on the back.

J2's lines from the module. IO17, IO18 and IO7 (north edge) and IO41 and IO42
(south edge) run on the front under the module to a via each, then west on
the back in one band at y 145.3 to 146.9, under J4's block, to their ports.
IO11 and IO12 take a via beside the pin, as IO13 does, and run west on the
back at y 137.6 and 138.05, under U3, to the two programming resistors.

J4's lines. IO9 and IO10 take a via beside the pin, the back under BTN_TEST,
and a second via; IO3 has no room for a via at its pin (IO46's pad is beside
it), so it runs north first. From there, and from rev A's vias for LT_RESET,
LT_UART_TX and LT_UART_RX, each runs straight south on the front to its part.

The handshake lines. J2's leave U3's west side, pass below U3 and run down
between the two blocks and round below the socket's lower row to pins 1 and
2. J4's run down U3's east side, west of J4's PIC, to rev 2026.11's two vias
and its back runs to pins 1 and 2.

3.3 V. J4's block is fed from the via that stands where U7's socket had its
pin 1, by a run down to U6's pin 10; J2's from J4's along the back. R21 is
fed from J4's via.
"""
import os

import pcbnew

from r13_edit import Board, mm

HERE = os.path.dirname(os.path.abspath(__file__))
BOARD = os.environ.get('R15_BOARD') or os.path.join(HERE, '..', 'airwriter.kicad_pcb')
F, B = pcbnew.F_Cu, pcbnew.B_Cu

J2_X, J4_X = 113.0, 132.5
NETS = {    # the block's lines by what they do: J2's net, J4's net
    'RO': ('/UART_RX', '/J4_RX'), 'TX': ('/UART_TX', '/J4_TX'), 'GPIO_DE': ('/J2_DE', '/J4_DE'),
    'DATA': ('/LT2_DATA', '/LT_DATA'), 'DE': ('/LT2_DE', '/LT_DE'),
    'MCLR': ('/LT2_MCLR', '/LT_RESET'), 'CTS': ('/LT2_CTS', '/LT_CTS'),
    'CLK': ('/LT2_ICSPCLK', '/LT_ICSPCLK'), 'DAT': ('/LT2_ICSPDAT', '/LT_ICSPDAT'),
    'UTX': ('/LT2_UART_TX', '/LT_UART_TX'), 'URX': ('/LT2_UART_RX', '/LT_UART_RX'),
    'A': ('/PR_TXD_P_IN', '/LT_RXD_P'), 'B': ('/PR_TXD_N_IN', '/LT_RXD_N'),
    'AJ': ('/PR_TXD_P', '/LT_PORT_RXD_P'), 'BJ': ('/PR_TXD_N', '/LT_PORT_RXD_N'),
    'Z': ('/PR_RXD_N', '/LT_TXD_N'), 'Y': ('/PR_RXD_P', '/LT_TXD_P'),
    'V': ('/+3V3', '/+3V3'), 'G': ('/GND', '/GND'),
}
# One socket's copper, x from its centre line: (line, layer or 'via', points, width)
BLOCK = [
    # DE: west from the transceiver, north past the PIC's pin 3 to the pull-down; the GPIO's 220 R on the way
    ('DE', F, [(-2.1, 148.4), (-3.25, 148.4), (-3.25, 141.9), (-3.05, 141.7), (-3.05, 141.575)], 0.2),
    ('DE', F, [(-3.25, 144.425), (-4.925, 144.425)], 0.2),
    ('DE', F, [(-3.25, 145.875), (-2.4, 145.875)], 0.2),
    # the data net: west and north to the PIC's pin 2; north under the transceiver to the 2.2 k and the 220 R
    ('DATA', F, [(-2.1, 148.9), (-3.65, 148.9), (-3.65, 145.695), (-4.925, 145.695)], 0.2),
    ('DATA', F, [(-2.1, 148.9), (-0.2, 148.9), (-0.2, 146.75), (-0.075, 146.625), (-0.075, 144.225),
                 (-0.85, 144.225)], 0.2),
    ('DATA', F, [(-0.075, 145.875), (0.7, 145.875)], 0.2),
    # RO: north from its pad, past the 2.2 k, to its port
    ('RO', F, [(-2.1, 147.4), (-1.625, 147.4), (-1.625, 143.3)], 0.2),
    ('RO', F, [(-1.625, 145.875), (-0.85, 145.875)], 0.2),
    ('GPIO_DE', F, [(-2.4, 144.225), (-2.4, 143.3)], 0.2),
    ('TX', F, [(0.7, 144.225), (0.7, 143.3)], 0.2),
    # the PIC: MCLR and CTS north from their pads, the programming lines north under it to their resistors
    ('MCLR', F, [(-4.925, 143.155), (-4.6, 143.155), (-4.6, 142.2)], 0.2),
    ('CTS', F, [(-9.875, 143.155), (-9.1, 143.155), (-9.1, 141.4)], 0.2),
    ('CLK', F, [(-9.875, 144.425), (-8.05, 144.425), (-8.05, 141.575)], 0.2),
    ('DAT', F, [(-9.875, 145.695), (-6.45, 145.695), (-6.45, 141.575)], 0.2),
    ('UTX', F, [(-8.05, 139.925), (-8.05, 138.95)], 0.2),
    ('URX', F, [(-6.45, 139.925), (-6.45, 138.95)], 0.2),
    # the bus pins: A to its 15 k, B between that resistor's pads to its own, both on to pins 8 and 5
    ('A', F, [(2.1, 147.9), (0.2, 147.9), (0.2, 150.0), (-1.9, 150.0), (-2.2, 150.3), (-2.2, 151.225)], 0.2),
    ('B', F, [(2.1, 148.4), (0.6, 148.4), (0.6, 150.4), (-0.9, 150.4), (-1.3, 150.8), (-1.3, 151.65),
              (-1.7, 152.05), (-3.2, 152.05), (-3.75, 152.245)], 0.2),
    ('AJ', F, [(-2.2, 152.875), (-2.2, 153.6), (-3.4, 154.8), (-3.4, 156.02)], 0.2),
    ('BJ', F, [(-3.75, 153.895), (-4.55, 154.695), (-4.55, 157.37), (-3.4, 158.52)], 0.2),
    # Z to its 4.7 k, then between pins 8 and 7 and between the rows to pin 3; Y down to pin 6, its 4.7 k on the pin
    ('Z', F, [(2.1, 148.9), (1.0, 148.9), (1.0, 151.0), (0.7, 151.225)], 0.2),
    ('Z', F, [(0.7, 151.225), (-0.2, 151.225), (-0.65, 151.675), (-0.65, 153.9), (-2.15, 155.4), (-2.15, 156.45),
              (-1.33, 157.27), (2.15, 157.27), (3.4, 158.52)], 0.2),
    ('Y', F, [(2.1, 149.4), (1.475, 150.025), (1.475, 153.9), (2.975, 155.4), (3.4, 155.6), (3.4, 156.02)], 0.2),
    ('Y', F, [(3.8, 152.875), (3.8, 154.9), (3.4, 155.3)], 0.2),
    # 3.3 V: the via above the bypass, the bypass and the 4.7 k, up to pin 10; the back to the PIC's bypass and pin 1
    ('V', 'via', [(3.025, 150.2)], None),
    ('V', F, [(3.025, 150.2), (2.25, 150.975), (2.25, 151.275)], 0.25),
    ('V', F, [(3.025, 150.2), (3.8, 150.975), (3.8, 151.225)], 0.25),
    ('V', F, [(3.025, 150.2), (3.15, 150.075), (3.15, 147.7), (2.85, 147.4), (2.1, 147.4)], 0.2),
    ('V', B, [(3.025, 150.2), (1.75, 148.925), (-6.675, 148.925), (-7.0, 148.6), (-7.0, 148.58)], 0.3),   # north of pin 5's ground via
    ('V', 'via', [(-7.0, 148.58)], None),
    ('V', F, [(-7.0, 148.58), (-5.975, 148.58)], 0.3),
    ('V', F, [(-5.975, 148.58), (-5.975, 146.965), (-4.925, 146.965)], 0.3),
    # ground: the transceiver's pin 2 under its body; its pin 5 and the PIC's bypass; the PIC's pin 8; the
    # pull-down; the bypass; Z's 4.7 k to the socket's ground pin
    ('G', 'via', [(-0.8, 147.9)], None),
    ('G', F, [(-2.1, 147.9), (-0.8, 147.9)], 0.2),
    ('G', 'via', [(-3.7, 149.95)], None),
    ('G', F, [(-2.1, 149.4), (-3.0, 149.4), (-3.55, 149.95), (-3.7, 149.95)], 0.2),
    ('G', F, [(-4.425, 148.58), (-4.425, 149.5), (-3.975, 149.95), (-3.7, 149.95)], 0.2),
    ('G', 'via', [(-9.875, 148.0)], None),
    ('G', F, [(-9.875, 146.965), (-9.875, 148.0)], 0.2),
    ('G', 'via', [(-4.0, 139.925)], None),
    ('G', F, [(-3.05, 139.925), (-4.0, 139.925)], 0.2),
    ('G', 'via', [(2.6, 153.95)], None),
    ('G', F, [(2.25, 152.825), (2.25, 153.6), (2.6, 153.95)], 0.2),
    ('G', F, [(0.7, 152.875), (0.7, 154.7), (1.3, 155.3), (1.3, 156.02)], 0.2),
]
# where each line from the ESP32 meets the block: J2's have a via here
PORTS = {'RO': (-1.625, 143.3), 'GPIO_DE': (-2.4, 143.3), 'TX': (0.7, 143.3), 'MCLR': (-4.6, 142.2),
         'CTS': (-9.1, 141.4), 'UTX': (-8.05, 138.95), 'URX': (-6.45, 138.95)}


def port(x0, name):
    return (round(x0 + PORTS[name][0], 4), PORTS[name][1])


def block(x0, which):
    out = []
    for line, layer, pts, width in BLOCK:
        out.append((NETS[line][which], layer, [(round(x0 + x, 4), y) for x, y in pts], width))
    return out


P2 = {k: port(J2_X, k) for k in PORTS}
P4 = {k: port(J4_X, k) for k in PORTS}

LINES = [
    # ---- J2's lines from the module ----
    # IO17 (UART_TX), IO18 (UART_RX), IO7 (J2_DE): the front under the module, a via, the back west
    ('/UART_TX', F, [(143.08, 131.044), (143.08, 144.6)], 0.2),
    ('/UART_TX', 'via', [(143.08, 144.6)], None),
    ('/UART_TX', B, [(143.08, 144.6), (142.38, 145.3), (114.4, 145.3), (113.7, 144.6), P2['TX']], 0.2),
    ('/UART_TX', 'via', [P2['TX']], None),
    ('/UART_RX', F, [(141.81, 131.044), (141.81, 145.2), (140.95, 146.06)], 0.2),
    ('/UART_RX', 'via', [(140.95, 146.06)], None),
    ('/UART_RX', B, [(140.95, 146.06), (140.59, 145.7), (111.975, 145.7), (111.375, 145.1), P2['RO']], 0.2),
    ('/UART_RX', 'via', [P2['RO']], None),
    ('/J2_DE', F, [(146.89, 131.044), (146.89, 132.9), (144.2, 135.59), (144.2, 145.2), (142.5, 146.9)], 0.2),
    ('/J2_DE', 'via', [(142.5, 146.9)], None),
    ('/J2_DE', B, [(142.5, 146.9), (141.6, 147.8), (138.5, 147.8), (136.8, 146.1), (111.2, 146.1), (110.6, 145.5),
                   P2['GPIO_DE']], 0.2),
    ('/J2_DE', 'via', [P2['GPIO_DE']], None),
    # IO42 (LT2_MCLR), IO41 (LT2_CTS): the same from the module's south edge
    ('/LT2_MCLR', F, [(148.16, 148.544), (148.16, 147.0), (147.46, 146.3), (144.3, 146.3)], 0.2),
    ('/LT2_MCLR', 'via', [(144.3, 146.3)], None),
    ('/LT2_MCLR', B, [(144.3, 146.3), (142.4, 148.2), (138.33, 148.2), (136.63, 146.5), (109.0, 146.5),
                      (108.4, 145.9), P2['MCLR']], 0.2),
    ('/LT2_MCLR', 'via', [P2['MCLR']], None),
    ('/LT2_CTS', F, [(146.89, 148.544), (146.89, 147.5), (146.49, 147.1), (145.3, 147.1)], 0.2),
    ('/LT2_CTS', 'via', [(145.3, 147.1)], None),
    ('/LT2_CTS', B, [(145.3, 147.1), (144.9, 147.1), (143.2, 148.8), (138.36, 148.8), (136.46, 146.9), (104.5, 146.9),
                     (103.9, 146.3), P2['CTS']], 0.2),
    ('/LT2_CTS', 'via', [P2['CTS']], None),
    # IO11 (LT2_UART_TX), IO12 (LT2_UART_RX): a via beside the pin, the back under BTN_TEST and U3
    ('/LT2_UART_TX', F, [(136.75, 137.889), (135.65, 137.89)], 0.2),
    ('/LT2_UART_TX', 'via', [(135.65, 137.89)], None),
    ('/LT2_UART_TX', B, [(135.65, 137.89), (135.36, 137.6), (105.35, 137.6), (104.95, 138.0), P2['UTX']], 0.2),
    ('/LT2_UART_TX', 'via', [P2['UTX']], None),
    ('/LT2_UART_RX', F, [(136.75, 139.159), (135.65, 139.16)], 0.2),
    ('/LT2_UART_RX', 'via', [(135.65, 139.16)], None),
    ('/LT2_UART_RX', B, [(135.65, 139.16), (134.54, 138.05), (107.45, 138.05), P2['URX']], 0.2),
    ('/LT2_UART_RX', 'via', [P2['URX']], None),
    # ---- J4's lines from the module ----
    # IO9 (J4_TX), IO10 (J4_RX): a via beside the pin, the back under BTN_TEST, a via, the front south
    ('/J4_TX', F, [(136.75, 135.349), (135.65, 135.35)], 0.2),
    ('/J4_TX', 'via', [(135.65, 135.35)], None),
    ('/J4_TX', B, [(135.65, 135.35), (133.2, 135.35)], 0.2),
    ('/J4_TX', 'via', [(133.2, 135.35)], None),
    ('/J4_TX', F, [(133.2, 135.35), P4['TX']], 0.2),
    ('/J4_RX', F, [(136.75, 136.619), (135.65, 136.62)], 0.2),
    ('/J4_RX', 'via', [(135.65, 136.62)], None),
    ('/J4_RX', B, [(135.65, 136.62), (135.37, 136.9), (131.2, 136.9)], 0.2),
    ('/J4_RX', 'via', [(131.2, 136.9)], None),
    ('/J4_RX', F, [(131.2, 136.9), (131.2, 142.5), (130.875, 142.825), P4['RO']], 0.2),
    # IO3 (J4_DE): north off its pin, the back under BTN_TEST, the front south beside J4_RX
    ('/J4_DE', F, [(136.75, 132.809), (136.2, 132.3), (135.8, 131.9), (135.8, 129.4)], 0.2),
    ('/J4_DE', 'via', [(135.8, 129.4)], None),
    ('/J4_DE', B, [(135.8, 129.4), (134.75, 128.35), (130.4, 128.35)], 0.2),
    ('/J4_DE', 'via', [(130.4, 128.35)], None),
    ('/J4_DE', F, [(130.4, 128.35), (130.4, 142.6), (130.1, 142.9), P4['GPIO_DE']], 0.2),
    # IO2 (LT_RESET): rev A's run along the back, on to a via above U7's pin 4
    ('/LT_RESET', F, [(151.97, 148.544), (151.97, 147.5423), (152.0301, 147.4822), (152.0301, 145.2155),
                      (151.0144, 144.1998), (151.0144, 133.874)], 0.2),
    ('/LT_RESET', 'via', [(151.0144, 133.874)], None),
    ('/LT_RESET', B, [(151.0144, 133.874), (150.7174, 134.171), (127.9, 134.171)], 0.2),
    ('/LT_RESET', 'via', [(127.9, 134.171)], None),
    ('/LT_RESET', F, [(127.9, 134.171), P4['MCLR']], 0.2),
    # IO6 (LT_CTS): rev A's run along the back, on to its port
    ('/LT_CTS', F, [(148.16, 131.044), (148.16, 134.8508), (148.6852, 135.376)], 0.2),
    ('/LT_CTS', 'via', [(148.6852, 135.376)], None),
    ('/LT_CTS', B, [(148.6852, 135.376), (144.2456, 135.376), (137.2337, 142.3879), (124.3879, 142.3879),
                    P4['CTS']], 0.2),
    ('/LT_CTS', 'via', [P4['CTS']], None),
    # IO4 (LT_UART_TX), IO5 (LT_UART_RX): rev A's runs north of the module and its two vias, then the front south
    ('/LT_UART_TX', F, [(150.7, 131.044), (150.7, 132.0457), (149.6557, 132.0457), (149.3844, 132.317)], 0.2),
    ('/LT_UART_TX', 'via', [(149.3844, 132.317)], None),
    ('/LT_UART_TX', B, [(149.3844, 132.317), (149.7672, 131.9342), (149.7672, 129.1354), (148.3153, 127.6835),
                        (132.8052, 127.6835), (131.3915, 126.2698), (125.839, 126.2698)], 0.2),
    ('/LT_UART_TX', 'via', [(125.839, 126.2698)], None),
    ('/LT_UART_TX', F, [(125.839, 126.2698), (124.45, 127.6588), P4['UTX']], 0.2),
    ('/LT_UART_RX', F, [(149.43, 131.044), (149.43, 129.5956), (149.2154, 129.381)], 0.2),
    ('/LT_UART_RX', 'via', [(149.2154, 129.381)], None),
    ('/LT_UART_RX', B, [(149.2154, 129.381), (146.138, 132.4584), (134.6927, 132.4584), (131.6303, 129.396),
                        (128.7227, 129.396)], 0.2),
    ('/LT_UART_RX', 'via', [(128.7227, 129.396)], None),
    ('/LT_UART_RX', F, [(128.7227, 129.396), (126.05, 132.0687), P4['URX']], 0.2),
    # ---- 3.3 V ----
    # a via where the socket's pin 1 joined front and back; east, and down beside BTN_TEST to U6's pin 10
    ('/+3V3', 'bigvia', [(131.71, 133.09)], None),
    ('/+3V3', F, [(131.71, 133.09), (133.95, 133.09), (134.25, 133.39), (134.25, 147.4)], 0.3),
    # J4's block to J2's along the back, and to R21's via
    ('/+3V3', B, [(125.5, 148.58), (125.18, 148.9), (118.3, 148.9), (117.0, 150.2), (116.025, 150.2)], 0.3),
    ('/+3V3', B, [(135.525, 150.2), (136.825, 151.5), (139.2, 151.5)], 0.3),
    # ---- the handshake lines ----
    # J2's: below U3, down between the blocks, round below the socket's lower row
    ('/PR_HSKO', F, [(115.1, 141.51), (115.1, 141.9), (115.4, 142.2), (118.2, 142.2), (118.2, 159.3), (117.3, 160.2),
                     (112.3, 160.2), (111.7, 159.6), (111.7, 158.52)], 0.2),
    ('/PR_HSKI', F, [(111.475, 139.26), (110.75, 139.985), (110.75, 141.9), (111.45, 142.6), (117.8, 142.6),
                     (117.8, 158.9), (116.9, 159.8), (114.9, 159.8), (114.3, 159.2), (114.3, 158.52)], 0.2),
    ('/GND', F, [(111.525, 140.76), (111.525, 141.75)], 0.2),          # C13, which PR_HSKI now cuts off from the pour
    ('/GND', 'via', [(111.525, 141.75)], None),
    # J4's: down U3's east side, west of U7, to rev 2026.11's vias and back runs
    ('/J4_HSKI', F, [(120.05, 136.43), (122.3, 136.43), (122.3, 142.1), (120.95, 142.1), (120.95, 149.6),
                     (123.75, 152.4), (123.95, 152.4)], 0.2),
    ('/J4_HSKI', 'via', [(123.95, 152.4)], None),
    ('/J4_HSKI', B, [(123.95, 152.4), (124.6, 153.05), (128.2, 153.05), (130.35, 155.2), (130.35, 157.67),
                     (131.2, 158.52)], 0.2),
    ('/J4_HSKO', F, [(120.05, 135.16), (122.8, 135.16), (122.8, 142.5), (121.35, 142.5), (121.35, 148.05),
                     (124.55, 151.25), (124.6, 151.25)], 0.2),
    ('/J4_HSKO', 'via', [(124.6, 151.25)], None),
    ('/J4_HSKO', B, [(124.6, 151.25), (128.6, 151.25), (132.7, 155.35), (132.7, 157.4), (133.8, 158.5)], 0.2),
    # R24, turned: HSKI_DRV from its via, ground by a via south of the band on the back
    ('/HSKI_DRV', F, [(119.75, 142.34), (119.575, 142.9), (119.575, 143.075)], 0.2),
    ('/GND', F, [(119.575, 144.725), (119.575, 147.6)], 0.2),
    ('/GND', 'via', [(119.575, 147.6)], None),
]


def main():
    e = Board(BOARD)
    if any(t.GetNetname() == '/LT2_DATA' for t in e.b.GetTracks()):
        print('already drawn')
        return
    runs = block(J2_X, 0) + block(J4_X, 1) + LINES
    for net, layer, pts, width in runs:
        if layer == 'via':
            e.add_via(net, pts[0])
        elif layer == 'bigvia':
            e.add_via(net, pts[0], 0.8, 0.4)
        else:
            e.draw(net, pts, layer, width)
    e.save()                                 # refills the zones
    print(f'{len(runs)} runs drawn: {len(BLOCK)} in each block, {len(LINES)} between them; zones refilled')


if __name__ == '__main__':
    main()
