#!/bin/sh
# Regenerate both PCBWay order packages for rev 2026.11.
#   sh hardware/combined/make_fab.sh [OUT_DIR [LC_OUT_DIR]]
# AirWriter: the combined file (main board + art panel); combine_boards.py must
# already have been run. Gerbers, drills, position file and assembly BOM (from
# the main-board schematic) into OUT_DIR (default hardware/combined/fab/),
# zipped to hardware/combined-fab.zip.
# AirWriter LC: the main board alone, no LED array, no cover. The same files,
# named airwriter-lc-*, into LC_OUT_DIR (default hardware/combined/fab-lc/),
# zipped to hardware/airwriter-lc-fab.zip.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
K=/Applications/KiCad/KiCad.app/Contents/MacOS/kicad-cli
OUT=${1:-$HERE/fab}
LC_OUT=${2:-$HERE/fab-lc}
PCB=$HERE/combined.kicad_pcb
MAIN=$ROOT/hardware/airwriter/airwriter
SCH=$MAIN.kicad_sch
TMP=$(mktemp -d -t make_fab)
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$OUT" "$LC_OUT"
rm -f "$OUT"/combined-* "$LC_OUT"/airwriter-lc-*
"$K" pcb export gerbers -o "$OUT/" "$PCB"
"$K" pcb export drill --excellon-separate-th --generate-map --map-format gerberx2 -o "$OUT/" "$PCB"
"$K" pcb export pos --format csv --units mm --exclude-dnp -o "$OUT/combined-pos.csv" "$PCB"
# The LC's files carry its name: export from a copy of the main board named so.
cp "$MAIN.kicad_pcb" "$TMP/airwriter-lc.kicad_pcb"
cp "$MAIN.kicad_pro" "$TMP/airwriter-lc.kicad_pro"
"$K" pcb export gerbers -o "$LC_OUT/" "$TMP/airwriter-lc.kicad_pcb"
"$K" pcb export drill --excellon-separate-th --generate-map --map-format gerberx2 -o "$LC_OUT/" "$TMP/airwriter-lc.kicad_pcb"
"$K" pcb export pos --format csv --units mm --exclude-dnp -o "$LC_OUT/airwriter-lc-pos.csv" "$TMP/airwriter-lc.kicad_pcb"
"$K" sch export bom --fields "Reference,Value,Footprint,LCSC,\${QUANTITY},Note" \
  --labels "Designator,Comment,Footprint,LCSC,Qty,Note" \
  --group-by "Value,Footprint,LCSC,DNP" --exclude-dnp \
  -o "$TMP/bom.csv" "$SCH"
# Assembly notes and the two variants: AirWriter (LED array and the cover's
# standoffs, no LC strap) and AirWriter LC (no LED array, U9 or LED caps; the LC
# strap R33 fitted). The schematic carries every part; the lists here say which
# each variant leaves off. The cover has no schematic, so its standoff line is
# added here, on AirWriter only.
python3 - "$TMP/bom.csv" "$OUT" "$LC_OUT" <<'PY'
import csv, os, re, sys
bom_path, out, lc_out = sys.argv[1:]
POS, LC_POS = os.path.join(out, 'combined-pos.csv'), os.path.join(lc_out, 'airwriter-lc-pos.csv')
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
AIRWRITER_OFF = ['R33']                                   # the LC strap
LC_OFF = expand('D10-D46,C30-C66') + ['C29', 'U9', 'R20', 'C21']   # LEDs, their caps, U9 and its cap, the LED bulk cap
STANDOFFS = ['MT1,MT2,MT3,MT4', '9774030151R (Wurth WA-SMSI M2.5x3 SMT spacer)',
             'panel:SMT_Standoff_M2.5_D6.0mm', '', '4',
             'Source from Digi-Key (48k stock, ~$0.60); panel is art-face-down in this production panel so these reflow on the SAME side as all other parts; ring paste aperture in stencil data']
SPACER = ['D3 (spacer)', 'LED4*8 nylon LED spacer, 4 mm x 8 mm', '-', 'C92326', '1',
          'Under D3: LED leads through the spacer bore, LED flange seated on it, lens up']
PLACEMENTS = {'AirWriter': 136, 'AirWriter LC': 55}

bom = list(csv.reader(open(bom_path)))
for r in bom[1:]:
    if r[0] in NOTES: r[5] = NOTES[r[0]]
    if r[5].startswith('Lite only'): r[5] = ''   # R33's schematic note: the lists here say it
pos = list(csv.reader(open(POS)))
lc_pos = list(csv.reader(open(LC_POS)))
on_board = {r[0] for r in lc_pos[1:]}
missing = [r for r in AIRWRITER_OFF + LC_OFF if r not in on_board]
assert not missing, f'variant lists name parts the board does not place: {missing}'
assert not on_board & set(expand('MT1-MT4')), 'the main board alone places the cover standoffs'

def variant(name, off, extra, pos, bom_out, pos_out):
    rows = [bom[0]]
    for r in bom[1:]:
        refs = expand(r[0]) if re.fullmatch(r'[A-Z0-9,-]+', r[0]) else [r[0]]
        keep = [x for x in refs if x not in off]
        if not keep:
            continue
        if len(keep) != len(refs):
            r = [','.join(keep), r[1], r[2], r[3], str(len(keep)), r[5]]
        rows.append(r)
    rows += extra
    with open(bom_out, 'w', newline='') as f:
        csv.writer(f, lineterminator='\r\n').writerows(rows)
    prows = [pos[0]] + [r for r in pos[1:] if r[0] not in off]
    with open(pos_out, 'w', newline='') as f:
        csv.writer(f, lineterminator='\n').writerows(prows)
    n = len(prows) - 1
    print(f'{name}: {os.path.basename(pos_out)} {n} placements, '
          f'{os.path.basename(bom_out)} {len(rows) - 1} lines')
    assert n == PLACEMENTS[name], f'{name} places {n} parts, not {PLACEMENTS[name]}'

variant('AirWriter', AIRWRITER_OFF, [STANDOFFS, SPACER], pos,
        os.path.join(out, 'combined-assembly-bom.csv'), POS)
variant('AirWriter LC', LC_OFF, [SPACER], lc_pos,
        os.path.join(lc_out, 'airwriter-lc-assembly-bom.csv'), LC_POS)
PY
# The drill floor: nothing under 0.30 mm, PCBWay's standard tier.
python3 - "$OUT" "$LC_OUT" <<'PY'
import glob, re, sys
for d in sys.argv[1:]:
    sizes = [float(m) for f in glob.glob(d + '/*.drl')
             for m in re.findall(r'^T\d+C([\d.]+)', open(f).read(), re.M)]
    assert sizes and min(sizes) >= 0.30 - 1e-9, f'drill under the 0.30 floor in {d}: {min(sizes)}'
    print(f'{d.rstrip("/").split("/")[-1]}: drill floor {min(sizes):.3f} mm')
PY
if [ "$OUT" = "$HERE/fab" ]; then
  rm -f "$ROOT/hardware/combined-fab.zip"
  (cd "$OUT" && zip -q -X "$ROOT/hardware/combined-fab.zip" combined-*)
fi
if [ "$LC_OUT" = "$HERE/fab-lc" ]; then
  rm -f "$ROOT/hardware/airwriter-lc-fab.zip"
  (cd "$LC_OUT" && zip -q -X "$ROOT/hardware/airwriter-lc-fab.zip" airwriter-lc-*)
fi
echo "fab packages written to $OUT and $LC_OUT"
