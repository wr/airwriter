import unittest

import params as p
import case


class TestShell(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.s = case.shell()

    def test_is_a_valid_single_solid(self):
        self.assertTrue(self.s.is_valid)
        self.assertEqual(len(self.s.solids()), 1)

    def test_external_bounding_box(self):
        bb = self.s.bounding_box()
        self.assertAlmostEqual(bb.size.X, p.EXT_W, places=3)
        self.assertAlmostEqual(bb.size.Y, p.EXT_L, places=3)
        self.assertAlmostEqual(bb.size.Z, p.EXT_H, places=3)

    def test_sits_in_the_case_frame(self):
        # X=0 is the west wall's inner face line, so the solid starts at -WALL.
        bb = self.s.bounding_box()
        self.assertAlmostEqual(bb.min.X, -p.WALL, places=3)
        self.assertAlmostEqual(bb.min.Y, -p.WALL, places=3)
        self.assertAlmostEqual(bb.min.Z, p.Z_FLOOR_BOTTOM, places=3)
        self.assertAlmostEqual(bb.max.Z, p.Z_WALL_TOP, places=3)

    def test_cavity_is_hollow_from_floor_top_upward(self):
        # A probe block filling the pocket must not intersect the shell.
        from build123d import Box, Pos
        probe = Pos(p.BOARD_W / 2, p.BOARD_L / 2,
                    (p.Z_WALL_TOP + p.Z_FLOOR_TOP) / 2) * Box(
            p.POCKET_W, p.POCKET_L, p.Z_WALL_TOP - p.Z_FLOOR_TOP)
        self.assertAlmostEqual((self.s & probe).volume, 0.0, places=3)

    def test_floor_is_solid_under_the_pocket(self):
        from build123d import Box, Pos
        probe = Pos(p.BOARD_W / 2, p.BOARD_L / 2,
                    (p.Z_FLOOR_TOP + p.Z_FLOOR_BOTTOM) / 2) * Box(
            p.BOARD_W, p.BOARD_L, p.FLOOR)
        self.assertAlmostEqual((self.s & probe).volume, probe.volume, places=2)


class TestStandoffsAndScrews(unittest.TestCase):
    def test_standoffs_sit_at_the_mount_holes_and_reach_the_board(self):
        import board_geom as bg
        s = case.standoffs()
        bb = s.bounding_box()
        self.assertAlmostEqual(bb.min.Z, p.Z_FLOOR_TOP, places=3)
        self.assertAlmostEqual(bb.max.Z, p.Z_BOARD_BOTTOM, places=3)
        self.assertEqual(len(s.solids()), 4)
        holes = bg.mount_holes()
        centres = sorted((round(sl.center().X, 3), round(sl.center().Y, 3))
                         for sl in s.solids())
        want = sorted((round(x, 3), round(y, 3)) for x, y in holes.values())
        self.assertEqual(centres, want)

    def test_screw_holes_pass_right_through_the_floor(self):
        h = case.screw_holes()
        bb = h.bounding_box()
        self.assertLessEqual(bb.min.Z, p.Z_FLOOR_BOTTOM)
        self.assertGreaterEqual(bb.max.Z, p.Z_BOARD_BOTTOM)
        self.assertEqual(len(h.solids()), 4)

    def test_counterbore_transition_is_coned_not_a_flat_ledge(self):
        # Printed floor-down, a flat annulus between a 5.0 counterbore and a
        # 2.8 clearance hole is an unsupported bridge. It must be a 45 cone.
        #
        # This has to be checked on the assembled result, not on the
        # screw_holes() tool by itself: a tool solid has no downward-facing
        # flat at that shoulder -- the tool's own material sits below and
        # wider there, so the face's outward normal there is +Z. The
        # unsupported ledge only exists on the boolean result, where the
        # corresponding face has flipped to face -Z.
        result = (case.shell() + case.standoffs()) - case.screw_holes()
        flats = [f for f in result.faces()
                 if abs(f.normal_at(f.center()).Z + 1.0) < 1e-6
                 and f.area > 2.0
                 and f.center().Z > p.Z_FLOOR_BOTTOM + 1e-6]
        self.assertEqual(flats, [], "downward flat inside a screw hole")


class TestPorts(unittest.TestCase):
    """connector_solid() and port_cutouts(), checked against three ground
    truths measured independently of this code (see task-5-report.md):

      1. J2/J4's mating faces stand 2.75 mm proud of the board's front edge
         -- the mini-DIN footprint's own descr says its origin IS the mating
         face, and the footprint sits at KiCad y=167.02 against the front
         edge at y=164.27.
      2. J2/J4's shells top out ~12.74 mm above the board's top face --
         scripts/board-model/README.md (shop repo) and this project's own
         design spec (docs/superpowers/specs/2026-09-08-airwriter-enclosure-
         design.md, section 2).
      3. J1's F.Fab shell face lands near case Y = -1.5 -- body ±3.65 about
         an origin at KiCad y=162.12, against the front edge at y=164.27.
    """

    @classmethod
    def setUpClass(cls):
        cls.tray_shell = case.shell() - case.port_cutouts()

    def test_each_connector_has_real_step_geometry(self):
        # J1's own vendor STEP (USB_C_Receptacle_HRO_TYPE-C-31-M-12.step)
        # measures 98.27 mm^3 -- real geometry, just under the round 100
        # the brief guessed before any STEP was actually measured. 90 still
        # rules out a degenerate or placeholder solid.
        for ref in ("J2", "J4", "J1"):
            solid = case.connector_solid(ref)
            self.assertTrue(solid.is_valid, ref)
            self.assertGreater(solid.volume, 90.0, ref)

    def test_minidin_mating_faces_stand_2_75_proud_of_the_board_edge(self):
        # Ground truth 1. Y=0 is the board's front edge; the footprint
        # origin IS the mating face, so the connector reaches Y = -2.75.
        for ref in ("J2", "J4"):
            bb = case.connector_solid(ref).bounding_box()
            self.assertAlmostEqual(bb.min.Y, -2.75, places=2, msg=ref)

    def test_minidin_shells_top_out_around_12_74_above_the_board(self):
        # Ground truth 2. The real vendor STEP measures 12.75 mm here --
        # 0.01 mm off the documented 12.74. Every axis-order/sign variant
        # tried during development other than the one implemented lands far
        # off (3.75, -3.75, 16.5 mm...), so the 0.01 mm gap is display/
        # measurement rounding in the hand-written design note, not a wrong
        # axis -- see task-5-report.md for the alternates that were ruled
        # out and why.
        for ref in ("J2", "J4"):
            bb = case.connector_solid(ref).bounding_box()
            self.assertAlmostEqual(bb.max.Z, 12.74, delta=0.02, msg=ref)

    def test_j1_shell_face_lands_near_case_y_minus_1_5(self):
        # Ground truth 3. Approximate by design: F.Fab is a drawn body
        # outline, not a mating-face datum -- +/-0.2 passes.
        bb = case.connector_solid("J1").bounding_box()
        self.assertAlmostEqual(bb.min.Y, -1.5, delta=0.2)

    def test_no_connector_touches_the_tray(self):
        # Scoped to the cavity/wall region (Z >= Z_FLOOR_TOP): that's what
        # port_cutouts() can affect, and what Task 5 owns. Unclipped, J2/J4
        # fail this by a sliver -- their real STEP pins dip ~0.15mm into the
        # floor slab (bb.min.Z = -3.75 vs Z_FLOOR_TOP = -3.6), a pre-existing
        # shortfall in Task 2's STANDOFF clearance for through-hole tails,
        # not a port-cutout defect. Reported in task-5-report.md rather than
        # patched here.
        from build123d import Box, Pos
        above_floor = Pos(0, 0, (p.Z_WALL_TOP + p.Z_FLOOR_TOP) / 2) * Box(
            1000, 1000, p.Z_WALL_TOP - p.Z_FLOOR_TOP)
        for ref in ("J2", "J4", "J1"):
            hit = self.tray_shell & (case.connector_solid(ref) & above_floor)
            self.assertAlmostEqual(hit.volume, 0.0, places=3, msg=ref)

    def test_openings_clear_each_connector_by_at_least_0_30(self):
        # offset() on the raw vendor STEP itself fails (see case._grown's
        # docstring and task-5-report.md) -- grow the connector's convex
        # hull instead, the same robust technique port_cutouts() uses, at
        # the spec's assertion floor of 0.30 (below the production 0.40).
        for ref in ("J2", "J4", "J1"):
            grown = case._grown(ref, 0.30)
            hit = self.tray_shell & grown
            self.assertAlmostEqual(hit.volume, 0.0, places=3, msg=ref)

    def test_front_wall_still_exists_between_the_two_minidins(self):
        # J2 and J4 are centred at case X 13.0 and 32.5; there must be
        # plastic between their openings at the midpoint.
        from build123d import Box, Pos
        probe = Pos(22.75, -p.WALL / 2, 5.0) * Box(1.0, p.WALL, 2.0)
        self.assertGreater((self.tray_shell & probe).volume, 0.5)

    def test_connectors_clear_the_floor_by_at_least_0_50(self):
        # Derives the requirement instead of restating a number (Finding 1,
        # task-5-report.md): whatever STANDOFF happens to be, every
        # connector with real STEP geometry has to clear Z_FLOOR_TOP by a
        # working margin, so the pins don't touch the floor and there is
        # room left for a hand-soldered fillet. STANDOFF went 2.00 -> 3.00
        # because J2/J4's real pin tips (measured off the Kycon STEP) were
        # found punching 0.15mm into the floor at the old value -- a future
        # board revision or connector swap that eats this margin again
        # fails here instead of silently interfering with the floor.
        margin = 0.50
        for ref in ("J2", "J4", "J1"):
            bb = case.connector_solid(ref).bounding_box()
            clearance = bb.min.Z - p.Z_FLOOR_TOP
            self.assertGreaterEqual(
                clearance, margin,
                f"{ref} clears the floor (Z_FLOOR_TOP={p.Z_FLOOR_TOP}) by "
                f"only {clearance:.3f}mm, need >= {margin}mm")

    def test_front_wall_retains_material_above_and_below_each_port(self):
        # Finding 2, task-5-report.md: _grown() uses a convex hull, so an
        # opening can over-cut where the real connector is concave, and
        # nothing previously checked that the wall itself survives that.
        # Anchor each probe to the *live* grown envelope (not a hand-typed
        # Z) so the test tracks the real geometry: if a future PORT_CLEAR
        # bump or connector STEP revision pushes an opening far enough to
        # reach the wall's own floor/top boundary, the wall there is eaten
        # and a probe placed just past the envelope's edge finds nothing.
        # The probe stops at -POCKET_CLEAR, not Y=0 -- past that the shell
        # is already hollow (the board pocket), not wall, regardless of any
        # port cutout, so including it would make the probe fail for a
        # reason that has nothing to do with port_cutouts().
        from build123d import Box, Pos

        wall_y0, wall_y1 = -p.WALL, -p.POCKET_CLEAR
        wall_yc = (wall_y0 + wall_y1) / 2
        wall_yh = wall_y1 - wall_y0
        h = 0.4  # thinner than the smallest measured margin (~0.85mm)

        for ref in ("J2", "J4", "J1"):
            gbb = case._grown(ref, p.PORT_CLEAR).bounding_box()
            xc = (gbb.min.X + gbb.max.X) / 2
            xw = gbb.max.X - gbb.min.X
            above = Pos(xc, wall_yc, gbb.max.Z + h / 2) * Box(xw, wall_yh, h)
            below = Pos(xc, wall_yc, gbb.min.Z - h / 2) * Box(xw, wall_yh, h)
            for label, probe in (("above", above), ("below", below)):
                hit = self.tray_shell & probe
                self.assertGreater(
                    hit.volume, 0.1 * probe.volume,
                    f"{ref} {label}: only {hit.volume:.3f} of "
                    f"{probe.volume:.3f}mm^3 of wall remains there")


if __name__ == "__main__":
    unittest.main()
