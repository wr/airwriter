"""The AirWriter enclosure: a tray shared by both builds, and a Lite lid.

Everything is modelled in the case frame (see board_geom): X=0 is the west
wall's inner face, Y=0 is the front (port) wall's inner face, Z=0 is the
board's top face with +Z up.
"""
from build123d import Box, Cone, Cylinder, Part, Pos

import board_geom as bg
import params as p


def shell() -> Part:
    """Floor plus four walls, hollow from the floor's top face upward.

    No openings: the port cutouts, the DIP slot, the pinhole, the screw holes
    and the carve-outs are all subtracted by tray().
    """
    outer = Pos(
        p.BOARD_W / 2,
        p.BOARD_L / 2,
        (p.Z_WALL_TOP + p.Z_FLOOR_BOTTOM) / 2,
    ) * Box(p.EXT_W, p.EXT_L, p.EXT_H)

    cavity = Pos(
        p.BOARD_W / 2,
        p.BOARD_L / 2,
        (p.Z_WALL_TOP + p.Z_FLOOR_TOP) / 2,
    ) * Box(p.POCKET_W, p.POCKET_L, p.Z_WALL_TOP - p.Z_FLOOR_TOP)

    return outer - cavity


STANDOFF_D = 6.00
"""Boss diameter under each mounting hole. Wide enough to carry the board and
to leave a printable wall around the 2.80 clearance bore."""


def standoffs() -> Part:
    """Four bosses from the floor's top face up to the board's underside."""
    h = p.Z_BOARD_BOTTOM - p.Z_FLOOR_TOP
    out = None
    for x, y in bg.mount_holes().values():
        boss = Pos(x, y, p.Z_FLOOR_TOP + h / 2) * Cylinder(STANDOFF_D / 2, h)
        out = boss if out is None else out + boss
    return out


def screw_holes() -> Part:
    """Clearance bore up through floor and boss, with a counterbore for the
    screw head in the floor's underside.

    The counterbore opens onto the print's first layer, so its transition to
    the clearance bore is a 45-degree cone rather than a flat annulus -- a
    flat there is an unsupported bridge (see the test)."""
    top = p.Z_BOARD_BOTTOM + 0.01
    bottom = p.Z_FLOOR_BOTTOM - 0.01
    out = None
    for x, y in bg.mount_holes().values():
        bore = Pos(x, y, (top + bottom) / 2) * Cylinder(
            p.SCREW_CLEAR_D / 2, top - bottom)
        cb_top = p.Z_FLOOR_BOTTOM + p.SCREW_HEAD_DEPTH
        cbore = Pos(x, y, (cb_top + bottom) / 2) * Cylinder(
            p.SCREW_HEAD_D / 2, cb_top - bottom)
        # 45-degree run-out from the counterbore up to the clearance bore.
        cone_h = (p.SCREW_HEAD_D - p.SCREW_CLEAR_D) / 2
        cone = Pos(x, y, cb_top + cone_h / 2) * Cone(
            p.SCREW_HEAD_D / 2, p.SCREW_CLEAR_D / 2, cone_h)
        one = bore + cbore + cone
        out = one if out is None else out + one
    return out
