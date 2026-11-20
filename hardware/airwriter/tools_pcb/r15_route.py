#!/usr/bin/env python3
# SPDX-License-Identifier: CERN-OHL-S-2.0
"""W-1176: draw the traces of the two-way ports.

    <KiCad python> tools_pcb/r15_route.py

Run after r15_clean.py and r15_place.py, with KiCad closed. Like r13_route.py:
the runs whose path is the design are drawn point by point (HAND), the short
stubs are found by r13_route's grid search (CONNECTIONS), and the copper the
new placement makes wrong is named and removed first (DROP). A board that
already has LT2_DATA drawn is left alone.

J2's five lines from the module (IO17, IO18 and IO7 on its north edge, IO41
and IO42 on its south) each run on the front under the module to a via, then
west on the back: UART_RX and UART_TX where rev 2026.11's ran, the other
three south of them, under U6 and U10.

The module's west pins sit between BTN_TEST and LED_DATA, so IO9 to IO12 each
take a via beside the pin (as IO13 does) and the back under BTN_TEST. IO9 and
IO12 come up again at R43 and R35, which stand between U7 and the module
where R27 was, and go on from a via below each resistor; IO10 and IO11 run on
into U7's rows. From there the front leaves between U7's pins, two tracks to
a gap 0.15 mm from the pins, under U7's TASHTALK label, to R34 and R42 below
the label. IO3 has no room for that via (LT_UART_RX and the 3.3 V run pass
under its pin), so R44 stands north of it and LT_DE comes to R44 along the
back from R22.

R27 stands north-west of U7 beside pin 4, and LT_RESET reaches it along the
back under U7's north row, where LT_MCLR ran.

U6's bus pins leave under its own body, and R11 bridges B so A and B reach
J4.8 and J4.5 in the socket's order; TxD- goes down between J4.8 and J4.7 as
before.
"""
import os
import sys

import pcbnew

import r13_route as R
from r13_edit import Board, near, mm

HERE = os.path.dirname(os.path.abspath(__file__))
BOARD = os.environ.get('R15_BOARD') or os.path.join(HERE, '..', 'airwriter.kicad_pcb')
R.BOARD = BOARD
F, B = pcbnew.F_Cu, pcbnew.B_Cu

# Nets drawn again from nothing: every track and via of theirs goes.
DROP_NETS = ['/LT_PORT_RXD_N', '/LT_PORT_RXD_P', '/LT_PORT_TXD_N', '/LT_PORT_TXD_P']
# (net, a, b) tracks and (net, 'via', p) vias that go; then (net, layer or 'via', points, width) to draw
DROP = [
    # 3.3 V: the stubs to U2's and U8's old supply pins and C18, U8's ground via
    ('/+3V3', (117.175, 150.105), (116.9, 150.38)),
    ('/+3V3', (116.9, 150.38), (116.9, 151.625)),
    ('/+3V3', (120.925, 150.105), (122.351, 150.105)),
    ('/+3V3', (122.351, 150.105), (122.351, 145.354)),
    ('/+3V3', (120.925, 150.105), (121.55, 150.73)),
    ('/+3V3', (121.55, 150.73), (121.55, 151.62)),
    ('/+3V3', (122.351, 145.354), (121.237, 144.24)),
    ('/GND', 'via', (120.925, 146.74)),
    # U6's old supply, C14's and R25's
    ('/+3V3', (133.275, 146.295), (132.927, 146.295)),
    ('/+3V3', (132.927, 146.295), (132.718, 146.295)),
    ('/+3V3', (132.718, 146.295), (131.893, 147.12)),
    ('/+3V3', (131.893, 147.12), (131.893, 151.218)),
    ('/+3V3', (131.893, 151.218), (132.3, 151.625)),
    ('/+3V3', (132.1, 150.825), (131.893, 150.825)),
    ('/+3V3', (132.125, 144.6), (132.927, 144.6)),
    ('/+3V3', 'via', (132.1, 150.825)),
    ('/+3V3', (132.9, 151.625), (132.1, 150.825)),
    ('/+3V3', (134.4, 151.625), (132.9, 151.625)),
    ('/+3V3', (135.6, 151.625), (134.4, 151.625)),
    ('/+3V3', (136.0, 151.825), (135.6, 151.625)),
    # TxD- left U2's old pin 6
    ('/PR_RXD_N', (112.225, 147.565), (110.2, 147.565)),
    ('/PR_RXD_N', (110.2, 147.565), (110.2, 151.35)),
    # ground vias that sat on C14's and R12's old pads; R25's and R21's feed from C14
    ('/GND', 'via', (133.3, 150.97)),
    ('/GND', 'via', (134.983, 152.98)),
    ('/+3V3', 'via', (134.4, 151.625)),
    ('/+3V3', (134.6, 151.5), (134.4, 151.625)),
    ('/+3V3', (139.2, 151.5), (134.6, 151.5)),
    # R27 moves north and LT_MCLR runs straight west from it, between U7's rows
    ('/LT_RESET', (134.12, 134.171), (134.12, 135.175)),
    ('/LT_RESET', 'via', (134.12, 134.171)),
    ('/LT_RESET', (134.12, 134.171), (150.7174, 134.171)),
    ('/LT_MCLR', (130.39, 134.17), (125.171, 134.171)),
    ('/LT_MCLR', (125.171, 134.171), (124.09, 133.09)),
    ('/LT_MCLR', (134.12, 136.825), (134.12, 137.9)),
    ('/LT_MCLR', 'via', (134.12, 137.9)),
    ('/LT_MCLR', (134.12, 137.9), (130.39, 134.17)),
    # U7's ICSP lines move over inside the socket's rows
    ('/LT_ICSPCLK', (125.36, 139.44), (125.36, 131.285)),
    ('/LT_ICSPCLK', (126.63, 140.71), (125.36, 139.44)),
    ('/LT_ICSPDAT', (127.875, 139.415), (127.875, 130.8)),
    ('/LT_ICSPDAT', (129.17, 140.71), (127.875, 139.415)),
]

