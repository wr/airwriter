#!/usr/bin/env python3
"""Generate the DP-08RP footprint and the two VRML bodies.

Every dimension below is read off the HRL DP-xxxx drawing, measured from the
PDF's own vector geometry rather than from the raster: the SVG paths were
transformed to page points and scaled by the drawing's 7.62mm pin-row dimension
(10.3780 pt/mm).  Values that came from the drawing's dimension text rather than
from measured geometry are marked SPEC.

    body            21.88 (A, SPEC) x 9.61 x 9.41 mm       measured 21.80/9.61/9.41
    pin rows        7.62 apart (SPEC), pitch 2.54 (SPEC)
    pin span B      17.78 = 2.54 x 7 (SPEC)
    body vs rows    centred: 0.994mm beyond each row -- NOT flush to the front row
    body vs ends    2.05mm beyond pin 1 / pin 8 centres
    holes           dia 0.97 +-0.05 (SPEC) -> 1.0mm drill, 1.6mm pad
    pin length      3.50 (SPEC), measured 3.40
    key             1.30 wide (SPEC, measured 1.20), 3.30 tall, 0.90 thick,
                    protrudes 2.40mm past the body face, top 0.40mm below body top
    keys align with the pin columns; the key face is the pins 1-8 row.
"""
import os

LIB = "/Users/wells/Projects/airwriter/hardware/airwriter/lib/airwriter.pretty"
D3 = "/Users/wells/Projects/airwriter/hardware/airwriter/lib/airwriter.3dshapes"

PITCH, ROWS = 2.54, 7.62
N = 8
HALF = PITCH * (N - 1) / 2.0          # 8.89
BODY_L, BODY_W, BODY_H = 21.88, 9.61, 9.41
BODY_HALF_W = BODY_W / 2.0            # 4.805, body centred on the rows
BODY_HALF_L = BODY_L / 2.0            # 10.94
KEY_W, KEY_OUT, KEY_T = 1.30, 2.40, 0.90
KEY_TOP_DROP = 0.40
KEY_TIP = ROWS / 2.0 + KEY_OUT + (BODY_HALF_W - ROWS / 2.0)   # 7.205 from centre
DRILL, PAD = 1.0, 1.6
CY = 0.25

def f(v): return ("%g" % round(v, 4))

def pads():
    out = []
    for i in range(N):                                   # 1..8, key row, local -y
        out.append((i + 1, -HALF + PITCH * i, -ROWS / 2.0))
    for i in range(N):                                   # 9..16, pin 9 opposite pin 8
        out.append((N + 1 + i, HALF - PITCH * i, ROWS / 2.0))
    return out

def line(x1, y1, x2, y2, layer, w):
    return ('\t(fp_line (start %s %s) (end %s %s) (stroke (width %s) (type solid))'
            ' (layer "%s"))\n' % (f(x1), f(y1), f(x2), f(y2), w, layer))

def rect(x1, y1, x2, y2, layer, w):
    return (line(x1, y1, x2, y1, layer, w) + line(x2, y1, x2, y2, layer, w) +
            line(x2, y2, x1, y2, layer, w) + line(x1, y2, x1, y1, layer, w))

def footprint():
    s = '(footprint "SW_DIP_8_RightAngle_DP-08RP"\n\t(version 20240108)\n\t(generator "airwriter")\n\t(layer "F.Cu")\n'
    s += ('\t(descr "HRL DP-08RP, 8-position right-angle piano DIP switch, LCSC C458097. '
          'Body 21.88 x 9.61 x 9.41mm standing on the board, piano keys on the vertical face '
          'protruding 2.40mm past it. Geometry measured from the manufacturer drawing\'s own '
          'vector paths (scaled by its 7.62mm pin-row dimension), not estimated: the body is '
          'CENTRED on the two pin rows, 0.994mm beyond each -- it does not stand flush to the '
          'front row. Holes dia 0.97+-0.05 -> 1.0mm drill. Keys align with the pin columns and '
          'sit on the pins 1-8 row. Replaces the stock top-slide SW_DIP_SPSTx08_Slide footprint, '
          'whose body and 3D model misrepresent this part.")\n')
    s += '\t(tags "DIP switch SPST right angle piano DP-08RP 7.62mm 300mil")\n\t(attr through_hole)\n'
    s += '\t(duplicate_pad_numbers_are_jumpers no)\n'
    s += ('\t(property "Reference" "SW**" (at 0 %s 0) (layer "F.SilkS")\n'
          '\t\t(effects (font (size 1 1) (thickness 0.15))))\n' % f(BODY_HALF_W + 1.4))
    s += ('\t(property "Value" "SW_DIP_8_RightAngle_DP-08RP" (at 0 %s 0) (layer "F.Fab")\n'
          '\t\t(effects (font (size 1 1) (thickness 0.15))))\n' % f(-KEY_TIP - 1.4))
    s += '\t(property "Footprint" "" (at 0 0 0) (layer "F.Fab") (hide yes)\n\t\t(effects (font (size 1 1) (thickness 0.15))))\n'
    # F.Fab: body, key tabs, pin-1 chamfer
    s += rect(-BODY_HALF_L, -BODY_HALF_W, BODY_HALF_L, BODY_HALF_W, "F.Fab", "0.1")
    for i in range(N):
        x = -HALF + PITCH * i
        s += rect(x - KEY_W / 2.0, -KEY_TIP, x + KEY_W / 2.0, -BODY_HALF_W, "F.Fab", "0.1")
    s += line(-BODY_HALF_L, -BODY_HALF_W + 1.0, -BODY_HALF_L + 1.0, -BODY_HALF_W, "F.Fab", "0.1")
    # F.SilkS: body rectangle, clear of the 1.6mm pads on both rows
    sx, sy = BODY_HALF_L + 0.11, BODY_HALF_W + 0.11
    s += rect(-sx, -sy, sx, sy, "F.SilkS", "0.12")
    s += line(-sx, -sy - 0.6, -sx + 1.2, -sy - 0.6, "F.SilkS", "0.12")   # pin-1 tick
    s += line(-sx, -sy - 0.6, -sx, -sy, "F.SilkS", "0.12")
    # F.CrtYd: includes the key overhang
    s += rect(-BODY_HALF_L - CY, -KEY_TIP - CY, BODY_HALF_L + CY, BODY_HALF_W + CY, "F.CrtYd", "0.05")
    s += '\t(fp_text user "${REFERENCE}" (at 0 0 0) (layer "F.Fab")\n\t\t(effects (font (size 1 1) (thickness 0.15))))\n'
    for num, x, y in pads():
        shape = "roundrect" if num == 1 else "circle"
        extra = ' (roundrect_rratio 0.15)' if num == 1 else ''
        s += ('\t(pad "%d" thru_hole %s (at %s %s) (size %s %s) (drill %s)'
              ' (layers "*.Cu" "*.Mask")%s)\n'
              % (num, shape, f(x), f(y), f(PAD), f(PAD), f(DRILL), extra))
    s += ('\t(model "${KIPRJMOD}/lib/airwriter.3dshapes/SW_DIP_8_RightAngle_DP-08RP.wrl"\n'
          '\t\t(offset (xyz 0 0 0))\n\t\t(scale (xyz 1 1 1))\n\t\t(rotate (xyz 0 0 0)))\n')
    s += ")\n"
    return s

