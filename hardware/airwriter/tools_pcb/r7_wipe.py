"""Round 7 step 0: drop the committed route.

The rebalanced panel moves the LED field and the mounting re-layout moves the
lower band, so every track on the board is routed to a pad that is about to be
somewhere else. The board goes back to placement-only until the final route
lands, which is this project's established shape for a placement round.

Deleting tracks does NOT clear the pour: the stored fill polygons keep the
clearance cut-outs the vanished traces needed, and they render as a ghost of the
dead route. r7_finish.py refills at the end of the round.
"""
import sys
sys.path.insert(0, "/Users/wells/Projects/airwriter/hardware/airwriter/tools_pcb")
import pcbnew
import r6_lib as L

b = L.load()
n = len(list(b.Tracks()))
for t in list(b.Tracks()):
    b.Remove(t)
pcbnew.SaveBoard(L.BOARD, b)
print(f"removed {n} tracks and vias; board is placement-only")
