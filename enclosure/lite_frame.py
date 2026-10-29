# SPDX-License-Identifier: CERN-OHL-S-2.0
"""The Lite frame: the printed part the Lite's clear acrylic top is glued to.

docs/superpowers/specs/2026-10-28-lite-acrylic-top-design.md. The Lite lid's
four legs (case.lite_legs: the same inserts, the same M2.5 x 10 from below),
stopped GLUE_LINE under CAVITY, joined by a rim round the top edge like the
lattice's lip and an arm from each leg out to the side rim, and a light tube
round the power LED D3, closed at the top but for a slit the shape of the
slash frosted over it, so D3 lights the slash and nothing else under the clear
top. The acrylic (hardware/panel/airwriter-lite-top.svg) is glued to the four
leg tops and lands on J2/J4's shells at CAVITY, as the Pro's panel does.
Printed rim-down, legs up."""
from build123d import Face, Part, Pos, Solid, Wire, extrude

import board_geom as bg
import case
import lattice
import params as p

TOP = p.CAVITY - p.GLUE_LINE
"""The frame's top face: the glue's, and the bed's when printed."""


def _rim() -> Part:
    """FRAME_RIM_H deep, from RIDGE_CLEAR short of the tray's pocket wall to
    FRAME_RIM_IN inside the board edge, all the way round."""
    o, i = p.POCKET_CLEAR - p.RIDGE_CLEAR, p.FRAME_RIM_IN
    z = TOP - p.FRAME_RIM_H / 2
    outer = case._rounded_box(p.BOARD_W + 2 * o, p.BOARD_L + 2 * o, p.FRAME_RIM_H,
                              p.BOARD_W / 2, p.BOARD_L / 2, z, p.CORNER_R + o)
    inner = case._rounded_box(p.BOARD_W - 2 * i, p.BOARD_L - 2 * i, p.FRAME_RIM_H + 2,
                              p.BOARD_W / 2, p.BOARD_L / 2, z, p.CORNER_R - i)
    return outer - inner


def _arms() -> Part:
    """From each leg's axis out to the side rim nearer it, at rim depth."""
    half = p.FRAME_ARM_W / 2
    arms = []
    for x, y in case.bg.mount_holes().values():
        x0, x1 = (0.0, x) if x < p.BOARD_W / 2 else (x, p.BOARD_W)
        arms.append(lattice._box(x0, x1, y - half, y + half, TOP - p.FRAME_RIM_H, TOP))
    return lattice._union(arms)


def slash_box() -> tuple[float, float, float, float]:
    """The Lite's frosted slash as a case-frame box (x0, x1, y0, y1)."""
    xs, ys = zip(*bg.lite_slash())
    return min(xs), max(xs), min(ys), max(ys)


def d3_tube() -> tuple[Part, Part]:
    """(tube, its void). The lattice's D3 tube round the Lite's slash, board
    to TOP, with LATTICE_WALL walls and a LATTICE_WALL cap; the void is its
    interior under the cap and a slit through the cap the slash's own shape.
    Seen through the clear top, the cap hides D3 and the slit lights the
    frost from right under it."""
    (ix0, ix1, iy0, iy1), outer = lattice._d3_tube_rects(slash_box())
    cap = p.LATTICE_WALL
    # Run on to the board edge, the tube's corner would stand square in the
    # pocket's rounded one: keep it inside the board's own outline.
    tube = lattice._box(*outer, 0, TOP) & case._rounded_box(
        p.BOARD_W, p.BOARD_L, TOP, p.BOARD_W / 2, p.BOARD_L / 2, TOP / 2, p.CORNER_R)
    hollow = lattice._box(ix0, ix1, iy0, iy1, -1, TOP - cap)
    face = Face(Wire.make_polygon([(x, y, 0) for x, y in bg.lite_slash()], close=True))
    slit = Pos(0, 0, TOP - cap - 0.5) * extrude(face, cap + 1.5, dir=(0, 0, 1))
    return tube, hollow + slit


def lite_frame() -> Solid:
    tube, tube_void = d3_tube()
    top = (_rim() + _arms() + tube) - lattice._part_voids() - tube_void
    body = top + case.lite_legs(TOP)
    # J2 and J4's hulls cut the rim along the front, and the piece between
    # them is left hanging from nothing; the acrylic rests on the shells there.
    return max(body.solids(), key=lambda s: s.volume)