HAND = [
    # ---- J2 ----
    # 3.3 V: straight down into U10.1 past C68; under PR_HSKO on the back to C3 and U2.10
    ('/+3V3', F, [(121.237, 144.24), (122.95, 144.24), (122.95, 146.895)], 0.5),
    ('/+3V3', 'via', [(115.4, 150.1)], None),
    ('/+3V3', B, [(115.4, 150.1), (113.95, 150.1)], 0.3),
    ('/+3V3', 'via', [(113.95, 150.1)], None),
    ('/+3V3', F, [(113.675, 150.8), (113.95, 150.1), (113.25, 149.4), (113.25, 149.2), (112.6, 149.2)], 0.3),
    ('/GND', 'via', [(111.95, 150.3)], None),                          # C3
    ('/GND', F, [(111.95, 150.3), (112.125, 150.8)], 0.3),
    ('/GND', 'via', [(121.15, 144.95)], None),                          # C68
    ('/GND', F, [(121.15, 144.95), (121.475, 145.275), (122.03, 145.275)], 0.3),
    ('/PR_TXD_P', F, [(112.75, 153.325), (112.75, 153.925)], 0.2),     # R29, moved
    # the bus pins: TxD+ and TxD- west and down to the back as before, the receive pair to R30 and R29
    ('/PR_RXD_P', F, [(112.6, 147.2), (109.9, 147.2), (109.2, 147.9), (109.2, 151.5)], 0.2),
    ('/PR_RXD_P', 'via', [(109.2, 151.5)], None),
    ('/PR_RXD_P', B, [(109.2, 151.5), (109.2, 151.75), (109.4, 151.95), (117.6, 151.95), (117.6, 154.82),
                      (116.4, 156.02)], 0.2),
    ('/PR_RXD_P', 'via', [(117.6, 153.6)], None),                      # R40
    ('/PR_RXD_P', F, [(117.6, 153.6), (117.075, 153.075), (117.05, 153.075)], 0.2),
    ('/PR_RXD_N', F, [(112.6, 147.7), (110.5, 147.7), (110.2, 148.0), (110.2, 151.35)], 0.2),
    ('/PR_RXD_N', 'via', [(118.45, 152.9)], None),                     # R41
    ('/PR_RXD_N', B, [(118.45, 152.9), (118.15, 152.9)], 0.2),
    ('/PR_RXD_N', F, [(118.45, 152.9), (118.925, 152.9)], 0.2),
    ('/PR_TXD_N_IN', F, [(112.6, 148.2), (111.2, 148.2), (110.875, 148.525), (110.875, 152.335),
                         (110.375, 152.835), (110.575, 153.25)], 0.2),
    ('/PR_TXD_P_IN', F, [(112.6, 148.7), (111.6, 148.7), (111.35, 148.95), (111.35, 151.45),
                         (112.175, 152.275), (112.75, 152.275)], 0.2),
    # U2 and U10 share DATA and DE across the gap, through the resistors' pads
    ('/LT2_DATA', F, [(116.8, 147.7), (120.9, 147.7), (121.365, 148.165), (122.225, 148.165)], 0.2),
    ('/LT2_DE', F, [(116.8, 148.2), (117.9, 148.2), (118.8, 149.1), (120.7, 149.1), (121.035, 149.435),
                    (122.225, 149.435)], 0.2),
    # IO18 (UART_RX): down under the module, the back to R37, on to U2's RO
    ('/UART_RX', F, [(141.81, 131.044), (141.81, 144.6665), (140.75, 145.7265)], 0.2),
    ('/UART_RX', 'via', [(140.75, 145.7265)], None),
    ('/UART_RX', B, [(140.75, 145.7265), (140.5735, 145.55), (118.73, 145.55), (118.63, 145.65)], 0.2),
    ('/UART_RX', 'via', [(118.63, 145.65)], None),
    ('/UART_RX', F, [(118.63, 145.65), (118.63, 146.05)], 0.2),
    ('/UART_RX', B, [(118.63, 145.65), (117.2, 147.08), (117.2, 150.3)], 0.2),
    ('/UART_RX', 'via', [(117.2, 150.3)], None),
    ('/UART_RX', F, [(117.2, 150.3), (117.2, 149.2), (116.8, 149.2)], 0.2),
    # IO17 (UART_TX) to R38
    ('/UART_TX', F, [(143.08, 131.044), (143.08, 144.322), (140.2454, 147.1566)], 0.2),
    ('/UART_TX', 'via', [(140.2454, 147.1566)], None),
    ('/UART_TX', B, [(140.2454, 147.1566), (139.0888, 146.0), (120.38, 146.0), (120.18, 146.2)], 0.2),
    ('/UART_TX', 'via', [(120.18, 146.2)], None),
    # IO7 (J2_DE): round the module's ground pad, the back to R39
    ('/J2_DE', F, [(146.89, 131.044), (146.89, 132.9), (144.2, 135.59), (144.2, 145.2), (142.5, 146.9)], 0.2),
    ('/J2_DE', 'via', [(142.5, 146.9)], None),
    ('/J2_DE', B, [(142.5, 146.9), (141.6, 147.8), (131.9, 147.8), (130.3, 149.4), (122.28, 149.4), (120.18, 151.5)], 0.2),
    ('/J2_DE', 'via', [(120.18, 151.5)], None),
    ('/J2_DE', F, [(120.18, 151.5), (120.18, 150.75)], 0.2),
    # IO42 (LT2_MCLR) to U10.4, IO41 (LT2_CTS) to U10.5: a via each under U10
    ('/LT2_MCLR', F, [(148.16, 148.544), (148.16, 147.0), (147.46, 146.3), (144.3, 146.3)], 0.2),
    ('/LT2_MCLR', 'via', [(144.3, 146.3)], None),
    ('/LT2_MCLR', B, [(144.3, 146.3), (142.4, 148.2), (132.3, 148.2), (130.7, 149.8), (123.95, 149.8), (122.3, 151.45)], 0.2),
    ('/LT2_MCLR', 'via', [(122.3, 151.45)], None),
    ('/LT2_MCLR', F, [(122.3, 151.45), (122.3, 150.705)], 0.2),
    ('/LT2_CTS', F, [(146.89, 148.544), (146.89, 147.5), (146.49, 147.1), (145.3, 147.1)], 0.2),
    ('/LT2_CTS', 'via', [(145.3, 147.1)], None),
    ('/LT2_CTS', B, [(145.3, 147.1), (144.9, 147.1), (143.2, 148.8), (132.3, 148.8), (130.7, 150.4), (125.45, 150.4)], 0.2),
    ('/LT2_CTS', 'via', [(125.45, 150.4)], None),
    ('/LT2_CTS', F, [(125.45, 150.4), (125.755, 150.705), (127.175, 150.705)], 0.2),
    # ground vias
    ('/GND', 'via', [(118.95, 151.6)], None),                          # R36
    ('/GND', F, [(118.95, 151.6), (118.63, 151.28), (118.63, 150.75)], 0.2),
    ('/GND', 'via', [(121.4, 152.9)], None),                           # R41
    ('/GND', F, [(121.4, 152.9), (120.575, 152.9)], 0.2),
    ('/GND', F, [(116.8, 147.2), (117.3, 147.2), (117.611, 146.889), (117.611, 145.401)], 0.2),      # U2.5
    ('/GND', F, [(116.8, 148.7), (117.6, 148.7), (117.85, 148.95), (117.85, 150.55), (118.05, 150.75),
                 (118.63, 150.75)], 0.2),                                                           # U2.2
    # ---- J4 ----
    # 3.3 V into U6.10 past C14, then down U6's east side to R45 and, by the back, R21
    ('/+3V3', F, [(132.927, 146.295), (132.927, 146.5), (133.827, 147.4), (134.55, 147.4)], 0.3),
    ('/+3V3', F, [(134.55, 147.4), (135.4, 147.4), (135.6, 147.6), (135.6, 149.9), (136.5, 150.8),
                  (136.5, 151.225)], 0.2),
    ('/+3V3', 'via', [(136.0, 150.3)], None),
    ('/+3V3', B, [(136.0, 150.3), (137.2, 151.5), (139.2, 151.5)], 0.3),
    ('/GND', 'via', [(134.975, 146.65)], None),                        # C14
    ('/GND', F, [(134.975, 145.8), (134.975, 146.65)], 0.3),
    ('/GND', 'via', [(129.05, 148.65)], None),                         # U6.2 and U6.5
    ('/GND', F, [(130.35, 147.9), (129.3, 147.9), (129.3, 149.4), (130.35, 149.4)], 0.2),
    ('/GND', F, [(129.05, 148.65), (129.3, 148.65)], 0.2),
    ('/GND', 'via', [(125.75, 146.9)], None),                          # U10.8
    ('/GND', F, [(125.75, 146.9), (127.175, 146.895)], 0.2),
    ('/GND', 'via', [(131.85, 153.6)], None),                          # R46
    ('/GND', F, [(131.85, 152.875), (131.85, 153.6)], 0.2),
    # the bus pins leave under U6: A to R11, B through R11 to R10, Z to R12 and R46, Y to R13 and R45
    ('/LT_RXD_P', F, [(134.55, 147.9), (132.7, 147.9), (132.7, 149.65), (132.4, 149.95), (130.75, 149.95),
                      (130.3, 150.4), (130.3, 151.175)], 0.2),
    ('/LT_RXD_N', F, [(134.55, 148.4), (133.1, 148.4), (133.1, 150.05), (132.8, 150.35), (131.075, 150.35),
                      (131.075, 151.75), (130.825, 152.0), (128.75, 152.0), (128.75, 152.245)], 0.2),
    ('/LT_TXD_N', F, [(134.55, 148.9), (133.5, 148.9), (133.5, 150.6), (133.4, 150.7), (133.4, 151.225),
                      (131.85, 151.225)], 0.2),
    ('/LT_TXD_P', F, [(134.55, 149.4), (134.95, 149.8), (134.95, 151.225), (135.35, 151.225), (135.725, 151.6),
                      (135.725, 152.5), (136.1, 152.875), (136.5, 152.875)], 0.2),
    ('/LT_PORT_RXD_P', F, [(130.3, 152.825), (130.3, 154.0), (129.5, 154.8), (129.1, 155.2), (129.1, 156.02)], 0.2),
    ('/LT_PORT_RXD_N', F, [(128.75, 153.895), (127.95, 154.695), (127.95, 157.37), (129.1, 158.52)], 0.2),
    ('/LT_PORT_TXD_N', F, [(133.4, 152.875), (133.4, 153.6), (132.8, 154.2), (131.2, 154.2), (130.35, 155.05),
                           (130.35, 156.381), (131.162, 157.193), (134.573, 157.193), (135.9, 158.52)], 0.2),
    ('/LT_PORT_TXD_P', F, [(134.95, 152.875), (134.95, 154.3), (135.9, 155.25), (135.9, 156.02)], 0.2),
    # U7's reset: LT_RESET along the back under U7's north row to R27, beside U7.4
    ('/LT_RESET', B, [(150.7174, 134.171), (135.6, 134.171), (135.07, 133.64), (133.4, 133.64), (132.87, 134.171),
                      (121.6, 134.171), (121.6, 129.75), (121.75, 129.6)], 0.2),          # 0.5 mm north past R43's via
    ('/LT_RESET', 'via', [(121.75, 129.6)], None),
    ('/LT_RESET', F, [(121.75, 129.6), (121.775, 130.6)], 0.2),
    ('/LT_MCLR', F, [(123.425, 130.6), (123.425, 131.9), (124.09, 132.565), (124.09, 133.09)], 0.2),
    ('/GND', 'via', [(123.75, 134.95)], None),      # the front pour round U3.15, which LT_MCLR's run cuts off
    # inside U7's rows: ICSPCLK and ICSPDAT step east to their pins, DE and DATA come down to the gaps
    ('/LT_ICSPCLK', F, [(125.36, 131.285), (125.36, 134.6), (126.63, 135.87), (126.63, 140.71)], 0.2),
    ('/LT_ICSPDAT', F, [(127.875, 130.8), (127.875, 134.2), (129.17, 135.495), (129.17, 140.71)], 0.2),
    ('/LT_DE', F, [(126.63, 133.09), (126.63, 134.0), (128.12, 135.49), (128.12, 142.0), (130.15, 144.03),
                   (130.15, 145.9), (130.55, 146.3), (131.5, 146.3), (131.9, 146.7), (131.9, 148.4),
                   (130.35, 148.4)], 0.2),
    ('/LT_DATA', F, [(129.17, 133.09), (130.22, 134.14), (130.22, 138.5), (130.44, 138.72), (130.44, 142.3),
                     (130.975, 142.835), (130.975, 145.0), (131.9, 145.0), (132.3, 145.4), (132.3, 148.9),
                     (130.35, 148.9)], 0.2),
    # IO9 (J4_TX): a via beside the pin, under BTN_TEST to a via above R43; from the via below R43, LT_DATA
    # west on the back to a via on U7's own LT_DATA run
    ('/J4_TX', F, [(136.75, 135.349), (135.65, 135.35)], 0.2),
    ('/J4_TX', 'via', [(135.65, 135.35)], None),
    ('/J4_TX', B, [(135.65, 135.35), (135.22, 135.35), (134.12, 134.25)], 0.2),
    ('/J4_TX', 'via', [(134.12, 134.25)], None),
    ('/J4_TX', F, [(134.12, 134.25), (134.12, 134.95)], 0.2),
    ('/LT_DATA', F, [(134.12, 136.6), (134.12, 137.3)], 0.2),
    ('/LT_DATA', 'via', [(134.12, 137.3)], None),
    ('/LT_DATA', B, [(134.12, 137.3), (130.22, 137.3)], 0.2),
    ('/LT_DATA', 'via', [(130.22, 137.3)], None),
    # IO12 (LT2_UART_RX) the same way through R35; ICSPDAT round U7.8 on the back to a via by U7.5, down
    # between U7.5 and U7.6 and under U10 to its pin 7
    ('/LT2_UART_RX', F, [(136.75, 139.159), (135.65, 139.16)], 0.2),
    ('/LT2_UART_RX', 'via', [(135.65, 139.16)], None),
    ('/LT2_UART_RX', B, [(135.65, 139.16), (134.59, 138.1), (134.12, 138.1)], 0.2),
    ('/LT2_UART_RX', 'via', [(134.12, 138.1)], None),
    ('/LT2_UART_RX', F, [(134.12, 138.1), (134.12, 138.8)], 0.2),
    ('/LT2_ICSPDAT', F, [(134.12, 140.45), (134.12, 141.15)], 0.2),
    ('/LT2_ICSPDAT', 'via', [(134.12, 141.15)], None),
    ('/LT2_ICSPDAT', B, [(134.12, 141.15), (133.27, 140.3), (133.27, 140.07), (132.8, 139.6), (124.8, 139.6)], 0.2),
    ('/LT2_ICSPDAT', 'via', [(124.8, 139.6)], None),
    ('/LT2_ICSPDAT', F, [(124.8, 139.6), (125.14, 139.94), (125.14, 148.165), (127.175, 148.165)], 0.2),
    # IO10 (J4_RX) and IO11 (LT2_UART_TX): the back between R43's two vias into U7's rows, a via, the front
    # out between U7's pins to R42 and R34
    ('/J4_RX', F, [(136.75, 136.619), (135.65, 136.62)], 0.2),
    ('/J4_RX', 'via', [(135.65, 136.62)], None),
    ('/J4_RX', B, [(135.65, 136.62), (134.73, 135.7), (127.375, 135.7)], 0.2),
    ('/J4_RX', 'via', [(127.375, 135.7)], None),
    ('/J4_RX', F, [(127.375, 135.7), (127.375, 138.5), (127.68, 138.805), (127.68, 142.5), (129.325, 144.145),
                   (129.325, 147.125), (129.6, 147.4), (130.35, 147.4)], 0.2),
    ('/LT2_UART_TX', F, [(136.75, 137.889), (135.65, 137.89)], 0.2),
    ('/LT2_UART_TX', 'via', [(135.65, 137.89)], None),
    ('/LT2_UART_TX', B, [(135.65, 137.89), (134.26, 136.5), (126.2, 136.5), (125.9, 136.8)], 0.2),
    ('/LT2_UART_TX', 'via', [(125.9, 136.8)], None),
    ('/LT2_UART_TX', F, [(125.9, 136.8), (125.58, 137.12), (125.58, 142.3), (126.275, 142.995), (126.275, 145.0)], 0.2),
    ('/LT2_ICSPCLK', F, [(127.925, 145.0), (127.925, 145.65), (128.45, 146.175), (128.45, 149.235),
                         (128.25, 149.435), (127.175, 149.435)], 0.2),
    # IO3 (J4_DE): north to R44, the one way off its pin; LT_DE comes to it on the back from R22
    ('/J4_DE', F, [(136.75, 132.809), (136.2, 132.3), (135.8, 131.9), (135.8, 129.425)], 0.2),
    ('/LT_DE', F, [(135.8, 127.775), (135.575, 127.775), (135.0, 128.35)], 0.2),
    ('/LT_DE', 'via', [(135.0, 128.35)], None),
    ('/LT_DE', B, [(135.0, 128.35), (125.2, 128.35), (124.9, 128.65)], 0.2),
    ('/LT_DE', 'via', [(124.9, 128.65)], None),
    ('/LT_DE', F, [(124.9, 128.65), (125.3, 129.05), (125.725, 129.05)], 0.2),
]

