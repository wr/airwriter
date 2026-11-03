# SPDX-License-Identifier: CERN-OHL-S-2.0
"""The LC frame (lc_frame.py): what the LC's acrylic top is glued to."""
import unittest

from build123d import Box, Cylinder, Pos

import board_geom as bg
import case
import components
import lc_frame
import params as p

TOP = p.ACRYLIC_UNDERSIDE - p.GLUE_LINE


def _box(x0, x1, y0, y1, z0, z1):
    return Pos((x0 + x1) / 2, (y0 + y1) / 2, (z0 + z1) / 2) * Box(x1 - x0, y1 - y0, z1 - z0)


def _vol(part):
    return part.volume if part else 0.0


class TestLCFrame(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.frame = lc_frame.lc_frame()

    def test_is_one_valid_solid(self):
        self.assertTrue(self.frame.is_valid)
        self.assertEqual(len(self.frame.solids()), 1)

    def test_glued_acrylic_sits_just_under_the_rim_clear_of_the_shells(self):
        # The frame stops GLUE_LINE short of the acrylic's underside. Its face
        # must never stand proud of the tray's rim, and should sit close enough
        # under it to read flush; its underside never below J2/J4's shells.
        bb = self.frame.bounding_box()
        self.assertAlmostEqual(bb.min.Z, 0.0, places=3)
        self.assertAlmostEqual(bb.max.Z + p.GLUE_LINE, p.ACRYLIC_UNDERSIDE, places=3)
        self.assertGreaterEqual(p.ACRYLIC_UNDERSIDE, p.MINIDIN_TOP - 1e-6)
        face = p.ACRYLIC_UNDERSIDE + p.ACRYLIC
        rim = case.shell().bounding_box().max.Z
        self.assertLessEqual(face, rim)
        self.assertGreaterEqual(face, rim - 0.20)

    def test_every_leg_top_is_a_glue_pad(self):
        # H3's leg loses a sliver to J2's grown hull, its whole height (93%
        # of the disc is left); the others are whole.
        for h, (x, y) in bg.mount_holes().items():
            pad = Pos(x, y, TOP - 0.05) * Cylinder(p.LEG_D / 2 - 0.05, 0.1)
            self.assertGreaterEqual(_vol(self.frame & pad), 0.9 * pad.volume, msg=h)

    def test_legs_are_the_lc_lids(self):
        # Same legs, inserts, screws, connector trims and foot relief as the
        # printed LC lid: below the rim the two parts are the same solid.
        # Away from D3's light tube, which stands on the board too.
        _, (ox0, ox1, oy0, oy1) = lc_frame.tube_rects()
        below = (_box(-5, p.BOARD_W + 5, -5, p.BOARD_L + 5, -1, TOP - p.FRAME_RIM_H - 0.1)
                 - _box(ox0 - 0.01, ox1 + 0.01, oy0 - 0.01, oy1 + 0.01, -2, TOP + 1))
        mine, lids = self.frame & below, case.lc_lid() & below
        self.assertAlmostEqual(_vol(mine), _vol(lids), places=2)
        self.assertAlmostEqual(_vol(mine - lids), 0.0, places=2)

    def test_it_fits_the_tray(self):
        margin = case.tray().distance_to(self.frame)
        self.assertGreaterEqual(margin, 0.05, f"frame clears the tray by only {margin:.4f}mm")

    def test_rim_and_arms_clear_every_part(self):
        z0 = TOP - p.FRAME_RIM_H - 0.01
        band = self.frame & _box(-5, p.BOARD_W + 5, -5, p.BOARD_L + 5, z0, TOP + 1)
        for ref, bands in components.envelopes().items():
            if ref in ("J1", "J2", "J4"):
                continue
            for x0, x1, y0, y1, top in bands:
                if top <= z0:
                    continue
                hit = _vol(band & _box(x0, x1, y0, y1, -1, top))
                self.assertAlmostEqual(hit, 0.0, places=3, msg=f"rim runs into {ref}")
        for ref in ("J1", "J2", "J4"):
            hit = _vol(band & case.connector_solid(ref))
            self.assertAlmostEqual(hit, 0.0, places=3, msg=f"rim runs into {ref}")

    def test_power_light_leaves_only_through_the_slash(self):
        # D3's tube: a wall at least 0.40 thick all round from the board to
        # the cap, nothing inside, the cap solid but for the slit, and the
        # slit open the slash's own shape.
        from build123d import Face, Wire, extrude
        z1 = TOP - p.FRAME_CAP
        board_up = _box(-5, p.BOARD_W + 5, -5, p.BOARD_L + 5, 0, z1)
        shell = (lc_frame.tube_hollow(0.40) - lc_frame.tube_hollow()) & board_up
        self.assertAlmostEqual(_vol(self.frame & shell), shell.volume, places=2,
                               msg="a wall of D3's tube is under 0.40 somewhere")
        inside = lc_frame.tube_hollow() & _box(-5, p.BOARD_W + 5, -5, p.BOARD_L + 5, 0.01, z1 - 0.01)
        self.assertAlmostEqual(_vol(self.frame & inside), 0.0, places=3,
                               msg="something inside D3's tube")
        ix0, ix1, iy0, iy1 = lc_frame.tube_rects()[0]
        lid = _box(ix0, ix1, iy0, iy1, z1, TOP) & lc_frame._rim_outline(
            p.FRAME_TUBE_MIN_WALL, z1, TOP)
        face = Face(Wire.make_polygon([(x, y, 0) for x, y in bg.lc_slash()], close=True))
        slit = Pos(0, 0, z1) * extrude(face, p.FRAME_CAP, dir=(0, 0, 1))
        self.assertAlmostEqual(_vol(self.frame & lid), lid.volume - slit.volume, places=2,
                               msg="the cap is open somewhere but the slit")
        self.assertAlmostEqual(_vol(self.frame & slit), 0.0, places=3, msg="the slit is closed")

    def test_rim_runs_round_the_back_and_both_sides(self):
        # Probes mid-way along the three edges the ports don't cut.
        o = p.POCKET_CLEAR - p.RIDGE_CLEAR
        w = o + p.FRAME_RIM_IN
        z = TOP - p.FRAME_RIM_H / 2
        probes = {
            "west": _box(-o + 0.05, -o + w - 0.05, 50, 51, z - 0.1, z + 0.1),
            "east": _box(p.BOARD_W + o - w + 0.05, p.BOARD_W + o - 0.05, 50, 51, z - 0.1, z + 0.1),
            "back": _box(30, 31, p.BOARD_L + o - w + 0.05, p.BOARD_L + o - 0.05, z - 0.1, z + 0.1),
        }
        for name, probe in probes.items():
            self.assertAlmostEqual(_vol(self.frame & probe), probe.volume, places=4, msg=name)


if __name__ == "__main__":
    unittest.main()
