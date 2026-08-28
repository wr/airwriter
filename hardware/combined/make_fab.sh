#!/bin/sh
# Regenerate the PCBWay order package for the combined (main board + art panel) file.
#   sh hardware/combined/make_fab.sh [OUT_DIR]
# Steps: combine_boards.py must already have been run. Emits gerbers, drills,
# position file, assembly BOM (from the main-board schematic) into OUT_DIR
# (default hardware/combined/fab/) and zips them to hardware/combined-fab.zip.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
K=/Applications/KiCad/KiCad.app/Contents/MacOS/kicad-cli
OUT=${1:-$HERE/fab}
PCB=$HERE/combined.kicad_pcb
SCH=$ROOT/hardware/airwriter/airwriter.kicad_sch
mkdir -p "$OUT"
rm -f "$OUT"/combined-*
"$K" pcb export gerbers -o "$OUT/" "$PCB"
"$K" pcb export drill --excellon-separate-th --generate-map --map-format gerberx2 -o "$OUT/" "$PCB"
"$K" pcb export pos --format csv --units mm --exclude-dnp -o "$OUT/combined-pos.csv" "$PCB"
"$K" sch export bom --fields "Reference,Value,Footprint,LCSC,\${QUANTITY},Note" \
  --labels "Designator,Comment,Footprint,LCSC,Qty,Note" \
  --group-by "Value,Footprint,LCSC,DNP" --exclude-dnp \
  -o "$OUT/combined-assembly-bom.csv" "$SCH"
# Assembly notes and the panel's standoff line (the panel has no schematic).
python3 - "$OUT/combined-assembly-bom.csv" <<'PY'
import csv, sys
p = sys.argv[1]
rows = list(csv.reader(open(p)))
NOTES = {
  'J2': 'DO NOT ASSEMBLE - hand-soldered after delivery (Kycon KMDGX-8S-BS, user-supplied)',
  'J4': 'DO NOT ASSEMBLE - hand-soldered after delivery (Kycon KMDGX-8S-BS, user-supplied)',
  'U7': 'Fit DIP-8 SOCKET only (LCSC C72124); PIC12F1840 is user-programmed and inserted later',
}
for r in rows[1:]:
    if r[0] in NOTES: r[5] = NOTES[r[0]]
rows.append(['MT1,MT2,MT3,MT4', '9774030151R (Wurth WA-SMSI M2.5x3 SMT spacer)',
             'panel:SMT_Standoff_M2.5_D6.0mm', '', '4',
             'Source from Digi-Key (48k stock, ~$0.60); panel is art-face-down in this production panel so these reflow on the SAME side as all other parts; ring paste aperture in stencil data'])
with open(p, 'w', newline='') as f:
    csv.writer(f, lineterminator='\r\n').writerows(rows)
PY
if [ "$OUT" = "$HERE/fab" ]; then
  rm -f "$ROOT/hardware/combined-fab.zip"
  (cd "$OUT" && zip -q -X "$ROOT/hardware/combined-fab.zip" combined-*)
fi
echo "fab package written to $OUT"
