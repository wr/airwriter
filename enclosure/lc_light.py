# SPDX-License-Identifier: CERN-OHL-S-2.0
"""The LC's light tube: a loose printed part round D26, the LC's one status
light, under the dot below the test dot (docs/hardware-rev-2026-11.md, "The
LC's one light"; Figma Frame 17 frosts that dot on the acrylic).

Walls from the board to LC_LIGHT_PLAY under the acrylic, round D26 and its
cap C46, LC_LIGHT_CLEAR clear of their courtyards, and a FRAME_CAP cap with a
round LC_LIGHT_APERTURE_D opening centred on the dot: smaller than the dot,
so the LED lights the frost through a hole, not through its whole width
(Wells, 2026-10-29: the LEDs are bright). D26 sits mid-board, far from the
frame, so the tube is a part of its own rather than an arm across the clear
top (Wells): set over D26 before the top goes on, it stands on the board, its
walls round the parts keep it on the dot, and the acrylic traps it.
Printed cap-down."""
from build123d import Cylinder, Part, Pos

import board_geom as bg
import components
import lattice
import params as p

TOP = p.ACRYLIC_UNDERSIDE - p.LC_LIGHT_PLAY
"""The tube's top face: the cap's, and the bed's when printed."""


def dot() -> tuple[float, float]:
    """D26's dot on the acrylic (row 4, column 3, under the test dot), case frame."""
    (d,) = [d for d in bg.panel_dots() if (d["r"], d["c"]) == (4, 3)]
    return d["x"], d["y"]


def light_rects() -> tuple[list[float], list[float]]:
    """(interior, outer) as [x0, x1, y0, y1]: round D26's and C46's
    courtyards, LC_LIGHT_CLEAR clear, with LATTICE_WALL walls."""
    env = components.envelopes()
    boxes = env["D26"] + env["C46"]
    c, w = p.LC_LIGHT_CLEAR, p.LATTICE_WALL
    inner = [min(b[0] for b in boxes) - c, max(b[1] for b in boxes) + c,
             min(b[2] for b in boxes) - c, max(b[3] for b in boxes) + c]
    return inner, [inner[0] - w, inner[1] + w, inner[2] - w, inner[3] + w]


def lc_light() -> Part:
    (ix0, ix1, iy0, iy1), outer = light_rects()
    x, y = dot()
    tube = (lattice._box(*outer, 0, TOP)
            - lattice._box(ix0, ix1, iy0, iy1, -1, TOP - p.FRAME_CAP))
    return tube - Pos(x, y, TOP) * Cylinder(p.LC_LIGHT_APERTURE_D / 2, 2 * p.FRAME_CAP + 1)
