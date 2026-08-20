#!/bin/sh
# The one supported way to regenerate the panel. Close KiCad first.
set -e
cd "$(dirname "$0")"
KIPY=/Applications/KiCad/KiCad.app/Contents/Frameworks/Python.framework/Versions/3.9/bin/python3
S=$(python3 -c "print(62.0/1100)")
python3 panel_layout.py
python3 svg2poly.py svg/panel.svg "$S" "#dda857" gold.polys.json
python3 svg2poly.py svg/panel.svg "$S" "white,#ffffff" silk.polys.json
python3 svg2poly.py svg/panel.svg "$S" "#e7ba9b" fr4.polys.json
"$KIPY" emit_board.py
