"""Generate the MODE touch pad and the BOOT service pads.

MODE (TouchPad_Core_D9mm): a solid gold core whose edge dissolves into the cover's
mist. Cells sit on the mist's own grid -- 0.72mm cells on a 0.91mm pitch, the same
size the grain artwork uses -- so the control reads as the mist condensing rather
than as a texture of its own. The copper underneath stays whole (self-capacitance
wants unbroken area); only the soldermask carries the pattern.

BOOT (BootPads_Service): plain bridge pads on the component side, where service
points belong. Two bare rectangles, a silk box and a BOOT label. Short with
tweezers to force download mode. No through-hole, so the visible face stays clean.
"""
import math, os
MIST_GRID = 0.91      # the grain artwork's pitch
MIST_CELL = 0.72      # and its cell size
OUT  = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'airwriter.pretty')

def rnd(i, j, salt):
    h = (i*73856093) ^ (j*19349663) ^ (salt*83492791)
    h &= 0xFFFFFFFF
    h ^= h >> 13; h = (h*1274126177) & 0xFFFFFFFF; h ^= h >> 16
    return h / 0xFFFFFFFF

def rects(cells, layer):
    return '\n'.join('\t(fp_rect (start %.4f %.4f) (end %.4f %.4f) (stroke (width 0.02) (type solid)) '
                     '(fill solid) (layer "%s"))'%(k[0],k[1],k[2],k[3],layer) for k in cells)

# ---- MODE: solid core, edge dissolving into the mist ---------------------------
R    = 4.6            # electrode radius; every opening must stay on copper
CORE = 2.0            # solid gold disc
AMB  = 0.055          # the mist's ambient density, where the fade ends
cells=[]
n = int(R/MIST_GRID)+2
for i in range(-n, n+1):
    for j in range(-n, n+1):
        cx, cy = i*MIST_GRID, j*MIST_GRID
        r = math.hypot(cx, cy)
        if r > R - MIST_CELL/2: continue      # stay on the electrode
        if r < CORE - MIST_CELL/2: continue   # the solid circle covers these
        t = max(0.0, (r - CORE) / (R - MIST_CELL/2 - CORE))
        p = AMB + (1-AMB) * (1-t)**1.0
        if rnd(i, j, 101) < p:
            cells.append((cx-MIST_CELL/2, cy-MIST_CELL/2, cx+MIST_CELL/2, cy+MIST_CELL/2))
open(os.path.join(OUT,'TouchPad_Core_D9mm.kicad_mod'),'w').write('''(footprint "TouchPad_Core_D9mm"
\t(version 20240108)
\t(generator "airwriter")
\t(layer "B.Cu")
\t(descr "Capacitive touch pad, back face. Solid 9.2mm copper disc -- self-capacitance wants unbroken area -- with the pattern in the soldermask: a solid gold core dissolving outward into the cover's mist, drawn in the mist's own 0.72mm cells. Pad 1 electrode, pad 2 ground tie.")
\t(tags "touch capacitive pad mist")
\t(attr smd exclude_from_pos_files)
\t(property "Reference" "SW**" (at 0 -6 0) (layer "B.SilkS")
\t\t(effects (font (size 0.8 0.8) (thickness 0.12)) (justify mirror)))
\t(property "Value" "TouchPad_Core_D9mm" (at 0 6 0) (layer "B.Fab") (hide yes)
\t\t(effects (font (size 1 1) (thickness 0.15)) (justify mirror)))
\t(fp_circle (center 0 0) (end %.2f 0) (stroke (width 0.05) (type solid)) (fill no) (layer "B.CrtYd"))
\t(fp_circle (center 0 0) (end %.2f 0) (stroke (width 0.02) (type solid)) (fill solid) (layer "B.Mask"))
%s
\t(pad "1" smd circle (at 0 0) (size %.2f %.2f) (layers "B.Cu"))
\t(pad "2" smd rect (at 0 6.1) (size 2 0.9) (layers "B.Cu"))
)
'''%(R+0.9, CORE, rects(cells,'B.Mask'), R*2, R*2))
print('MODE: solid %.1fmm core + %d mist cells over a %.1fmm disc'%(CORE*2, len(cells), R*2))

# ---- BOOT: plain service pads, component side ----------------------------------
open(os.path.join(OUT,'BootPads_Service.kicad_mod'),'w').write('''(footprint "BootPads_Service"
\t(version 20240108)
\t(generator "airwriter")
\t(layer "F.Cu")
\t(descr "BOOT bridge pads on the component side: two bare rectangles you short with tweezers to force download mode. A service point styled as one -- the visible face carries no trace of it. Pad 1 BOOT_CFG (at origin, where the existing track lands), pad 2 ground.")
\t(tags "boot bridge pads service")
\t(attr smd exclude_from_pos_files)
\t(property "Reference" "SW**" (at 1.05 -2.2 0) (layer "F.SilkS")
\t\t(effects (font (size 0.8 0.8) (thickness 0.12))))
\t(property "Value" "BootPads_Service" (at 1.05 2.2 0) (layer "F.Fab") (hide yes)
\t\t(effects (font (size 1 1) (thickness 0.15))))
\t(fp_rect (start -1.15 -1.4) (end 3.25 1.4) (stroke (width 0.12) (type solid)) (fill no) (layer "F.SilkS"))
\t(fp_rect (start -1.35 -1.6) (end 3.45 1.6) (stroke (width 0.05) (type solid)) (fill no) (layer "F.CrtYd"))
\t(fp_text user "BOOT" (at 5.2 0 0) (layer "F.SilkS")
\t\t(effects (font (size 0.8 0.8) (thickness 0.12))))
\t(pad "1" smd rect (at 0 0) (size 1.3 1.8) (layers "F.Cu" "F.Mask"))
\t(pad "2" smd rect (at 2.1 0) (size 1.3 1.8) (layers "F.Cu" "F.Mask"))
)
''')
print('BOOT: plain pads, front')
