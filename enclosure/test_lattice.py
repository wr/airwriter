"""The light-guide lattice: fit, light paths, clearances, printability."""
import unittest

from build123d import Box, Cone, Cylinder, GeomType, Pos, Vector

import board_geom as bg
import case
import components
import lattice
import params as p
from test_printable import upward_flats

H = p.CAVITY


def _vol(a, b):
    x = a & b
    return x.volume if x is not None else 0.0


def _d3_wall_gaps(part):
    """Points 0.59 out from the D3 tube's interior (its thinnest allowed
    wall is 0.6) that are not inside the part, at heights from just above the
    board to just under the panel."""
    x0, x1, y0, y1 = lattice.d3_window()
    c = p.D3_TUBE_CLEAR
    ix0, ix1, iy0, iy1 = x0 - c, x1 + c, y0 - c, y1 + c
    t = 0.6 - 0.01
    d = t / 2 ** 0.5
    pts = [(ix0 - d, iy0 - d), (ix1 + d, iy0 - d), (ix0 - d, iy1 + d), (ix1 + d, iy1 + d)]
    for k in range(1, 10):
        fx = ix0 + k / 10 * (ix1 - ix0)
        fy = iy0 + k / 10 * (iy1 - iy0)
        pts += [(fx, iy0 - t), (fx, iy1 + t), (ix0 - t, fy), (ix1 + t, fy)]
    return [(round(px, 2), round(py, 2), z) for z in (0.2, 0.6, 1.0, 1.5, H / 2, H - 0.5)
            for px, py in pts if not part.is_inside(Vector(px, py, z))]


