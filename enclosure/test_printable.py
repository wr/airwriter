"""The gate between "the model closes" and "the print will not fail".

Both parts print flat-side-down with no supports. The check that matters is
unsupported flats steeper than the real FDM 45-degree overhang limit: a
curved overhang (a port hole's top arc) prints fine at these diameters, a
horizontal or near-horizontal ledge in mid-air does not.

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
import params as p

MIN_WALL = 2.80
"""3.00 nominal minus the 0.20 pocket clearance."""

FDM_OVERHANG_LIMIT = math.cos(math.radians(45))
"""cos(45 deg) ~= 0.70711: the real FDM unsupported-overhang limit, expressed
as a face normal's Z component. A face tilted exactly 45 degrees off
horizontal has |n.Z| == this value; flatter (|n.Z| bigger, closer to a pure
ceiling) can't bridge without support, steeper (|n.Z| smaller, closer to a
vertical wall) prints fine. This project's own 45-degree ridge roofs
(_ridge_roof) and insert-bore cones sit exactly at this limit by
construction, so the comparison in `_overhangs` below is a strict '>':
exactly 45 degrees passes, anything flatter fails."""

MIN_BAND_AREA = 5.0
"""Per Z-band (see `_overhangs`), an unsupported area under this genuinely
bridges at this project's 0.4mm nozzle / 0.2mm layers -- not a guess: the
shipped tray's own J2/J4 port roofs leave exactly this kind of harmless
residue. `_port_roof()` roofs a RECTANGLE sized to each grown connector
hull's full bounding box, but the hull's actual top is domed/tapering, not
flat all the way to that bbox -- so the roof's own dead-flat rectangular
base sticks out past the hull's real (smaller) apex footprint in a few
places, leaving small facets of the roof's OWN underside exposed on the
finished tray, not just leftover bits of the hull. Measured on the shipped
geometry: 52 such facets, 9.70mm^2 total, spread across 3 Z-bands
(2.16/3.37/4.17mm^2 each, mixing J2 and J4 since both land at nearly
identical heights) -- comfortably under this threshold, and comfortably
above what a single facet could hide under the OLD per-face-only check
(every one of the 52 is < 2.0mm^2 alone; see task-8-report.md Finding 3).
5.0 clears that real, accepted residue while staying far below every
genuine offender this gate has ever caught (32.9/8.4/53.1mm^2 for the
unroofed port and DIP-slot ceilings -- see the deliberately-broken-variant
numbers in the follow-up report), so reverting the roof fix still fails
loudly."""


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
        flats = downward_flats(self.t, p.Z_FLOOR_BOTTOM)
        self.assertEqual(
            [(round(f.center().X, 1), round(f.center().Y, 1),
              round(f.center().Z, 1), round(f.area, 1)) for f in flats], [])

    def test_walls_are_at_least_2_80_thick(self):
        # Probe each wall mid-height, and the floor mid-span, away from any
        # opening. `want` is MIN_WALL (or FLOOR, for the floor -- nothing
        # thins it away from a screw hole) times the probe's own LATERAL
        # (non-thickness) area, spelled out per probe rather than divided
        # back out of `size` -- size[axis] == the nominal thickness for
        # every probe below, so a generic "* volume / p.WALL" cancels to
        # the same constant for all of them and checks nothing extra.
        from build123d import Box, Pos
        probes = [
            ("west", Pos(-p.WALL / 2, 90.0, 12.0), (p.WALL, 4.0, 2.0),
             MIN_WALL * 4.0 * 2.0),
            ("east", Pos(p.BOARD_W + p.WALL / 2, 90.0, 12.0), (p.WALL, 4.0, 2.0),
             MIN_WALL * 4.0 * 2.0),
            ("back", Pos(31.0, p.BOARD_L + p.WALL / 2, 12.0), (4.0, p.WALL, 2.0),
             MIN_WALL * 4.0 * 2.0),
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


if __name__ == "__main__":
    unittest.main()
