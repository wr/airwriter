#!/bin/sh
# Rev 2026.11: rebuild the board's pre-routing state from the schematic.
#
#   sh hardware/airwriter/tools_pcb/r13_build.sh
#
# NOT AFTER ROUTING: it starts from rev A's copper (the hw-rev-a tag) and
# throws away every trace drawn since. Once Wells has routed the board, change
# placement by hand in KiCad, not here. Close KiCad first.
#
# Steps: rev A's board, then r13_sync.py (follow the schematic), r13_place.py
# (place parts, D3, TP9, silk, refill), r13_clean.py (remove copper the
# changes made wrong; refuses to cut any net the design didn't change),
# r13_place.py again so the pours are refilled round the cleaned copper, and
# r13_reroute.py (the rev A copper and parts moved to make room, U3's channel
# swap). Then r13_route.py draws the new traces.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
BOARD_DIR=$(cd "$HERE/.." && pwd)
ROOT=$(cd "$BOARD_DIR/../.." && pwd)
KP=/Applications/KiCad/KiCad.app/Contents/Frameworks/Python.framework/Versions/3.9/bin/python3
K=/Applications/KiCad/KiCad.app/Contents/MacOS/kicad-cli
NET=$(mktemp -t r13net).xml
quiet() { grep -v -i -E 'fontconfig|wxapp|image handler|memory leak' || true; }
# run a step through quiet, and stop if the step itself failed (sh has no pipefail)
step() { LOG=$(mktemp -t r13step); "$@" > "$LOG" 2>&1 && rc=0 || rc=$?; quiet < "$LOG"; rm -f "$LOG"; [ $rc -eq 0 ] || exit $rc; }

git -C "$ROOT" show hw-rev-a:hardware/airwriter/airwriter.kicad_pcb > "$BOARD_DIR/airwriter.kicad_pcb"
"$K" sch export netlist --format kicadxml -o "$NET" "$BOARD_DIR/airwriter.kicad_sch" >/dev/null 2>&1
python3 "$BOARD_DIR/tools_sch/check_rev2026_11.py" "$NET"
step "$KP" "$HERE/r13_sync.py" "$NET"
step "$KP" "$HERE/r13_place.py"
step "$KP" "$HERE/r13_clean.py"
step "$KP" "$HERE/r13_place.py"
step "$KP" "$HERE/r13_reroute.py"
rm -f "$NET"
