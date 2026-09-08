"""Every number this project CHOOSES, with where it came from.

Anything the board decides lives in board_geom.py instead. If you are about to
add a board dimension here, you are about to introduce drift -- read it from
the board file.

Source for the rulings below:
docs/superpowers/specs/2026-09-08-airwriter-enclosure-design.md
"""
import board_geom as bg

# --- ruled by the spec -----------------------------------------------------

WALL = 3.00
"""Uniform, all four walls. Set by the mini-DIN mating faces, which stand
2.75mm proud of the board edge and want to land 0.25mm recessed. Wells ruled
uniform over honouring S1's 2.25mm west-wall placement, so S1's piano keys sit
0.75mm recessed -- spec section 3."""

FLOOR = 2.40
"""12 layers at 0.2mm."""

STANDOFF = 2.00
"""Board sits this far off the floor, clearing the through-hole tails of
J2/J4/S1/SW3/U7."""

PCB = 1.60
PANEL = 1.60
"""The art panel, and the Lite lid that stands in for it."""

CAVITY = 14.34
"""Board top face to lid underside. NOT a free variable: the mini-DIN shells
top out 12.74mm above the board face, the lattice is squashed to exactly that,
and the panel's PEM SMTSO-M25-3ET nuts stand 1.60mm proud of its underside."""

POCKET_CLEAR = 0.20
"""Per side, on the board pocket and the panel/lid seat."""

# --- fasteners -------------------------------------------------------------

SCREW_CLEAR_D = 2.80
"""M2.5 clearance."""

SCREW_HEAD_D = 5.00
"""Counterbore for an M2.5 pan head, in the floor's underside."""

SCREW_HEAD_DEPTH = 1.40

INSERT_D = 3.60
"""Bore for an M2.5 brass heat-set insert in the Lite lid's legs. Final MPN is
an open item in the spec; re-cut this if the chosen insert differs."""

INSERT_DEPTH = 4.50

# --- openings --------------------------------------------------------------

PORT_CLEAR = 0.40
"""Radial clearance added to each connector's own solid when it is subtracted
from the front wall. The spec's assertion floor is 0.30."""

USBC_CHAMFER = 1.20
"""45-degree chamfer on the OUTSIDE of the USB-C opening. The receptacle sits
1.5mm behind the wall, so a plug's overmould needs somewhere to go. Tuned on
the first print against a real cable -- open item 3 in the spec."""

RESET_PINHOLE_D = 1.20
"""East wall, on SW1's actuator axis."""

PLUNGER_BORE_D = 2.25
"""SW3's test plunger. Matches the art panel's own 2.25mm NPTH so the Lite lid
behaves identically -- the plunger is <= 2.0mm."""

DIP_SLOT_Z0 = 7.60
DIP_SLOT_Z1 = 9.50
"""S1's key slot, z above the board's top face. HANDOFF records the key tips
travelling 8.11-9.01; 0.5mm of reach is added each way so a pen tip can work a
key recessed 0.75mm. Confirm against the STEP interference check."""

DIP_SLOT_MARGIN = 0.60
"""Added at each end of the key band (the pad columns at footprint-local
x = -8.89 .. +8.89)."""

CARVEOUT_CLEAR = 0.50
"""Radial air added around an internal part's keep-out cylinder."""

# --- derived ---------------------------------------------------------------

_min_x, _min_y, _max_x, _max_y = bg.outline()
BOARD_W = round(_max_x - _min_x, 6)
BOARD_L = round(_max_y - _min_y, 6)

EXT_W = round(BOARD_W + 2 * WALL, 6)
EXT_L = round(BOARD_L + 2 * WALL, 6)

Z_BOARD_BOTTOM = -PCB
Z_FLOOR_TOP = Z_BOARD_BOTTOM - STANDOFF
Z_FLOOR_BOTTOM = Z_FLOOR_TOP - FLOOR
Z_LID_UNDER = CAVITY
Z_WALL_TOP = CAVITY + PANEL

EXT_H = round(Z_WALL_TOP - Z_FLOOR_BOTTOM, 6)

POCKET_W = round(BOARD_W + 2 * POCKET_CLEAR, 6)
POCKET_L = round(BOARD_L + 2 * POCKET_CLEAR, 6)
