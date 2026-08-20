#!/usr/bin/env python3
"""panel_layout.json + *.polys.json -> panel.kicad_pcb. KiCad python only."""
import json, math, pathlib, pcbnew

HERE = pathlib.Path(__file__).parent
mm = pcbnew.FromMM

def rounded_outline(board, w, h, r):
    for (x1, y1, x2, y2) in [(r, 0, w - r, 0), (w, r, w, h - r),
                             (w - r, h, r, h), (0, h - r, 0, r)]:
        s = pcbnew.PCB_SHAPE(board, pcbnew.SHAPE_T_SEGMENT)
        s.SetStart(pcbnew.VECTOR2I(mm(x1), mm(y1)))
        s.SetEnd(pcbnew.VECTOR2I(mm(x2), mm(y2)))
        s.SetLayer(pcbnew.Edge_Cuts); s.SetWidth(mm(0.1))
        board.Add(s)
    for (cx, cy, a0) in [(r, r, 180), (w - r, r, 270), (w - r, h - r, 0), (r, h - r, 90)]:
        a = pcbnew.PCB_SHAPE(board, pcbnew.SHAPE_T_ARC)
        a.SetCenter(pcbnew.VECTOR2I(mm(cx), mm(cy)))
        sx = cx + r * math.cos(math.radians(a0)); sy = cy + r * math.sin(math.radians(a0))
        a.SetStart(pcbnew.VECTOR2I(mm(sx), mm(sy)))
        a.SetArcAngleAndEnd(pcbnew.EDA_ANGLE(90, pcbnew.DEGREES_T), False)
        a.SetLayer(pcbnew.Edge_Cuts); a.SetWidth(mm(0.1))
        board.Add(a)

def filled_circle(board, layer, x, y, dia):
    c = pcbnew.PCB_SHAPE(board, pcbnew.SHAPE_T_CIRCLE)
    c.SetCenter(pcbnew.VECTOR2I(mm(x), mm(y)))
    c.SetEnd(pcbnew.VECTOR2I(mm(x + dia / 2), mm(y)))
    c.SetFilled(True); c.SetWidth(0); c.SetLayer(layer)
    board.Add(c)

def npth(board, x, y, dia):
    fp = pcbnew.FOOTPRINT(board)
    fp.SetPosition(pcbnew.VECTOR2I(mm(x), mm(y)))
    fp.SetReference("H_TEST"); fp.Reference().SetVisible(False)
    pad = pcbnew.PAD(fp)
    pad.SetAttribute(pcbnew.PAD_ATTRIB_NPTH)
    pad.SetShape(pcbnew.PAD_SHAPE_CIRCLE)
    pad.SetSize(pcbnew.VECTOR2I(mm(dia), mm(dia)))
    pad.SetDrillSize(pcbnew.VECTOR2I(mm(dia), mm(dia)))
    pad.SetPosition(pcbnew.VECTOR2I(mm(x), mm(y)))
    pad.SetLayerSet(pcbnew.PAD.UnplatedHoleMask())
    fp.Add(pad)
    board.Add(fp)

RECIPE = {  # mat -> (copper_F, mask_F, mask_B)
    "G": (True, True, False), "T": (True, True, False),
    "P": (False, True, True), "B": (False, False, True), "L": (False, False, False),
}

def main():
    lay = json.loads((HERE / "panel_layout.json").read_text())
    board = pcbnew.CreateEmptyBoard()
    p, g = lay["panel"], lay["grid"]
    rounded_outline(board, p["w"], p["h"], p["corner_r"])
    for d in lay["dots"]:
        cu, mf, mb = RECIPE[d["mat"]]
        dia = g["dot_dia"]
        if cu: filled_circle(board, pcbnew.F_Cu, d["x"], d["y"], dia)
        if mf: filled_circle(board, pcbnew.F_Mask, d["x"], d["y"], dia)
        if mb: filled_circle(board, pcbnew.B_Mask, d["x"], d["y"], dia)
        if d["mat"] == "T":
            npth(board, d["x"], d["y"], g["hole_dia"])
    board.Save(str(HERE / "panel.kicad_pcb"))
    print("wrote panel.kicad_pcb:", len(lay["dots"]), "dots")

if __name__ == "__main__":
    main()
