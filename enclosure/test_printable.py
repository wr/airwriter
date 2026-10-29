"""The gate between "the model closes" and "the print will not fail".

Both parts print flat-side-down with no supports. The check that matters is
unsupported flats steeper than the real FDM 45-degree overhang limit: a
curved overhang (a port hole's top arc) prints fine at these diameters, a
horizontal or near-horizontal ledge in mid-air does not. The one flat that IS
allowed is a lintel: the flat top of an opening cut clean through a wall, a
bridge no deeper than the wall and anchored at both ends (`wall_lintels`).

The two parts print in OPPOSITE orientations relative to their own model:

  - The tray prints floor-down, in the SAME orientation it's modelled --
    model -Z is down on the bed. `downward_flats()` is its check.
  - The Lite lid prints plate-down: its own Z_WALL_TOP face sits on the
    bed, legs pointing up into the air (see case.lite_lid()'s docstring),
    which INVERTS the model -- print-down is model +Z. `upward_flats()` is
    its check. Calling `downward_flats()` on the lid would look at model -Z
    faces, which print-side-up are always supported -- it can never see a
    real overhang; see task-8-report.md / the review that caught this.
"""
import math
import unittest

from build123d import GeomType

import case
import lite_frame
import params as p

MIN_WALL = p.WALL - p.POCKET_CLEAR
"""3.00 nominal minus the 0.30 pocket clearance. Every wall is held to this
only at the panel seat; below it, all four are THIN_WALL on purpose."""

FDM_OVERHANG_LIMIT = math.cos(math.radians(45))
"""cos(45 deg) ~= 0.70711: the real FDM unsupported-overhang limit, expressed
as a face normal's Z component. A face tilted exactly 45 degrees off
horizontal has |n.Z| == this value; flatter (|n.Z| bigger, closer to a pure
ceiling) can't bridge without support, steeper (|n.Z| smaller, closer to a
vertical wall) prints fine. This project's own counterbore cones sit
exactly at this limit by construction, so the comparison in `_overhangs` below is a strict '>':
exactly 45 degrees passes, anything flatter fails."""

MIN_BAND_AREA = 5.0
"""Per Z-band (see `_overhangs`), an unsupported area under this genuinely
bridges at this project's 0.4mm nozzle / 0.2mm layers. It clears the slivers
a hull-derived opening leaves (each well under 1mm^2) while staying far below
every genuine offender this gate has caught."""

MAX_BRIDGE_SPAN = 25.0
"""The longest lintel the tray may ask a printer to bridge, in mm. The DIP
slot is the longest at 20.7; J2/J4 are 14.0 and J1 6.5. A bridge this short
and only a wall deep (2.8mm) prints clean with part cooling on; the README's
first-print checklist is the backstop."""


def wall_lintels():
    """(name, bounding box, span) for each opening cut clean through a tray
    wall, clipped to that wall's own thickness. A downward flat inside one of
    these is the opening's lintel -- a bridge across `span`, anchored in the
    wall at both ends -- not a ledge in mid-air. These used to be capped
    with 45-degree ridge roofs instead; the J2/J4 ridges came out above
    Z_WALL_TOP and notched the rim, and each roof layer bridged the same
    span anyway."""
    from build123d import Box, Pos
    depth = p.WALL - p.POCKET_CLEAR
    mid = -(p.WALL + p.POCKET_CLEAR) / 2
    out = []
    bb = case.dip_slot().bounding_box()
    wall = Pos(mid, (bb.min.Y + bb.max.Y) / 2, (bb.min.Z + bb.max.Z) / 2) * Box(
        depth, bb.size.Y, bb.size.Z)
    out.append(("S1", wall.bounding_box(), bb.size.Y))
    for ref in ("J2", "J4", "J1"):
        bb = case._grown(ref, p.PORT_CLEAR).bounding_box()
        wall = Pos((bb.min.X + bb.max.X) / 2, mid, (bb.min.Z + bb.max.Z) / 2) * Box(
            bb.size.X, depth, bb.size.Z)
        out.append((ref, wall.bounding_box(), bb.size.X))
    return out


