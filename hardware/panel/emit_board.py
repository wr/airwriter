#!/usr/bin/env python3
"""panel_layout.json + *.polys.json -> panel.kicad_pcb. KiCad python only."""
import json, math, pathlib, re, uuid, pcbnew

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

def _circle_pts(cx, cy, r, segs=64):
    return [(cx + r * math.cos(2 * math.pi * i / segs),
             cy + r * math.sin(2 * math.pi * i / segs)) for i in range(segs)]

def filled_ring(board, layer, x, y, od, idia, segs=64):
    chain = pcbnew.SHAPE_POLY_SET()
    ol = pcbnew.SHAPE_LINE_CHAIN()
    for px, py in _circle_pts(x, y, od / 2, segs):
        ol.Append(mm(px), mm(py))
    ol.SetClosed(True)
    chain.AddOutline(ol)
    hl = pcbnew.SHAPE_LINE_CHAIN()
    for px, py in _circle_pts(x, y, idia / 2, segs):
        hl.Append(mm(px), mm(py))
    hl.SetClosed(True)
    chain.AddHole(hl)
    chain.Fracture()
    s = pcbnew.PCB_SHAPE(board, pcbnew.SHAPE_T_POLY)
    s.SetPolyShape(chain); s.SetFilled(True); s.SetWidth(0); s.SetLayer(layer)
    board.Add(s)

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

def panel_interior(w, h, r, margin, segs_per_corner=24):
    # The rounded-rect board boundary, inset by margin, as a SHAPE_POLY_SET.
    # Used to clip silk against the true board silhouette (straight edges
    # *and* the rounded corners) instead of hand-rolling edge cases.
    ri = r - margin
    chain = pcbnew.SHAPE_LINE_CHAIN()
    corners = [(r, r, 180), (w - r, r, 270), (w - r, h - r, 0), (r, h - r, 90)]
    for cx, cy, a0 in corners:
        for i in range(segs_per_corner + 1):
            a = math.radians(a0 + 90 * i / segs_per_corner)
            chain.Append(mm(cx + ri * math.cos(a)), mm(cy + ri * math.sin(a)))
    chain.SetClosed(True)
    poly = pcbnew.SHAPE_POLY_SET()
    poly.AddOutline(chain)
    return poly

def poly_shape(board, layer, polys, clip=None):
    for p in polys:
        chain = pcbnew.SHAPE_POLY_SET()
        ol = pcbnew.SHAPE_LINE_CHAIN()
        for x, y in p["outline"]:
            ol.Append(mm(x), mm(y))
        ol.SetClosed(True)
        chain.AddOutline(ol)
        for hole in p["holes"]:
            hl = pcbnew.SHAPE_LINE_CHAIN()
            for x, y in hole:
                hl.Append(mm(x), mm(y))
            hl.SetClosed(True)
            chain.AddHole(hl)
        if clip is not None:
            chain.BooleanIntersection(clip)
        chain.Fracture()
        s = pcbnew.PCB_SHAPE(board, pcbnew.SHAPE_T_POLY)
        s.SetPolyShape(chain); s.SetFilled(True); s.SetWidth(0); s.SetLayer(layer)
        board.Add(s)

def split_mask_knockouts(silk, dots, dot_r):
    # A white path fully inside a gold-family (G/T) dot's disc is Wells's
    # black-letters-on-gold design (e.g. the "test" tag): mask kept over the
    # glyph strokes instead of separate silkscreen. Data-driven - any glyph
    # meeting the geometry test gets this treatment, no name/position special
    # case. Returns (remaining_silk, {dot_index: [glyph_polys]}).
    gold_idxs = [i for i, d in enumerate(dots) if d["mat"] in ("G", "T")]
    remaining = []
    knockouts = {}
    for poly in silk:
        owner = next((i for i in gold_idxs
                      if all(math.hypot(x - dots[i]["x"], y - dots[i]["y"]) < dot_r
                             for x, y in poly["outline"])), None)
        if owner is None:
            remaining.append(poly)
        else:
            knockouts.setdefault(owner, []).append(poly)
    return remaining, knockouts

