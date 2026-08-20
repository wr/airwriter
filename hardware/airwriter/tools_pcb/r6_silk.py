"""Round 6 step 4: re-home the silkscreen the structural moves collided.

Board-level connector labels and a handful of reference designators were placed
against the old arrangement.  Relocate each to the nearest spot whose real text
bounding box clears every pad, every silk graphic, and every other silk text.
"""
import sys, math
sys.path.insert(0, "/Users/wells/Projects/airwriter/hardware/airwriter/tools_pcb")
import pcbnew, r6_lib as L

TEXTS = ["USB-C", "LOCALTALK", "PWR", "PRINTER"]
REFS = ["U7", "U8", "U5", "U9", "SW1", "SW3", "U2", "U6", "U3", "U4"]  # S1 placed by hand
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

MAX_REF_OFFSET = 2.5   # a designator further than this from its own courtyard
                       # is labelling the wrong part, whatever DRC says

def off_own(box, cy):
    dx = max(cy[0] - box[2], box[0] - cy[2], 0.0)
    dy = max(cy[1] - box[3], box[1] - cy[3], 0.0)
    return math.hypot(dx, dy)

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
    own = L.cy_bbox(fp) if fp is not None else None
    if own is not None and off_own(bb(item), own) > MAX_REF_OFFSET:
        # already adrift; restart the search from the part's own centre
        x0, y0 = (own[0] + own[2]) / 2, (own[1] + own[3]) / 2
        item.SetPosition(pcbnew.VECTOR2I(L.nm(x0), L.nm(y0)))
    if not clash(bb(item), id(item)) and (own is None or off_own(bb(item), own) <= MAX_REF_OFFSET):
        print("  %-12s already clear at (%.2f, %.2f)" % (name, x0, y0))
        continue
    best = None
    for r in [i * 0.25 for i in range(1, 61)]:
        for a in range(0, 360, 10):
            x = x0 + r * math.cos(math.radians(a))
            y = y0 + r * math.sin(math.radians(a))
            item.SetPosition(pcbnew.VECTOR2I(L.nm(x), L.nm(y)))
            if own is not None and off_own(bb(item), own) > MAX_REF_OFFSET:
                continue
            if not clash(bb(item), id(item)):
                best = (x, y, r); break
        if best: break
    if best:
        x, y, r = best
        item.SetPosition(pcbnew.VECTOR2I(L.nm(x), L.nm(y)))
        silks[:] = [(k, q) for k, q in silks if k != id(item)]
        silks.append((id(item), bb(item)))
        print("  %-12s (%.2f, %.2f) -> (%.2f, %.2f)  moved %.2fmm" % (name, x0, y0, x, y, r))
    elif fp is not None:
        # no clear silk spot close enough to be honest about what it labels --
        # drop it to F.Fab, the project's own convention for R/C designators
        item.SetPosition(pcbnew.VECTOR2I(L.nm(x0), L.nm(y0)))
        item.SetLayer(pcbnew.F_Fab)
        print("  %-12s no clear silk spot within %.1fmm of its part -> moved to F.Fab"
              % (name, MAX_REF_OFFSET))
    else:
        item.SetPosition(p0)
        print("  %-12s NO CLEAR SPOT within 15mm -- left at (%.2f, %.2f)" % (name, x0, y0))

pcbnew.SaveBoard(L.BOARD, b)
print("saved")
