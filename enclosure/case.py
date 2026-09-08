"""The AirWriter enclosure: a tray shared by both builds, and a Lite lid.

Everything is modelled in the case frame (see board_geom): X=0 is the west
wall's inner face, Y=0 is the front (port) wall's inner face, Z=0 is the
board's top face with +Z up.
"""
import math

from build123d import (
    Box,
    Cone,
    ConvexPolyhedron,
    Cylinder,
    Part,
    Pos,
    RectangleRounded,
    Rot,
    import_step,
    loft,
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
    if fp.layer != "F.Cu":
        # The transform below places the STEP as authored, with no mirror.
        # Every connector this project touches is a front-side (F.Cu)
        # footprint; a back-side one would need an X (or Z, depending on
        # convention) mirror before the rotation, which nothing here does.
        raise NotImplementedError(
            f"{ref} is on {fp.layer}, not F.Cu -- connector_solid() doesn't "
            "mirror back-side footprints")
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


def _sphere_directions(n: int) -> list[tuple[float, float, float]]:
    """`n` unit vectors spread roughly evenly over the sphere (a golden-angle
    Fibonacci spiral -- cheap, no external dependency, no clustering at the
    poles the way a naive lat/long grid has)."""
    golden_angle = math.pi * (3 - math.sqrt(5))
    dirs = []
    for i in range(n):
        z = 1 - 2 * (i + 0.5) / n
        r = math.sqrt(max(0.0, 1 - z * z))
        theta = golden_angle * i
        dirs.append((r * math.cos(theta), r * math.sin(theta), z))
    return dirs


_GROWN_DIRS = _sphere_directions(98)
"""Direction set for _grown()'s outward growth. 98 was checked directly
against the 0.30mm spec floor at PORT_CLEAR=0.40: 14 directions measures
0.298mm clearance to the finished wall (fails the spec outright), 26 gives
0.344, 50 gives 0.371, 98 gives 0.385 -- accuracy keeps climbing as the
direction set approximates a true sphere more closely, with no free lunch at
few directions. The cost of more directions turned out to be flat in
practice (the tessellation and first hull-reduction below dominate runtime,
not the final grow-and-rehull), so there was no reason to settle for a
thinner margin: 98 is the number the fix was verified against, kept for
one-to-one parity with that verification."""


def _grown(ref: str, amount: float) -> Part:
    """A connector's solid, grown outward by `amount` -- what gets cut from
    the front wall for a `amount`-mm-clearance opening.

    This does NOT hull the BRep's topological vertices (that was the bug:
    see git history/task-5-report.md Finding 1). A vertex hull chords across
    every curved face -- it is the hull of the solid's CORNERS, not of the
    solid -- so on a barrel or a rounded shell it cuts inside the true
    surface by the chord's sagitta (~0.37mm on the USB-C's corner radii, big
    enough to eat almost the entire 0.40mm PORT_CLEAR). The hull of a dense
    tessellation of the actual boundary doesn't have this problem: as the
    tessellation gets finer it converges on the hull of the true solid, not
    the hull of a handful of its corners.

    Growing that hull outward by `amount` in every direction is a discrete
    approximation of the true Minkowski sum with a ball of radius `amount`
    (hull(P) (+) amount*ball): for a finite direction set D and any point
    set P, hull(P (+) amount*D) == hull(P) (+) amount*hull(D) (Minkowski sum
    distributes over convex hull), and hull(D) approaches the unit ball as D
    covers the sphere more densely -- so growing every tessellated point
    along every direction in _GROWN_DIRS and re-hulling approximates the
    true offset of hull(solid), and hull(solid) contains solid, so the
    result still clears the real connector by at least `amount` everywhere
    (possibly more near a concave feature, same caveat as before: this
    grows the SOLID's hull, not the solid itself).

    Growing every one of the ~30000 tessellated points along all 98
    directions before re-hulling would work but is needlessly slow (~3
    million points into scipy's ConvexHull). It doesn't have to: a convex
    hull is decided entirely by its own extreme points, so
    hull(P (+) amount*D) == hull(vertices(hull(P)) (+) amount*D) exactly
    (same identity applied twice) -- hulling the raw tessellation first
    strips it down to its own few hundred corner points, and only those get
    grown and re-hulled. Verified numerically against the un-reduced
    computation on J2 at a smaller direction count: identical volume to
    within float noise (4.5e-13 mm^3), ~35% faster.

    See task-5-report.md for the measured before/after clearances and the
    failed direct-`offset()`-on-the-STEP attempts from the prior round.
    """
    key = (ref, round(amount, 6))
    if key in _GROWN_CACHE:
        return _GROWN_CACHE[key]

    solid = connector_solid(ref)
    verts, _ = solid.tessellate(0.05)
    surface_pts = [tuple(v) for v in verts]
    corners = [tuple(v) for v in ConvexPolyhedron(surface_pts).vertices()]
    grown = ConvexPolyhedron([
        (x + amount * dx, y + amount * dy, z + amount * dz)
        for (x, y, z) in corners
        for (dx, dy, dz) in _GROWN_DIRS
    ])
    _GROWN_CACHE[key] = grown
    return grown


_GROWN_CACHE: dict[tuple[str, float], Part] = {}
"""_grown() is a pure function of (ref, amount), but a full call costs ~2s
(dominated by tessellate()'s ~30000-point mesh), and the test suite calls it
several times per ref at the same amount (setUpClass builds port_cutouts(),
then individual tests grow the same connectors again at their own
clearance). Cached the same way _imported_step() is: ConvexPolyhedron
results aren't mutated by the boolean ops callers apply to them."""


def _usbc_flare() -> Part:
    """The outside flare on J1's USB-C opening: a loft between a
    port-sized rectangle at the transition into the straight PORT_CLEAR
    envelope and a larger rectangle at the wall's outer face, so a plug's
    overmould has somewhere to go.

    This replaces a cone whose wide end pointed the wrong way (Finding 3,
    task-5-report.md): with `rotation=(90,0,0)` a Cone's wide (bottom_radius)
    end lands at the INNER end of the range build123d places it over, so the
    old code flared toward the *board*, leaving the opening narrowest right
    where a plug enters -- backwards from the stated purpose. It also grew
    its cone radius from j1.size.X (the connector's 8.94mm WIDTH) and swept
    that same radius the full height of a circular cone, so the cut's
    vertical reach had nothing to do with the port's actual 4.21mm height --
    it over-cut down into the floor slab and up well past the receptacle.

    Here the flare's rectangles are sized from the connector's real X and Z
    extents directly (no radius standing in for two different dimensions),
    so the Z reach is clamped to the port's own height, not derived from its
    width -- this is what keeps the cut off the floor slab without needing a
    separate vertical limit.
    """
    j1 = connector_solid("J1").bounding_box()
    xc = (j1.min.X + j1.max.X) / 2
    zc = (j1.min.Z + j1.max.Z) / 2

    inner_w = j1.size.X + 2 * p.PORT_CLEAR
    inner_h = j1.size.Z + 2 * p.PORT_CLEAR
    outer_w = inner_w + 2 * p.USBC_CHAMFER
    outer_h = inner_h + 2 * p.USBC_CHAMFER

    # Where the flare meets the straight _grown() envelope (matches the old
    # cone's chamfer depth), and the wall's own outer face.
    y_inner = -p.WALL + p.USBC_CHAMFER
    y_outer = -p.WALL

    inner_face = Pos(xc, y_inner, zc) * Rot(90, 0, 0) * RectangleRounded(
        inner_w, inner_h, min(inner_w, inner_h) / 2 * 0.9)
    outer_face = Pos(xc, y_outer, zc) * Rot(90, 0, 0) * RectangleRounded(
        outer_w, outer_h, min(outer_w, outer_h) / 2 * 0.9)
    return loft([inner_face, outer_face])


def port_cutouts() -> Part:
    """What the front wall gives up to J2, J4 and J1.

    Each opening is the connector's own solid grown by PORT_CLEAR (see
    `_grown`), so the barrel diameters, flanges and shell heights come from
    the vendor geometry rather than from a datasheet reading. The USB-C
    receptacle sits ~1.5mm behind the wall's outer face, so its opening also
    gets an outside flare wide enough for a plug's overmould (`_usbc_flare`).
    """
    out = None
    for ref in ("J2", "J4", "J1"):
        cut = _grown(ref, p.PORT_CLEAR)
        out = cut if out is None else out + cut
    return out + _usbc_flare()
