# SPDX-License-Identifier: CERN-OHL-S-2.0
#
# SW1's 3D model, drawn from the part's listing and drawing:
#
#   SW_BZCN_TSB001A3518A.step  BZCN TSB001A3518A side-press tactile switch
#                              (LCSC C2888448): body 7.7 x 3.08 x 3.52 mm, a
#                              3.0 x 1.5 mm button 0.71 mm proud of its front,
#                              its axis 1.9 mm above the board, and two 0.9 mm
#                              pegs 0.83 mm into the board.
#
# The enclosure's lattice and case read SW1's envelope from the board's STEP
# export. LCSC's own model is not licensed for this repo.
#
# Frame: the footprint's origin, KiCad's model axes (Y up, so the footprint's
# -y, where the button is, is +Y here). Z = 0 is the board top.
#
#   enclosure/.venv/bin/python hardware/airwriter/lib/make_sw1_model.py
from pathlib import Path

from build123d import Align, Box, BuildPart, Cylinder, Locations, export_step

OUT = Path(__file__).parent / 'airwriter.3dshapes'
MIN = (Align.MIN, Align.MIN, Align.MIN)

BODY = (-3.85, -1.48, 0.0, 3.85, 1.6, 3.52)     # x0, Y0, z0, x1, Y1, z1
BUTTON = (-1.5, 1.6, 1.15, 1.5, 2.31, 2.65)
PEG_D, PEG_DEPTH = 0.9, 0.83
PEGS = ((-1.15, 0.48), (1.15, 0.48))


def box(b):
    x0, y0, z0, x1, y1, z1 = b
    with Locations((x0, y0, z0)):
        Box(x1 - x0, y1 - y0, z1 - z0, align=MIN)


with BuildPart() as sw:
    box(BODY)
    box(BUTTON)
    with Locations(*[(x, y, -PEG_DEPTH) for x, y in PEGS]):
        Cylinder(PEG_D / 2, PEG_DEPTH, align=(Align.CENTER, Align.CENTER, Align.MIN))

export_step(sw.part, str(OUT / 'SW_BZCN_TSB001A3518A.step'))
print(OUT / 'SW_BZCN_TSB001A3518A.step')
