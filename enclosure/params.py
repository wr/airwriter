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

BOTTOM_CHAMFER = 0.80
"""45-degree chamfer around the tray's bottom outside edge, 4 layers tall. It
softens the arris the case rests and slides on, and it keeps the first
layers' elephant's-foot flare inside the wall line instead of proud of it. 45
degrees is exactly the unsupported-overhang limit (test_printable), so it
prints floor-down with no supports; a fillet there would start flatter than
that. Under a third of FLOOR and of WALL, so neither gets thin."""

TOP_CHAMFER = 0.40
"""45-degree chamfer around the tray's top outside edge, 2 layers. Added
2026-10-13 (Wells: "a small top chamfer"). Outside edge only: the pocket edge
the panel / Lite lid sits against stays sharp, so no groove opens round the
panel. Sized by the J2/J4 lintels, whose openings top out 1.19mm under
Z_WALL_TOP: a 45-degree chamfer takes its own depth off a lintel at the
wall's outer face, so 0.40 leaves 0.79mm there, where matching
BOTTOM_CHAMFER's 0.80 would leave two layers of bridge over a mini-DIN. It
faces up when printed floor-down, so the overhang gate has no say in it."""

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

MINIDIN_TOP = 13.04
"""Top of J2/J4's shells above the board's top face, MEASURED on rev A
(Wells, 2026-10-14). The Kycon STEP tops out at 12.75, so connector_solid()
lifts both mini-DINs 0.29. Whether the real body sits higher on its standoffs
or is just taller, the lift keeps their windows clear: 0.40 above, and at
least 0.11 below even if only the top is taller."""

CAVITY = 13.04
"""Board top face to lid underside.

MEASURED 2026-10-14: the panel rests on J2/J4's shells, which Wells measured
at 13.04 (MINIDIN_TOP) on rev A, not the 12.74 read off the GLB below. At
12.74 the panel would sit 0.30 proud of the rim, and the lattice would leave
a 0.30 gap under the panel for light to cross between cells. Everything below
is the earlier history of this number.

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

POCKET_CLEAR = 0.30
"""Per side, on the board pocket and the panel/lid seat.

Raised from 0.20 on 2026-10-14, after the first rev A board would not go into
a printed tray. PCBWay routes the outline to +-0.2mm and an FDM pocket comes
out a little undersize, so 0.20 left no margin for either. The panel/lid gap
grows by the same 0.10."""

THIN_WALL = 1.70
"""Thickness of all four walls from the floor up to the panel seat. A 45-degree
ramp returns each to the full wall at Z_LID_UNDER, so the panel seat, the rim
seen from above and the outside are unchanged.

The west wall has to bow outward while S1's keys pass its rim. The keys stand
2.20mm past the board's west edge, into the wall, and the front-wall windows
hold the board within 0.40mm side to side, so a rigid west wall leaves no path
in (docs/enclosure-build-log/flex-wall-report.md). Thinned, it springs back
over the keys, whose tips then stand in the DIP window. Print the tray in
PETG: the bend pulls at the layers near the floor, and the first print, at
0.80, split there.

ALL FOUR walls, the whole of each, since 2026-10-17 (Wells). The thinning used
to be on the west wall only, tapering back to the full wall near each corner,
and every print had raised lines up the outside at both ends of the taper. The
slicer stops and restarts its wall lines where a wall changes thickness, at the
same spot on every layer, and translucent PETG shows the change itself. One
thickness all round leaves no change behind any flat face; only the ramp's
horizontal line under the rim remains. Printed 2026-10-17 in translucent PETG:
no lines, and the board slips in as before (Wells).

