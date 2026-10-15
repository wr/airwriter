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


def test_plunger_axis() -> tuple[float, float]:
    """SW3's plunger centre, from the top band of its STEP envelope."""
    x0, x1, y0, y1, _ = components.envelopes()["SW3"][-1]
    return (x0 + x1) / 2, (y0 + y1) / 2


def d3_window() -> tuple[float, float, float, float]:
    """The panel FR4 window that D3 sits under."""
    f = bg.footprint("D3")
    x, y = bg.to_case(f.x, f.y)
    hits = [w for w in bg.fr4_windows() if w[0] < x < w[1] and w[2] < y < w[3]]
    if len(hits) != 1:
        raise RuntimeError(f"expected one FR4 window over D3, found {len(hits)}")
    return hits[0]


def _d3_tube_rects() -> tuple[tuple[float, float, float, float],
                              tuple[float, float, float, float]]:
    """(interior, outer) of the D3 tube as (x0, x1, y0, y1). The outer runs on
    to the board edge on any side within 1 mm of it, so the edge ridge has a
    wall to hang from at the corner."""
    x0, x1, y0, y1 = d3_window()
    c, w = p.D3_TUBE_CLEAR, p.LATTICE_WALL
    inner = (x0 - c, x1 + c, y0 - c, y1 + c)
    ox0, ox1, oy0, oy1 = inner[0] - w, inner[1] + w, inner[2] - w, inner[3] + w
    ox0 = 0.0 if ox0 < 1.0 else ox0
    ox1 = p.BOARD_W if ox1 > p.BOARD_W - 1.0 else ox1
    oy0 = 0.0 if oy0 < 1.0 else oy0
    oy1 = p.BOARD_L if oy1 > p.BOARD_L - 1.0 else oy1
    return inner, (ox0, ox1, oy0, oy1)


def _d3_tube() -> tuple[Part, Part]:
    """(tube + arm to the H4 post, the tube's void)."""
    (ix0, ix1, iy0, iy1), outer = _d3_tube_rects()
    tube = _box(*outer, 0, H)
    hx, hy = bg.mount_holes()["H4"]
    arm = _box(hx, ix0, iy1 - 1.0, hy, 0, H)
    return tube + arm, _box(ix0, ix1, iy0, iy1, -1, H + 1)


def light_pipe() -> Part:
    """The clear-PETG light pipe that drops into the D3 tube: the tube's
    interior less PIPE_CLEAR, standing on the board, PIPE_TOP_GAP under the
    panel, with a slot right across it over each part inside (D3 and R17) so
    the LED sits inside the pipe. Every face is flat and square, so it prints
    on any side; lying on a wide face keeps the layers along the light."""
    (ix0, ix1, iy0, iy1), _ = _d3_tube_rects()
    c, k = p.PIPE_CLEAR, p.PIPE_POCKET_CLEAR
    px0, px1, py0, py1 = ix0 + c, ix1 - c, iy0 + c, iy1 - c
    pipe = _box(px0, px1, py0, py1, 0, H - p.PIPE_TOP_GAP)
    slots = [_box(px0 - 1, px1 + 1, y0 - k, y1 + k, -1, top + k)
             for bands in components.envelopes().values()
             for x0, x1, y0, y1, top in bands
             if x1 + k > px0 and x0 - k < px1 and y1 + k > py0 and y0 - k < py1]
    return pipe - _union(slots) if slots else pipe


def _collar() -> Part:
    """A ring round the test plunger in the lattice's top COLLAR_H, tied to
    the test cell's four walls by LATTICE_WALL ribs."""
    x, y = test_plunger_axis()
    (t,) = [d for d in bg.panel_dots() if d["mat"] == "T"]
    x0, x1, y0, y1 = cell_bounds()[(t["r"], t["c"])]
    w, h = p.LATTICE_WALL, p.COLLAR_H
    z0 = H - h
    r = p.COLLAR_BORE_D / 2
    ring = Pos(x, y, z0 + h / 2) * Cylinder(r + p.COLLAR_WALL, h)
    ribs = [_box(x0 - w / 2, x, y - w / 2, y + w / 2, z0, H),
            _box(x, x1 + w / 2, y - w / 2, y + w / 2, z0, H),
            _box(x - w / 2, x + w / 2, y0 - w / 2, y, z0, H),
            _box(x - w / 2, x + w / 2, y, y1 + w / 2, z0, H)]
    bore = Pos(x, y, H / 2) * Cylinder(r, H + 2)
    return _union([ring] + ribs) - bore


