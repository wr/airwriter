#!/usr/bin/env python3
"""Route the board with Freerouting, start to finish, with no GUI.

    /Applications/KiCad/KiCad.app/Contents/Frameworks/Python.framework/Versions/3.9/bin/python3 \
        route.py [--keep-wiring] [--passes N]

`kicad-cli` in KiCad 10 has no Specctra DSN export and no SES import, which is why
this used to be a manual round trip through the GUI. KiCad's own `pcbnew` Python
module still has both, so run this with the interpreter inside KiCad.app.

Two things have to happen between export and Freerouting:

  Coordinates are rounded to whole units. KiCad writes fractional coordinates while
  declaring `(resolution um 10)`. Freerouting turns those into exact rationals, and
  the numerators explode in Line.intersection, so it spins in normalize_traces at
  100% CPU and never finishes loading. Rounding costs half a unit of precision.

  Existing wiring is dropped by default. Freerouting hits the same arithmetic
  blowup when it re-reads its own previous output, so it routes the whole board
  from scratch each time. Pass --keep-wiring to leave the existing traces in and
  route only what is missing -- expect it to hang if it has routed this board before.
"""
import argparse
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
BOARD = os.path.join(HERE, 'airwriter.kicad_pcb')
DSN = os.path.join(HERE, 'freerouting.dsn')
SES = os.path.join(HERE, 'freerouting.ses')
JAR = os.path.expanduser(
    '~/Documents/KiCad/10.0/3rdparty/plugins/app_freerouting_kicad-plugin/jar/freerouting-2.2.4.jar')
KICAD_CLI = '/Applications/KiCad/KiCad.app/Contents/MacOS/kicad-cli'

NUMBER = re.compile(r'(?<![\w.])-?\d+\.\d+(?![\w.])')


def scope_span(text, keyword):
    """Return the (start, end) character span of a top-level s-expression scope."""
    start = text.index('(' + keyword)
    depth, i = 0, start
    while True:
        if text[i] == '(':
            depth += 1
        elif text[i] == ')':
            depth -= 1
            if depth == 0:
                return start, i + 1
        i += 1


EDGE_INSET_UM = 300


def shrink_boundary(text):
    """Pull the board outline inward so the router cannot hug the edge.

    Freerouting routes right up to the boundary, which leaves vias a few microns
    inside KiCad's 0.25mm copper-to-edge rule. Moving every outline point toward
    the centroid keeps the router honest without touching the real Edge.Cuts.
    """
    start, end = scope_span(text, 'boundary')
    scope = text[start:end]
    m = re.search(r'\(path pcb (\d+)([\s\S]*?)\)', scope)
    if not m:
        return text
    nums = [float(v) for v in m.group(2).split()]
    xs, ys = nums[0::2], nums[1::2]
    cx, cy = sum(xs) / len(xs), sum(ys) / len(ys)
    out = []
    for x, y in zip(xs, ys):
        dx, dy = x - cx, y - cy
        d = max((dx * dx + dy * dy) ** 0.5, 1e-9)
        out += [round(x - dx / d * EDGE_INSET_UM), round(y - dy / d * EDGE_INSET_UM)]
    body = '  '.join(f'{a} {b}' for a, b in zip(out[0::2], out[1::2]))
    return text[:start] + scope[:m.start()] + f'(path pcb {m.group(1)}  {body})' + scope[m.end():] + text[end:]


def prepare_dsn(path, keep_wiring):
    text = open(path).read()
    text, rounded = NUMBER.subn(lambda m: str(round(float(m.group(0)))), text)
    text = shrink_boundary(text)
    if not keep_wiring:
        start, end = scope_span(text, 'wiring')
        text = text[:start] + '(wiring\n  )' + text[end:]
    open(path, 'w').write(text)
    return rounded


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--keep-wiring', action='store_true',
                    help='route only the missing connections instead of the whole board')
    ap.add_argument('--passes', type=int, default=25)
    args = ap.parse_args()

    try:
        import pcbnew
    except ImportError:
        sys.exit('Run this with the python3 inside KiCad.app -- see the docstring.')

    board = pcbnew.LoadBoard(BOARD)
    if not pcbnew.ExportSpecctraDSN(board, DSN):
        sys.exit('DSN export failed')
    rounded = prepare_dsn(DSN, args.keep_wiring)
    print(f'exported {os.path.basename(DSN)}, rounded {rounded} coordinates'
          f'{"" if args.keep_wiring else ", dropped existing wiring"}')

    if os.path.exists(SES):
        os.remove(SES)
    run = subprocess.run(['java', '-jar', JAR, '-de', DSN, '-do', SES,
                          '-mp', str(args.passes)],
                         capture_output=True, text=True, timeout=900)
    done = [l for l in run.stdout.splitlines() if 'session completed' in l]
    if not done:
        print('\n'.join((run.stdout + run.stderr).splitlines()[-15:]))
        sys.exit('Freerouting did not finish a routing session')
    print(done[-1].split('INFO')[-1].strip())
    if not os.path.exists(SES) or os.path.getsize(SES) < 10_000:
        sys.exit(f'Freerouting wrote no usable session file ({SES})')

    if not pcbnew.ImportSpecctraSES(board, SES):
        sys.exit('SES import failed')
    board.Save(BOARD)
    print('session imported and board saved')

    subprocess.run([KICAD_CLI, 'pcb', 'drc', '--refill-zones', '--save-board',
                    '--schematic-parity', '--severity-error',
                    '-o', os.path.join(HERE, 'drc.rpt'), BOARD],
                   capture_output=True, text=True)
    report = open(os.path.join(HERE, 'drc.rpt')).read()
    for line in report.splitlines():
        if line.startswith('** Found'):
            print(line.strip('* '))


if __name__ == '__main__':
    main()
