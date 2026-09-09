"""The AirWriter enclosure: a tray shared by both builds, and a Lite lid.

Everything is modelled in the case frame (see board_geom): X=0 is the west
wall's inner face, Y=0 is the front (port) wall's inner face, Z=0 is the
board's top face with +Z up.
"""
import math

from build123d import (
    Axis,
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


def _rounded_box(w: float, l: float, h: float, cx: float, cy: float,
                  cz: float, radius: float) -> Part:
    """A Box(w, l, h) centred at (cx, cy, cz), with its four VERTICAL edges
    filleted to `radius` -- a rounded rectangle extruded through h. Only the
    vertical edges are touched (top and bottom rims stay sharp): both the
    tray's exterior/pocket and the lite lid's plate print with this shape
    flat-side-down, and a rounded bottom rim would be an overhang."""
    box = Pos(cx, cy, cz) * Box(w, l, h)
    verticals = box.edges().filter_by(Axis.Z)
    return box.fillet(radius, verticals)


def shell() -> Part:
    """Floor plus four walls, hollow from the floor's top face upward.

    The board (and the art panel it shares its outline with) has R2.0
    corners, not square ones -- board_geom.corner_radius() reads that off
    the board file rather than assuming it. The tray's exterior corners are
    that radius grown by WALL (EXT_R, what offsetting the board outline by
    the wall thickness naturally produces); the pocket's corners are that
    radius grown by POCKET_CLEAR (POCKET_R), so the board's own rounded
    corners register against the pocket instead of floating in a square
    one. Only the vertical edges are filleted -- see `_rounded_box`.

    No openings: the port cutouts, the DIP slot, the pinhole, the screw holes
    and the carve-outs are all subtracted by tray().
    """
    outer = _rounded_box(
        p.EXT_W, p.EXT_L, p.EXT_H,
        p.BOARD_W / 2, p.BOARD_L / 2, (p.Z_WALL_TOP + p.Z_FLOOR_BOTTOM) / 2,
        p.EXT_R)

    cavity = _rounded_box(
        p.POCKET_W, p.POCKET_L, p.Z_WALL_TOP - p.Z_FLOOR_TOP,
        p.BOARD_W / 2, p.BOARD_L / 2, (p.Z_WALL_TOP + p.Z_FLOOR_TOP) / 2,
        p.POCKET_R)

    return outer - cavity


def standoffs() -> Part:
    """Four bosses from the floor's top face up to BOSS_RELIEF short of the
    board's underside.

    Stopping short of Z_BOARD_BOTTOM, rather than running flush to it, is
    deliberate: the board's exported bottom mask/copper sheets sit at case Z
    -1.68, 0.08mm below the nominal -1.60 underside, so a boss running flush
    to -1.60 penetrated those sheets by 0.08mm -- the boss should seat on
    laminate, not on solder mask or a copper pour. See params.BOSS_RELIEF.
    Does not change STANDOFF (the board's stand-off height itself), only
    where this boss's own top face stops."""
    h = p.Z_BOARD_BOTTOM - p.BOSS_RELIEF - p.Z_FLOOR_TOP
    out = None
    for x, y in bg.mount_holes().values():
        boss = Pos(x, y, p.Z_FLOOR_TOP + h / 2) * Cylinder(p.STANDOFF_D / 2, h)
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
    #
    # y_inner is hardcoded to -WALL + USBC_CHAMFER (currently -1.800) rather
    # than derived from _grown("J1", PORT_CLEAR)'s own reach (currently
    # -1.891), so the two must be kept in sync by hand: this flare has to
    # start at or before the point where the straight envelope actually
    # ends, or a gap opens between them. At today's values there's a 0.091mm
    # overlap (safe -- the flare and the envelope both cover -1.891..-1.800,
    # so the union is unbroken). If USBC_CHAMFER is ever lowered, y_inner
    # moves toward the wall's outer face and can cross past -1.891, which
    # reopens a blind pocket of material between the flare and the straight
    # envelope; test_ports_are_actually_open catches that today (confirmed:
    # at USBC_CHAMFER=1.0, port_cutouts() leaves a 0.436mm^3 plug in the
    # port). No test currently asserts the overlap margin itself, only its
    # symptom.
    y_inner = -p.WALL + p.USBC_CHAMFER
    y_outer = -p.WALL

    inner_face = Pos(xc, y_inner, zc) * Rot(90, 0, 0) * RectangleRounded(
        inner_w, inner_h, min(inner_w, inner_h) / 2 * 0.9)
    outer_face = Pos(xc, y_outer, zc) * Rot(90, 0, 0) * RectangleRounded(
        outer_w, outer_h, min(outer_w, outer_h) / 2 * 0.9)
    return loft([inner_face, outer_face])


def _ridge_roof(x0: float, x1: float, y0: float, y1: float, z0: float,
                 rise: float, taper_axis: str) -> Part:
    """A 45-degree ridge-roof prism over the rectangle (x0..x1, y0..y1) at
    z0, rising to a ridge LINE at z0+rise -- unioned onto a cutting tool so
    its own flat top doesn't leave an unsupported horizontal ceiling in
    whatever it's subtracted from. `taper_axis` is the SHORT dimension that
    supplies the rise (`rise` should be half of it, for 45 degrees); the
    ridge runs the length of the other, long dimension. Same idea as
    screw_holes()'s counterbore cone, just a ridge instead of a point of
    revolution because these caps are long and thin, not round."""
    base = [(x0, y0, z0), (x1, y0, z0), (x1, y1, z0), (x0, y1, z0)]
    if taper_axis == "x":
        xm = (x0 + x1) / 2
        ridge = [(xm, y0, z0 + rise), (xm, y1, z0 + rise)]
    elif taper_axis == "y":
        ym = (y0 + y1) / 2
        ridge = [(x0, ym, z0 + rise), (x1, ym, z0 + rise)]
    else:
        raise ValueError(taper_axis)
    return ConvexPolyhedron(base + ridge)


def _dip_slot_roof() -> Part:
    """dip_slot() cuts a plain box into the west wall; its own top face is a
    ~19mm-wide, dead flat ceiling sitting mid-wall (see test_printable.py) --
    a real unsupported bridge, not a curved-overhang false alarm. Roofed the
    same way the port openings are below: taper across the wall's own
    thickness (WALL to POCKET_CLEAR, the short dimension), ridge running the
    length of the key band."""
    bb = dip_slot().bounding_box()
    x0, x1 = -p.WALL, -p.POCKET_CLEAR
    rise = (x1 - x0) / 2
    return _ridge_roof(x0, x1, bb.min.Y, bb.max.Y, bb.max.Z, rise, "x")


def _port_roof(ref: str) -> Part:
    """_grown(ref, PORT_CLEAR)'s own top is frequently flat -- a connector's
    mating flange or shield top, grown outward, stays flat -- leaving an
    unsupported ceiling inside the front wall once it's subtracted (see
    test_printable.py). Roofed the same way _dip_slot_roof() is, just
    rotated 90 degrees: this wall's short (depth) dimension is Y, so the
    ridge runs along X instead. Clipped to the wall's own real Y bounds
    (WALL..POCKET_CLEAR) rather than the connector's full grown bounding
    box, which reaches well past the wall into the open cavity."""
    bb = _grown(ref, p.PORT_CLEAR).bounding_box()
    y0, y1 = -p.WALL, -p.POCKET_CLEAR
    rise = (y1 - y0) / 2
    return _ridge_roof(bb.min.X, bb.max.X, y0, y1, bb.max.Z, rise, "y")


def _print_reliefs() -> Part:
    """Extra material removed above dip_slot() and each port opening so
    tray() has no unsupported downward-facing ceiling once printed
    floor-down -- see test_printable.py. Not part of dip_slot()/
    port_cutouts() themselves: those two describe the FUNCTIONAL opening
    (what the switch/connector needs); this is a printability-only add-on,
    invisible to anything that fits through the real opening."""
    out = _dip_slot_roof()
    for ref in ("J2", "J4", "J1"):
        out = out + _port_roof(ref)
    return out


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


def dip_slot() -> Part:
    """S1's eight piano keys, out through the west wall.

    S1's footprint origin is the centre of its pin field; the pad columns run
    footprint-local x = -8.89 .. +8.89 and the keys align with them. The tips
    reach local y = -7.205 (body 9.61 deep centred on the 7.62 pin rows, keys
    2.40 proud), which lands at KiCad x = 97.80 -- 2.20mm past the board edge,
    so behind a 3.00 wall the keys sit 0.80mm recessed by design.

    The slot's Y span is built from the KEY BAND'S OWN EDGES, not the pad
    column centres: each outer key extends DIP_KEY_HALF_WIDTH past its pad
    column's centre (vendor VRML, see params.DIP_KEY_HALF_WIDTH), and only
    then does DIP_SLOT_MARGIN get added, past that real edge. S1's rotation
    is a pure 90-degree turn, so a local-x distance carries over to case-Y
    unscaled -- DIP_KEY_HALF_WIDTH can be added directly in case-frame mm.
    (The old code added the margin straight onto the pad centres, with no
    allowance for the key's own width, and the two outer keys ended up
    0.05mm inside the wall material -- see params.DIP_SLOT_MARGIN.)
    """
    fp = bg.footprint("S1")
    pad_xs = [lx for lx, _ in bg.pad_positions("S1")]
    ys = [bg.to_case(*bg.local_to_board(fp, lx, 0.0))[1]
          for lx in (min(pad_xs), max(pad_xs))]
    edge = p.DIP_KEY_HALF_WIDTH + p.DIP_SLOT_MARGIN
    y0, y1 = min(ys) - edge, max(ys) + edge
    depth = p.WALL + 1.0  # overshoot inward so the cut is clean
    return Pos(
        -p.WALL / 2 + 0.5,  # spans x = -WALL .. +1.0
        (y0 + y1) / 2,
        (p.DIP_SLOT_Z0 + p.DIP_SLOT_Z1) / 2,
    ) * Box(depth, y1 - y0, p.DIP_SLOT_Z1 - p.DIP_SLOT_Z0)


def reset_pinhole() -> Part:
    """A pinhole through the EAST wall on SW1's actuator axis.

    HANDOFF has a superseded line putting this on the west wall at x=100; the
    live ruling is the east edge, with SW1's plate face at KiCad x ~ 159.4.

    SW1 has no 3D model placed on the board, so its Z can't be booleaned from
    a STEP -- p.RESET_ACTUATOR_Z is derived from its footprint's own descr
    (body 6.2 x 2.55 x 3.5mm) instead; see params.py and task-6-report.md.
    """
    fp = bg.footprint("SW1")
    _, y = bg.to_case(fp.x, fp.y)
    z = p.RESET_ACTUATOR_Z
    length = p.WALL + 2.0  # overshoot both ends so the cut is clean
    return Pos(p.BOARD_W + p.WALL - length / 2 + 1.0, y, z) * Cylinder(
        p.RESET_PINHOLE_D / 2, length, rotation=(0, 90, 0))


def _sw3_plunger_centre() -> tuple[float, float]:
    """SW3's body centre, in KiCad mm -- where the Lite lid's test-plunger
    bore lands (HANDOFF: under the art panel's test dot).

    SW3's four through-hole pads sit at footprint-local (0,0), (6.5,0),
    (0,4.5), (6.5,4.5): a rectangle centred on the body's own middle at
    local (3.25, 2.25). Deriving the centre from those pads (rather than
    writing KiCad (131.0, 91.0) directly, which is what this used to do)
    means a future SW3 move or rotation moves the bore with it instead of
    leaving it silently behind."""
    fp = bg.footprint("SW3")
    xs = [x for x, _ in bg.pad_positions("SW3")]
    ys = [y for _, y in bg.pad_positions("SW3")]
    lx, ly = (min(xs) + max(xs)) / 2, (min(ys) + max(ys)) / 2
    return bg.local_to_board(fp, lx, ly)


def lite_lid() -> Part:
    """The panel stand-in for boards built without the dot-grid art panel.

    A plate at the panel's OWN size (BOARD_W x BOARD_L, the board's outline,
    which the art panel shares -- see params.CORNER_R) and corner radius
    (CORNER_R), seating in the pocket that was cut POCKET_CLEAR oversize for
    it. Sized at the panel's own outline rather than at the pocket's, this
    lands the same 0.20mm/side the pocket gives every other seated part --
    at the pocket's own size, the plate would need zero clearance to drop
    in, which does not print (see test_it_fits_the_tray_with_no_interference).
    Thickness on four legs that reproduce in plastic the CAVITY (12.74mm) the
    Pro's mini-DIN lattice spans, so the plate's own top lands flush with
    Z_WALL_TOP -- the same face the Pro's panel top registers against (see
    params.CAVITY for the nut-stack correction and its open flag). Each leg
    takes an M2.5 heat-set insert at its BOTTOM, level with the board's top
    face, so the screw from below engages brass after a few millimetres
    instead of threading through the full leg.

    Prints plate-down (the plate's own Z_WALL_TOP face on the bed, legs
    pointing up into the air), so any OTHER model surface facing +Z becomes,
    once flipped for the print, an unsupported overhang -- a downward flat
    that isn't the bed-contact plate face. There isn't one here: each leg is
    a constant-diameter cylinder from the board face (Z=0) up to exactly the
    plate's underside (Z=CAVITY), so the leg-to-plate junction is a clean
    coincident union with no exposed shoulder, and the insert bore is cut
    from the leg's free (Z=0) end, opening the same direction the leg does.
    """
    plate = _rounded_box(
        p.BOARD_W, p.BOARD_L, p.PANEL,
        p.BOARD_W / 2, p.BOARD_L / 2, p.Z_WALL_TOP - p.PANEL / 2,
        p.CORNER_R)

    body = plate
    for x, y in bg.mount_holes().values():
        leg = Pos(x, y, p.CAVITY / 2) * Cylinder(p.LEG_D / 2, p.CAVITY)
        body = body + leg

    for x, y in bg.mount_holes().values():
        # Overshoots 0.02 below Z=0 so the bore opens all the way through the
        # leg's free end (the board-face-level face) rather than leaving a
        # sliver of material sealing it.
        bore = Pos(x, y, p.INSERT_DEPTH / 2 - 0.01) * Cylinder(
            p.INSERT_D / 2, p.INSERT_DEPTH + 0.02)
        # The insert doesn't reach past INSERT_DEPTH, so a flat-bottomed
        # blind bore leaves a flat cap INSIDE the leg at that depth, model
        # normal -Z (solid above, toward Z=CAVITY; void below). That is NOT
        # a bridge: the lid prints plate-down, which makes print-down model
        # +Z (see this function's own docstring and test_printable.py), so
        # a model -Z face is print-UP -- this cap sits on solid material in
        # the print's own down direction and comes out as an ordinary
        # supported floor. Coned to a point anyway, the same idea as
        # screw_holes()'s counterbore cone (radius and height both
        # INSERT_D/2, so it closes over exactly as fast as it rises), so
        # the insert has a lead-in and nothing to hang up on going in --
        # not required by the printability gate.
        tip_r = p.INSERT_D / 2
        tip = Pos(x, y, p.INSERT_DEPTH + tip_r / 2) * Cone(tip_r, 0, tip_r)
        body = body - bore - tip

    px, py = bg.to_case(*_sw3_plunger_centre())
    plunger = Pos(px, py, p.Z_WALL_TOP - p.PANEL / 2) * Cylinder(
        p.PLUNGER_BORE_D / 2, p.PANEL + 0.02)
    return body - plunger


def tray() -> Part:
    """The finished tray, shared by Pro and Lite.

    No carveouts() term: RULING 1 (task-6-report.md) found the brief's
    version vacuous -- the cavity is one open prism and U4/R3/U9 all sit well
    inboard of it, so subtracting cylinders there would subtract air from
    air. test_case.TestSideFeaturesAndTray checks the real requirement (each
    part keeps >=1mm of air to the tray) without a no-op boolean term.

    _print_reliefs() is last: it only widens holes screw_holes()/
    port_cutouts()/dip_slot() already opened, so order doesn't matter for
    the result, just for reading this as "the real openings, then the
    print-only touch-ups" (task-8-report.md; test_printable.py is the gate
    this earns).
    """
    body = shell() + standoffs()
    return (body - screw_holes() - port_cutouts() - dip_slot()
            - reset_pinhole() - _print_reliefs())
