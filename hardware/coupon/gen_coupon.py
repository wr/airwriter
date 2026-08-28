"""Build the artwork swatch coupon: coupon.kicad_pcb, 60 x 45mm, two layers.

An inexpensive way to see the cover's visual language in real materials before
committing the main board: black mask, ENIG gold, bare-FR4 windows. Nothing on
it is electrical -- every copper feature is a netless graphic under the same
mask openings the real board uses.

What it carries:
  - the full AirWriter wordmark, harvested cell-for-cell from the routed board
    (gold cells, and the bare-FR4 window cells that light from behind)
  - an ambient mist field at the cover's density
  - three MODE pad candidates, A/B/C from the design review, each with the
    real 9.2mm electrode disc and the real bare guard annulus around it
  - a strip of LED window sizes (D1/D2, D3, and a 2mm square) to check how
    visible bare FR4 is with and without light behind it

Run with KiCad's bundled python:
    /Applications/KiCad/KiCad.app/Contents/Frameworks/Python.framework/Versions/3.9/bin/python3 gen_coupon.py
"""
import pcbnew, math, os

HERE = os.path.dirname(os.path.abspath(__file__))
SRC  = os.path.join(HERE, '..', 'airwriter', 'airwriter.kicad_pcb')
OUT  = os.path.join(HERE, 'coupon.kicad_pcb')

W, H = 60.0, 45.0
MIST_GRID, MIST_CELL, AMB = 0.91, 0.72, 0.055
R_EL, R_GUARD = 4.6, 6.1                     # electrode and guard, as on the real board
PADS = [(13.0, 27.0, 'A'), (30.0, 27.0, 'B'), (47.0, 27.0, 'C')]
WINDOWS = [(21.0, 39.5, 3.4, 3.0, 'D1 D2'), (30.0, 39.5, 1.6, 2.2, 'D3'), (38.0, 39.5, 2.0, 2.0, '2MM')]

# the wordmark band on the source board, and where it lands here
BX0, BX1, BY0, BY1 = 100.3, 149.7, 90.2, 101.0
DX, DY = 30.0 - (BX0+BX1)/2, 10.0 - (BY0+BY1)/2

mm = lambda v: int(round(v*1e6))
def rnd(i, j, salt):
    h = (i*73856093) ^ (j*19349663) ^ (salt*83492791)
    h &= 0xFFFFFFFF
    h ^= h >> 13; h = (h*1274126177) & 0xFFFFFFFF; h ^= h >> 16
    return h / 0xFFFFFFFF

# ---- harvest the wordmark from the routed board --------------------------------
src = pcbnew.LoadBoard(SRC)
Lname = src.GetLayerName
gold, fr4, silk = [], [], []     # (x0,y0,x1,y1) in coupon coordinates
for d in src.GetDrawings():
    if not (isinstance(d, pcbnew.PCB_SHAPE) and d.GetShape() == pcbnew.SHAPE_T_RECT):
        continue
    bb = d.GetBoundingBox()
    cx, cy = bb.GetCenter().x/1e6, bb.GetCenter().y/1e6
    if not (BX0 < cx < BX1 and BY0 < cy < BY1): continue
    k = (bb.GetLeft()/1e6+DX+0.05, bb.GetTop()/1e6+DY+0.05,
         bb.GetRight()/1e6+DX-0.05, bb.GetBottom()/1e6+DY-0.05)   # rects carry 0.05 stroke
    L = Lname(d.GetLayer())
    if L == 'F.Mask': fr4.append(k)
    elif L == 'B.Mask': gold.append(k)
    elif L == 'B.Silkscreen': silk.append(k)
# gold list still contains the B.Mask twins of every FR4 cell; drop them
def near(a, b): return abs(a[0]-b[0]) < 0.05 and abs(a[1]-b[1]) < 0.05
gold = [g for g in gold if not any(near(g, f) for f in fr4)]
print('harvested: %d gold, %d FR4, %d silk cells' % (len(gold), len(fr4), len(silk)))

# ---- the coupon board ----------------------------------------------------------
b = pcbnew.CreateEmptyBoard()

def edge(x0, y0, x1, y1):
    s = pcbnew.PCB_SHAPE(b, pcbnew.SHAPE_T_SEGMENT)
    s.SetStart(pcbnew.VECTOR2I(mm(x0), mm(y0))); s.SetEnd(pcbnew.VECTOR2I(mm(x1), mm(y1)))
    s.SetLayer(pcbnew.Edge_Cuts); s.SetWidth(mm(0.1)); b.Add(s)
