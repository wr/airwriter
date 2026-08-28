"""Round 6 step 3: write the solver's positions onto the board."""
import sys, json
sys.path.insert(0, "/Users/wells/Projects/airwriter/hardware/airwriter/tools_pcb")
import pcbnew, r6_lib as L

P = json.load(open("/Users/wells/Projects/airwriter/hardware/airwriter/tools_pcb/r6_placement.json"))
b = L.load()
fps = {f.GetReference(): f for f in b.Footprints()}
for ref, (x, y, rot) in sorted(P.items()):
    f = fps[ref]
    f.SetOrientationDegrees(rot)
    f.SetPosition(pcbnew.VECTOR2I(L.nm(x), L.nm(y)))
    print("  %-4s -> (%8.3f, %8.3f) rot %3d" % (ref, x, y, rot))
pcbnew.SaveBoard(L.BOARD, b)
print("applied %d" % len(P))