def _ridge() -> Part:
    """The lip round the lattice's top edge: from just inside the lattice's
    perimeter wall out to RIDGE_CLEAR short of the tray's pocket, along every
    edge the lattice reaches -- the grid's back and sides, the west rail and
    the D3 corner."""
    o = p.POCKET_CLEAR - p.RIDGE_CLEAR
    i = p.LATTICE_INSET + 0.05
    z0 = H - p.RIDGE_H
    outer = case._rounded_box(p.BOARD_W + 2 * o, p.BOARD_L + 2 * o, p.RIDGE_H,
                              p.BOARD_W / 2, p.BOARD_L / 2, z0 + p.RIDGE_H / 2,
                              p.CORNER_R + o)
    inner = case._rounded_box(p.BOARD_W - 2 * i, p.BOARD_L - 2 * i, p.RIDGE_H + 2,
                              p.BOARD_W / 2, p.BOARD_L / 2, z0 + p.RIDGE_H / 2,
                              p.CORNER_R - i)
    yf = front_line() - p.LATTICE_WALL / 2
    _, y3 = bg.mount_holes()["H3"]
    _, (tx0, _, _, ty1) = _d3_tube_rects()
    keep = (_box(-5, p.BOARD_W + 5, yf, p.BOARD_L + 5, z0 - 1, H + 1)
            + _box(-5, 1.0, y3, yf + 0.01, z0 - 1, H + 1)
            + _box(tx0, p.BOARD_W + 5, -5, ty1, z0 - 1, H + 1))
    return (outer - inner) & keep


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
    body = ((_grid() + _posts() + _rails() + tube) & _outline()) + _ridge()
    body = (body - _post_voids() - tube_void - _part_voids() - _keepout_void()
            - _reset_path_void())
    # The collar goes on last: SW3's model envelope (a plain 3.5 plunger to
    # 15 mm) would otherwise void it. It clears the measured plunger instead.
    return body + _collar()


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
        sides = {"west": ((x0 - w, x0, y0, y1), (r, c - 1), "x"),
                 "east": ((x1, x1 + w, y0, y1), (r, c + 1), "x"),
                 "back": ((x0, x1, y1, y1 + w), (r - 1, c), "y"),
                 "front": ((x0, x1, y0 - w, y0), (r + 1, c), "y")}
        for side, (wall, nb, across) in sides.items():
            if _opened_through(part, *wall, across):
                kind = ("lit|lit" if nb in lit else
                        "lit|test" if nb in test else "lit|dark")
                out.append(f"r{r}c{c} {side} {kind}")
    return out


def _opened_through(part: Part, x0, x1, y0, y1, across: str) -> bool:
    """Whether something cut a hole straight through this wall box (full
    height), across X or across Y. A notch that only thins the wall lets no
    light through and does not count, however big; one that goes through
    counts, however small. The cuts are boxes and hulls, so a hole is
    straight: what is missing from a thin slice at one face of the wall is
    also missing from the slice at the other face."""
    probe = _box(x0, x1, y0, y1, 0, H)
    got = part & probe
    if got is not None and got.volume >= probe.volume - 1e-4:
        return False
    missing = probe - part if got is not None else probe
    s = 0.05
    if across == "x":
        near = _box(x0, x0 + s, y0, y1, -1, H + 1)
        far = _box(x1 - s, x1, y0, y1, -1, H + 1)
        shift = Pos(-(x1 - x0 - s), 0, 0)
    else:
        near = _box(x0, x1, y0, y0 + s, -1, H + 1)
        far = _box(x0, x1, y1 - s, y1, -1, H + 1)
        shift = Pos(0, -(y1 - y0 - s), 0)
    a, b = missing & near, missing & far
    if a is None or b is None or a.volume < 1e-6 or b.volume < 1e-6:
        return False
    both = a & (shift * b)
    return both is not None and both.volume > s * 0.01  # >= 0.01 mm^2 through
