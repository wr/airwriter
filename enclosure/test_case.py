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
        # STANDOFF (params.py) now leaves >=0.85mm between every connector's
        # real STEP geometry and the floor (see
        # test_connectors_clear_the_floor_by_at_least_0_50 below), so this no
        # longer needs the above-the-floor clip a previous round used to
        # dodge a genuine 0.15mm floor interference -- that interference is
        # fixed at the source now, not worked around here.
        for ref in ("J2", "J4", "J1"):
            hit = self.tray_shell & case.connector_solid(ref)
            self.assertAlmostEqual(hit.volume if hit else 0.0, 0.0, places=3, msg=ref)

    def test_openings_clear_each_connector_by_at_least_0_30(self):
        # Finding 2, task-5-report.md: the previous version intersected the
        # tray with case._grown(ref, 0.30), but the tray was built by
        # SUBTRACTING case._grown(ref, 0.40) -- the 0.30 envelope is a
        # geometric subset of the 0.40 one that was already cut away, so the
        # intersection was empty by construction for any geometry, including
        # a connector mis-placed by 6mm. Assert on real distance instead: the
        # nearest point of whatever wall survives around this connector to
        # the connector's own (real, not grown) solid.
        from build123d import Box, Pos
        for ref in ("J2", "J4", "J1"):
            solid = case.connector_solid(ref)
            bb = solid.bounding_box()
            xc = (bb.min.X + bb.max.X) / 2
            local = self.tray_shell & (
                Pos(xc, -p.WALL / 2, 4.5) * Box(20, p.WALL, 26))
            self.assertGreaterEqual(local.distance_to(solid), 0.30, ref)

    def test_ports_are_actually_open(self):
        # Finding 4, task-5-report.md: nothing previously checked that a
        # connector's opening actually reaches the wall's outer face.
        # _grown(ref, PORT_CLEAR) alone spans Y -1.90..6.80 for J1 and never
        # reaches Y=-3.00 (the wall's outer face) -- only the USB-C flare
        # punches the rest of the way through. A bar spanning the full wall
        # thickness at each connector's centre must find (almost) no
        # material: a "port" that is still plugged with plastic is not open.
        from build123d import Box, Pos
        for ref in ("J2", "J4", "J1"):
            solid = case.connector_solid(ref)
            bb = solid.bounding_box()
            xc = (bb.min.X + bb.max.X) / 2
            zc = (bb.min.Z + bb.max.Z) / 2
            bar = Pos(xc, -p.WALL / 2, zc) * Box(2.0, p.WALL, 2.0)
            hit = self.tray_shell & bar
            self.assertAlmostEqual(
                hit.volume if hit else 0.0, 0.0, places=3,
                msg=f"{ref}: port is blind, {(hit.volume if hit else 0.0):.3f}"
                    f"mm^3 of plastic blocks it")

    def test_j2_placement_registers_against_the_boards_own_pads(self):
        # Finding 6, task-5-report.md: a deliberate +6mm X mis-placement of
        # all three connectors was previously caught by exactly one test.
        # This check is independent of connector_solid()'s own placement
        # transform -- it parses J2's 11 through-hole pads (8 signal pins +
        # 3 shield/mounting posts) straight out of the board file and maps
        # each footprint-local position to the case frame via
        # board_geom.local_to_board() + to_case(), the same primitives
        # mount_holes() uses, not the STEP-placement code under test. A
        # small probe just below the board's underside at each pad's real
        # (x, y) must find real material: if connector_solid("J2") ever
        # drifts off the board's actual copper, some pins go dark.
        from build123d import Box, Pos
        import board_geom as bg

        fp = bg.footprint("J2")
        pads = bg.pad_positions("J2")
        self.assertEqual(len(pads), 11)

        solid = case.connector_solid("J2")
        for i, (lx, ly) in enumerate(pads):
            bx, by = bg.local_to_board(fp, lx, ly)
            cx, cy = bg.to_case(bx, by)
            probe = Pos(cx, cy, p.Z_BOARD_BOTTOM - 0.5) * Box(0.6, 0.6, 1.0)
            hit = solid & probe
            vol = hit.volume if hit else 0.0
            # Physics, not the measured value: a pin's tail either passes
            # through this probe or it doesn't. 0.005 is far below every
            # real hit measured here (0.025 - 0.18mm^3) and far above the
            # exact 0.0 a miss produces.
            self.assertGreater(vol, 0.005, f"J2 pad {i} at case ({cx:.2f}, "
                                f"{cy:.2f}): only {vol:.4f}mm^3, not really there")

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
        # Finding 5, task-5-report.md: the previous version anchored both
        # probes to case._grown(ref, PORT_CLEAR)'s own live bounding box, so
        # they always sat just outside whatever the opening happened to be
        # -- at PORT_CLEAR=3.0 the opening ate the entire front wall from
        # floor top to wall top and this test still passed; it only failed
        # once the opening reached 6.0. Anchoring to the live envelope can
        # never catch "the envelope grew too tall", because the probe grows
        # right along with it.
        #
        # Anchor to the wall's own FIXED structural limits instead --
        # Z_WALL_TOP and Z_FLOOR_TOP don't move when PORT_CLEAR or a
        # connector's STEP does -- and require a minimum absolute surviving
        # thickness, not a fraction of an arbitrarily-sized probe. Measured
        # today, real geometry leaves the full 0.8mm probe intact at every
        # band for all three connectors (thickness_mm ~= 0.8); the 0.5mm
        # floor is comfortably below that and comfortably above what a
        # genuinely eaten wall leaves (~0.28-0.31mm at PORT_CLEAR=3.0 in
        # this same geometry, 0.0mm at PORT_CLEAR=6.0 -- see
        # task-5-report.md for the deliberate-break measurements).
        from build123d import Box, Pos

        wall_y0, wall_y1 = -p.WALL, -p.POCKET_CLEAR
        wall_yc = (wall_y0 + wall_y1) / 2
        wall_yh = wall_y1 - wall_y0
        h = 0.8
        min_thickness = 0.5

        for ref in ("J2", "J4", "J1"):
            bb = case.connector_solid(ref).bounding_box()
            xc = (bb.min.X + bb.max.X) / 2
            xw = bb.size.X + 4.0  # margin around the connector's own width
            top = Pos(xc, wall_yc, p.Z_WALL_TOP - h / 2) * Box(xw, wall_yh, h)
            bottom = Pos(xc, wall_yc, p.Z_FLOOR_TOP + h / 2) * Box(xw, wall_yh, h)
            for label, probe in (("top", top), ("bottom", bottom)):
                hit = self.tray_shell & probe
                vol = hit.volume if hit else 0.0
                thickness = (vol / probe.volume) * h
                self.assertGreaterEqual(
                    thickness, min_thickness,
                    f"{ref} {label}: only {thickness:.3f}mm of wall survives "
                    f"there, need >= {min_thickness}mm")


if __name__ == "__main__":
    unittest.main()
