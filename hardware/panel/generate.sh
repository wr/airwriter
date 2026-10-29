#!/bin/sh
# The one supported way to regenerate the panel. Close KiCad first.
set -e
cd "$(dirname "$0")"
KIPY=/Applications/KiCad/KiCad.app/Contents/Frameworks/Python.framework/Versions/3.9/bin/python3
S=$(python3 -c "print(62.0/1100)")
python3 panel_layout.py
python3 svg2poly.py svg/panel.svg "$S" "#dda857" gold.polys.json
# Ink, not silkscreen: geometry decides each poly's fate downstream. A poly
# that sits fully inside a gold dot becomes a mask knockout (dark letters on
# exposed gold); everything else is real silkscreen. Frame 9b draws the test
# tag in #111111 rather than white, which is what it looks like in the
# product -- white was only ever a proxy for "knock this out". The artboard
# background is #111111 too, and svg2poly drops it by its viewbox-sized bbox.
python3 svg2poly.py svg/panel.svg "$S" "white,#ffffff,#111111" silk.polys.json
python3 svg2poly.py svg/panel.svg "$S" "#e7ba9b" fr4.polys.json
# The Lite's acrylic top takes the same art, frosted into clear acrylic.
python3 lite_top.py
python3 floors.py
"$KIPY" emit_board.py
