# SPDX-License-Identifier: CERN-OHL-S-2.0
"""The Lite frame: the printed part the Lite's clear acrylic top is glued to.

docs/superpowers/specs/2026-10-28-lite-acrylic-top-design.md. The Lite lid's
four legs (case.lite_legs: the same inserts, the same M2.5 x 10 from below),
stopped GLUE_LINE under CAVITY, joined by a rim round the top edge like the
lattice's lip and an arm from each leg out to the side rim. The acrylic
(hardware/panel/airwriter-lite-top.svg) is glued to the four leg tops and
lands on J2/J4's shells at CAVITY, as the Pro's panel does. Printed rim-down,
legs up."""
from build123d import Part, Solid

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


def _slash_void() -> Part:
    """The panel's power-slash window over D3, grown by D3_TUBE_CLEAR, so
    D3's light reaches the slash frosted into the acrylic."""
    x0, x1, y0, y1 = lattice.d3_window()
    c = p.D3_TUBE_CLEAR
    return lattice._box(x0 - c, x1 + c, y0 - c, y1 + c, -1, TOP + 1)


def lite_frame() -> Solid:
    top = (_rim() + _arms()) - lattice._part_voids() - _slash_void()
    body = top + case.lite_legs(TOP)
    # J2 and J4's hulls cut the rim along the front, and the piece between
    # them is left hanging from nothing; the acrylic rests on the shells there.
    return max(body.solids(), key=lambda s: s.volume)
