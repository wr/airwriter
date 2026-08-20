"""Round 6 step 2: the structural block layout.

U1 flush eats the whole east third of the lower band (its body plus the on-board
antenna rule area), so the band is re-organised into four zones:

  north-east strip (above U1's power pad row) : U4 + the power/EN/bypass column
  middle block     (west of U1's body)        : U7, U8, U6, U3, U2, U5 + satellites
  west control edge                           : S1, SW1 (anchors, set in step 1)
  south-west strip / region E / region F      : the J2-shell and J1-CC passives

Positions here are chosen by hand from the real courtyard extents; the leftover
satellites are fitted by r6_solve.py against this frame.
"""
import sys
sys.path.insert(0, "/Users/wells/Projects/airwriter/hardware/airwriter/tools_pcb")
import pcbnew, r6_lib as L

BLOCKS = [
    # --- north-east strip: regulator + the rail/EN column at U1's power pads
    ("U4",  144.335, 131.530, 180),   # rot 180 turns its rail pins toward the cap column
    ("C4",  150.725, 128.875,   0),   # 10u +3V3, at U4's output pin
    ("C2",  150.725, 131.125,   0),   # 10u +3V3
    ("C21", 150.725, 133.375,   0),   # 22u VBUS, at U4's input pin
    ("R3",  154.195, 128.625,   0),   # 10k, /EN
    ("C6",  154.195, 130.375,   0),   # 1u,  /EN
    ("C5",  154.195, 132.125,   0),   # 100n rail
    ("C9",  154.195, 133.875,   0),   # 100n, U1 rail (nearest U1 pad 2)
    # --- middle block
    ("U7",  131.035, 136.995, 180),   # PIC + socket; rot 180 keeps the same
                                      # courtyard box but faces VDD at the free pocket
    ("U8",  127.000, 142.000, 180),   # LocalTalk A/B transceiver
    ("U6",  127.000, 148.500,   0),   # LocalTalk Y/Z transceiver
    ("U3",  114.695, 141.495,   0),   # RS-232 (printer handshake)
    ("U2",  113.395, 150.595,  90),   # printer differential driver
    ("U5",  133.045, 140.695,   0),   # USB ESD, at U1's USB pads
    # --- H4, region E
    ("H4",  143.700, 158.391,   0),
    ("R1",  142.200, 162.512,  90),   # 5.1k /CC1
    ("R2",  144.000, 162.512,  90),   # 5.1k /CC2
    # --- region F, east of J1 below the antenna zone
    ("R17", 158.240, 162.560,  90),   # 1k, the power indicator's series pair
    ("D3",  160.025, 162.560,  90),   # green PWR LED
    # --- south-west strip, west of J2
    ("R5",  101.825, 155.325,   0),   # 120R printer TX termination
    ("R6",  101.825, 157.075,   0),
    ("R7",  101.825, 158.825,   0),
    ("R8",  101.825, 160.575,   0),   # 0R /SHELL
    ("C15", 101.825, 162.325,   0),   # 1n /SHELL
]

b = L.load()
fps = {f.GetReference(): f for f in b.Footprints()}
for ref, x, y, rot in BLOCKS:
    f = fps[ref]
    f.SetOrientationDegrees(rot)
    f.SetPosition(pcbnew.VECTOR2I(L.nm(x), L.nm(y)))
    cy = L.cy_bbox(f)
    print("  %-4s -> (%8.3f, %8.3f) rot %3d  cy [%.3f,%.3f - %.3f,%.3f]"
          % (ref, x, y, rot, cy[0], cy[1], cy[2], cy[3]))
pcbnew.SaveBoard(L.BOARD, b)
print("saved %d block positions" % len(BLOCKS))