def mask_knockout(board, cx, cy, dia, glyphs):
    # Mask KEPT (not opened) over each glyph's own outline - dark letters on
    # exposed gold copper. One gr_poly: the dot's disc with each glyph's
    # outline as a hole (a "hole" here in the geometric sense of pcbnew's own
    # SHAPE_POLY_SET API, not a mask opening - it cuts the glyph shape *out*
    # of the open-mask disc, so mask stays solid/kept there). A glyph's own
    # hole (a letter counter, e.g. inside "e") must still read as open gold -
    # that can't be expressed as a hole-of-a-hole in one poly, so punch each
    # counter back open as its own small standalone F.Mask poly.
    chain = pcbnew.SHAPE_POLY_SET()
    ol = pcbnew.SHAPE_LINE_CHAIN()
    for px, py in _circle_pts(cx, cy, dia / 2):
        ol.Append(mm(px), mm(py))
    ol.SetClosed(True)
    chain.AddOutline(ol)
    for glyph in glyphs:
        hl = pcbnew.SHAPE_LINE_CHAIN()
        for x, y in glyph["outline"]:
            hl.Append(mm(x), mm(y))
        hl.SetClosed(True)
        chain.AddHole(hl)
    chain.Fracture()
    s = pcbnew.PCB_SHAPE(board, pcbnew.SHAPE_T_POLY)
    s.SetPolyShape(chain); s.SetFilled(True); s.SetWidth(0); s.SetLayer(pcbnew.F_Mask)
    board.Add(s)
    for glyph in glyphs:
        for hole in glyph["holes"]:
            poly_shape(board, pcbnew.F_Mask, [dict(outline=hole, holes=[])])

def check_floors(lay, gold, silk, fr4):
    g = lay["grid"]
    assert g["hole_dia"] >= 0.30, "drill floor"
    assert g["pitch"] - g["dot_dia"] >= 0.15, "dot-to-dot clearance floor"
    for name, polys, lo in (("gold", gold, 3), ("silk", silk, 4), ("fr4", fr4, 1)):
        assert len(polys) >= lo, f"{name} art missing (got {len(polys)})"
        for p in polys:
            for x, y in p["outline"]:
                assert -0.1 <= x <= lay["panel"]["w"] + 0.1, f"{name} x out of panel"
                assert -0.1 <= y <= lay["panel"]["h"] + 0.1, f"{name} y out of panel"

STACKUP = """\t\t(stackup
\t\t\t(layer "F.SilkS"
\t\t\t\t(type "Top Silk Screen")
\t\t\t\t(color "White")
\t\t\t)
\t\t\t(layer "F.Mask"
\t\t\t\t(type "Top Solder Mask")
\t\t\t\t(color "Black")
\t\t\t\t(thickness 0.01)
\t\t\t)
\t\t\t(layer "F.Cu"
\t\t\t\t(type "copper")
\t\t\t\t(thickness 0.035)
\t\t\t)
\t\t\t(layer "dielectric 1"
\t\t\t\t(type "core")
\t\t\t\t(thickness 1.51)
\t\t\t\t(material "FR4")
\t\t\t\t(epsilon_r 4.5)
\t\t\t\t(loss_tangent 0.02)
\t\t\t)
\t\t\t(layer "B.Cu"
\t\t\t\t(type "copper")
\t\t\t\t(thickness 0.035)
\t\t\t)
\t\t\t(layer "B.Mask"
\t\t\t\t(type "Bottom Solder Mask")
\t\t\t\t(color "Black")
\t\t\t\t(thickness 0.01)
\t\t\t)
\t\t\t(layer "B.SilkS"
\t\t\t\t(type "Bottom Silk Screen")
\t\t\t\t(color "White")
\t\t\t)
\t\t\t(copper_finish "ENIG")
\t\t\t(dielectric_constraints no)
\t\t)
"""

