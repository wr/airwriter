"""Swap S1 onto the real DP-08RP footprint, preserving reference, value and nets.

Follows the documented pcbnew trap: every FootprintLoad and net lookup happens
BEFORE any Remove() call, because Remove() corrupts both for the rest of the
process. Also re-applies the lib nickname by hand, since FootprintLoad drops it.
"""
import sys
sys.path.insert(0, "/Users/wells/Projects/airwriter/hardware/airwriter/tools_pcb")
import pcbnew, r6_lib as L

LIB_DIR = "/Users/wells/Projects/airwriter/hardware/airwriter/lib/airwriter.pretty"
NAME = "SW_DIP_8_RightAngle_DP-08RP"

board = L.load()
old = {f.GetReference(): f for f in board.Footprints()}["S1"]
value = old.GetValue()

# --- nets first ---
nets = {}
for p in old.Pads():
    nets[p.GetPadName()] = board.FindNet(p.GetNetname())
    assert nets[p.GetPadName()] is not None, p.GetPadName()
assert len(nets) == 16, len(nets)

# --- load the replacement before any Remove() ---
new = pcbnew.FootprintLoad(LIB_DIR, NAME)
assert new is not None, "failed to load " + NAME
new.SetFPID(pcbnew.LIB_ID("airwriter", NAME))
new.SetReference("S1")
new.SetValue(value)
for p in new.Pads():
    p.SetNet(nets[p.GetPadName()])

# Body face 0.2mm off the x=100 edge, as before; the keys now overhang 2.2mm
# past it, which is how the connectors already meet the case.
new.SetOrientationDegrees(90)
new.SetPosition(pcbnew.VECTOR2I(L.nm(105.005), L.nm(138.785)))

board.Remove(old)
board.Add(new)
pcbnew.SaveBoard(L.BOARD, board)
print("S1 swapped to", NAME)
