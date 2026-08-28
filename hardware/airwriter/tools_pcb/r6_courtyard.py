"""Rewrite U1's F.CrtYd as the module's real physical outline.

The stock ESP32-S3-WROOM-1 courtyard fuses two different ideas into one L: the
module's physical body, and a 48 x 21mm RF clearance region.  With the module
flush to the board edge that RF rectangle lands on J1 and J4 and reads as a
courtyard collision, which it is not -- nothing physical is in conflict there.

So this round splits them: F.CrtYd becomes the module's real footprint (18mm
body + 0.5mm margin over its pad rows, full 25.5mm length out to the flush
antenna tip), and the RF clearance stays where it belongs, in the "WROOM ANTENNA
KEEPOUT" rule area, which bars copper rather than parts.

Local coords; U1 is rot -90, so local -y is the antenna direction (world +x).
"""
import sys, re
sys.path.insert(0, "/Users/wells/Projects/airwriter/hardware/airwriter/tools_pcb")
from r6_sexp import top_blocks, block_end

BOARD = "/Users/wells/Projects/airwriter/hardware/airwriter/airwriter.kicad_pcb"

HALF_W = 9.5      # 18.0mm module + 0.5mm; 0.3mm clear of the pad rows at +-8.75+-0.45
Y_TIP, Y_TAIL = -12.75, 13.45     # antenna tip .. past the far pad row
LOOP = [(-HALF_W, Y_TIP), (HALF_W, Y_TIP), (HALF_W, Y_TAIL), (-HALF_W, Y_TAIL)]

def fmt(v):
    return "%g" % round(v, 6)

def main():
    s = open(BOARD).read()
    for st, en, blk in top_blocks(s, "footprint"):
        m = re.search(r'\(property "Reference" "([^"]*)"', blk)
        if not m or m.group(1) != "U1":
            continue
        spans, uuids = [], []
        for mm in re.finditer(r"\(fp_line", blk):
            e = block_end(blk, mm.start())
            if '(layer "F.CrtYd")' in blk[mm.start():e]:
                spans.append((mm.start(), e))
                uuids.append(re.search(r'\(uuid "([^"]*)"\)', blk[mm.start():e]).group(1))
        assert len(spans) in (4, 8), "unexpected courtyard segment count %d" % len(spans)
        segs = [(LOOP[i], LOOP[(i + 1) % 4]) for i in range(4)]
        reps = []
        for i, (a, b) in enumerate(segs):
            reps.append((spans[i], '(fp_line\n\t\t\t(start %s %s)\n\t\t\t(end %s %s)\n\t\t\t(stroke\n\t\t\t\t'
                         '(width 0.05)\n\t\t\t\t(type solid)\n\t\t\t)\n\t\t\t(layer "F.CrtYd")\n\t\t\t'
                         '(uuid "%s")\n\t\t)' % (fmt(a[0]), fmt(a[1]), fmt(b[0]), fmt(b[1]), uuids[i])))
        for sp in spans[4:]:
            reps.append((sp, ""))
        out = blk
        for (a, b), rep in sorted(reps, key=lambda t: -t[0][0]):
            tail = out[b:]
            if rep == "" and tail[:3] == "\n\t\t":
                b += 3                        # swallow the now-orphan indent
            out = out[:a] + rep + out[b:]
        s = s[:st] + out + s[en:]
        open(BOARD, "w").write(s)
        print("U1 F.CrtYd -> rectangle local x +-%g, y %g..%g (4 segments)" % (HALF_W, Y_TIP, Y_TAIL))
        return
    raise SystemExit("U1 not found")

main()
