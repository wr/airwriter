#!/usr/bin/env python3
# SPDX-License-Identifier: CERN-OHL-S-2.0
"""W-1176: remove the copper the two-way ports make wrong.

    <KiCad python> tools_pcb/r15_clean.py

Run after r13_sync.py has taken the schematic (U2 and U6 swapped for the
THVD1452's VSSOP-10 in place, U8, R14, R25 and C18 gone, the new parts parked
east of the board). r13_clean.py's two passes:

  1. a track or via that meets a pad of another net goes (the runs to the old
     SOIC pads of U2, U6 and U8 that a VSSOP pad now sits on);
  2. then, repeatedly, a track end or via joined to nothing of its own net
     goes, back to the pad it started from, on the port signal nets only.

The second pass leaves +3V3 and GND alone: their old runs to U2, U8 and C3 are
also U3's supply, and r15_route.py joins the new parts to them. It also leaves
nets this change doesn't touch: r13_clean.py's test reads a few of rev
2026.11's own joins (EN at SW1's bracket, BOOT_CFG, HSKI_DRV) as loose ends.

r15_route.py draws the new runs. Close KiCad first. Safe to run twice.
"""
import r13_clean

SIGNALS = {
    '/UART_TX', '/UART_RX', '/LT_DATA', '/LT_DE',
    '/LT_TXD_P', '/LT_TXD_N', '/LT_RXD_P', '/LT_RXD_N',
    '/PR_RXD_P', '/PR_RXD_N', '/PR_TXD_P_IN', '/PR_TXD_N_IN',
}
r13_clean.NEW_NETS = SIGNALS | {'/+3V3', '/GND'}     # pass 1 may cut these
r13_clean.KEEP = set()
_dangling = r13_clean.dangling


def dangling(board, nets):
    return [d for d in _dangling(board, nets) if d[0].GetNetname() in SIGNALS]


r13_clean.dangling = dangling

if __name__ == '__main__':
    r13_clean.main()