# ---- VRML -----------------------------------------------------------------
# KiCad reads .wrl in units of 0.1 inch (2.54mm) at scale 1,1,1, and its model
# frame flips Y relative to the footprint frame. Both are verified by rendering.
U = 2.54

def box(name, cx, cy, cz, sx, sy, sz, rgb):
    return ("""\t# %s
\tTransform {
\t\ttranslation %s %s %s
\t\tchildren [ Shape {
\t\t\tappearance Appearance { material Material {
\t\t\t\tdiffuseColor %s
\t\t\t\tspecularColor 0.15 0.15 0.15
\t\t\t\tshininess 0.2
\t\t\t} }
\t\t\tgeometry Box { size %s %s %s }
\t\t} ]
\t}
""" % (name, f(cx / U), f(cy / U), f(cz / U), rgb, f(sx / U), f(sy / U), f(sz / U)))

def wrl_dip():
    s = "#VRML V2.0 utf8\n# HRL DP-08RP 8-way right-angle piano DIP switch.\n"
    s += "# Authored in mm and divided by 2.54: KiCad reads .wrl in 0.1 inch units at scale 1.\n"
    s += "# Footprint -y (the key face) is model +y -- KiCad's model frame flips Y.\n"
    s += "Group { children [\n"
    s += box("body 21.88 x 9.61 x 9.41", 0, 0, BODY_H / 2.0, BODY_L, BODY_W, BODY_H, "0.15 0.15 0.17")
    keyz = BODY_H - KEY_TOP_DROP - KEY_T / 2.0
    for i in range(N):
        x = -HALF + PITCH * i
        s += box("key %d" % (i + 1), x, BODY_HALF_W + KEY_OUT / 2.0, keyz, KEY_W, KEY_OUT, KEY_T,
                 "0.85 0.85 0.85")
    s += "] }\n"
    return s

def wrl_evq():
    L_, W_, H_ = 6.2, 3.0, 3.5
    s = "#VRML V2.0 utf8\n# Panasonic EVQP44B3M side-press SMD tactile switch.\n"
    s += "# Body dims from the LCSC listing; the actuator plate matches the derived footprint's\n"
    s += "# F.Fab overhang (0.45mm past the +X body face). Authored in mm / 2.54.\n"
    s += "Group { children [\n"
    s += box("body 6.2 x 3.0 x 3.5", 0, 0, H_ / 2.0, L_, W_, H_, "0.12 0.12 0.14")
    s += box("actuator plate", L_ / 2.0 + 0.225, 0, H_ / 2.0, 0.45, 0.9, 1.0, "0.80 0.80 0.82")
    s += "] }\n"
    return s

open(os.path.join(LIB, "SW_DIP_8_RightAngle_DP-08RP.kicad_mod"), "w").write(footprint())
open(os.path.join(D3, "SW_DIP_8_RightAngle_DP-08RP.wrl"), "w").write(wrl_dip())
open(os.path.join(D3, "EVQP44B3M_SidePress.wrl"), "w").write(wrl_evq())
print("wrote footprint + 2 VRML bodies")
print("key tip at local y = %.3f ; body face %.3f ; pad rows +-%.3f" % (-KEY_TIP, -BODY_HALF_W, ROWS/2))
print("courtyard local x +-%.3f, y %.3f..%.3f" % (BODY_HALF_L+CY, -KEY_TIP-CY, BODY_HALF_W+CY))