1.70 is four 0.4mm lines. On a test piece of the old taper, 1.70 felt almost
smooth where 1.20 had a ridge. FEA (flex-wall/fea.py, PETG E 2 GPa, 0.8mm
mesh, the rim moved out 2.0mm over the keys' path): 46 N, peak strain 2.16% at
the DIP window's corners, 1.17% vertical at the floor. The old west-only
thinning took 32 N and 1.99% at 1.20, 53 N and 2.31% at 1.70."""

COVE_R = 1.50
"""Fillet where each thin wall meets the floor. The west wall bends there, and
across the print's layers, so the corner is rounded to spread the strain."""

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

DIP_SLOT_Z0 = 4.60
DIP_SLOT_Z1 = 9.90
"""S1's key slot, z above the board's top face.

WIDENED 2026-10-14 from 5.20-9.30 after the first print: the keys touched
both the top and the bottom of the slot and could barely be toggled. A piano
key rocks, so its tip needs room above and below; 0.60 more on each edge. The
top now stands 0.49 above S1's body (9.41), which reopens a sliver of the view
past the switch described below, traded for keys that work.

What follows is the slot's earlier history.

Sized to the switch's OWN key recess, not to the key tips. The part in the
render is generated by the shop repo's scripts/board-model/tools/dp08rp.mjs,
which models more than the footprint's plain-box VRML: each key rocks in a
1.7mm-wide slot spanning z 5.6-9.0, the key levers carry a lip running down to
z 5.8, the keys themselves sit at 8.11-9.01, and the moulded body tops out at
9.41.

So the opening runs 5.20-9.30: it clears the whole lever, not just the tip, and
stops 0.11 below the body's top so the wall never opens onto the void above the
switch. An earlier 6.60-10.60 did exactly that -- Wells saw a black bar above
the keys, which was the case looking past the part into empty cavity. The one
before that, 7.60-9.50, was only 1.90mm and could not be worked with a tool.
"""

FOOT_RELIEF = 0.60
FOOT_RELIEF_H = 1.60
"""The Lite lid's legs are trimmed by FOOT_RELIEF for their first
FOOT_RELIEF_H, so a leg does not stand on a small SMD part crowding a
mounting hole. C22 (a 0603, 1.24mm tall) reaches within 2.844mm of H4's
centre against the 3.00mm leg radius; 0.60 of relief clears it by 0.44.
Above the relief the leg keeps its full LEG_D, so the heat-set insert has
1.20mm of wall over most of INSERT_DEPTH."""

DIP_SLOT_R = 1.00
"""Corner radius of the DIP slot window, seen from the side. The window sits
in the flexing west wall (THIN_WALL) and its square corners were the peak
strain there, 2.0%; rounded, 1.65%. The rounding fills back only the very
corners: the outer keys' own corners sit DIP_SLOT_MARGIN (0.80) in from the
window's ends and 0.30 below its top, 0.63mm inside the 1.00 arc."""

DIP_KEY_HALF_WIDTH = 0.65
"""Half a key's width. The DP-08RP's keys are 1.30mm wide (dp08rp.mjs's KEY.w,
traced from the footprint's F.Fab rectangles), so the key BAND reaches this far
past the outermost key centre. The slot is built from the band's edges, not
from the pad column centres."""

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

# --- light-guide lattice ---------------------------------------------------
# docs/superpowers/specs/2026-10-14-light-guide-lattice-design.md. Its height
# is CAVITY: the panel rests on lattice and mini-DIN shells together.

LATTICE_WALL = 0.80
"""Cell walls: two 0.4mm perimeters, opaque in black (Wells, 2026-10-14; his
earlier hand-drawn lattice used 0.60)."""

LATTICE_INSET = 0.25
"""Lattice outline inside the board outline; with POCKET_CLEAR that is 0.55
to the tray walls."""

CELL_R = 1.00
"""Inner corner radius of each cell, as Wells's earlier lattice had."""

POST_D = 8.00
"""Posts at H1-H4: a 0.80 wall round the standoff pocket."""

NUT_POCKET_D = 6.40
NUT_POCKET_DEPTH = 3.30
"""Pocket in each post top for the panel's Würth 9774030151R standoff (Ø5.1
x 3.0, soldered to a 6.0 land): the land plus fillet, and 0.3 below the
standoff's end so the panel bears on the lattice, not on the standoffs."""

PART_CLEAR = 0.40
PART_TOP_CLEAR = 0.50
"""Grown onto each part envelope sideways / on top (components.py)."""

D3_TUBE_CLEAR = 0.25
"""The power LED's tube: the panel's FR4 window plus this, inside."""

WEST_RAIL_X1 = 7.00
EAST_RAIL_X0 = 50.00
EAST_RAIL_X1 = 55.75
"""Rails forward to the H3 / H4 posts. East: over U1's shield, west of the
antenna keepout (56.00) by KEEPOUT_CLEAR."""

KEEPOUT_CLEAR = 0.25

TEST_PLUNGER_D_TOP = 3.10
TEST_PLUNGER_D_BASE = 3.50
TEST_BODY_TOP = 3.20
"""SW3's plunger, measured by Wells on rev A: 3.1 at the lattice top, tapering
to 3.5 where it enters the switch body, which stands about 3.2. (The KiCad
model's plunger is a plain 3.5 cylinder.)"""

TEST_PLUNGER_OFFSET = (0.20, -0.20)
"""Where the real plunger stands relative to the KiCad model's axis (east,
north). A collar on the model's axis pushed the plunger about 0.4 west and
0.4 back; moved 0.4 east and forward, it pushed it east and forward instead.
So, split the difference (Wells, 2026-10-15)."""

COLLAR_H = 2.00
COLLAR_BORE_D = 3.60
COLLAR_WALL = 1.00
"""Collar bracing the tall test plunger in the lattice's top COLLAR_H only:
0.25 a side on the plunger's 3.1 at the top, 0.21 on its 3.18 at the collar's
foot, so it steadies the plunger without dragging on it. Held to the cell's
four walls by LATTICE_WALL ribs. (Wells, 2026-10-15: "really tall and
wobbly".)"""

RIDGE_H = 1.50
RIDGE_CLEAR = 0.10
"""Lip round the lattice's top edge, reaching to RIDGE_CLEAR short of the
tray's pocket wall, so the lattice sits snug and the gap round the panel's
edge shows black plastic instead of the inside of the case (Wells,
2026-10-15)."""

