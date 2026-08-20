"""Attach the shipped SK6812MINI-E body to the derived TopFiring footprint.

The stock model belongs to the ReverseMount footprint, where the part is
soldered on the component side and emits DOWN through a board cutout. Our
TopFiring variant strips that cutout and emits UP, so the model needs its
orientation corrected -- flipping it through the board like the reverse-mount
reference would bury the body under the PCB.

Both the library footprint and every board instance need it: a board footprint
carries its own copy of the model reference from when it was placed, so the
library edit alone leaves the 37 placed LEDs bodiless (the same trap SW1 hit).

    python3 r6_ledmodel.py <rotx> <offz>
"""
import sys, re
sys.path.insert(0, "/Users/wells/Projects/airwriter/hardware/airwriter/tools_pcb")
import pcbnew, r6_lib as L

MODEL = "${KICAD10_3DMODEL_DIR}/LED_SMD.3dshapes/LED_SK6812MINI-E_3.2x2.8mm_P1.5mm_ReverseMount.step"
FP = "/Users/wells/Projects/airwriter/hardware/airwriter/lib/airwriter.pretty/LED_SK6812MINI-E_3.2x2.8mm_P1.5mm_TopFiring.kicad_mod"

rotx = float(sys.argv[1]) if len(sys.argv) > 1 else 180.0
offz = float(sys.argv[2]) if len(sys.argv) > 2 else 0.0

# --- library footprint ---
s = open(FP).read()
block = ('\t(model "%s"\n\t\t(offset\n\t\t\t(xyz 0 0 %g)\n\t\t)\n\t\t(scale\n\t\t\t(xyz 1 1 1)\n\t\t)'
         '\n\t\t(rotate\n\t\t\t(xyz %g 0 0)\n\t\t)\n\t)\n' % (MODEL, offz, rotx))
s = re.sub(r'\t\(model "[^"]*"\n(?:.*?\n)*?\t\)\n', '', s)
s = s.rstrip()
assert s.endswith(')')
open(FP, "w").write(s[:-1].rstrip('\n') + '\n' + block + ')\n')

# --- every board instance ---
b = L.load()
n = 0
for f in b.Footprints():
    r = f.GetReference()
    if not (r.startswith("D") and r[1:].isdigit() and int(r[1:]) >= 10):
        continue
    f.Models().clear()
    m = pcbnew.FP_3DMODEL()
    m.m_Filename = MODEL
    m.m_Show = True
    m.m_Opacity = 1.0
    m.m_Offset = pcbnew.VECTOR3D(0.0, 0.0, offz)
    m.m_Rotation = pcbnew.VECTOR3D(rotx, 0.0, 0.0)
    f.Models().push_back(m)
    n += 1
pcbnew.SaveBoard(L.BOARD, b)
print("attached to %d LED footprints: rotate x=%g, offset z=%g" % (n, rotx, offz))
