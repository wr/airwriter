# SPDX-License-Identifier: CERN-OHL-S-2.0
"""The LC's light tube: a loose printed part round D26, the LC's one status
light, under the dot below the test dot (docs/hardware-rev-2026-11.md, "The
LC's one light"; Figma Frame 17 frosts that dot on the acrylic), and round the
test switch SW3 above it.

Two chambers in one block, from the board to LC_LIGHT_PLAY under the acrylic:

- D26's: round D26 and its cap C46, LC_LIGHT_CLEAR clear of their courtyards,
  closed by a FRAME_CAP cap with a round LC_LIGHT_APERTURE_D opening centred
  on the dot: smaller than the dot, so the LED lights the frost through a
  hole, not through its whole width (Wells, 2026-10-29: the LEDs are bright).
- SW3's: a sleeve LC_SWITCH_CLEAR round the switch's body, and the lattice's
  collar (COLLAR_BORE_D, COLLAR_H) round its plunger at the top. The switch
  holds the part on its dots, and the part braces the tall switch (Wells:
  "this will also help stabilize the switch and center the part").

D26 sits mid-board, far from the frame, so this is a part of its own rather
than an arm across the clear top (Wells): set over D26 and SW3 before the top
goes on, it stands on the board and the acrylic traps it. Printed cap-down."""
from build123d import Cylinder, Part, Pos

import board_geom as bg
import components
import lattice
import params as p

TOP = p.ACRYLIC_UNDERSIDE - p.LC_LIGHT_PLAY
"""The tube's top face: the caps', and the bed's when printed."""


def dot() -> tuple[float, float]:
    """D26's dot on the acrylic (row 4, column 3, under the test dot), case frame."""
    (d,) = [d for d in bg.panel_dots() if (d["r"], d["c"]) == (4, 3)]
    return d["x"], d["y"]


def light_rects() -> tuple[list[float], list[float]]:
    """(interior, outer) of D26's chamber as [x0, x1, y0, y1]: round D26's and
    C46's courtyards, LC_LIGHT_CLEAR clear, with LATTICE_WALL walls."""
    env = components.envelopes()
    boxes = env["D26"] + env["C46"]
    c, w = p.LC_LIGHT_CLEAR, p.LATTICE_WALL
    inner = [min(b[0] for b in boxes) - c, max(b[1] for b in boxes) + c,
             min(b[2] for b in boxes) - c, max(b[3] for b in boxes) + c]
    return inner, [inner[0] - w, inner[1] + w, inner[2] - w, inner[3] + w]


def switch_body() -> list[tuple[float, float, float, float, float]]:
    """SW3's body bands (x0, x1, y0, y1, top), its legs' included: the
    envelope's bands wider than the plunger, BAND tall each."""
    return [b for b in components.envelopes()["SW3"] if b[1] - b[0] > 4.0]


def switch_axis() -> tuple[float, float]:
    """The middle of SW3's body, which the sleeve, and so the collar, centre on.
    The part sits wherever the switch does, so the plunger's own offset on the
    board (TEST_PLUNGER_OFFSET, rev A) moves the collar with it."""
    x0, x1, y0, y1, _ = switch_body()[-1]
    return (x0 + x1) / 2, (y0 + y1) / 2


def sleeve() -> Part:
    """SW3's chamber: each body band LC_SWITCH_CLEAR out, then the body's top
    band carried up to the collar."""
    c, body = p.LC_SWITCH_CLEAR, switch_body()
    boxes = [lattice._box(x0 - c, x1 + c, y0 - c, y1 + c, -1 if i == 0 else top - components.BAND, top)
             for i, (x0, x1, y0, y1, top) in enumerate(body)]
    x0, x1, y0, y1, top = body[-1]
    boxes.append(lattice._box(x0 - c, x1 + c, y0 - c, y1 + c, top - components.BAND, TOP - p.COLLAR_H))
    return lattice._union(boxes)


def outer() -> list[float]:
    """The block round both chambers, LATTICE_WALL outside each, [x0, x1, y0, y1]."""
    (ix0, ix1, iy0, iy1), _ = light_rects()
    sb = sleeve().bounding_box()
    w = p.LATTICE_WALL
    return [min(ix0, sb.min.X) - w, max(ix1, sb.max.X) + w,
            min(iy0, sb.min.Y) - w, max(iy1, sb.max.Y) + w]


def lc_light() -> Part:
    (ix0, ix1, iy0, iy1), _ = light_rects()
    x, y = dot()
    sx, sy = switch_axis()
    block = lattice._box(*outer(), 0, TOP)
    d26 = lattice._box(ix0, ix1, iy0, iy1, -1, TOP - p.FRAME_CAP)
    aperture = Pos(x, y, TOP) * Cylinder(p.LC_LIGHT_APERTURE_D / 2, 2 * p.FRAME_CAP + 1)
    collar = Pos(sx, sy, TOP) * Cylinder(p.COLLAR_BORE_D / 2, 2 * p.COLLAR_H + 1)
    return block - d26 - aperture - sleeve() - collar