UUID_NS = uuid.UUID("6f2c6b0a-5b0a-4f3e-9c1e-000000000000")

def derandomize_uuids(path):
    # board.Save() assigns every shape/pad/footprint a fresh random UUID, so
    # two generate.sh runs never hash the same even with identical geometry.
    # Emission order is deterministic, so replace each uuid in file order with
    # one derived from its position -- stable across runs, still unique.
    text = path.read_text()
    counter = [0]
    def repl(m):
        i = counter[0]; counter[0] += 1
        return f'(uuid "{uuid.uuid5(UUID_NS, str(i))}")'
    text = re.sub(r'\(uuid "[0-9a-f-]+"\)', repl, text)
    path.write_text(text)

def inject_stackup(path):
    # BOARD_STACKUP isn't exposed by this pcbnew build's SWIG bindings (no
    # BS_ITEM_TYPE_* enum, GetStackupDescriptor() returns an opaque
    # SwigPyObject with no methods), so the finish/colour can't be set through
    # the object API. Splice the stackup block into the saved file's (setup
    # ...) section directly, matching the format KiCad itself writes.
    text = path.read_text()
    assert "(stackup" not in text
    spliced = text.replace("\t(setup\n", "\t(setup\n" + STACKUP, 1)
    assert "(stackup" in spliced, "splice anchor '\\t(setup\\n' not found - board writer format changed"
    path.write_text(spliced)

def main():
    lay = json.loads((HERE / "panel_layout.json").read_text())
    board = pcbnew.CreateEmptyBoard()
    p, g = lay["panel"], lay["grid"]
    rounded_outline(board, p["w"], p["h"], p["corner_r"])

    gold = json.loads((HERE / "gold.polys.json").read_text())
    silk = json.loads((HERE / "silk.polys.json").read_text())
    fr4 = json.loads((HERE / "fr4.polys.json").read_text())
    check_floors(lay, gold, silk, fr4)
    silk, mask_knockouts = split_mask_knockouts(silk, lay["dots"], g["dot_dia"] / 2)

    for i, d in enumerate(lay["dots"]):
        cu, mf, mb = RECIPE[d["mat"]]
        dia = g["dot_dia"]
        if cu:
            if d["mat"] == "T":
                # Relieve copper around the NPTH: a solid disc drilled through
                # its centre leaves 0mm hole clearance under DRC even though the
                # physical result is the same gold annulus. Draw the annulus
                # directly, sized to clear the board's 0.25mm hole clearance rule.
                filled_ring(board, pcbnew.F_Cu, d["x"], d["y"], dia, g["hole_dia"] + 0.6)
            else:
                filled_circle(board, pcbnew.F_Cu, d["x"], d["y"], dia)
        if mf:
            if i in mask_knockouts:
                mask_knockout(board, d["x"], d["y"], dia, mask_knockouts[i])
            else:
                filled_circle(board, pcbnew.F_Mask, d["x"], d["y"], dia)
        if mb: filled_circle(board, pcbnew.B_Mask, d["x"], d["y"], dia)
        if d["mat"] == "T":
            npth(board, d["x"], d["y"], g["hole_dia"])

    silk_clip = panel_interior(p["w"], p["h"], p["corner_r"], 0.15)
    poly_shape(board, pcbnew.F_Cu, gold)     # gold = copper...
    poly_shape(board, pcbnew.F_Mask, gold)   # ...plus mask opening
    poly_shape(board, pcbnew.F_SilkS, silk, clip=silk_clip)   # excludes mask knockouts
    poly_shape(board, pcbnew.F_Mask, fr4)    # bare FR4 = open both faces
    poly_shape(board, pcbnew.B_Mask, fr4)

    out = HERE / "panel.kicad_pcb"
    board.Save(str(out))
    derandomize_uuids(out)
    inject_stackup(out)
    print("wrote panel.kicad_pcb:", len(lay["dots"]), "dots")

if __name__ == "__main__":
    main()