edge(0,0,W,0); edge(W,0,W,H); edge(W,H,0,H); edge(0,H,0,0)

def rect(k, layer, filled=True):
    s = pcbnew.PCB_SHAPE(b, pcbnew.SHAPE_T_RECT)
    s.SetStart(pcbnew.VECTOR2I(mm(k[0]), mm(k[1]))); s.SetEnd(pcbnew.VECTOR2I(mm(k[2]), mm(k[3])))
    s.SetFilled(filled); s.SetLayer(layer); s.SetWidth(mm(0.02)); b.Add(s)

def circle(cx, cy, r, layer, filled=True, w=0.02):
    s = pcbnew.PCB_SHAPE(b, pcbnew.SHAPE_T_CIRCLE)
    s.SetCenter(pcbnew.VECTOR2I(mm(cx), mm(cy))); s.SetEnd(pcbnew.VECTOR2I(mm(cx+r), mm(cy)))
    s.SetFilled(filled); s.SetLayer(layer); s.SetWidth(mm(w)); b.Add(s)

def text(t, x, y, layer, size=1.0, mirror=False):
    s = pcbnew.PCB_TEXT(b)
    s.SetText(t); s.SetPosition(pcbnew.VECTOR2I(mm(x), mm(y)))
    s.SetLayer(layer)
    s.SetTextSize(pcbnew.VECTOR2I(mm(size), mm(size))); s.SetTextThickness(mm(size*0.15))
    if mirror: s.SetMirrored(True)
    b.Add(s)

# copper pours, both faces, netless: they exist to put gold behind the mask openings
for layer in (pcbnew.F_Cu, pcbnew.B_Cu):
    z = pcbnew.ZONE(b)
    z.SetLayer(layer)
    ch = pcbnew.SHAPE_LINE_CHAIN()
    for x, y in ((0.6,0.6),(W-0.6,0.6),(W-0.6,H-0.6),(0.6,H-0.6)): ch.Append(mm(x), mm(y))
    ch.SetClosed(True)
    z.Outline().AddOutline(ch)
    z.SetLocalClearance(mm(0.2)); z.SetMinThickness(mm(0.254))
    b.Add(z)

# no-copper regions: wordmark FR4 cells, guard annuli, LED windows.
# One rule area per polygon -- a multi-outline zone loses all but its first
# outline on save, which left copper under every window on the first attempt.
def add_keepout(pts, name):
    ka = pcbnew.ZONE(b)
    ka.SetIsRuleArea(True)
    ka.SetDoNotAllowZoneFills(True); ka.SetDoNotAllowTracks(False); ka.SetDoNotAllowVias(False)
    ka.SetLayerSet(pcbnew.LSET.AllCuMask(2))
    ka.SetZoneName(name)
    ch = pcbnew.SHAPE_LINE_CHAIN()
    for x, y in pts: ch.Append(mm(x), mm(y))
    ch.SetClosed(True)
    ka.Outline().AddOutline(ch)
    b.Add(ka)
def add_keepout_rect(k, name, grow=0.15):
    add_keepout([(k[0]-grow, k[1]-grow), (k[2]+grow, k[1]-grow),
                 (k[2]+grow, k[3]+grow), (k[0]-grow, k[3]+grow)], name)
for n, k in enumerate(fr4): add_keepout_rect(k, 'WORDMARK %d' % n)
for (x, y, w, h, label) in WINDOWS:
    add_keepout([(x-w/2, y-h/2), (x+w/2, y-h/2), (x+w/2, y+h/2), (x-w/2, y+h/2)], 'WINDOW '+label)
guard_out = []
for (px, py, kind) in PADS:
    pts = [(px+R_GUARD*math.cos(math.radians(a)), py+R_GUARD*math.sin(math.radians(a)))
           for a in range(0, 360, 45)]
    add_keepout(pts, 'GUARD '+kind)
    guard_out.append((px, py))

# wordmark: gold cells on the back mask, FR4 cells open on both masks, silk on top
for k in gold: rect(k, pcbnew.B_Mask)
for k in silk: rect(k, pcbnew.B_SilkS)
for k in fr4:
    rect(k, pcbnew.B_Mask); rect(k, pcbnew.F_Mask)

