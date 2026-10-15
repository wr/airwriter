"""The light-guide lattice: the printed part between the main board and the
dot-grid art panel in the Pro build.

docs/superpowers/specs/2026-10-14-light-guide-lattice-design.md. An egg-crate
grid of open cells on the dot pitch, posts at H1-H4 carrying the panel's
standoffs, side rails forward to the H3/H4 posts, and a tube up to the panel's
FR4 window over the power LED D3. Everything is read from the board, the panel
layout and KiCad's STEP export (components.py), then the parts' shadows, the
connector hulls, U1's antenna keepout and SW1's reset path are cut away.
Printed panel-face down."""
import functools
import operator

from build123d import Box, Cone, Cylinder, Part, Pos, RectangleRounded, extrude

import board_geom as bg
import case
import components
import params as p

H = p.CAVITY


def _union(parts):
    return functools.reduce(operator.add, parts)


def _box(x0, x1, y0, y1, z0, z1) -> Part:
    return Pos((x0 + x1) / 2, (y0 + y1) / 2, (z0 + z1) / 2) * Box(x1 - x0, y1 - y0, z1 - z0)


def wall_lines() -> tuple[list[float], list[float]]:
    """Case-frame lines midway between dot centres: 8 in X (west to east) and
    9 in Y (back to front)."""
    g = bg.panel_grid()
    (ox, oy), pitch = g["origin"], g["pitch"]
    xs = [bg.panel_to_case(ox + (c - 0.5) * pitch, 0)[0] for c in range(g["cols"] + 1)]
    ys = [bg.panel_to_case(0, oy + (r - 0.5) * pitch)[1] for r in range(g["rows"] + 1)]
    return xs, ys


def front_line() -> float:
    """The grid's front wall centre: the line in front of the last row."""
    return wall_lines()[1][-1]


def cell_bounds() -> dict[tuple[int, int], tuple[float, float, float, float]]:
    """(r, c) -> (x0, x1, y0, y1), each cell's open interior. Outer cells run
    out to the inset outline less one wall."""
    xs, ys = wall_lines()
    g = bg.panel_grid()
    w, i = p.LATTICE_WALL, p.LATTICE_INSET
    out = {}
    for r in range(g["rows"]):
        for c in range(g["cols"]):
            x0 = i + w if c == 0 else xs[c] + w / 2
            x1 = p.BOARD_W - i - w if c == g["cols"] - 1 else xs[c + 1] - w / 2
            y1 = p.BOARD_L - i - w if r == 0 else ys[r] - w / 2
            y0 = ys[r + 1] + w / 2
            out[(r, c)] = (x0, x1, y0, y1)
    return out


def _outline() -> Part:
    i = p.LATTICE_INSET
    return case._rounded_box(p.BOARD_W - 2 * i, p.BOARD_L - 2 * i, H,
                             p.BOARD_W / 2, p.BOARD_L / 2, H / 2, p.CORNER_R - i)


def _grid() -> Part:
    block = _box(0, p.BOARD_W, front_line() - p.LATTICE_WALL / 2, p.BOARD_L, 0, H)
    cells = [Pos((x0 + x1) / 2, (y0 + y1) / 2, H / 2) * extrude(
        RectangleRounded(x1 - x0, y1 - y0, p.CELL_R), H / 2 + 1, both=True)
        for x0, x1, y0, y1 in cell_bounds().values()]
    return block - _union(cells)


def _posts() -> Part:
    return _union([Pos(x, y, H / 2) * Cylinder(p.POST_D / 2, H)
                   for x, y in bg.mount_holes().values()])


def _post_voids() -> Part:
    """Bore through each post; the standoff pocket from the top; a 45-degree
    cone from the pocket floor down into the bore, so printed panel-face down
    the pocket floor is not a flat ceiling."""
    rb, rp = p.SCREW_CLEAR_D / 2, p.NUT_POCKET_D / 2
    z_floor = H - p.NUT_POCKET_DEPTH
    out = []
    for x, y in bg.mount_holes().values():
        out.append(Pos(x, y, H / 2) * Cylinder(rb, H + 2))
        out.append(Pos(x, y, z_floor + (p.NUT_POCKET_DEPTH + 1) / 2)
                   * Cylinder(rp, p.NUT_POCKET_DEPTH + 1))
        out.append(Pos(x, y, z_floor - (rp - rb) / 2) * Cone(rb, rp, rp - rb))
    return _union(out)


def _rails() -> Part:
    yf = front_line()
    (_, y3), (_, y4) = bg.mount_holes()["H3"], bg.mount_holes()["H4"]
    return (_box(p.LATTICE_INSET, p.WEST_RAIL_X1, y3, yf, 0, H)
            + _box(p.EAST_RAIL_X0, p.EAST_RAIL_X1, y4, yf, 0, H))


