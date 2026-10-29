# SPDX-License-Identifier: CERN-OHL-S-2.0
"""The Lite frame: the printed part the Lite's clear acrylic top is glued to.

docs/superpowers/specs/2026-10-28-lite-acrylic-top-design.md. The Lite lid's
four legs (case.lite_legs: the same inserts, the same M2.5 x 10 from below),
stopped GLUE_LINE under the acrylic, joined by a rim round the top edge like the
lattice's lip and an arm from each leg out to the side rim, and a light tube
round the power LED D3, closed at the top but for a slit the shape of the
slash frosted over it, so D3 lights the slash and nothing else under the clear
top. The acrylic (hardware/panel/airwriter-lite-top.svg) is glued to the four
leg tops, its face ACRYLIC_FACE_UNDER under the tray's rim.
Printed rim-down, legs up."""
from build123d import Face, Part, Pos, Solid, Wire, extrude

import board_geom as bg
import case
import components
import lattice
import params as p

TOP = p.ACRYLIC_UNDERSIDE - p.GLUE_LINE
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


def tube_rects() -> tuple[list[float], list[float]]:
    """(interior, outer) of D3's tube as [x0, x1, y0, y1]: round the slash and
    D3's envelope (rev 2026.11: a 3 mm LED on a 4 mm spacer), D3_TUBE_CLEAR
    clear, with LATTICE_WALL walls. A side within 1 mm of the board edge runs
    on to the rim's outer line instead, and keeps FRAME_TUBE_MIN_WALL of wall,
    closing in on D3 to do so if it must."""
    x0, x1, y0, y1 = slash_box()
    for bx0, bx1, by0, by1, _ in components.envelopes()["D3"]:
        x0, x1, y0, y1 = min(x0, bx0), max(x1, bx1), min(y0, by0), max(y1, by1)
    c, w, o = p.D3_TUBE_CLEAR, p.LATTICE_WALL, p.POCKET_CLEAR - p.RIDGE_CLEAR
    inner = [x0 - c, x1 + c, y0 - c, y1 + c]
    outer = [inner[0] - w, inner[1] + w, inner[2] - w, inner[3] + w]
    for k, edge, out in ((0, -o, -1), (1, p.BOARD_W + o, 1),
                         (2, -o, -1), (3, p.BOARD_L + o, 1)):
        if out * (outer[k] - edge) > -1.0:
            outer[k] = edge
            inner[k] = edge - out * max(p.FRAME_TUBE_MIN_WALL, out * (edge - inner[k]))
    return inner, outer


def _rim_outline(inset: float, z0: float, z1: float) -> Part:
    """The rim's outer outline, rounded at the corners, `inset` inside it."""
    o = p.POCKET_CLEAR - p.RIDGE_CLEAR - inset
    return case._rounded_box(p.BOARD_W + 2 * o, p.BOARD_L + 2 * o, z1 - z0,
                             p.BOARD_W / 2, p.BOARD_L / 2, (z0 + z1) / 2, p.CORNER_R + o)


def tube_hollow(grow: float = 0.0) -> Part:
    """D3's tube's inside, under the cap, grown by `grow`: its interior box,
    its corner rounded FRAME_TUBE_MIN_WALL inside the rim's, so the wall keeps
    its thickness round the board's corner too."""
    ix0, ix1, iy0, iy1 = tube_rects()[0]
    z1 = TOP - p.FRAME_CAP
    return (lattice._box(ix0 - grow, ix1 + grow, iy0 - grow, iy1 + grow, -1, z1)
            & _rim_outline(p.FRAME_TUBE_MIN_WALL - grow, -1, z1))


def d3_tube() -> tuple[Part, Part]:
    """(tube, its void). Walls from the board to TOP round the Lite's slash
    and D3, and a FRAME_CAP cap; the void is the tube's inside and a slit
    through the cap the slash's own shape. Seen through the clear top, the cap
    hides D3 and the slit lights the frost from right under it."""
    tube = lattice._box(*tube_rects()[1], 0, TOP) & _rim_outline(0.0, 0, TOP)
    face = Face(Wire.make_polygon([(x, y, 0) for x, y in bg.lite_slash()], close=True))
    slit = Pos(0, 0, TOP - p.FRAME_CAP - 0.5) * extrude(face, p.FRAME_CAP + 1.5, dir=(0, 0, 1))
    return tube, tube_hollow() + slit


def lite_frame() -> Solid:
    tube, tube_void = d3_tube()
    # D3 stands inside its tube, which clears it; its grown shadow would cut
    # the tube's east wall, one line from the board edge, clean away.
    top = (_rim() + _arms() + tube) - lattice._part_voids(skip=("D3",)) - tube_void
    body = top + case.lite_legs(TOP)
    # J2 and J4's hulls cut the rim along the front, and the piece between
    # them is left hanging from nothing; the acrylic spans it, over the shells.
    return max(body.solids(), key=lambda s: s.volume)
