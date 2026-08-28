"""Put the four board-level connector labels back on the connectors they name.

r6_silk.py's nearest-free-spot search is fine for reference designators but it
scattered these: it moved LOCALTALK on top of U1 and PRINTER up beside the LED
grid.  A label that names the wrong thing is worse than a tight one, so these
four are placed by hand, inside the body outline of the part they label.
"""
import sys
sys.path.insert(0, "/Users/wells/Projects/airwriter/hardware/airwriter/tools_pcb")
import pcbnew, r6_lib as L

LABELS = {"PRINTER":   (113.00, 160.20),   # inside J2's body
          "LOCALTALK": (132.00, 160.20),   # inside J4's body
          "USB-C":     (152.00, 161.30),   # inside J1's body
          "PWR":       (152.00, 163.30)}

b = L.load()
for d in b.GetDrawings():
    if d.GetClass() == "PCB_TEXT" and d.GetText() in LABELS and d.GetLayer() == pcbnew.F_SilkS:
        x, y = LABELS[d.GetText()]
        p = d.GetPosition()
        d.SetPosition(pcbnew.VECTOR2I(L.nm(x), L.nm(y)))
        print("  %-10s (%.2f, %.2f) -> (%.2f, %.2f)" % (d.GetText(), L.mm(p.x), L.mm(p.y), x, y))
pcbnew.SaveBoard(L.BOARD, b)
print("saved")