class TestLattice(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.L = lattice.lattice()

    def test_one_valid_solid_cavity_tall(self):
        self.assertTrue(self.L.is_valid)
        self.assertEqual(len(self.L.solids()), 1)
        bb = self.L.bounding_box()
        self.assertAlmostEqual(bb.min.Z, 0.0, places=3)
        self.assertAlmostEqual(bb.max.Z, H, places=3)
        # The edge ridge reaches RIDGE_CLEAR short of the tray's pocket.
        reach = p.POCKET_CLEAR - p.RIDGE_CLEAR
        self.assertAlmostEqual(bb.min.X, -reach, places=3)
        self.assertAlmostEqual(bb.max.X, p.BOARD_W + reach, places=3)
        self.assertAlmostEqual(bb.max.Y, p.BOARD_L + reach, places=3)

    def test_every_lit_dot_has_an_open_column_the_dot_wide(self):
        r = bg.panel_grid()["dot_dia"] / 2 - 0.01
        for d in bg.panel_dots():
            if d["led"]:
                col = Pos(d["x"], d["y"], H / 2) * Cylinder(r, H + 2)
                self.assertLess(_vol(self.L, col), 1e-3, f"r{d['r']}c{d['c']}")

    def test_the_test_cell_is_open_below_the_collar(self):
        (t,) = [d for d in bg.panel_dots() if d["mat"] == "T"]
        top = H - p.COLLAR_H - 0.05
        col = Pos(t["x"], t["y"], top / 2) * Cylinder(bg.panel_grid()["hole_dia"] / 2, top)
        self.assertLess(_vol(self.L, col), 1e-3)

    def test_collar_clears_the_measured_plunger(self):
        # Wells's caliper: 3.1 at the lattice top, 3.5 where the plunger
        # enters the 3.2-tall body. 0.15 a side must stay free all the way.
        x, y = lattice.test_plunger_axis()
        c = 0.15
        h = H - p.TEST_BODY_TOP
        plunger = Pos(x, y, p.TEST_BODY_TOP + h / 2) * Cone(
            p.TEST_PLUNGER_D_BASE / 2 + c, p.TEST_PLUNGER_D_TOP / 2 + c, h)
        self.assertLess(_vol(self.L, plunger), 1e-3)

    def test_collar_braces_the_plunger_near_the_top(self):
        x, y = lattice.test_plunger_axis()
        r = p.COLLAR_BORE_D / 2
        ring = Pos(x, y, H - p.COLLAR_H / 2) * (
            Cylinder(r + 0.8, p.COLLAR_H - 0.1) - Cylinder(r + 0.05, p.COLLAR_H - 0.1))
        self.assertGreater(_vol(self.L, ring) / ring.volume, 0.95)
        # held to all four cell walls at mid-collar
        x0, x1, y0, y1 = lattice.cell_bounds()[(3, 3)]
        z = H - p.COLLAR_H / 2
        for px, py in (((x0 + x - r) / 2 - 0.5, y), ((x1 + x + r) / 2 + 0.5, y),
                       (x, (y0 + y - r) / 2 - 0.5), (x, (y1 + y + r) / 2 + 0.5)):
            self.assertTrue(self.L.is_inside(Vector(px, py, z)), f"no rib at ({px:.1f}, {py:.1f})")

    def test_ridge_hides_the_panel_edge_gap(self):
        # 0.18 past the board edge: inside the gap round the panel (the
        # pocket is 0.30 out), so looking down it you see the ridge.
        o = 0.18
        z = H - p.RIDGE_H / 2
        yf = lattice.front_line()
        for px, py in ((31.0, p.BOARD_L + o), (p.BOARD_W + o, 60.0), (p.BOARD_W + o, yf + 5),
                       (-o, 60.0), (-o, 25.0), (p.BOARD_W + o, 3.0)):
            self.assertTrue(self.L.is_inside(Vector(px, py, z)), f"no ridge at ({px}, {py})")

    def test_posts_bores_and_standoff_pockets(self):
        for ref, (x, y) in bg.mount_holes().items():
            bore = Pos(x, y, H / 2) * Cylinder(p.SCREW_CLEAR_D / 2 - 0.05, H + 2)
            self.assertLess(_vol(self.L, bore), 1e-3, ref)
            nut = Pos(x, y, H - 1.5) * Cylinder(5.1 / 2 + 0.1, 3.0)
            self.assertLess(_vol(self.L, nut), 1e-3, ref)
            ring = Pos(x, y, 6) * (Cylinder(3.5, 4) - Cylinder(1.5, 4))
            frac = _vol(self.L, ring) / ring.volume
            self.assertGreater(frac, 0.95 if ref in ("H1", "H2") else 0.35, ref)

    def test_clears_every_part_shadow(self):
        for ref, bands in components.envelopes().items():
            if ref in ("J1", "J2", "J4"):
                continue
            for x0, x1, y0, y1, top in bands:
                if ref == "SW3" and top > H - p.COLLAR_H:
                    continue  # the model's plunger; the measured one is tested above
                shadow = Pos((x0 + x1) / 2, (y0 + y1) / 2, top / 2) * Box(x1 - x0, y1 - y0, top)
                self.assertLess(_vol(self.L, shadow), 1e-3, ref)
        for ref in ("J1", "J2", "J4"):
            self.assertLess(_vol(self.L, case.connector_solid(ref)), 1e-3, ref)

    def test_clears_the_antenna_keepout_in_front_of_the_grid(self):
        (zone,) = bg.keepout_zones("U1")
        xs = [q[0] for q in zone]
        ys = [q[1] for q in zone]
        y1 = min(max(ys), lattice.front_line() - p.LATTICE_WALL / 2)
        ko = Pos((min(xs) + max(xs)) / 2, (min(ys) + y1) / 2, H / 2) * Box(
            max(xs) - min(xs), y1 - min(ys), H)
        self.assertLess(_vol(self.L, ko), 1e-3)

    def test_clears_the_reset_paperclip_path(self):
        _, y = bg.to_case(bg.footprint("SW1").x, bg.footprint("SW1").y)
        path = Pos(p.BOARD_W - 1.0, y, p.RESET_ACTUATOR_Z) * Cylinder(
            p.RESET_PINHOLE_D / 2, 3.0, rotation=(0, 90, 0))
        self.assertLess(_vol(self.L, path), 1e-3)

    def test_fits_the_tray(self):
        self.assertLess(_vol(self.L, case.tray()), 1e-3)

    def test_d3_tube_is_open_over_the_window_and_walled_all_round(self):
        x0, x1, y0, y1 = lattice.d3_window()
        col = Pos((x0 + x1) / 2, (y0 + y1) / 2, H / 2) * Box(x1 - x0, y1 - y0, H + 2)
        self.assertLess(_vol(self.L, col), 1e-3)
        self.assertEqual(_d3_wall_gaps(self.L), [])

    def test_d3_wall_check_catches_a_breach_near_the_board(self):
        # The part voids that could breach the tube all sit low (<= 1.3), so
        # the check must look there, not only near the top.
        x0, x1, y0, y1 = lattice.d3_window()
        c = p.D3_TUBE_CLEAR
        breach = Pos(x0 - c - p.LATTICE_WALL / 2, (y0 + y1) / 2, 0.5) * Box(2.0, 1.0, 1.0)
        self.assertNotEqual(_d3_wall_gaps(self.L - breach), [])

    def test_d3_and_its_resistor_sit_inside_the_tube(self):
        x0, x1, y0, y1 = lattice.d3_window()
        c = p.D3_TUBE_CLEAR
        env = components.envelopes()
        for ref in ("D3", "R17"):
            (bx0, bx1, by0, by1, _), = env[ref]
            self.assertTrue(x0 - c <= bx0 and bx1 <= x1 + c and y0 - c <= by0 and by1 <= y1 + c,
                            ref)

    def test_grid_walls_are_lattice_wall_thick(self):
        xs, _ = lattice.wall_lines()
        x0, x1, y0, y1 = lattice.cell_bounds()[(2, 3)]
        probe = Pos(xs[4], (y0 + y1) / 2, 6.5) * Box(2.0, 2.0, 1.0)
        self.assertAlmostEqual(_vol(self.L, probe), p.LATTICE_WALL * 2.0, delta=0.05)

    def test_no_notch_lets_a_lit_cell_into_another_window(self):
        # Lit-to-lit bleed, or a lit cell leaking into the test cell, whose
        # panel hole shows light. Lit-to-gold or into the front is accepted.
        self.assertEqual([w for w in lattice.leaky_walls(self.L)
                          if "lit|lit" in w or "lit|test" in w], [])

    def test_leak_report_counts_the_test_cell_as_a_window(self):
        (t,) = [d for d in bg.panel_dots() if d["mat"] == "T"]
        x0, x1, y0, y1 = lattice.cell_bounds()[(t["r"], t["c"])]
        notch = Pos(x1 + p.LATTICE_WALL / 2, (y0 + y1) / 2, 1.0) * Box(2.0, 2.0, 2.0)
        leaks = lattice.leaky_walls(self.L - notch)
        self.assertIn(f"r{t['r']}c{t['c'] + 1} west lit|test", leaks)

    def _wall_between(self, r, c):
        # The wall between cell (r, c) and (r, c + 1), mid-height, mid-cell.
        xs, _ = lattice.wall_lines()
        _, _, y0, y1 = lattice.cell_bounds()[(r, c)]
        return xs[c + 1], (y0 + y1) / 2

    def test_leak_report_catches_a_small_hole_right_through_a_wall(self):
        # A 1 x 1 mm hole through the wall between two lit cells (r2c3 L,
        # r2c4 P) is 1% of the wall: big enough to see, small by volume.
        x, y = self._wall_between(2, 3)
        hole = Pos(x, y, 6.0) * Box(2.0, 1.0, 1.0)
        self.assertIn("r2c3 east lit|lit", lattice.leaky_walls(self.L - hole))

    def test_leak_report_ignores_a_notch_that_stops_inside_the_wall(self):
        # Thinning a wall (a part's envelope grazing it) lets no light through.
        x, y = self._wall_between(2, 3)
        dent = Pos(x - p.LATTICE_WALL / 2 + 0.15, y, 6.0) * Box(0.3, 3.0, 3.0)
        self.assertNotIn("r2c3 east lit|lit", lattice.leaky_walls(self.L - dent))

    def test_d3_window_is_the_one_over_d3(self):
        x0, x1, y0, y1 = lattice.d3_window()
        f = bg.footprint("D3")
        x, y = bg.to_case(f.x, f.y)
        self.assertTrue(x0 < x < x1 and y0 < y < y1)

    def test_prints_panel_face_down_without_supports(self):
        flats = upward_flats(self.L, ceiling_z=H)
        self.assertEqual([(round(f.center().X, 1), round(f.center().Y, 1),
                           round(f.center().Z, 1), round(f.area, 1)) for f in flats], [])



class TestLightPipe(unittest.TestCase):
    """The clear-PETG pipe that drops into the D3 tube."""

    @classmethod
    def setUpClass(cls):
        cls.L = lattice.lattice()
        cls.P = lattice.light_pipe()

    def test_one_valid_solid_on_the_board_just_under_the_panel(self):
        self.assertTrue(self.P.is_valid)
        self.assertEqual(len(self.P.solids()), 1)
        bb = self.P.bounding_box()
        self.assertAlmostEqual(bb.min.Z, 0.0, places=3)
        self.assertAlmostEqual(bb.max.Z, H - p.PIPE_TOP_GAP, places=3)

    def test_drops_into_the_tube_without_touching_the_lattice(self):
        self.assertLess(_vol(self.L, self.P), 1e-3)

    def test_covers_the_whole_window_at_the_top(self):
        x0, x1, y0, y1 = lattice.d3_window()
        for i in range(11):
            for j in range(11):
                q = Vector(x0 + i / 10 * (x1 - x0), y0 + j / 10 * (y1 - y0), H - 0.3)
                self.assertTrue(self.P.is_inside(q), f"window not covered at {q}")

    def test_clears_d3_and_r17_and_wraps_d3(self):
        env = components.envelopes()
        for ref in ("D3", "R17"):
            (x0, x1, y0, y1, top), = env[ref]
            box = Pos((x0 + x1) / 2, (y0 + y1) / 2, top / 2) * Box(x1 - x0, y1 - y0, top)
            self.assertLess(_vol(self.P, box), 1e-3, ref)
        (x0, x1, y0, y1, top), = env["D3"]
        above = Vector((x0 + x1) / 2, (y0 + y1) / 2, top + p.PIPE_POCKET_CLEAR + 0.1)
        self.assertTrue(self.P.is_inside(above), "no pipe right over D3")

    def test_every_face_is_flat_and_square_so_it_prints_on_any_side(self):
        for f in self.P.faces():
            self.assertEqual(f.geom_type, GeomType.PLANE)
            n = f.normal_at(f.center())
            self.assertAlmostEqual(max(abs(n.X), abs(n.Y), abs(n.Z)), 1.0, places=6)


if __name__ == "__main__":
    unittest.main()
