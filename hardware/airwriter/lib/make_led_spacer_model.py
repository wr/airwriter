# SPDX-License-Identifier: CERN-OHL-S-2.0
#
# D3's spacer, drawn from the HIWA LED4*8 listing (LCSC C92326): a nylon tube
# 4.0 mm across, 2.8 mm bore, 8.0 mm tall, that stands a 3 mm LED's flange
# 8 mm off the board so its lens sits about 1.2 mm under the top case. The
# enclosure's lattice reads D3's envelope from the board's STEP export, so the
# tube it cuts around D3 has to see the spacer as well as the LED.
#
# Frame: centred on the LED's axis, Z up from the board (0 = board top).
#
#   enclosure/.venv/bin/python hardware/airwriter/lib/make_led_spacer_model.py
from pathlib import Path

from build123d import BuildPart, Cylinder, Mode, Align, export_step

OD, ID, H = 4.0, 2.8, 8.0

with BuildPart() as spacer:
    Cylinder(OD / 2, H, align=(Align.CENTER, Align.CENTER, Align.MIN))
    Cylinder(ID / 2, H, align=(Align.CENTER, Align.CENTER, Align.MIN), mode=Mode.SUBTRACT)

out = Path(__file__).parent / 'airwriter.3dshapes' / 'LED_Spacer_D4.0mm_H8mm.step'
export_step(spacer.part, str(out))
print(out)
