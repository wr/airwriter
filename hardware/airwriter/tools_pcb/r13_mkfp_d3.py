#!/usr/bin/env python3
"""Rev 2026.11: D3's footprint, a 3 mm THT LED standing on an 8 mm spacer.

    <KiCad python> tools_pcb/r13_mkfp_d3.py

Starts from KiCad's LED_D3.0mm (pads, silk, fab) and changes what the spacer
changes. The courtyard is a circle round the spacer (4.0 mm + 0.15 mm) rather
than the stock rectangle, because D3 stands 0.9 mm from J1's shell and the
stock outline meets J1's generous courtyard. The 3D models put the LED 8 mm up
on a spacer model (lib/make_led_spacer_model.py), so the lattice and case
read its real envelope. Saved through KiCad's own footprint writer.
"""
import os

import pcbnew

HERE = os.path.dirname(os.path.abspath(__file__))
LIB = os.path.normpath(os.path.join(HERE, '..', 'lib', 'airwriter.pretty'))
STOCK = '/Applications/KiCad/KiCad.app/Contents/SharedSupport/footprints/LED_THT.pretty'
NAME = 'LED_D3.0mm_Spacer_H8mm'
AXIS_X = 1.27              # the LED's axis, between its pads
SPACER_R, COURTYARD_R = 2.0, 2.15
SPACER_H = 8.0
MODELS = ('${KICAD10_3DMODEL_DIR}/LED_THT.3dshapes/LED_D3.0mm.step',
          '${KIPRJMOD}/lib/airwriter.3dshapes/LED_Spacer_D4.0mm_H8mm.step')


def circle(fp, layer, r, width):
    c = pcbnew.PCB_SHAPE(fp, pcbnew.SHAPE_T_CIRCLE)
    c.SetCenter(pcbnew.VECTOR2I(pcbnew.FromMM(AXIS_X), 0))
    c.SetEnd(pcbnew.VECTOR2I(pcbnew.FromMM(AXIS_X + r), 0))
    c.SetLayer(layer)
    c.SetWidth(pcbnew.FromMM(width))
    fp.Add(c)


def main():
    # Loading a board registers the SWIG types Models() returns; without it the
    # list comes back opaque.
    pcbnew.LoadBoard(os.path.join(HERE, '..', 'airwriter.kicad_pcb'))
    fp = pcbnew.FootprintLoad(STOCK, 'LED_D3.0mm')
    fp.SetFPIDAsString('airwriter:' + NAME)
    fp.SetLibDescription('3 mm round THT LED on a 4 x 8 mm nylon spacer '
                         '(HIWA LED4*8, LCSC C92326): lens about 11.85 mm above the board')
    fp.SetKeywords('LED THT 3mm spacer standoff')
    models = fp.Models()           # the stock LED model, lifted onto the spacer
    led = models[0]
    led.m_Filename = MODELS[0]
    led.m_Offset.z = SPACER_H
    spacer = pcbnew.FP_3DMODEL()
    spacer.m_Filename = MODELS[1]
    spacer.m_Offset.x = AXIS_X
    models.push_back(spacer)
    # (after the models: SWIG's lists go opaque once an item is removed)
    for item in list(fp.GraphicalItems()):
        if item.GetLayer() == pcbnew.F_CrtYd:
            fp.Remove(item)
    circle(fp, pcbnew.F_CrtYd, COURTYARD_R, 0.05)
    circle(fp, pcbnew.F_Fab, SPACER_R, 0.1)
    pcbnew.FootprintSave(LIB, fp)
    print('wrote', os.path.join(LIB, NAME + '.kicad_mod'))


if __name__ == '__main__':
    main()
