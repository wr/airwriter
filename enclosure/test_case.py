import math
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
        # A probe filling the pocket must not intersect the shell. The
        # pocket itself is rounded at POCKET_R (the board's own corners are
        # round, see TestCornerRounding below), so the probe is too --
        # unlike a square probe, which would now find the shell's retained
        # corner material and wrongly fail.
        from build123d import Axis, Box, Pos
        probe = Pos(p.BOARD_W / 2, p.BOARD_L / 2,
                    (p.Z_WALL_TOP + p.Z_FLOOR_TOP) / 2) * Box(
            p.POCKET_W, p.POCKET_L, p.Z_WALL_TOP - p.Z_FLOOR_TOP)
        probe = probe.fillet(p.POCKET_R, probe.edges().filter_by(Axis.Z))
        hit = self.s & probe
        self.assertAlmostEqual(hit.volume if hit else 0.0, 0.0, places=3)

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
        # The boss stops BOSS_RELIEF short of the board's underside by
        # design (params.BOSS_RELIEF) -- it does not run flush to
        # Z_BOARD_BOTTOM.
        self.assertAlmostEqual(
            bb.max.Z, p.Z_BOARD_BOTTOM - p.BOSS_RELIEF, places=3)
        self.assertEqual(len(s.solids()), 4)
        holes = bg.mount_holes()
        centres = sorted((round(sl.center().X, 3), round(sl.center().Y, 3))
                         for sl in s.solids())
        want = sorted((round(x, 3), round(y, 3)) for x, y in holes.values())
        self.assertEqual(centres, want)

    def test_standoffs_clear_the_boards_underside_by_boss_relief(self):
        # Would have caught the boss penetrating the board's exported
        # bottom mask/copper sheets (measured at case Z -1.68, 0.08mm below
        # the nominal -1.60 underside): the boss's own top face must sit
        # exactly BOSS_RELIEF below Z_BOARD_BOTTOM, not flush with it.
        s = case.standoffs()
        gap = p.Z_BOARD_BOTTOM - s.bounding_box().max.Z
        self.assertAlmostEqual(gap, p.BOSS_RELIEF, places=3)
        self.assertGreater(gap, 0.0)

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
        # The real, fully-assembled tray -- shell() + standoffs() minus
        # every cut INCLUDING _print_reliefs() (the 45-degree roofs over
        # the DIP slot and each port opening, tray()-only, see case.tray()'s
        # docstring). tray_shell above never sees those roofs, so nothing
        # here previously caught a roof eating into the front wall's own
        # retained material or a connector's clearance (task-8-report.md
        # Finding 4). The probes below re-run against this too.
        cls.tray_full = case.tray()

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
        # plastic between their openings at the midpoint. The probe is
        # 1.0 * WALL(3.0) * 2.0 = 6.0mm^3; real geometry leaves 5.600mm^3
        # (93.3% full) here, so the threshold is set close to that rather
        # than at 0.5 (an 8.3% floor that would also pass a web reduced to
        # a sliver by a wider connector or a tighter PORT_CLEAR).
        from build123d import Box, Pos
        probe = Pos(22.75, -p.WALL / 2, 5.0) * Box(1.0, p.WALL, 2.0)
        self.assertGreater((self.tray_shell & probe).volume, 4.0)

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

    def test_openings_clear_each_connector_by_at_least_0_30_on_the_full_tray(self):
        # Finding 4, task-8-report.md: _print_reliefs() (the 45-degree roofs
        # over each port opening, tray()-only -- see case.tray()'s
        # docstring) is invisible to every check above, because they all run
        # against tray_shell = shell() - port_cutouts(), which never
        # subtracts it. A roof widens the opening near the wall's outer
        # face; nothing previously re-checked the 0.30mm clearance spec
        # floor against the roofed opening a real print actually has.
        # Same probe as test_openings_clear_each_connector_by_at_least_0_30,
        # against cls.tray_full instead of cls.tray_shell.
        from build123d import Box, Pos
        for ref in ("J2", "J4", "J1"):
            solid = case.connector_solid(ref)
            bb = solid.bounding_box()
            xc = (bb.min.X + bb.max.X) / 2
            local = self.tray_full & (
                Pos(xc, -p.WALL / 2, 4.5) * Box(20, p.WALL, 26))
            self.assertGreaterEqual(local.distance_to(solid), 0.30, ref)

    def test_front_wall_retains_material_above_and_below_each_port_on_the_full_tray(self):
        # Finding 4, task-8-report.md: the top/bottom band probes above run
        # against tray_shell, which never sees _print_reliefs() -- so
        # nothing guards a future roof eating into the retained material
        # this test exists to protect. Same probes as
        # test_front_wall_retains_material_above_and_below_each_port,
        # against cls.tray_full instead of cls.tray_shell.
        from build123d import Box, Pos

        wall_y0, wall_y1 = -p.WALL, -p.POCKET_CLEAR
        wall_yc = (wall_y0 + wall_y1) / 2
        wall_yh = wall_y1 - wall_y0
        h = 0.8
        min_thickness = 0.5

        for ref in ("J2", "J4", "J1"):
            bb = case.connector_solid(ref).bounding_box()
            xc = (bb.min.X + bb.max.X) / 2
            xw = bb.size.X + 4.0
            top = Pos(xc, wall_yc, p.Z_WALL_TOP - h / 2) * Box(xw, wall_yh, h)
            bottom = Pos(xc, wall_yc, p.Z_FLOOR_TOP + h / 2) * Box(xw, wall_yh, h)
            for label, probe in (("top", top), ("bottom", bottom)):
                hit = self.tray_full & probe
                vol = hit.volume if hit else 0.0
                thickness = (vol / probe.volume) * h
                self.assertGreaterEqual(
                    thickness, min_thickness,
                    f"{ref} {label}: only {thickness:.3f}mm of wall survives "
                    f"there (full tray), need >= {min_thickness}mm")

    def test_usbc_flare_widens_from_the_wall_interior_to_the_exterior(self):
        # Finding 3's flare fix (task-5-report.md) is only correct if the
        # opening is WIDER at the wall's outer face than it is just inside
        # it -- a plug's overmould enters from outside, so that's the whole
        # point of the chamfer. Nothing before this pinned the direction:
        # the old, inverted cone (Cone(rotation=(90,0,0)), radius from
        # j1.size.X) is a completely different construction and the full
        # suite stayed green with it in place. Measure the real cut opening
        # (not the flare's own construction parameters) by slicing
        # port_cutouts() with a thin Y slab at J1's Z centre and reading the
        # surviving X width, at the wall's outer face (Y=-2.99, WALL=3.00)
        # and just inside the flare-to-straight-envelope transition
        # (Y=-1.81, y_inner=-WALL+USBC_CHAMFER=-1.80).
        from build123d import Box, Pos

        j1 = case.connector_solid("J1").bounding_box()
        xc = (j1.min.X + j1.max.X) / 2
        zc = (j1.min.Z + j1.max.Z) / 2

        def opening_width(y):
            # xspan=15 stays clear of J2/J4's own openings (case X ~13 and
            # ~32.5 against J1's ~52), so this isolates J1's cut alone.
            probe = Pos(xc, y, zc) * Box(15, 0.02, 10)
            hit = case.port_cutouts() & probe
            self.assertTrue(hit and hit.volume > 1e-9, f"no opening at Y={y}")
            return hit.bounding_box().size.X

        outer = opening_width(-2.99)
        inner = opening_width(-1.81)
        self.assertGreater(
            outer - inner, 1.0,
            f"outer opening ({outer:.3f}mm) should be at least 1mm wider "
            f"than the inner one ({inner:.3f}mm) -- a narrower-outside "
            "flare is inverted")

    def test_port_cutouts_do_not_intersect_the_floor_slab(self):
        # The old inverted cone (see previous test) also over-cut
        # vertically: its Z reach was swept from J1's X width rather than
        # its own Z extent, and dipped into the floor slab (0.147mm^3 in
        # this exact geometry -- reproduced below). port_cutouts() must
        # remove nothing from the floor slab at all.
        from build123d import Box, Pos

        floor = Pos(
            p.BOARD_W / 2, p.BOARD_L / 2,
            (p.Z_FLOOR_TOP + p.Z_FLOOR_BOTTOM) / 2,
        ) * Box(p.EXT_W, p.EXT_L, p.Z_FLOOR_TOP - p.Z_FLOOR_BOTTOM)
        hit = case.port_cutouts() & floor
        vol = hit.volume if hit else 0.0
        self.assertAlmostEqual(
            vol, 0.0, places=4,
            msg=f"port_cutouts() removes {vol:.4f}mm^3 from the floor slab")


