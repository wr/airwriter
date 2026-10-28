"""The light-guide lattice: fit, light paths, clearances, printability."""
import unittest

from build123d import Box, Cone, Cylinder, Pos, Vector

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
    """Points 0.44 out from the D3 tube's interior (its thinnest allowed
    wall is one printed line, 0.45, beside J1) that are not inside the part,
    at heights from just above the board to just under the panel. A side the
    board's edge closes (the tube runs on to it) is not probed past the
    lattice's own outline, rounded corner included."""
    (ix0, ix1, iy0, iy1), _ = lattice._d3_tube_rects()
    t = 0.45 - 0.01
    d = t / 2 ** 0.5
    pts = [(ix0 - d, iy0 - d), (ix1 + d, iy0 - d), (ix0 - d, iy1 + d), (ix1 + d, iy1 + d)]
    for k in range(1, 10):
        fx = ix0 + k / 10 * (ix1 - ix0)
        fy = iy0 + k / 10 * (iy1 - iy0)
        pts += [(fx, iy0 - t), (fx, iy1 + t), (ix0 - t, fy), (ix1 + t, fy)]
    outline = lattice._outline()
    pts = [(px, py) for px, py in pts if outline.is_inside(Vector(px, py, H / 2))]
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

    def test_collar_sits_where_the_real_plunger_stands(self):
        # 2026-10-15 prints: on the KiCad model's axis the collar pushed the
        # real plunger 0.4 west and back; 0.4 east and forward overshot. The
        # bore is centred halfway, 0.2 east and 0.2 forward.
        x0, x1, y0, y1, _ = components.envelopes()["SW3"][-1]
        x = (x0 + x1) / 2 + p.TEST_PLUNGER_OFFSET[0]
        y = (y0 + y1) / 2 + p.TEST_PLUNGER_OFFSET[1]
        self.assertEqual(p.TEST_PLUNGER_OFFSET, (0.20, -0.20))
        r = p.COLLAR_BORE_D / 2
        z = H - p.COLLAR_H / 2
        for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
            self.assertFalse(self.L.is_inside(Vector(x + dx * (r - 0.1), y + dy * (r - 0.1), z)),
                             f"bore not open {dx},{dy}")
            self.assertTrue(self.L.is_inside(Vector(x + dx * (r + 0.3), y + dy * (r + 0.3), z)),
                            f"no collar {dx},{dy}")

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
                       (-o, 60.0), (-o, 25.0), (p.BOARD_W + o, 3.0),
                       # the front edge (Wells: the biggest gap is there), and
                       # the east edge in front of the grid
                       (2.5, -o), (22.75, -o), (43.5, -o), (52.0, -o), (59.0, -o),
                       (-o, 5.0), (p.BOARD_W + o, 20.0)):
            self.assertTrue(self.L.is_inside(Vector(px, py, z)), f"no ridge at ({px}, {py})")

    def test_ridge_stops_at_the_mini_dins(self):
        o = 0.18
        z = H - p.RIDGE_H / 2
        for ref in ("J2", "J4"):
            bb = case.connector_solid(ref).bounding_box()
            self.assertFalse(self.L.is_inside(Vector((bb.min.X + bb.max.X) / 2, -o, z)), ref)

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
        # Below the top band only: the front skin and the lip may cross the
        # zone 10 mm above the module (copper keepout, not plastic).
        top = H - p.RIDGE_H
        ko = Pos((min(xs) + max(xs)) / 2, (min(ys) + y1) / 2, top / 2) * Box(
            max(xs) - min(xs), y1 - min(ys), top)
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
        # The part voids that could breach the tube sit low (J1's shell), so
        # the check must look there, not only near the top.
        (ix0, ix1, iy0, iy1), _ = lattice._d3_tube_rects()
        breach = Pos(ix0 - 0.3, (iy0 + iy1) / 2, 0.5) * Box(1.0, 1.0, 1.0)
        self.assertNotEqual(_d3_wall_gaps(self.L - breach), [])

    def test_d3_tube_clears_the_led_on_its_spacer(self):
        # Rev 2026.11: the tube is cut round the LED and its spacer as well as
        # the window, D3_TUBE_CLEAR all round.
        (ix0, ix1, iy0, iy1), _ = lattice._d3_tube_rects()
        c = p.D3_TUBE_CLEAR - 1e-6
        for bx0, bx1, by0, by1, _ in components.envelopes()["D3"]:
            self.assertTrue(ix0 <= bx0 - c and bx1 + c <= ix1 and iy0 <= by0 - c and by1 + c <= iy1)
        x0, x1, y0, y1 = lattice.d3_window()
        self.assertTrue(ix0 <= x0 - c and x1 + c <= ix1 and iy0 <= y0 - c and y1 + c <= iy1)
        self.assertLess(_vol(self.L, Pos((ix0 + ix1) / 2, (iy0 + iy1) / 2, H / 2)
                             * Box(ix1 - ix0 - 0.02, iy1 - iy0 - 0.02, H + 2)), 1e-3)

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



if __name__ == "__main__":
    unittest.main()
