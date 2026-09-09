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
0.80mm recessed -- spec section 3."""

FLOOR = 2.40
"""12 layers at 0.2mm."""

STANDOFF = 3.00
"""Board sits this far off the floor, clearing the through-hole tails of
J2/J4/S1/SW3/U7. Measured off the vendor STEP (Task 5): J2/J4's pin tips
reach 2.150mm below the board's underside on the Kycon MiniDIN-8 STEP, so
3.00 leaves 0.85mm under the tips for hand-soldered fillets the STEP
doesn't model. The original 2.00 was an estimate that left the pins
poking 0.15mm into the floor slab -- see task-5-report.md."""

BOSS_RELIEF = 0.10
"""Added 2026-09-08: each standoff boss stops this far SHORT of
Z_BOARD_BOTTOM instead of running flush to it. The board's exported bottom
mask/copper sheets (~/Projects/shop's board_geom GLB) sit at case Z -1.68,
0.08mm below the nominal -1.60 underside -- with the boss running flush to
-1.60 it penetrated those sheets by 0.08mm (z-fighting in the 3D viewer,
and physically the boss would be seating on solder mask or a copper pour,
not laminate). 0.10 clears the measured 0.08mm with a little margin. Does
NOT change STANDOFF -- only where the boss's own top face stops; see
case.standoffs()."""

PCB = 1.60
PANEL = 1.60
"""The art panel, and the Lite lid that stands in for it."""

CAVITY = 12.74
"""Board top face to lid underside.

CORRECTED 2026-09-08: this used to be 14.34 -- the spec's derivation of
"mini-DIN shells top out 12.74mm above the board face, plus 1.60mm for the
panel's PEM SMTSO-M25-3ET nuts standing proud of its underside" -- which
made Z_WALL_TOP (= CAVITY + PANEL, the panel's own thickness) double-count
that 1.60mm: once inside CAVITY, once again as PANEL. The wall stood 1.6mm
proud of the panel's top edge as a result -- visible in the assembled 3D
viewer.

Measured directly off the two real GLBs instead (shop's board_geom, board
frame -> this project's case frame, ~/Projects/shop/public/models/
airwriter-pro.glb):
    LATTICE   case Z  0.00 .. 12.74
    J2 shell  case Z -3.75 .. 12.74
    PANEL     case Z  9.65 .. 14.32      (panel top ~14.32, PANEL=1.60 thick)
The panel rests directly on the lattice at 12.74, not on top of proud nuts --
so CAVITY is that lattice height, board top face to lid underside, full
stop, and Z_WALL_TOP (CAVITY + PANEL = 14.34) comes out flush with the
panel's own measured top (14.32, within GLB/print rounding).

FLAG: this contradicts the spec's own nut-stack derivation above. If the
physical PEM nuts really do stand proud of the panel's UNDERSIDE by 1.60mm
(as opposed to sitting in a lattice counterbore, which the GLB doesn't
model), the panel would not seat flush at 12.74 and this case would come out
1.60mm short of clearing the nuts -- re-check against a real panel + real
nuts before printing, not just against the GLB."""

POCKET_CLEAR = 0.20
"""Per side, on the board pocket and the panel/lid seat."""

# --- fasteners -------------------------------------------------------------

SCREW_CLEAR_D = 2.80
"""M2.5 clearance."""

SCREW_HEAD_D = 5.00
"""Counterbore for an M2.5 pan head, in the floor's underside."""

SCREW_HEAD_DEPTH = 1.40

STANDOFF_D = 6.00
"""Boss diameter under each mounting hole, in the tray. Wide enough to carry
the board and to leave a printable wall around the 2.80 clearance bore."""

LEG_D = 6.00
"""Lite lid leg diameter -- enough plastic round a 3.60 insert bore."""

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

DIP_SLOT_Z0 = 6.60
DIP_SLOT_Z1 = 10.60
"""S1's key slot, z above the board's top face. HANDOFF records the key tips
travelling 8.11-9.01.

CORRECTED 2026-09-08: this used to be 7.60/9.50 (1.90mm open), which left
only ~0.5mm of reach above and below the 8.11-9.01 key band -- not enough to
get a tool onto a key at the bottom of a 3mm-deep tunnel and rock it. Widened
to 6.60/10.60 (4.00mm open, ~1.5mm of access each side of the key band) for
finger/tool access down the slot, not just key clearance. Confirm against
the STEP interference check."""

DIP_KEY_HALF_WIDTH = 0.65
"""Half the key's own width, traced from the vendor VRML (hardware/airwriter/
lib/airwriter.3dshapes/SW_DIP_8_RightAngle_DP-08RP.wrl, authored in mm/2.54):
each of the eight keys is 1.300mm wide (x 2.400 protrusion x 0.900 tall)."""

DIP_SLOT_MARGIN = 0.80
"""Added at each end of the key BAND'S OWN EDGE (outermost key centre +-
DIP_KEY_HALF_WIDTH), not at the pad column centres directly.

CORRECTED 2026-09-08: this used to be 0.60, added straight onto the pad
column centres (footprint-local x = -8.89 .. +8.89) with no allowance for
the key's own 1.30mm width -- so the outer two keys' own edges, at
centre +- DIP_KEY_HALF_WIDTH (0.65), poked 0.05mm past the old margin and
into the wall material (0.65 - 0.60 = 0.05mm overlap), which is what showed
as the side-surface glitch. Raised to 0.80 so there is real margin (0.15mm)
outside each key's own edge once DIP_KEY_HALF_WIDTH is subtracted first --
see case.dip_slot()."""

RESET_ACTUATOR_Z = 3.5 / 2
"""Height of SW1's (Panasonic EVQP44B3M side-press) actuator axis above the
board's top face. SW1 has no 3D model placed on the board, so this can't be
booleaned from a STEP; its footprint's own descr instead records the body as
6.2 x 2.55 x 3.5mm. A side-press switch's SMD body sits flush on the board
like any other SMD part (Z=0 at its base), and the side button is centred on
the body's own height, so the axis lands at half of 3.5mm -- a derivation
from the footprint descr, unverified against a Panasonic mechanical drawing
(not in this repo). This is consistent with (NOT confirmed by)
EVQP44B3M_SidePress.wrl (hand-authored, not wired to the footprint -- OCCT
can't import .wrl anyway): its actuator Transform's Z translation is 0.689
(the file's own header: values are mm/2.54) = 1.7499mm, matching to within
the file's own rounding. That agreement is NOT independent evidence -- the
WRL was hand-written from measured dimensions with the actuator's Z set
equal to the body's centre Z, the same "centred on the body" assumption used
above, so the two were guaranteed to agree to four decimal places regardless
of whether the assumption is right. The number (1.75mm) stands; task 6 report
has the full derivation and the ruling that struck the "independently
confirmed" framing.

RULING 1 (task-6-report.md) drops carveouts() and its CARVEOUT_R/
CARVEOUT_CLEAR params: the tray's interior is one open prism, and U4/R3/U9
all sit well inboard of every wall, so subtracting cylinders there would
subtract air from air. test_case.TestSideFeaturesAndTray covers the
requirement's real intent instead -- see its
test_internal_parts_keep_at_least_1mm_of_air_to_the_tray."""

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

CORNER_R = bg.corner_radius()
"""The board's own corner radius, read from its Edge.Cuts arcs (2.00mm as
drawn). The art panel (hardware/panel/panel.kicad_pcb) shares this exact
outline -- same 62 x 104.27 footprint, same R2.0 corners -- so this one
number, read only off the main board, also governs the panel/lid seat."""

POCKET_R = round(CORNER_R + POCKET_CLEAR, 6)
"""The board's corner radius grown by the pocket clearance: the board pocket
and the panel/lid seat's corner radius. What the board's rounded corner
naturally sweeps out at POCKET_CLEAR standoff."""

EXT_R = round(CORNER_R + WALL, 6)
"""The board's corner radius grown by the wall thickness: what offsetting
the board outline outward by WALL naturally produces at the tray's exterior
corner, and a better printed part than a sharp vertical arris."""
