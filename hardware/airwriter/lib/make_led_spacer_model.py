# SPDX-License-Identifier: CERN-OHL-S-2.0
#
# D3's two 3D models, drawn from the parts' listings:
#
#   LED_Spacer_D4.0mm_H8mm.step  HIWA LED4*8 (LCSC C92326): a nylon tube 4.0 mm
#                                across, 2.8 mm bore, 8.0 mm tall.
#   LED_D3.0mm_FlatTop.step      TOGIALED TJ-L3FPTHBGMCGSFLC2G-A5 (LCSC C20613324):
#                                3 mm flat-top LED, 3.85 mm tall with its 3.8 mm
#                                flange, two 0.5 mm leads 2.54 apart.
#
# The spacer stands the LED's flange 8 mm off the board, so its face sits about
# 1.2 mm under the top case. KiCad's own LED_D3.0mm model is a 5 mm dome on 3 mm
# of leg; lifted 8 mm it would reach 16.3 mm, through the top case. The
# enclosure's lattice reads D3's envelope from the board's STEP export, so the
# tube it cuts round D3 has to see these parts as they are.
#
# Frame: centred on the LED's axis. The spacer's Z = 0 is the board top; the
# LED's Z = 0 is the underside of its flange (the footprint lifts it 8 mm).
#
#   enclosure/.venv/bin/python hardware/airwriter/lib/make_led_spacer_model.py
from pathlib import Path

from build123d import Align, Box, BuildPart, Cylinder, Locations, Mode, export_step

OUT = Path(__file__).parent / 'airwriter.3dshapes'
MIN = (Align.CENTER, Align.CENTER, Align.MIN)

SPACER_OD, SPACER_ID, SPACER_H = 4.0, 2.8, 8.0
FLANGE_D, FLANGE_H = 3.8, 1.0
BODY_D, LED_H = 3.0, 3.85
LEAD, PITCH = 0.5, 2.54

with BuildPart() as spacer:
    Cylinder(SPACER_OD / 2, SPACER_H, align=MIN)
    Cylinder(SPACER_ID / 2, SPACER_H, align=MIN, mode=Mode.SUBTRACT)

with BuildPart() as led:
    Cylinder(FLANGE_D / 2, FLANGE_H, align=MIN)
    with Locations((0, 0, FLANGE_H)):
        Cylinder(BODY_D / 2, LED_H - FLANGE_H, align=MIN)
    # the leads, down through the spacer's bore and the board
    with Locations((-PITCH / 2, 0, -SPACER_H - 1.5), (PITCH / 2, 0, -SPACER_H - 1.5)):
        Box(LEAD, LEAD, SPACER_H + 1.5, align=MIN)

for part, name in ((spacer, 'LED_Spacer_D4.0mm_H8mm.step'), (led, 'LED_D3.0mm_FlatTop.step')):
    export_step(part.part, str(OUT / name))
    print(OUT / name)
