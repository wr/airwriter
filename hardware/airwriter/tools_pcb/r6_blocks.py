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
    ("U4",  144.335, 130.795, 180),   # rot 180 turns its rail pins toward the cap column
    ("C4",  150.725, 128.175,   0),   # 10u +3V3, at U4's output pin
    ("C2",  150.725, 130.425,   0),   # 10u +3V3
    ("C21", 150.725, 132.675,   0),   # 22u VBUS, at U4's input pin
    ("R3",  154.195, 127.925,   0),   # 10k, /EN
    ("C5",  154.195, 129.675,   0),   # 100n rail
    ("C6",  154.195, 131.425,   0),   # 1u, /EN -- one row lower than the rail cap,
                                      # closer to U1 pad 3 and the escape channel
    ("C9",  154.195, 133.175,   0),   # 100n, U1 rail (nearest U1 pad 2)
    # --- middle block
    ("U7",  131.035, 136.995, 180),   # PIC + socket; rot 180 keeps the same
                                      # courtyard box but faces VDD at the free pocket
    ("U8",  127.000, 142.000, 180),   # LocalTalk A/B transceiver
    ("U6",  127.000, 148.500,   0),   # LocalTalk Y/Z transceiver
    ("U3",  114.695, 141.495,   0),   # RS-232 (printer handshake)
    ("U2",  113.395, 150.595,  90),   # printer differential driver
    # --- region E, between J4 and J1. The ESD clamp goes as close to J1's data
    # pins as J1's own body allows -- J1's courtyard is already 4.57mm wide
    # before its first data pad, so 5.7mm is the floor from this side. Sitting
    # U5 at the TOP of the pocket rather than level with J1's pad row costs
    # 0.02mm on /USB_DM and 0.48mm on /USB_DP, and buys the room H4 needs.
    ("U5",  144.440, 156.340, 180),   # USBLC6-2SC6, hard against J1's courtyard;
                                      # rot 180 turns its pin-1 silk marker away from
                                      # U1's pad row and lowers the worse of the two
                                      # data-pin distances (6.73 -> 6.27mm)
    ("H4",  143.948, 160.741,   0),   # see r6_h4.py: every frame-legal spot
                                      # elsewhere gives a far worse support hull
    # --- LED chain entry. The bottom grid row has two unlit dot cells; the one
    # at x=147.91 has a 3.2 x 3.8mm gap between D10's and D11's caps, which fits
    # the buffer. Puts U9's output pad 5.0mm from D10's DIN (was 7.6mm) with its
    # input pad facing west into the open row-gap lane, instead of wedged in the
    # 1.5mm keyhole between the cap column and the LED row where /LED_DATA kept
    # failing. U9 is 1.1mm tall, so grid-zone margin placement is legal.
    ("U9",  147.900, 124.500,   0),   # 74AHCT1G125 level shifter
    ("C29", 147.900, 121.000,   0),   # its VBUS bypass, follows it
    # --- region F, east of J1 below the antenna zone
    ("R17", 158.240, 162.560,  90),   # 1k, the power indicator's series pair
    ("D3",  160.025, 162.560,  90),   # green PWR LED
    # --- south-west strip, west of J2
    ("R5",  101.825, 155.325,   0),   # 120R printer TX termination
    ("R7",  101.825, 157.742,   0),
    ("R8",  101.825, 160.159,   0),   # 0R /SHELL
    ("C15", 101.825, 162.576,   0),   # 1n /SHELL, kept at the connector
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
