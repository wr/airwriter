"""Generate the MODE and BOOT pads as dithered mask over solid copper.

Cells sit on the same 0.455mm grid as the cover artwork, so the controls read as part
of the pattern rather than as hardware dropped on top. The copper underneath stays
whole -- self-capacitance wants unbroken area -- and only the soldermask is dithered.
Density falls off with radius, which is what makes a flat field read as a target.
"""
import math, os
GRID = 0.455          # matches the cover's gold cells
CELL = 0.40           # slightly under the grid so dams survive
OUT  = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'airwriter.pretty')

def rnd(i, j, salt):
    h = (i*73856093) ^ (j*19349663) ^ (salt*83492791)
    h &= 0xFFFFFFFF
    h ^= h >> 13; h = (h*1274126177) & 0xFFFFFFFF; h ^= h >> 16
    return h / 0xFFFFFFFF

def dither(R, prob, salt, keep=None):
    n = int(R/GRID)+2; out=[]
    for i in range(-n, n+1):
        for j in range(-n, n+1):
            cx, cy = i*GRID, j*GRID
            r = math.hypot(cx, cy)
            if keep and not keep(r): continue
            if r > R - CELL/2: continue
            if rnd(i, j, salt) < prob(r/R):
                out.append((cx-CELL/2, cy-CELL/2, cx+CELL/2, cy+CELL/2))
    return out

def rects(cells, layer):
    return '\n'.join('\t(fp_rect (start %.4f %.4f) (end %.4f %.4f) (stroke (width 0.02) (type solid)) '
                     '(fill solid) (layer "%s"))'%(k[0],k[1],k[2],k[3],layer) for k in cells)

# ---- MODE: one solid electrode, density fading outward -------------------------
R = 4.6
cells = dither(R, lambda t: max(0.0, 1.06 - t**1.5), salt=7)
open(os.path.join(OUT,'TouchPad_Dither_D9mm.kicad_mod'),'w').write('''(footprint "TouchPad_Dither_D9mm"
\t(version 20240108)
\t(generator "airwriter")
\t(layer "B.Cu")
\t(descr "Capacitive touch pad, back face. Solid 9.2mm copper disc -- self-capacitance wants unbroken area -- with the pattern in the soldermask, dithered on the cover artwork's 0.455mm grid and fading outward. Pad 1 electrode, pad 2 ground tie.")
\t(tags "touch capacitive pad dither")
\t(attr smd exclude_from_pos_files)
\t(property "Reference" "SW**" (at 0 -6 0) (layer "B.SilkS")
\t\t(effects (font (size 0.8 0.8) (thickness 0.12)) (justify mirror)))
\t(property "Value" "TouchPad_Dither_D9mm" (at 0 6 0) (layer "B.Fab") (hide yes)
\t\t(effects (font (size 1 1) (thickness 0.15)) (justify mirror)))
\t(fp_circle (center 0 0) (end %.2f 0) (stroke (width 0.05) (type solid)) (fill no) (layer "B.CrtYd"))
%s
\t(pad "1" smd circle (at 0 0) (size %.2f %.2f) (layers "B.Cu"))
\t(pad "2" smd rect (at 0 6.1) (size 2 0.9) (layers "B.Cu"))
)
'''%(R+0.9, rects(cells,'B.Mask'), R*2, R*2))
print('MODE: %d dither cells over a %.1fmm disc'%(len(cells), R*2))

# ---- BOOT: same language, half the size, split into two pads -------------------
core = dither(1.1, lambda t: 1.0, salt=11)
ring = dither(2.5, lambda t: 0.85, salt=13, keep=lambda r: r > 1.62)
open(os.path.join(OUT,'BootPads_Dither_D5mm.kicad_mod'),'w').write('''(footprint "BootPads_Dither_D5mm"
\t(version 20240108)
\t(generator "airwriter")
\t(layer "B.Cu")
\t(descr "BOOT bridge pads, back face: a dithered disc inside a dithered ring, 0.5mm of bare gap between them. Short with tweezers to force download mode. Same 0.455mm grid as the cover artwork, half the diameter of the MODE pad so it reads as secondary.")
\t(tags "boot bridge pads dither")
\t(attr smd exclude_from_pos_files)
\t(property "Reference" "SW**" (at 0 -3.6 0) (layer "B.SilkS")
\t\t(effects (font (size 0.8 0.8) (thickness 0.12)) (justify mirror)))
\t(property "Value" "BootPads_Dither_D5mm" (at 0 3.6 0) (layer "B.Fab") (hide yes)
\t\t(effects (font (size 1 1) (thickness 0.15)) (justify mirror)))
\t(fp_circle (center 0 0) (end 3.1 0) (stroke (width 0.05) (type solid)) (fill no) (layer "B.CrtYd"))
%s
%s
\t(pad "1" smd circle (at 0 0) (size 2.2 2.2) (layers "B.Cu"))
\t(pad "2" smd custom (at 2.05 0) (size 0.5 0.5) (layers "B.Cu")
\t\t(options (clearance outline) (anchor circle))
\t\t(primitives (gr_circle (center -2.05 0) (end 0 0) (width 0.9) (fill no)))
\t)
)
'''%(rects(core,'B.Mask'), rects(ring,'B.Mask')))
print('BOOT: %d core cells + %d ring cells'%(len(core), len(ring)))