def d3_window() -> tuple[float, float, float, float]:
    """The panel FR4 window that D3 sits under."""
    f = bg.footprint("D3")
    x, y = bg.to_case(f.x, f.y)
    hits = [w for w in bg.fr4_windows() if w[0] < x < w[1] and w[2] < y < w[3]]
    if len(hits) != 1:
        raise RuntimeError(f"expected one FR4 window over D3, found {len(hits)}")
    return hits[0]


def _d3_tube() -> tuple[Part, Part]:
    """(tube + arm to the H4 post, the tube's void)."""
    x0, x1, y0, y1 = d3_window()
    c, w = p.D3_TUBE_CLEAR, p.LATTICE_WALL
    ix0, ix1, iy0, iy1 = x0 - c, x1 + c, y0 - c, y1 + c
    tube = _box(ix0 - w, ix1 + w, iy0 - w, iy1 + w, 0, H)
    hx, hy = bg.mount_holes()["H4"]
    arm = _box(hx, ix0, iy1 - 1.0, hy, 0, H)
    return tube + arm, _box(ix0, ix1, iy0, iy1, -1, H + 1)


def _part_voids() -> Part:
    """Every part's shadow (components.py), grown by PART_CLEAR and
    PART_TOP_CLEAR, from below the board to its top. The connectors use their
    vendor hulls grown by PORT_CLEAR instead, as the tray does."""
    c = p.PART_CLEAR
    out = []
    for ref, bands in components.envelopes().items():
        if ref in ("J1", "J2", "J4"):
            continue
        for x0, x1, y0, y1, top in bands:
            out.append(_box(x0 - c, x1 + c, y0 - c, y1 + c, -1, top + p.PART_TOP_CLEAR))
    out += [case._grown(ref, p.PORT_CLEAR) for ref in ("J1", "J2", "J4")]
    return _union(out)


def _keepout_void() -> Part:
    """U1's antenna keepout, grown by KEEPOUT_CLEAR, clipped at the grid's
    front wall: the zone runs 1.85 into the grid, but the module's antenna
    ends 1.7 short of it, and cutting the grid there opened a lit cell."""
    (zone,) = bg.keepout_zones("U1")
    k = p.KEEPOUT_CLEAR
    xs = [q[0] for q in zone]
    ys = [q[1] for q in zone]
    y1 = min(max(ys) + k, front_line() - p.LATTICE_WALL / 2)
    return _box(min(xs) - k, max(xs) + k, min(ys) - k, y1, -1, H + 1)


def _reset_path_void() -> Part:
    """SW1's reset paperclip path, from the switch out through the east edge,
    and its shadow down to the board."""
    _, y = bg.to_case(bg.footprint("SW1").x, bg.footprint("SW1").y)
    half = p.RESET_PINHOLE_D / 2 + p.PART_CLEAR
    x0 = bg.courtyard("SW1")[1] - 1.0
    return _box(x0, p.BOARD_W + 1, y - half, y + half, -1, p.RESET_ACTUATOR_Z + half)


def lattice() -> Part:
    tube, tube_void = _d3_tube()
    body = (_grid() + _posts() + _rails() + tube) & _outline()
    return (body - _post_voids() - tube_void - _part_voids() - _keepout_void()
            - _reset_path_void())


def leaky_walls(part: Part) -> list[str]:
    """Each lit cell's walls that a clearance notch has opened, as
    "r{r}c{c} {side} lit|lit", "... lit|test" or "... lit|dark". The test
    cell is unlit but its panel hole shows light; dark is a gold (unlit,
    copper-covered) cell or outside the grid. Lit-to-dark bleed is accepted
    (the spec); the other two are what a viewer would see."""
    dots = bg.panel_dots()
    lit = {(d["r"], d["c"]) for d in dots if d["led"]}
    test = {(d["r"], d["c"]) for d in dots if d["mat"] == "T"}
    cells = cell_bounds()
    w = p.LATTICE_WALL
    out = []
    for (r, c) in sorted(lit):
        x0, x1, y0, y1 = cells[(r, c)]
        sides = {"west": ((x0 - w, x0, y0, y1), (r, c - 1)),
                 "east": ((x1, x1 + w, y0, y1), (r, c + 1)),
                 "back": ((x0, x1, y1, y1 + w), (r - 1, c)),
                 "front": ((x0, x1, y0 - w, y0), (r + 1, c))}
        for side, ((a, b, cc, d), nb) in sides.items():
            probe = _box(a, b, cc, d, 0, H)
            got = part & probe
            if (got.volume if got is not None else 0.0) < 0.98 * probe.volume:
                kind = ("lit|lit" if nb in lit else
                        "lit|test" if nb in test else "lit|dark")
                out.append(f"r{r}c{c} {side} {kind}")
    return out