class TestSideFeaturesAndTray(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.t = case.tray()

    def test_dip_slot_is_in_the_west_wall_at_the_key_band(self):
        import board_geom as bg
        bb = case.dip_slot().bounding_box()
        self.assertLessEqual(bb.min.X, -p.WALL)
        self.assertGreaterEqual(bb.max.X, 0.0)
        self.assertAlmostEqual(bb.min.Z, p.DIP_SLOT_Z0, places=3)
        self.assertAlmostEqual(bb.max.Z, p.DIP_SLOT_Z1, places=3)

        # Key band: S1's own pad columns, read from the board rather than
        # mirroring dip_slot()'s literal -- a footprint move must be able
        # to fail this test (Finding 2). The slot's edge is the pad column
        # PLUS the key's own half-width, THEN the margin -- not the margin
        # added straight onto the pad column (that was the bug).
        fp = bg.footprint("S1")
        pad_xs = [lx for lx, _ in bg.pad_positions("S1")]
        lo = bg.to_case(*bg.local_to_board(fp, min(pad_xs), 0.0))[1]
        hi = bg.to_case(*bg.local_to_board(fp, max(pad_xs), 0.0))[1]
        lo, hi = min(lo, hi), max(lo, hi)
        edge = p.DIP_KEY_HALF_WIDTH + p.DIP_SLOT_MARGIN
        self.assertAlmostEqual(bb.min.Y, lo - edge, places=2)
        self.assertAlmostEqual(bb.max.Y, hi + edge, places=2)

    def test_dip_slot_never_opens_above_the_switch_body(self):
        # S1's moulded body tops out 9.41 above the board (dp08rp.mjs / the
        # footprint descr's 9.41mm height). A slot reaching past that opens the
        # wall onto empty cavity -- it showed up in the viewer as a black bar
        # above the keys. Stay under it.
        S1_BODY_TOP = 9.41
        bb = case.dip_slot().bounding_box()
        self.assertLess(bb.max.Z, S1_BODY_TOP,
                        f"slot top {bb.max.Z:.2f} is above S1's body top {S1_BODY_TOP}")

    def test_dip_slot_clears_the_whole_key_lever(self):
        # The levers run from z 5.8 (the lip) to 9.01 (the key top), inside the
        # switch's own 5.6-9.0 recess. A slot that starts above the lip cuts the
        # travel off mid-stroke.
        LEVER_BOTTOM, KEY_TOP = 5.8, 9.01
        bb = case.dip_slot().bounding_box()
        self.assertLessEqual(bb.min.Z, LEVER_BOTTOM, "slot starts above the lever lip")
        self.assertGreaterEqual(bb.max.Z, KEY_TOP, "slot ends below the key top")

    def test_dip_slot_fully_contains_the_key_band_with_real_margin(self):
        # Would have caught the outer keys sitting inside the wall
        # material: build the KEY BAND itself (pad column centres +-
        # DIP_KEY_HALF_WIDTH -- the keys' own physical extent, not the pad
        # columns), and require the slot to contain it with margin > 0 on
        # both sides. Proven red at the old MARGIN=0.60 below.
        import board_geom as bg
        bb = case.dip_slot().bounding_box()
        fp = bg.footprint("S1")
        pad_xs = [lx for lx, _ in bg.pad_positions("S1")]
        lo = bg.to_case(*bg.local_to_board(fp, min(pad_xs), 0.0))[1]
        hi = bg.to_case(*bg.local_to_board(fp, max(pad_xs), 0.0))[1]
        lo, hi = min(lo, hi), max(lo, hi)
        key_lo = lo - p.DIP_KEY_HALF_WIDTH
        key_hi = hi + p.DIP_KEY_HALF_WIDTH

        self.assertLess(
            bb.min.Y, key_lo,
            "slot's own edge does not clear the outer key's edge")
        self.assertGreater(
            bb.max.Y, key_hi,
            "slot's own edge does not clear the outer key's edge")

        margin_lo = key_lo - bb.min.Y
        margin_hi = bb.max.Y - key_hi
        self.assertGreater(margin_lo, 0.0)
        self.assertGreater(margin_hi, 0.0)

        # At the old MARGIN=0.60 (added onto the pad columns, no
        # DIP_KEY_HALF_WIDTH), the outer keys sat inside the wall: prove
        # that combination is red rather than trusting this test's own
        # logic to have caught it.
        old_margin = 0.60
        old_y0 = lo - old_margin
        old_y1 = hi + old_margin
        # old_margin (0.60) < DIP_KEY_HALF_WIDTH (0.65), so the old slot's
        # own edge lands INSIDE the real key edge on both ends -- it does
        # NOT reach out to key_lo/key_hi. That gap is exactly the 0.05mm of
        # key sitting inside the wall material that Wells saw.
        self.assertGreater(
            old_y0, key_lo,
            "old margin=0.60 was expected to fail to clear the key band, "
            "but didn't reproduce the bug")
        self.assertLess(
            old_y1, key_hi,
            "old margin=0.60 was expected to fail to clear the key band, "
            "but didn't reproduce the bug")

    def test_dip_slot_spans_the_wall_with_a_1mm_inward_overshoot(self):
        # RULING 2 (task-6-report.md): the brief's expression had a stray
        # "- 0.5" that cancelled the +0.5, leaving the slot centred on the
        # wall's OUTER face instead of overshooting 1mm past the inner face.
        # Pin the exact span so that typo can't silently come back: flush
        # with the outer face at -WALL, 1mm past the inner face at 0.0.
        bb = case.dip_slot().bounding_box()
        self.assertAlmostEqual(bb.min.X, -p.WALL, places=3)
        self.assertAlmostEqual(bb.max.X, 1.0, places=3)

    def test_reset_pinhole_is_in_the_EAST_wall(self):
        # HANDOFF carries a superseded note putting this on the west wall.
        import board_geom as bg
        bb = case.reset_pinhole().bounding_box()
        self.assertGreaterEqual(bb.max.X, p.BOARD_W + p.WALL)
        self.assertLessEqual(bb.min.X, p.BOARD_W)
        _, cy = bg.to_case(*[bg.footprint("SW1").x, bg.footprint("SW1").y])
        self.assertAlmostEqual((bb.min.Y + bb.max.Y) / 2, cy, places=2)
        self.assertAlmostEqual(bb.size.Z, p.RESET_PINHOLE_D, places=3)

    def test_reset_pinhole_z_matches_sw1s_derived_actuator_height(self):
        # SW1 (Panasonic EVQP44B3M side-press, see task-6-report.md) has no
        # 3D model on the board, so its actuator axis can't be booleaned
        # from a placed STEP. Its footprint's own descr records the body as
        # 6.2 x 2.55 x 3.5mm; a side-press body sits flush on the board like
        # any SMD part (Z=0 at its base), so its actuator is centred at half
        # the body height above the board's top face. The brief's guess
        # ((DIP_SLOT_Z0+DIP_SLOT_Z1)/2 - 5.0 = 3.55) is a different number
        # from that derivation and must not silently come back.
        bb = case.reset_pinhole().bounding_box()
        z_centre = (bb.min.Z + bb.max.Z) / 2
        self.assertAlmostEqual(z_centre, 3.5 / 2, places=2)

    def test_tray_is_one_valid_watertight_solid(self):
        self.assertTrue(self.t.is_valid)
        self.assertEqual(len(self.t.solids()), 1)

    def test_tray_keeps_its_external_envelope(self):
        bb = self.t.bounding_box()
        self.assertAlmostEqual(bb.size.X, p.EXT_W, places=3)
        self.assertAlmostEqual(bb.size.Y, p.EXT_L, places=3)
        self.assertAlmostEqual(bb.size.Z, p.EXT_H, places=3)

    def test_internal_parts_keep_at_least_1mm_of_air_to_the_tray(self):
        # RULING 1 (task-6-report.md): the brief's carveouts() would be
        # vacuous -- the tray's interior is one open prism and all three of
        # U4/R3/U9 sit well inboard of every wall, so subtracting cylinders
        # there subtracts air from air. This test proves the requirement's
        # actual intent instead: each part, grown by exactly the 1.0mm this
        # test claims, must still clear the tray. Sizes are each part's own
        # vendor STEP bounding box (X-extent, Z-height), measured via
        # KiCad's bundled 3D library (Package_TO_SOT_SMD.3dshapes/SOT-223.
        # step 7.0x6.5x1.7, Resistor_SMD.3dshapes/R_0603_1608Metric.step
        # 1.6x0.8x0.45, Package_TO_SOT_SMD.3dshapes/SOT-353_SC-70-5.step
        # 2.1x2.0x1.1mm) -- not committed here since board_geom only
        # resolves ${KIPRJMOD} paths, not the system ${KICAD10_3DMODEL_DIR}
        # these three use.
        import board_geom as bg
        from build123d import Cylinder, Pos

        # ref -> (largest planar (X or Y) extent, height), from the part's
        # own vendor STEP.
        sizes = {"U4": (7.0, 1.7), "R3": (1.6, 0.45), "U9": (2.1, 1.1)}
        margin = 1.0
        for ref, (xy, h) in sizes.items():
            fp = bg.footprint(ref)
            x, y = bg.to_case(fp.x, fp.y)
            radius = xy / 2 + margin
            top = h + margin
            probe = Pos(x, y, top / 2) * Cylinder(radius, top)
            hit = self.t & probe
            vol = hit.volume if hit else 0.0
            self.assertAlmostEqual(vol, 0.0, places=3,
                                    msg=f"{ref}: {vol:.4f}mm^3 of tray is "
                                        "within 1mm of this part")


class TestLiteLid(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.lid = case.lite_lid()

    def test_is_one_valid_solid(self):
        self.assertTrue(self.lid.is_valid)
        self.assertEqual(len(self.lid.solids()), 1)

    def test_plate_matches_the_art_panel_and_seats_flush(self):
        # The plate is sized to the panel's own outline (BOARD_W x
        # BOARD_L), not the pocket's -- the pocket is POCKET_CLEAR bigger
        # on every side so a printed plate actually drops in (Finding 1).
        bb = self.lid.bounding_box()
        self.assertAlmostEqual(bb.size.X, p.BOARD_W, places=3)
        self.assertAlmostEqual(bb.size.Y, p.BOARD_L, places=3)
        self.assertAlmostEqual(bb.max.Z, p.Z_WALL_TOP, places=3)

    def test_legs_span_the_cavity_to_the_board_face(self):
        bb = self.lid.bounding_box()
        self.assertAlmostEqual(bb.min.Z, 0.0, places=3)
        self.assertAlmostEqual(bb.size.Z, p.CAVITY + p.PANEL, places=3)

    def test_lid_top_matches_the_walls_own_top_nothing_stands_proud(self):
        # Would have caught the wall standing 1.6mm proud of the panel:
        # compares the TRAY's own built wall geometry against the LID's own
        # built plate geometry directly (not just each against p.Z_WALL_TOP
        # separately, which both derive from the same constant and so can't
        # catch one of the two code paths drifting from the other). The
        # spec's promise is that Pro and Lite look identical from outside --
        # this is that promise, checked on the actual solids.
        wall_top = case.shell().bounding_box().max.Z
        lid_top = self.lid.bounding_box().max.Z
        self.assertAlmostEqual(wall_top, lid_top, places=3)
        self.assertAlmostEqual(lid_top, p.Z_WALL_TOP, places=3)

    def test_it_fits_the_tray_with_no_interference(self):
        # A zero-volume intersection (the old assertion here) is satisfied
        # identically by "clears by 2mm" and "touches exactly" -- it can't
        # tell a real air gap from a plate cut at the pocket's own size
        # (Finding 1: that was the actual bug, and this test passed right
        # through it). Assert an actual margin instead: the plate is cut
        # POCKET_CLEAR undersize from the pocket on every side, so it must
        # clear by close to that all around.
        margin = case.tray().distance_to(self.lid)
        self.assertGreaterEqual(margin, 0.15,
                                 f"lid clears the pocket by only {margin:.4f}mm")

    def test_plunger_bore_sits_over_SW3(self):
        # SW3's own pad centroid (its four through-hole pads sit at
        # footprint-local (0,0)/(6.5,0)/(0,4.5)/(6.5,4.5), centred on the
        # body at local (3.25, 2.25)), not the hardcoded KiCad (131, 91)
        # this test used to mirror (Finding 2) -- so a footprint move is
        # caught here instead of leaving the bore silently behind it.
        from build123d import Cylinder, Pos
        import board_geom as bg
        fp = bg.footprint("SW3")
        pad_xs = [lx for lx, _ in bg.pad_positions("SW3")]
        pad_ys = [ly for _, ly in bg.pad_positions("SW3")]
        lx = (min(pad_xs) + max(pad_xs)) / 2
        ly = (min(pad_ys) + max(pad_ys)) / 2
        x, y = bg.to_case(*bg.local_to_board(fp, lx, ly))
        probe = Pos(x, y, p.Z_WALL_TOP - p.PANEL / 2) * Cylinder(0.4, p.PANEL)
        hit = self.lid & probe
        self.assertAlmostEqual(hit.volume if hit else 0.0, 0.0, places=3)

    def test_legs_do_not_run_through_a_connector(self):
        # The legs stand where the board does. J2's shell comes within 2.220mm
        # of H3's centre against a 3.00mm leg radius, for the leg's whole
        # height -- about 27mm^3 of solid overlap, and the lid would not have
        # gone on.
        #
        # Scoped to the LEGS: the plate's underside is meant to touch the
        # mini-DIN shells at CAVITY (that contact sets the lid's height,
        # exactly as the Pro's panel rests on them), and the shells'
        # tessellation puts a micron or so of that contact on the wrong side
        # of a strict overlap test. The legs have no business touching
        # anything.
        import board_geom as bg
        from build123d import Cylinder, Pos
        for ref in ("J2", "J4", "J1"):
            conn = case.connector_solid(ref)
            for h, (x, y) in bg.mount_holes().items():
                envelope = Pos(x, y, p.CAVITY / 2) * Cylinder(p.LEG_D / 2 + 0.1, p.CAVITY)
                region = self.lid & envelope
                if not region:
                    continue
                hit = region & conn
                vol = hit.volume if hit else 0.0
                self.assertAlmostEqual(vol, 0.0, places=2,
                                       msg=f"{h}'s leg puts {vol:.3f}mm^3 inside {ref}")

    def test_leg_feet_are_relieved_for_parts_crowding_a_hole(self):
        # C22, a 0603 1.24mm tall, reaches within 2.844mm of H4's centre. A
        # full-diameter leg stands on it and cracks it. Check the foot is
        # actually narrower than the leg over the part's height.
        import board_geom as bg
        from build123d import Cylinder, Pos
        x, y = bg.mount_holes()["H4"]
        # A ring at the part's height, between the relieved and full radii:
        # must be empty if the relief is really there.
        ring = (Pos(x, y, p.FOOT_RELIEF_H / 2) * Cylinder(p.LEG_D / 2, p.FOOT_RELIEF_H)
                - Pos(x, y, p.FOOT_RELIEF_H / 2) * Cylinder(p.LEG_D / 2 - p.FOOT_RELIEF, p.FOOT_RELIEF_H))
        hit = self.lid & ring
        self.assertAlmostEqual(hit.volume if hit else 0.0, 0.0, places=3,
                               msg="leg foot is not relieved")

    def test_insert_bores_open_downward_at_the_board_face(self):
        import board_geom as bg
        from build123d import Cylinder, Pos
        for x, y in bg.mount_holes().values():
            probe = Pos(x, y, p.INSERT_DEPTH / 2) * Cylinder(
                p.INSERT_D / 2 - 0.1, p.INSERT_DEPTH)
            hit = self.lid & probe
            self.assertAlmostEqual(hit.volume if hit else 0.0, 0.0, places=3)


def _corners(x0, y0, x1, y1):
    """The four corners of a box spanning x0..x1, y0..y1, each with the unit
    sign (sx, sy) that points INWARD from that corner -- the direction its
    fillet arc centre sits in, and the direction a probe must step to move
    off the sharp corner and into the box."""
    return [
        (x0, y0, 1, 1),
        (x0, y1, 1, -1),
        (x1, y0, -1, 1),
        (x1, y1, -1, -1),
    ]


class TestCornerRounding(unittest.TestCase):
    """Task 10: the board (hardware/airwriter/airwriter.kicad_pcb) and the
    art panel it shares an outline with both have R2.0 corners, not square
    ones (board_geom.corner_radius()). shell()'s exterior is rounded at
    EXT_R, its pocket at POCKET_R, and lite_lid()'s plate at CORNER_R --
    the panel's own outline, not the pocket's (Finding 1: cutting the plate
    at POCKET_R/POCKET_W/POCKET_L, the pocket's own size, leaves it zero
    clearance to seat in) (params.py derives all three from CORNER_R). Only
    the vertical edges are filleted -- top and bottom rims stay sharp, since
    a rounded bottom rim on the tray (which prints floor-down) would be an
    unsupported overhang.

    Every probe here targets ONE of the four corners along its own 45-degree
    diagonal: a point just off the sharp corner tip (always outside any
    fillet arc, regardless of radius) and a point just inside the fillet
    arc's own radius (always on the rounded surface, regardless of radius).
    A square-cornered part and a rounded one disagree at both points, which
    is exactly why this failed loudly against the pre-Task-10 square
    geometry (see task-10-report.md for the recorded red/green run).
    """

    @classmethod
    def setUpClass(cls):
        cls.shell = case.shell()
        cls.lid = case.lite_lid()

    def _assert_solid_corner_rounded(self, part, x0, y0, x1, y1, radius, z,
                                      label):
        """For a SOLID part whose outer footprint is x0..x1, y0..y1: the
        square-corner wedge a fillet cuts away must be empty, and the
        fillet's own rounded body must be solid. Used for the tray's
        exterior and the lite lid's plate -- both solids with a rounded
        outer edge."""
        from build123d import Box, Pos
        for cx, cy, sx, sy in _corners(x0, y0, x1, y1):
            centre_x, centre_y = cx + sx * radius, cy + sy * radius

            tip = Pos(cx + sx * 0.03, cy + sy * 0.03, z) * Box(0.05, 0.05, 1.0)
            hit = part & tip
            vol = hit.volume if hit else 0.0
            self.assertAlmostEqual(
                vol, 0.0, places=4,
                msg=f"{label} corner ({cx:.2f},{cy:.2f}): {vol:.4f}mm^3 of "
                    "square-corner material where a rounded corner should "
                    "be empty")

            # Step from the arc CENTRE back toward the corner tip (-sx,-sy)
            # by just under `radius` -- that point is inside the arc (still
            # on the rounded surface, still solid) but as close to the
            # corner as the fillet body reaches.
            diag = 1 / math.sqrt(2)
            body = Pos(centre_x - sx * (radius - 0.5) * diag,
                       centre_y - sy * (radius - 0.5) * diag,
                       z) * Box(0.3, 0.3, 1.0)
            hit = part & body
            vol = hit.volume if hit else 0.0
            self.assertGreater(
                vol, 0.5 * body.volume,
                msg=f"{label} corner ({cx:.2f},{cy:.2f}): only {vol:.4f}mm^3 "
                    f"of {body.volume:.4f}mm^3 on the fillet's own body is "
                    "solid")

    def test_tray_exterior_corners_are_rounded_at_ext_r(self):
        self._assert_solid_corner_rounded(
            self.shell, -p.WALL, -p.WALL, p.BOARD_W + p.WALL,
            p.BOARD_L + p.WALL, p.EXT_R, z=5.0, label="tray exterior")

    def test_pocket_corners_are_rounded_at_pocket_r(self):
        # The pocket is a VOID cut into the shell, so the polarity is
        # reversed from the solid case above: the square-corner wedge a
        # rounded pocket LEAVES BEHIND is solid shell material (the pocket
        # never reaches it), and the fillet's own body -- the pocket's
        # actual rounded corner -- is empty.
        from build123d import Box, Pos
        x0 = p.BOARD_W / 2 - p.POCKET_W / 2
        y0 = p.BOARD_L / 2 - p.POCKET_L / 2
        x1 = p.BOARD_W / 2 + p.POCKET_W / 2
        y1 = p.BOARD_L / 2 + p.POCKET_L / 2
        z = (p.Z_WALL_TOP + p.Z_FLOOR_TOP) / 2
        diag = 1 / math.sqrt(2)

        for cx, cy, sx, sy in _corners(x0, y0, x1, y1):
            centre_x, centre_y = cx + sx * p.POCKET_R, cy + sy * p.POCKET_R

            tip = Pos(cx + sx * 0.03, cy + sy * 0.03, z) * Box(0.05, 0.05, 1.0)
            hit = self.shell & tip
            vol = hit.volume if hit else 0.0
            self.assertAlmostEqual(
                vol, tip.volume, places=4,
                msg=f"pocket corner ({cx:.2f},{cy:.2f}): the square-corner "
                    "wedge a rounded pocket should retain as material is "
                    f"only {vol:.4f}/{tip.volume:.4f}mm^3 solid")

            body = Pos(centre_x - sx * (p.POCKET_R - 0.5) * diag,
                       centre_y - sy * (p.POCKET_R - 0.5) * diag,
                       z) * Box(0.3, 0.3, 1.0)
            hit = self.shell & body
            vol = hit.volume if hit else 0.0
            self.assertAlmostEqual(
                vol, 0.0, places=3,
                msg=f"pocket corner ({cx:.2f},{cy:.2f}): {vol:.4f}mm^3 of "
                    "material inside the pocket's own rounded region")

    def test_lite_lid_plate_corners_are_rounded_at_corner_r(self):
        # The plate is cut at the panel's OWN outline (BOARD_W x BOARD_L,
        # CORNER_R), not the pocket's (Finding 1) -- so this checks the
        # plate's real footprint and radius, not the pocket's.
        x0 = 0.0
        y0 = 0.0
        x1 = p.BOARD_W
        y1 = p.BOARD_L
        z = p.Z_WALL_TOP - p.PANEL / 2
        self._assert_solid_corner_rounded(
            self.lid, x0, y0, x1, y1, p.CORNER_R, z, label="lite lid plate")

    def test_lid_outline_matches_the_panel_it_stands_in_for(self):
        # The lid's plate is cut to the art panel's OWN outline -- BOARD_W x
        # BOARD_L, rounded at CORNER_R -- not the pocket's (Finding 1: a
        # plate cut to the pocket's own POCKET_W x POCKET_L/POCKET_R has
        # ZERO clearance in the pocket it's meant to seat in, which does not
        # print). Build that exact rounded rectangle independently (Box +
        # fillet on its vertical edges, not by calling into case.py's own
        # construction) and require the lid's plate to match it exactly:
        # nothing sticking out past it (a square-cornered plate WOULD stick
        # out at all four corners -- a square footprint always contains the
        # smaller rounded one, so only checking "the panel outline fits
        # inside the lid" can't catch this; it has to be checked in this
        # direction), and nothing missing from inside it either.
        from build123d import Axis, Box, Cylinder, Pos
        import board_geom as bg
        z = p.Z_WALL_TOP - p.PANEL / 2
        panel_shape = Pos(p.BOARD_W / 2, p.BOARD_L / 2, z) * Box(
            p.BOARD_W, p.BOARD_L, p.PANEL)
        panel_shape = panel_shape.fillet(
            p.CORNER_R, panel_shape.edges().filter_by(Axis.Z))
        # lite_lid() also cuts SW3's plunger bore through the plate -- a
        # real, intentional hole, not a corner defect. Cut the same bore
        # out of the reference shape so it isn't mistaken for one. Derived
        # from SW3's own pad centroid (Finding 2), not a hardcoded KiCad
        # coordinate, so this stays in sync with case.py's own derivation.
        sw3 = bg.footprint("SW3")
        pad_xs = [lx for lx, _ in bg.pad_positions("SW3")]
        pad_ys = [ly for _, ly in bg.pad_positions("SW3")]
        lx = (min(pad_xs) + max(pad_xs)) / 2
        ly = (min(pad_ys) + max(pad_ys)) / 2
        px, py = bg.to_case(*bg.local_to_board(sw3, lx, ly))
        plunger = Pos(px, py, z) * Cylinder(p.PLUNGER_BORE_D / 2, p.PANEL + 0.5)
        panel_shape = panel_shape - plunger

        # Isolate the lid's PLATE within its own Z band -- the legs sit
        # entirely below it (topping out exactly at the plate's underside)
        # and must not be swept into this comparison.
        plate_zband = Pos(p.BOARD_W / 2, p.BOARD_L / 2, z) * Box(
            p.POCKET_W + 2.0, p.POCKET_L + 2.0, p.PANEL)
        plate = self.lid & plate_zband
        self.assertIsNotNone(plate, "no lid material in the plate's own Z band")

        extra = plate - panel_shape
        extra_vol = extra.volume if extra else 0.0
        self.assertAlmostEqual(
            extra_vol, 0.0, delta=1.0,
            msg=f"{extra_vol:.3f}mm^3 of the lid plate sticks out past the "
                "panel's own rounded footprint (a square corner?)")

        missing = panel_shape - plate
        missing_vol = missing.volume if missing else 0.0
        self.assertAlmostEqual(
            missing_vol, 0.0, delta=1.0,
            msg=f"{missing_vol:.3f}mm^3 of the panel's rounded footprint "
                "has no lid material under it")

    def test_corner_fillets_are_concentric_with_2_80_radial_wall(self):
        """Carried over from review: a fillet is exactly where a wall gets
        thin, and test_printable.py's wall probes are all at mid-wall (e.g.
        y=90, x=31) -- nowhere near a corner. shell()'s exterior fillet
        (EXT_R) and pocket fillet (POCKET_R) at each of the four corners
        only make a uniform 2.80mm radial wall (WALL - POCKET_CLEAR)
        because they're both CORNER_R grown by a different constant
        (params.py) -- nothing stops a future edit from giving EXT_R and
        POCKET_R independent corner radii, which would silently thin (or
        thicken) the wall exactly where it's already weakest, and none of
        the volume-probe corner tests above would notice (they check "is
        there material/void here", with margin, not the wall's actual
        thickness).

        Read the two fillet faces at each corner back as real cylinder
        geometry -- geom_adaptor().Cylinder() -- rather than probing volume:
        their axes must coincide (same vertical line) and their radii must
        differ by exactly WALL - POCKET_CLEAR.
        """
        from build123d import GeomType

        cyl_faces = self.shell.faces().filter_by(GeomType.CYLINDER)

        # Group the fillet faces by corner: each corner's exterior and
        # pocket fillet share one vertical axis line, identified by that
        # axis's (X, Y) -- Z is just an arbitrary point on an infinite
        # line, not meaningful, so it's left out of the key.
        by_corner: dict[tuple[float, float], list] = {}
        for f in cyl_faces:
            cyl = f.geom_adaptor().Cylinder()
            axis = cyl.Axis()
            loc = axis.Location()
            key = (round(loc.X(), 3), round(loc.Y(), 3))
            by_corner.setdefault(key, []).append((cyl.Radius(), loc, axis.Direction()))

        self.assertEqual(
            len(by_corner), 4,
            f"expected the tray's 4 rounded corners, found fillet faces "
            f"grouped into {len(by_corner)}: {sorted(by_corner)}")

        want_radial_wall = p.WALL - p.POCKET_CLEAR
        for (x, y), entries in by_corner.items():
            self.assertEqual(
                len(entries), 2,
                f"corner ({x},{y}): expected 2 fillet faces (exterior + "
                f"pocket), found {len(entries)}")
            (r0, loc0, d0), (r1, loc1, d1) = entries

            # Same axis: parallel direction, and one axis's location point
            # lies on the other axis's (X, Y) line.
            self.assertAlmostEqual(d0.X(), d1.X(), places=6, msg=f"corner ({x},{y}) axis direction X")
            self.assertAlmostEqual(d0.Y(), d1.Y(), places=6, msg=f"corner ({x},{y}) axis direction Y")
            self.assertAlmostEqual(d0.Z(), d1.Z(), places=6, msg=f"corner ({x},{y}) axis direction Z")
            self.assertAlmostEqual(loc0.X(), loc1.X(), places=6, msg=f"corner ({x},{y}) axis location X")
            self.assertAlmostEqual(loc0.Y(), loc1.Y(), places=6, msg=f"corner ({x},{y}) axis location Y")

            radial_wall = abs(r0 - r1)
            self.assertAlmostEqual(
                radial_wall, want_radial_wall, places=4,
                msg=f"corner ({x},{y}): radial wall is {radial_wall:.6f}mm "
                    f"(radii {r0:.6f} and {r1:.6f}), want {want_radial_wall}mm")


if __name__ == "__main__":
    unittest.main()
