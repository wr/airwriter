"""Attach the EVQP44B3M body to SW1's board instance.

A board footprint carries its own copy of the model reference, taken at the
moment it was instantiated. SW1 was placed in round 5, before the library
footprint had a model, so adding one to the library alone leaves the board
instance bodiless -- which is exactly how it rendered.
"""
import sys
sys.path.insert(0, "/Users/wells/Projects/airwriter/hardware/airwriter/tools_pcb")
import pcbnew, r6_lib as L

PATH = "${KIPRJMOD}/lib/airwriter.3dshapes/EVQP44B3M_SidePress.wrl"
b = L.load()
sw1 = {f.GetReference(): f for f in b.Footprints()}["SW1"]
if list(sw1.Models()):
    print("SW1 already has a model; nothing to do")
    raise SystemExit(0)
m = pcbnew.FP_3DMODEL()
m.m_Filename = PATH
m.m_Show = True
m.m_Opacity = 1.0
sw1.Models().push_back(m)
pcbnew.SaveBoard(L.BOARD, b)
print("SW1 models now:", [x.m_Filename for x in
                          {f.GetReference(): f for f in L.load().Footprints()}["SW1"].Models()])
