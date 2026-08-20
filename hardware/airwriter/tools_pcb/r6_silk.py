"""Round 6 step 4: re-home the silkscreen the structural moves collided.

Board-level connector labels and a handful of reference designators were placed
against the old arrangement.  Relocate each to the nearest spot whose real text
bounding box clears every pad, every silk graphic, and every other silk text.
"""
import sys, math
sys.path.insert(0, "/Users/wells/Projects/airwriter/hardware/airwriter/tools_pcb")
import pcbnew, r6_lib as L

TEXTS = ["USB-C", "LOCALTALK", "PWR", "PRINTER"]
REFS = ["U7", "U8", "U5", "SW1", "SW3", "S1", "U2", "U6", "U3", "U4"]
PAD_CLR = 0.15
SILK_CLR = 0.15

b = L.load()
fps = {f.GetReference(): f for f in b.Footprints()}

def bb(item):
    r = item.GetBoundingBox()
    return (L.mm(r.GetLeft()), L.mm(r.GetTop()), L.mm(r.GetRight()), L.mm(r.GetBottom()))

pads, silks = [], []   # silks holds (id, bbox); a mover's entry refreshes as it lands
for f in b.Footprints():
    for p in f.Pads():
        if p.IsOnLayer(pcbnew.F_Cu):
            pads.append(bb(p))
    for it in f.GraphicalItems():
        if it.GetLayer() == pcbnew.F_SilkS:
            silks.append((id(it), bb(it)))
for d in b.GetDrawings():
    if d.GetLayer() == pcbnew.F_SilkS:
        silks.append((id(d), bb(d)))

movers = []
for d in b.GetDrawings():
    if d.GetClass() in ("PCB_TEXT",) and d.GetText() in TEXTS and d.GetLayer() == pcbnew.F_SilkS:
        movers.append((d.GetText(), d, None))
for r in REFS:
    fld = fps[r].Reference()
    if fld.GetLayer() == pcbnew.F_SilkS:
        movers.append((r + ".ref", fld, fps[r]))
        silks.append((id(fld), bb(fld)))

def clash(box, me):
    for q in pads:
        if L.rect_overlap(box, q, PAD_CLR):
            return True
    for k, q in silks:
        if k == me:
            continue
        if L.rect_overlap(box, q, SILK_CLR):
            return True
    return not (100.3 <= box[0] and box[2] <= 161.7 and 60.3 <= box[1] and box[3] <= 163.97)

for name, item, fp in movers:
    p0 = item.GetPosition()
    x0, y0 = L.mm(p0.x), L.mm(p0.y)
    if not clash(bb(item), id(item)):
        print("  %-12s already clear at (%.2f, %.2f)" % (name, x0, y0))
        continue
    best = None
    for r in [i * 0.25 for i in range(1, 61)]:
        for a in range(0, 360, 10):
            x = x0 + r * math.cos(math.radians(a))
            y = y0 + r * math.sin(math.radians(a))
            item.SetPosition(pcbnew.VECTOR2I(L.nm(x), L.nm(y)))
            if not clash(bb(item), id(item)):
                best = (x, y, r); break
        if best: break
    if best:
        x, y, r = best
        item.SetPosition(pcbnew.VECTOR2I(L.nm(x), L.nm(y)))
        silks[:] = [(k, q) for k, q in silks if k != id(item)]
        silks.append((id(item), bb(item)))
        print("  %-12s (%.2f, %.2f) -> (%.2f, %.2f)  moved %.2fmm" % (name, x0, y0, x, y, r))
    else:
        item.SetPosition(p0)
        print("  %-12s NO CLEAR SPOT within 15mm -- left at (%.2f, %.2f)" % (name, x0, y0))

pcbnew.SaveBoard(L.BOARD, b)
print("saved")
