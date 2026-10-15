"""The light-guide lattice: fit, light paths, clearances, printability."""
import unittest

from build123d import Box, Cylinder, Pos, Vector

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
        self.assertGreaterEqual(bb.min.X, p.LATTICE_INSET - 1e-3)
        self.assertLessEqual(bb.max.X, p.BOARD_W - p.LATTICE_INSET + 1e-3)

    def test_every_lit_dot_has_an_open_column_the_dot_wide(self):
        r = bg.panel_grid()["dot_dia"] / 2 - 0.01
        for d in bg.panel_dots():
            if d["led"]:
                col = Pos(d["x"], d["y"], H / 2) * Cylinder(r, H + 2)
                self.assertLess(_vol(self.L, col), 1e-3, f"r{d['r']}c{d['c']}")

    def test_the_test_plunger_cell_is_open(self):
        (t,) = [d for d in bg.panel_dots() if d["mat"] == "T"]
        col = Pos(t["x"], t["y"], H / 2) * Cylinder(bg.panel_grid()["hole_dia"] / 2, H + 2)
        self.assertLess(_vol(self.L, col), 1e-3)

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
        c = p.D3_TUBE_CLEAR
        ix0, ix1, iy0, iy1 = x0 - c, x1 + c, y0 - c, y1 + c
        t = 0.6 - 0.01  # the thinnest wall the spec allows
        pts = []
        for k in range(1, 10):
            fx = ix0 + k / 10 * (ix1 - ix0)
            fy = iy0 + k / 10 * (iy1 - iy0)
            pts += [(fx, iy0 - t), (fx, iy1 + t), (ix0 - t, fy), (ix1 + t, fy)]
        d = t / 2 ** 0.5
        pts += [(ix0 - d, iy0 - d), (ix1 + d, iy0 - d), (ix0 - d, iy1 + d), (ix1 + d, iy1 + d)]
        for px, py in pts:
            self.assertTrue(self.L.is_inside(Vector(px, py, H - 0.5)),
                            f"no wall at ({px:.2f}, {py:.2f})")

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
