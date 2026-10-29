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
# Assembly notes, the panel's standoff line (the panel has no schematic), and
# the two variants: AirWriter (LED array, no Lite strap) and AirWriter Lite (no
# LED array, U9 or LED caps; the Lite strap R33 fitted). The schematic carries
# every part; the lists here say which each variant leaves off.
python3 - "$OUT" <<'PY'
import csv, os, re, sys
out = sys.argv[1]
BOM, POS = os.path.join(out, 'combined-assembly-bom.csv'), os.path.join(out, 'combined-pos.csv')
NOTES = {
  'J2': 'ASSEMBLE - Kycon KMDGX-8S-BS, source from Mouser (none at LCSC); through-hole, 11 joints',
  'J4': 'DO NOT ASSEMBLE - fitted by hand with the TashTalk add-on (Kycon KMDGX-8S-BS)',
  'U7': 'Fit DIP-8 SOCKET only (LCSC C72124); PIC12F1840 is user-programmed and inserted later',
}
def expand(spec):
    out = []
    for part in spec.split(','):
        m = re.fullmatch(r'([A-Z]+)(\d+)-([A-Z]+)?(\d+)', part)
        if m:
            out += [f'{m.group(1)}{n}' for n in range(int(m.group(2)), int(m.group(4)) + 1)]
        elif part:
            out.append(part)
    return out
AIRWRITER_OFF = ['R33']                                   # the Lite strap
LITE_OFF = expand('D10-D46,C30-C66') + ['C29', 'U9', 'R20', 'C21']   # LEDs, their caps, U9 and its cap, the LED bulk cap

bom = list(csv.reader(open(BOM)))
for r in bom[1:]:
    if r[0] in NOTES: r[5] = NOTES[r[0]]
    if r[5].startswith('Lite only'): r[5] = ''   # R33's schematic note: the lists below say it
bom.append(['MT1,MT2,MT3,MT4', '9774030151R (Wurth WA-SMSI M2.5x3 SMT spacer)',
            'panel:SMT_Standoff_M2.5_D6.0mm', '', '4',
            'Source from Digi-Key (48k stock, ~$0.60); panel is art-face-down in this production panel so these reflow on the SAME side as all other parts; ring paste aperture in stencil data'])
bom.append(['D3 (spacer)', 'LED4*8 nylon LED spacer, 4 mm x 8 mm', '-', 'C92326', '1',
            'Under D3: LED leads through the spacer bore, LED flange seated on it, lens up'])
pos = list(csv.reader(open(POS)))
on_board = {r[0] for r in pos[1:]}
missing = [r for r in AIRWRITER_OFF + LITE_OFF if r not in on_board]
assert not missing, f'variant lists name parts the board does not place: {missing}'

def variant(off, bom_path, pos_path):
    rows = [bom[0]]
    for r in bom[1:]:
        refs = expand(r[0]) if re.fullmatch(r'[A-Z0-9,-]+', r[0]) else [r[0]]
        keep = [x for x in refs if x not in off]
        if not keep:
            continue
        if len(keep) != len(refs):
            r = [','.join(keep), r[1], r[2], r[3], str(len(keep)), r[5]]
        rows.append(r)
    with open(bom_path, 'w', newline='') as f:
        csv.writer(f, lineterminator='\r\n').writerows(rows)
    prows = [pos[0]] + [r for r in pos[1:] if r[0] not in off]
    with open(pos_path, 'w', newline='') as f:
        csv.writer(f,
                   lineterminator='\n').writerows(prows)
    print(f'{os.path.basename(pos_path)}: {len(prows) - 1} placements; '
          f'{os.path.basename(bom_path)}: {len(rows) - 1} lines')

variant(AIRWRITER_OFF, os.path.join(out, 'combined-assembly-bom.csv'), os.path.join(out, 'combined-pos.csv'))
variant(LITE_OFF, os.path.join(out, 'combined-lite-assembly-bom.csv'), os.path.join(out, 'combined-lite-pos.csv'))
PY
# The drill floor: nothing under 0.30 mm, PCBWay's standard tier.
python3 - "$OUT" <<'PY'
import glob, re, sys
sizes = [float(m) for f in glob.glob(sys.argv[1] + '/combined-*.drl')
         for m in re.findall(r'^T\d+C([\d.]+)', open(f).read(), re.M)]
assert sizes and min(sizes) >= 0.30 - 1e-9, f'drill under the 0.30 floor: {min(sizes)}'
print(f'drill floor {min(sizes):.3f} mm')
PY
if [ "$OUT" = "$HERE/fab" ]; then
  rm -f "$ROOT/hardware/combined-fab.zip"
  (cd "$OUT" && zip -q -X "$ROOT/hardware/combined-fab.zip" combined-*)
fi
echo "fab package written to $OUT"