# Nothing is left to the grid search: every run above is drawn. A stub added later goes here, as in r13_route.py.
CONNECTIONS = []


def drop(e):
    for t in list(e.b.GetTracks()):
        if t.GetNetname() in DROP_NETS:
            e.drop(t)
    for item in DROP:
        if item[1] == 'via':                # by place alone: KiCad gives a free via the net of a pad put on it
            hit = [t for t in e.b.GetTracks() if isinstance(t, pcbnew.PCB_VIA)
                   and near((mm(t.GetPosition().x), mm(t.GetPosition().y)), item[2], 0.01)]
            if len(hit) != 1:
                raise SystemExit(f'missing via {item[0]} {item[2]}')
            e.drop(hit[0])
        else:
            e.drop(e.track(item[0], item[1], item[2]))


def main():
    only = sys.argv[sys.argv.index('--only') + 1:] if '--only' in sys.argv else None
    e = Board(BOARD)
    if any(t.GetNetname() == '/LT2_DATA' for t in e.b.GetTracks()):
        print('already drawn')
    else:
        drop(e)
        for net, layer, pts, width in HAND:
            if layer == 'via':
                e.add_via(net, pts[0])
            else:
                e.draw(net, pts, layer, width)
        e.save()
        print(f'{len(DROP)} items removed, {len(HAND)} hand runs drawn')
    done = []
    todo = list(CONNECTIONS)
    for extra in (0.0, 3.0):
        todo = R.route_all(todo, only, done, extra)
        if not todo:
            break
    board = pcbnew.LoadBoard(BOARD)
    pcbnew.ZONE_FILLER(board).Fill(board.Zones())
    board.Save(BOARD)
    print(f'{len(done)} connections drawn, {len(todo)} not; zones refilled')


if __name__ == '__main__':
    main()
