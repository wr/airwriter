"""Round 9 relief trial, built reliably from the git base (d6f64b5).
The shell restores airwriter.kicad_pcb via `git checkout` first; this script then
applies the round-9 target edits (SW3 z-scale + U4/C4 into the gold gap) PLUS an
optional relief candidate, all as ABSOLUTE positions, and saves in place.

Usage: r9_trial.py "C13,119.425,146.275;R6,124.0,140.575"   (relief spec, may be empty)
"""
import sys
sys.path.insert(0, "/Users/wells/Projects/airwriter/hardware/airwriter/tools_pcb")
import pcbnew, r6_lib as L

BOARD = L.BOARD
relief = sys.argv[1] if len(sys.argv) > 1 else ""

b = pcbnew.LoadBoard(BOARD)
if b is None:
    sys.exit("LoadBoard returned None -- base not restored")
fps = {f.GetReference(): f for f in b.GetFootprints()}

# guard: must be the git base (U4 at its round-8 spot), not a stale board
u4x = round(fps["U4"].GetPosition().x / 1e6, 2)
assert abs(u4x - 147.394) < 0.01, f"base not clean: U4 at x={u4x}, expected 147.394"

# CHANGE 2: SW3 actuator +2mm via model z-scale (Models() yields live refs)
sw3 = fps["SW3"]
for m in sw3.Models():
    m.m_Scale.z = 15.0 / 13.0

# CHANGE 3: U4 + C4 into the D14/D15 gold gap (target placement)
base = {"U4": (116.5, 116.364), "C4": (122.545, 116.364)}
# relief candidate overrides / additions
for mv in [x for x in relief.split(";") if x.strip()]:
    ref, x, y = mv.split(","); base[ref] = (float(x), float(y))
for ref, (x, y) in base.items():
    fps[ref].SetPosition(pcbnew.VECTOR2I(L.nm(x), L.nm(y)))

# clearance report for everything we moved
for ref in base:
    f = fps[ref]; cy = L.cy_bbox(f)
    ov = [g.GetReference() for g in b.GetFootprints()
          if g.GetReference() != ref and L.fp_overlap(f, g, 0.15)]
    print(f"{ref} @({base[ref][0]},{base[ref][1]}) cy[{cy[0]:.2f},{cy[1]:.2f}-{cy[2]:.2f},{cy[3]:.2f}] overlaps<0.15: {ov}")
pcbnew.SaveBoard(BOARD, b)
print("applied SW3-scale + U4/C4 grid + relief:", list(base))