def foot_bridges():
    """(name, bounding box, span) for each rubber-foot pocket in the floor's
    underside. Its ceiling is a bridge across the pocket, anchored all the way
    round, with the screw counterbore opening up through it."""
    from build123d import Box, Pos
    d = p.FOOT_D + 2 * p.FOOT_POCKET_CLEAR
    out = []
    for i, (x, y) in enumerate(case.foot_positions()):
        box = Pos(x, y, p.Z_FLOOR_BOTTOM + p.FOOT_POCKET_DEPTH / 2) * Box(d, d, p.FOOT_POCKET_DEPTH)
        out.append((f"foot {i}", box.bounding_box(), d))
    return out


def _inside(face, box, tol=0.02):
    fb = face.bounding_box()
    return all(
        getattr(fb.min, a) >= getattr(box.min, a) - tol
        and getattr(fb.max, a) <= getattr(box.max, a) + tol
        for a in "XYZ")


def _overhangs(part, sign, bed_z, min_area=MIN_BAND_AREA):
    """The shared engine behind `downward_flats`/`upward_flats`.

    `sign` is -1.0 to look at faces whose normal points toward model -Z, or
    +1.0 for model +Z -- whichever direction is "print-down" for this part
    (see the module docstring). A face qualifies as a candidate overhang
    when it's PLANAR (a curved face like a round hole's barrel is never an
    unsupported bridge at these diameters -- see the geom_type guard below,
    added after the first version of this gate flagged the east wall's
    round reset pinhole) and tilted flatter than the 45-degree FDM limit.

    Candidates are then grouped into bands by rounded model Z, and the
    threshold is applied to each band's SUMMED area, not to any one face:
    a hull-derived opening (see `MIN_BAND_AREA`) or a tiled convex-hull
    ceiling can spread real area across many small facets, each easily
    under `min_area` on its own while the ceiling they form together is not
    (task-8-report.md Finding 3).

    `bed_z` is the model Z of the face that is actually this part's
    build-plate contact -- the tray's floor underside, or the lid's plate
    top -- excluded outright rather than flagged, exactly like every other
    real overhang.
    """
    candidates = []
    for f in part.faces():
        if f.geom_type != GeomType.PLANE:
            continue
        try:
            n = f.normal_at(f.center())
        except Exception:
            continue
        if sign * n.Z > FDM_OVERHANG_LIMIT + 1e-6:
            candidates.append(f)

    if sign < 0:
        candidates = [f for f in candidates if f.center().Z > bed_z + 1e-6]
    else:
        candidates = [f for f in candidates if f.center().Z < bed_z - 1e-6]

    bands: dict[float, list] = {}
    for f in candidates:
        bands.setdefault(round(f.center().Z, 2), []).append(f)

    out = []
    for faces in bands.values():
        if sum(f.area for f in faces) > min_area:
            out.extend(faces)
    return out


def downward_flats(part, floor_z, min_area=MIN_BAND_AREA):
    """Overhangs for a part printed in the SAME orientation it's modelled in
    -- model -Z is print-down. Use for the tray. `floor_z` is the model Z of
    the floor's own underside (the real build-plate contact)."""
    return _overhangs(part, -1.0, floor_z, min_area)


def upward_flats(part, ceiling_z, min_area=MIN_BAND_AREA):
    """Overhangs for a part printed INVERTED relative to its own model --
    model +Z is print-down. Use for the Lite lid, which prints plate-down
    (case.lite_lid()'s docstring). `ceiling_z` is the model Z of the plate's
    own top face (the real build-plate contact)."""
    return _overhangs(part, 1.0, ceiling_z, min_area)