# LED windows: mask open both faces, 0.4mm smaller than the copper-free hole, per the board recipe
for (x, y, w, h, label) in WINDOWS:
    m = (x-w/2+0.2, y-h/2+0.2, x+w/2-0.2, y+h/2-0.2)
    rect(m, pcbnew.B_Mask); rect(m, pcbnew.F_Mask)
    text(label, x, y+3.4, pcbnew.F_SilkS, 0.8)

# MODE candidates: real electrode disc under each pattern
def cells_for(kind, px, py):
    out = []
    n = int(R_EL/MIST_GRID)+2
    i0, j0 = int(px/MIST_GRID), int(py/MIST_GRID)
    for i in range(-n, n+1):
        for j in range(-n, n+1):
            cx, cy = px+i*MIST_GRID, py+j*MIST_GRID
            r = math.hypot(cx-px, cy-py)
            if r > R_EL - MIST_CELL/2: continue
            if kind == 'A':
                p = AMB + (1-AMB) * max(0.0, 1 - r/(R_EL-MIST_CELL/2))**1.9
                keep = rnd(i0+i, j0+j, 1) < p
            elif kind == 'B':
                keep = (any(abs(r-q) < 0.40 for q in (4.0, 2.0)) or r < 0.6) and rnd(i0+i, j0+j, 202) < 0.9
            else:  # C, exactly the production pattern
                if r < 2.0 - MIST_CELL/2: continue
                t = max(0.0, (r-2.0) / (R_EL - MIST_CELL/2 - 2.0))
                p = AMB + (1-AMB) * (1-t)**1.0
                keep = rnd(i, j, 101) < p
            if keep:
                out.append((cx-MIST_CELL/2, cy-MIST_CELL/2, cx+MIST_CELL/2, cy+MIST_CELL/2))
    return out

for (px, py, kind) in PADS:
    circle(px, py, R_EL, pcbnew.B_Cu)                     # the electrode
    if kind == 'C': circle(px, py, 2.0, pcbnew.B_Mask)    # the solid core
    for k in cells_for(kind, px, py): rect(k, pcbnew.B_Mask)
    text(kind, px, py+8.2, pcbnew.F_SilkS, 1.2)

# ambient mist over everything the features don't claim
def free(x, y):
    if not (1.5 < x < W-1.5 and 1.5 < y < H-1.5): return False
    if 3.8 < y < 16.4: return False                       # the wordmark band
    for (px, py) in guard_out:
        if math.hypot(x-px, y-py) < R_GUARD+0.8: return False
    for (wx, wy, w, h, _) in WINDOWS:
        if abs(x-wx) < w/2+1.0 and abs(y-wy) < h/2+1.0: return False
    return True
n_mist = n_white = 0
for i in range(int(W/MIST_GRID)+1):
    for j in range(int(H/MIST_GRID)+1):
        x, y = i*MIST_GRID, j*MIST_GRID
        if not free(x, y): continue
        k = (x-MIST_CELL/2, y-MIST_CELL/2, x+MIST_CELL/2, y+MIST_CELL/2)
        if rnd(i, j, 7) < AMB:
            rect(k, pcbnew.B_Mask)
            n_mist += 1
        elif rnd(i, j, 23) < 0.012:          # bare-FR4 cells, the cover's white mist
            rect(k, pcbnew.B_Mask); rect(k, pcbnew.F_Mask)
            add_keepout_rect(k, 'WHITE %d.%d' % (i, j))
            n_white += 1
        elif rnd(i, j, 41) < 0.004:          # and the rare printed-silk cell
            rect(k, pcbnew.B_SilkS)

# front-face notes (the hidden side)
text('AIRWRITER ARTWORK COUPON', W/2, 3.0, pcbnew.F_SilkS, 1.4)
text('BLACK MASK / ENIG / 1.6MM', W/2, 5.6, pcbnew.F_SilkS, 0.9)

# ZONE_FILLER segfaults on a CreateEmptyBoard() board (no project context);
# saving and reloading gives it one.
b.Save(OUT)
b2 = pcbnew.LoadBoard(OUT)
pcbnew.ZONE_FILLER(b2).Fill(b2.Zones())
b2.Save(OUT)
print('coupon saved: %d gold + %d white mist cells, 3 pads, %d windows' % (n_mist, n_white, len(WINDOWS)))
