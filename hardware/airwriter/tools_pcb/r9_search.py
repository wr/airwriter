"""Round 9 option-b search: place U4 at a target, auto-place C4 in adjacent
opaque space, verify no light leak (courtyard vs lit-dot windows) and no
courtyard overlap, then save. Shell routes + checks breaks.

Usage: r9_search.py U4X U4Y            (auto C4)
       r9_search.py U4X U4Y C4X C4Y    (explicit C4)
Built from the git base each call (shell does `git checkout` first).
"""
import sys, json, math
sys.path.insert(0, "/Users/wells/Projects/airwriter/hardware/airwriter/tools_pcb")
import pcbnew, r6_lib as L

PANEL = json.load(open("/Users/wells/Projects/airwriter/hardware/panel/panel_layout.json"))
RWIN = PANEL["grid"]["dot_dia"] / 2.0          # light aperture radius, board mm
LIT = [(100.0 + d["x"], 60.0 + d["y"]) for d in PANEL["dots"] if d.get("led")]

def leak(cy):
    """True if courtyard rect cy overlaps any lit-dot window circle."""
    x0, y0, x1, y1 = cy
    for lx, ly in LIT:
        nx = min(max(lx, x0), x1); ny = min(max(ly, y0), y1)
        if math.hypot(lx - nx, ly - ny) < RWIN - 0.02:
            return (round(lx, 1), round(ly, 1))
    return None

u4 = (float(sys.argv[1]), float(sys.argv[2]))
c4 = (float(sys.argv[3]), float(sys.argv[4])) if len(sys.argv) > 4 else None

import time
b = None
for _try in range(8):
    try:
        b = pcbnew.LoadBoard(L.BOARD)
    except Exception:
        b = None
    if b is not None and len(list(b.GetFootprints())) > 100:
        break
    b = None
    time.sleep(0.4)
if b is None: sys.exit("None board after retries")
fps = {f.GetReference(): f for f in b.GetFootprints()}
assert abs(fps["U4"].GetPosition().x/1e6 - 147.394) < 0.01, "base not clean"

# SW3 +2mm already committed on the base, so it is present; nothing to do here.

fps["U4"].SetPosition(pcbnew.VECTOR2I(L.nm(u4[0]), L.nm(u4[1])))

def clear(ref):
    f = fps[ref]; cy = L.cy_bbox(f)
    ov = [g.GetReference() for g in b.GetFootprints()
          if g.GetReference() != ref and L.fp_overlap(f, g, 0.15)]
    return cy, ov

# auto-place C4: try offsets around U4 (east preferred = /+3V3 toward U1), pick
# first that is leak-free and overlap-free
if c4 is None:
    cand = []
    for dx in (6.4, 7.2, -6.4, 5.6, -7.2):
        for dy in (0, 3.6, -3.6):
            cand.append((u4[0] + dx, u4[1] + dy))
    for cx, cy_ in cand:
        fps["C4"].SetPosition(pcbnew.VECTOR2I(L.nm(cx), L.nm(cy_)))
        cyb, ov = clear("C4")
        if not ov and not leak(cyb):
            c4 = (cx, cy_); break
    if c4 is None:
        print("NO clean C4 spot near U4"); sys.exit(2)
else:
    fps["C4"].SetPosition(pcbnew.VECTOR2I(L.nm(c4[0]), L.nm(c4[1])))

for ref in ("U4", "C4"):
    cyb, ov = clear(ref)
    lk = leak(cyb)
    print(f"{ref} @({fps[ref].GetPosition().x/1e6:.3f},{fps[ref].GetPosition().y/1e6:.3f}) "
          f"overlaps<0.15:{ov} leak:{lk}")
    if ov or lk:
        print("  -> INVALID placement"); sys.exit(3)
pcbnew.SaveBoard(L.BOARD, b)
print(f"OK U4({u4[0]:.3f},{u4[1]:.3f}) C4({c4[0]:.3f},{c4[1]:.3f})")
