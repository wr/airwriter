#!/usr/bin/env python3
# SPDX-License-Identifier: CERN-OHL-S-2.0
"""Rev 2026.11: the 37 status LEDs are OPSCO's SK6812MINI-012, not the SK6812MINI-C.

    python3 hardware/airwriter/tools_sch/led_part.py

LCSC lists the SK6812MINI-C (C7423117) as discontinued. The SK6812MINI-012 (C2886570) is
the same maker's catalogue part in the same package: the same pads, chamfer corner, pin
positions, GRB order and timing by its datasheet, so the footprint, the nets and the
firmware stay as they are (docs/hardware-rev-2026-11.md).

Only the part's name changes: the Value, MPN, LCSC and Datasheet fields of D10-D46, in the
schematic and in the board's footprints, which carry the same fields. It rewrites exactly
those property values and stops unless it finds 37 of each in both files, so a second run,
or a file that has moved on, changes nothing. Close KiCad first. Then
hardware/combine_boards.py and hardware/combined/make_fab.sh rebuild the panel and the
order packages.
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
FILES = [os.path.join(HERE, '..', f) for f in ('airwriter.kicad_sch', 'airwriter.kicad_pcb')]
OLD_MPN, NEW_MPN = 'SK6812MINI-C', 'SK6812MINI-012'
OLD_LCSC, NEW_LCSC = 'C7423117', 'C2886570'
URL = 'https://www.lcsc.com/product-detail/%s.html'
SWAPS = [
    ('(property "Value" "%s"' % OLD_MPN, '(property "Value" "%s"' % NEW_MPN),
    ('(property "MPN" "%s"' % OLD_MPN, '(property "MPN" "%s"' % NEW_MPN),
    ('(property "LCSC" "%s"' % OLD_LCSC, '(property "LCSC" "%s"' % NEW_LCSC),
    ('(property "Datasheet" "%s"' % (URL % OLD_LCSC), '(property "Datasheet" "%s"' % (URL % NEW_LCSC)),
]
LEDS = 37

texts = {}
for path in FILES:
    s = open(path).read()
    counts = [s.count(old) for old, _ in SWAPS]
    if counts != [LEDS] * len(SWAPS):
        sys.exit('%s: expected %d of each LED field, found %s; nothing written'
                 % (os.path.basename(path), LEDS, counts))
    for old, new in SWAPS:
        s = s.replace(old, new)
    if OLD_MPN in s or OLD_LCSC in s:
        sys.exit('%s: the old part is named somewhere else too; nothing written'
                 % os.path.basename(path))
    texts[path] = s
for path, s in texts.items():
    with open(path, 'w') as f:
        f.write(s)
    print('%s: D10-D46 are %s (%s)' % (os.path.basename(path), NEW_MPN, NEW_LCSC))
