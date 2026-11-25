# SPDX-License-Identifier: CERN-OHL-S-2.0
"""A jig for drilling SW1's reset hole through the tray's east wall.

The tray prints with the east wall solid there: a hole that small, printed
sideways through a wall, comes out as a slit and marks the wall's layers
(params.RESET_DRILL_D). The jig guides a RESET_DRILL_D bit square into the
wall on SW1's button axis instead.

In use, in the case frame: the empty tray stands on a table, before its feet
go on, and the jig stands on the same table against the outside of the east
wall, its fence round the corner against the back wall. The table sets the
height, the fence the place along the wall, and the jig's face the angle.
The bit goes in at the jig's outer face and out of its inner face straight
into the wall.

It prints lying on its outer face (`print_pose`), so the guide bore stands
upright and prints round, and every other face is upright too.
"""
from build123d import Box, Cone, Cylinder, Location, Part, Pos, Rot

import case
import params as p

X_WALL = p.BOARD_W + p.WALL
"""The east wall's outer face, which the jig's inner face bears on."""

Y_BACK = p.BOARD_L + p.WALL
"""The back wall's outer face, which the fence bears on."""

LEAD_IN_D = 3.00
"""The countersink round the bore's mouth, to start the bit."""


def _box(x0, x1, y0, y1, z0, z1) -> Part:
    return Pos((x0 + x1) / 2, (y0 + y1) / 2, (z0 + z1) / 2) * Box(x1 - x0, y1 - y0, z1 - z0)


def reset_jig() -> Part:
    """The jig, in the case frame, where it stands against the tray."""
    y, z = case.reset_axis()
    z0, z1 = p.Z_FLOOR_BOTTOM, z + p.JIG_PAST
    x1 = X_WALL + p.JIG_BORE_L
    block = _box(X_WALL, x1, y - p.JIG_PAST, Y_BACK + p.JIG_FENCE, z0, z1)
    fence = _box(X_WALL - p.JIG_REACH, x1, Y_BACK, Y_BACK + p.JIG_FENCE, z0, z1)
    bore = Pos(X_WALL + p.JIG_BORE_L / 2, y, z) * Cylinder(
        p.JIG_BORE_D / 2, p.JIG_BORE_L + 0.02, rotation=(0, 90, 0))
    # A 45-degree countersink at the outer face, where the bit goes in. It
    # opens onto the bed in print, so it is an overhang at exactly 45 degrees.
    h = (LEAD_IN_D - p.JIG_BORE_D) / 2
    lead = Pos(x1 - h / 2 + 0.005, y, z) * Cone(
        p.JIG_BORE_D / 2, LEAD_IN_D / 2 + 0.005, h + 0.01, rotation=(0, 90, 0))
    return block + fence - bore - lead


def print_location(part: Part) -> Location:
    """Case frame to print frame: the jig turned onto its outer face (case
    X = X_WALL + JIG_BORE_L) on the bed at Z = 0, the bore upright."""
    turn = Rot(0, 90, 0)  # case +X becomes -Z
    bb = (turn * part).bounding_box()
    return Pos(-bb.min.X, -bb.min.Y, -bb.min.Z) * turn


def print_pose(part: Part) -> Part:
    return print_location(part) * part


def reset_jig_printable() -> Part:
    return print_pose(reset_jig())
