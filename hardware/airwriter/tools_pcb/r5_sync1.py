#!/usr/bin/env python3
"""Round 5, process 1: schematic-driven board sync.
Removes J6, R18, R19, J3 footprints; swaps SW1's footprint (BootPads_Service
-> EVQP44B3M_SidePress) preserving reference/nets. Run with the pcbnew-bundled
interpreter. FootprintLoad calls happen before any Remove() call, per the
known trap (Remove() corrupts subsequent FootprintLoad/GetNetInfo in-process).
"""
import pcbnew
from pathlib import Path

REPO = Path("/Users/wells/Projects/airwriter")
BOARD = REPO / "hardware/airwriter/airwriter.kicad_pcb"
LIB_DIR = str(REPO / "hardware/airwriter/lib/airwriter.pretty")

board = pcbnew.LoadBoard(str(BOARD))

# --- fetch nets BEFORE any Remove() call ---
net_boot = board.FindNet("/BOOT_CFG")
net_gnd = board.FindNet("/GND")
assert net_boot is not None, "BOOT_CFG net missing"
assert net_gnd is not None, "GND net missing"
print("BOOT_CFG net code:", net_boot.GetNetCode())
print("GND net code:", net_gnd.GetNetCode())

# --- load the new SW1 footprint before any Remove() ---
new_sw1 = pcbnew.FootprintLoad(LIB_DIR, "EVQP44B3M_SidePress")
assert new_sw1 is not None, "failed to load EVQP44B3M_SidePress"
new_sw1.SetFPID(pcbnew.LIB_ID("airwriter", "EVQP44B3M_SidePress"))
new_sw1.SetReference("SW1")
new_sw1.SetValue("EVQP44B3M side-press")

for pad in new_sw1.Pads():
    if pad.GetNumber() == "1":
        pad.SetNet(net_boot)
    elif pad.GetNumber() == "2":
        pad.SetNet(net_gnd)
    else:
        raise AssertionError(f"unexpected pad number {pad.GetNumber()!r}")

# provisional position -- final west-edge placement happens in a later pass
new_sw1.SetPosition(pcbnew.VECTOR2I(pcbnew.FromMM(116.925), pcbnew.FromMM(135.675)))
new_sw1.SetOrientationDegrees(0.0)

# --- now remove the dead footprints (J6, R18, R19, J3, old SW1) ---
DOOMED = {"J6", "R18", "R19", "J3", "SW1"}
removed = []
for fp in list(board.GetFootprints()):
    if fp.GetReference() in DOOMED:
        removed.append(fp.GetReference())
        board.Remove(fp)
assert set(removed) == DOOMED, (sorted(removed), sorted(DOOMED))
print("Removed:", sorted(removed))

board.Add(new_sw1)
print("Added new SW1 at", new_sw1.GetPosition())

board.Save(str(BOARD))
print("Saved.")