class TestTray(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.t = case.tray()

    def test_single_watertight_solid(self):
        self.assertTrue(self.t.is_valid)
        self.assertEqual(len(self.t.solids()), 1)

    def test_no_unsupported_downward_flats(self):
        lintels = [box for _, box, _ in wall_lintels() + foot_bridges()]
        flats = [f for f in downward_flats(self.t, p.Z_FLOOR_BOTTOM)
                 if not any(_inside(f, box) for box in lintels)]
        self.assertEqual(
            [(round(f.center().X, 1), round(f.center().Y, 1),
              round(f.center().Z, 1), round(f.area, 1)) for f in flats], [])

    def test_every_lintel_is_a_short_bridge(self):
        for name, _, span in wall_lintels() + foot_bridges():
            self.assertLessEqual(span, MAX_BRIDGE_SPAN, name)

    def test_walls_keep_their_thickness(self):
        # Probe each wall mid-height, and the floor mid-span, away from any
        # opening. `want` is MIN_WALL (or FLOOR, for the floor -- nothing
        # thins it away from a screw hole) times the probe's own LATERAL
        # (non-thickness) area, spelled out per probe rather than divided
        # back out of `size` -- size[axis] == the nominal thickness for
        # every probe below, so a generic "* volume / p.WALL" cancels to
        # the same constant for all of them and checks nothing extra.
        from build123d import Box, Pos
        # The seat band runs Z_LID_UNDER..Z_WALL_TOP, under the 0.40 top
        # chamfer, so its probes sit 0.1 inside both. Below the seat every
        # wall is THIN_WALL.
        seat_z = (p.Z_LID_UNDER + p.Z_WALL_TOP - p.TOP_CHAMFER) / 2
        seat_h = p.Z_WALL_TOP - p.TOP_CHAMFER - p.Z_LID_UNDER - 0.2
        probes = [
            ("west seat", Pos(-p.WALL / 2, 90.0, seat_z), (p.WALL, 4.0, seat_h),
             MIN_WALL * 4.0 * seat_h),
            ("east seat", Pos(p.BOARD_W + p.WALL / 2, 90.0, seat_z), (p.WALL, 4.0, seat_h),
             MIN_WALL * 4.0 * seat_h),
            ("west", Pos(-p.WALL / 2, 90.0, 4.0), (p.WALL, 4.0, 2.0),
             p.THIN_WALL * 4.0 * 2.0),
            ("east", Pos(p.BOARD_W + p.WALL / 2, 90.0, 4.0), (p.WALL, 4.0, 2.0),
             p.THIN_WALL * 4.0 * 2.0),
            ("back", Pos(31.0, p.BOARD_L + p.WALL / 2, 4.0), (4.0, p.WALL, 2.0),
             p.THIN_WALL * 4.0 * 2.0),
            ("floor", Pos(31.0, 52.0, (p.Z_FLOOR_TOP + p.Z_FLOOR_BOTTOM) / 2),
             (4.0, 4.0, p.FLOOR), p.FLOOR * 4.0 * 4.0),
        ]
        for name, pos, size, want in probes:
            probe = pos * Box(*size)
            got = (self.t & probe).volume
            self.assertGreaterEqual(got, want - 0.05, name)


class TestLid(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.lid = case.lite_lid()

    def test_single_watertight_solid(self):
        self.assertTrue(self.lid.is_valid)
        self.assertEqual(len(self.lid.solids()), 1)

    def test_no_unsupported_downward_flats_printed_plate_down(self):
        # Printed plate-down, print-down is model +Z (module docstring), so
        # the check is for UPWARD model flats, excluding the plate's own top
        # face at Z_WALL_TOP -- that's the bed, not an overhang.
        flats = upward_flats(self.lid, ceiling_z=p.Z_WALL_TOP)
        self.assertEqual(
            [(round(f.center().X, 1), round(f.center().Y, 1),
              round(f.center().Z, 1), round(f.area, 1)) for f in flats], [])


class TestLiteFrame(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.frame = lite_frame.lite_frame()

    def test_no_unsupported_downward_flats_printed_rim_down(self):
        # Printed rim-down like the lid, so print-down is model +Z; the rim's
        # and leg tops' own face at lite_frame.TOP is the bed.
        flats = upward_flats(self.frame, ceiling_z=lite_frame.TOP)
        self.assertEqual(
            [(round(f.center().X, 1), round(f.center().Y, 1),
              round(f.center().Z, 1), round(f.area, 1)) for f in flats], [])


if __name__ == "__main__":
    unittest.main()
