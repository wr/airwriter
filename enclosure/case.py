"""The AirWriter enclosure: a tray shared by both builds, and a Lite lid.

Everything is modelled in the case frame (see board_geom): X=0 is the west
wall's inner face, Y=0 is the front (port) wall's inner face, Z=0 is the
board's top face with +Z up.
"""
from build123d import (
    Box,
    Cone,
    ConvexPolyhedron,
    Cylinder,
    Part,
    Pos,
    Rot,
    import_step,
    offset,
)

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


_STEP_CACHE: dict[str, Part] = {}


def _imported_step(path: str) -> Part:
    """import_step() takes real time (~0.15s here); connector_solid() is
    called many times across the port tests, so cache the raw import per
    path. Callers still get their own transformed copy -- Location * Shape
    returns a new object, it doesn't mutate the cached one."""
    if path not in _STEP_CACHE:
        _STEP_CACHE[path] = import_step(path)
    return _STEP_CACHE[path]


def connector_solid(ref: str) -> Part:
    """A connector's own vendor STEP, placed in the case frame by the FULL
    KiCad 3D-model transform: model scale, then model rotation, then model
    offset, then the footprint's own board rotation and position, then the
    case frame's Y flip.

    KiCad negates a footprint's stored `rotate` before applying it, as three
    separate rotations about the FIXED model axes, in the order X, then Y,
    then Z (not the model's own rotating axes, and not Z-Y-X). Applying the
    footprint's own board rotation to that same untouched numeric frame,
    using its board-file sign as-is (no negation), and then translating by
    board_geom.to_case(...), lands correctly in the case frame: the sign
    flip that a naive read of local_to_board would suggest, and the Y flip
    from the case frame's own convention, cancel each other out. All of this
    was derived empirically against the three measured ground truths in
    test_case.TestPorts -- see task-5-report.md for the alternates (e.g.
    rotating Z-then-X, or not negating the stored rotation) that were tried
    and clearly wrong (off by mm, not fractions of one).
    """
    fp = bg.footprint(ref)
    if not fp.model or not fp.model.endswith(".step"):
        raise RuntimeError(f"{ref} has no STEP model: {fp.model}")
    part = _imported_step(fp.model)

    if fp.model_scale != (1.0, 1.0, 1.0):
        part = part.scale(fp.model_scale, about=(0, 0, 0))

    mrx, mry, mrz = fp.model_rotate
    part = Rot(-mrx, 0, 0) * part
    part = Rot(0, -mry, 0) * part
    part = Rot(0, 0, -mrz) * part
    part = Pos(*fp.model_offset) * part

    part = Rot(0, 0, fp.rot) * part
    x, y = bg.to_case(fp.x, fp.y)
    return Pos(x, y, 0) * part


def _grown(ref: str, amount: float) -> Part:
    """A connector's solid, grown outward by `amount`.

    The vendor STEPs (300+ faces on the mini-DINs, with pins spaced close
    together) don't survive build123d's `offset()` -- BRepOffsetAPI's true
    3D offset returns a null shape, or a near-zero-volume degenerate one,
    for every amount and tolerance tried against connector_solid() directly.
    Offsetting the connector's convex hull instead is robust (a handful of
    large flat/curved faces, nothing close enough to self-intersect) and
    still uses the real measured geometry, not a guessed envelope: the hull
    always contains the solid (same bounding box; only fills in
    concavities), so the grown hull always contains the true grown solid,
    and an opening cut from it clears the connector by at least `amount`
    everywhere -- possibly more near a concave feature. See task-5-report.md
    for the failed direct-offset attempts.
    """
    solid = connector_solid(ref)
    hull = ConvexPolyhedron([tuple(v) for v in solid.vertices()])
    return offset(hull, amount)


def port_cutouts() -> Part:
    """What the front wall gives up to J2, J4 and J1.

    Each opening is the connector's own solid (by way of its convex hull,
    see `_grown`) grown by PORT_CLEAR, so the barrel diameters, flanges and
    shell heights come from the vendor geometry rather than from a
    datasheet reading. The USB-C receptacle sits ~1.5mm behind the wall's
    outer face, so its opening also gets an outside chamfer wide enough for
    a plug's overmould.
    """
    out = None
    for ref in ("J2", "J4", "J1"):
        cut = _grown(ref, p.PORT_CLEAR)
        out = cut if out is None else out + cut

    j1 = connector_solid("J1").bounding_box()
    flare = p.PORT_CLEAR + p.USBC_CHAMFER
    chamfer = Pos(
        (j1.min.X + j1.max.X) / 2,
        -p.WALL + p.USBC_CHAMFER / 2,
        (j1.min.Z + j1.max.Z) / 2,
    ) * Cone(
        (j1.size.X + 2 * flare) / 2,
        (j1.size.X + 2 * p.PORT_CLEAR) / 2,
        p.USBC_CHAMFER,
        rotation=(90, 0, 0),
    )
    return out + chamfer
