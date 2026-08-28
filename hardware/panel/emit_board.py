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

def filled_ring(board, layer, x, y, od, idia, segs=64, net=None):
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
    if net is not None: s.SetNetCode(net.GetNetCode())
    board.Add(s)

def pth(board, x, y, dia, pad_dia, net=None):
    # Plated, netless: the hole wall plates copper and takes ENIG, so the bore
    # itself reads gold ("a big via" - Wells, 2026-08-28). The pad's annulus
    # carries the gold right to the drill edge; the art annulus takes over at
    # pad_dia. Mask apertures are NOT drawn by the pad (copper layers only) -
    # the mask_knockout graphic owns the front opening so the dark test-tag
    # glyphs survive, and the back annulus stays tented (hidden face).
    fp = pcbnew.FOOTPRINT(board)
    fp.SetPosition(pcbnew.VECTOR2I(mm(x), mm(y)))
    fp.SetReference("H_TEST"); fp.Reference().SetVisible(False)
    fp.SetExcludedFromPosFiles(True); fp.SetExcludedFromBOM(True)  # a hole, not a part
    pad = pcbnew.PAD(fp)
    pad.SetAttribute(pcbnew.PAD_ATTRIB_PTH)
    pad.SetShape(pcbnew.PAD_SHAPE_CIRCLE)
    pad.SetSize(pcbnew.VECTOR2I(mm(pad_dia), mm(pad_dia)))
    pad.SetDrillSize(pcbnew.VECTOR2I(mm(dia), mm(dia)))
    pad.SetPosition(pcbnew.VECTOR2I(mm(x), mm(y)))
    pad.SetLayerSet(pcbnew.LSET.AllCuMask(2))
    if net is not None: pad.SetNetCode(net.GetNetCode())
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

def poly_shape(board, layer, polys, clip=None, net=None):
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
        if net is not None: s.SetNetCode(net.GetNetCode())
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
        # 1.45r reach: Frame 26 hangs the test glyphs on a squared-off tab
        # whose far corner sits at r*sqrt(2); nothing else silk-like comes
        # within 1.45r of a gold dot (the lockup is >300px away).
        owner = next((i for i in gold_idxs
                      if all(math.hypot(x - dots[i]["x"], y - dots[i]["y"]) < dot_r * 1.45
                             for x, y in poly["outline"])), None)
        if owner is None:
            remaining.append(poly)
        else:
            knockouts.setdefault(owner, []).append(poly)
    return remaining, knockouts

def mask_knockout(board, cx, cy, dia, glyphs, keep_dia=None):
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
    # Boolean subtract, not AddHole: Frame 26 lets glyphs cross the disc edge
    # (they continue onto the gold tab), and a "hole" poking outside its
    # outline is invalid geometry that silently voids the whole opening.
    cut = pcbnew.SHAPE_POLY_SET()
    if keep_dia:
        # Frame 26 paints the hole surround black: keep mask closed out to the
        # marker circle, so no bare-FR4 ring shows between drill and gold.
        kc = pcbnew.SHAPE_LINE_CHAIN()
        for px, py in _circle_pts(cx, cy, keep_dia / 2):
            kc.Append(mm(px), mm(py))
        kc.SetClosed(True)
        cut.AddOutline(kc)
    for glyph in glyphs:
        hl = pcbnew.SHAPE_LINE_CHAIN()
        for x, y in glyph["outline"]:
            hl.Append(mm(x), mm(y))
        hl.SetClosed(True)
        cut.AddOutline(hl)
    chain.BooleanSubtract(cut)
    chain.Fracture()
    s = pcbnew.PCB_SHAPE(board, pcbnew.SHAPE_T_POLY)
    s.SetPolyShape(chain); s.SetFilled(True); s.SetWidth(0); s.SetLayer(pcbnew.F_Mask)
    board.Add(s)
    for glyph in glyphs:
        for hole in glyph["holes"]:
            poly_shape(board, pcbnew.F_Mask, [dict(outline=hole, holes=[])])

def back_copper_flood(board, p, dots, g, fr4):
    # The black soldermask alone still transmits some teal - without a copper
    # backstop behind it, an LED would glow through the whole back face and
    # bleed between neighbouring dots. Flood B.Cu solid everywhere (behind
    # gold dots, the lockup, all of it) except the windows light actually
    # needs: the 37 translucent (P/B/L) dot openings, oversized by 0.15mm of
    # registration slack so misalignment can't clip the lit edge; the fr4
    # slash's footprint (it sits behind mask, so a plain rect is invisible);
    # and the test NPTH's drill clearance.
    #
    # Inset 0.5mm (not the 0.25mm first specified) - this board's default
    # copper-to-edge clearance rule is 0.5mm, and a 0.25mm inset failed DRC
    # outright regardless of polygon precision. +0.05mm extra margin absorbs
    # the rounded corners' own arc-sampling error.
    flood = panel_interior(p["w"], p["h"], p["corner_r"], 0.55)
    for d in dots:
        if d["mat"] in ("P", "B", "L"):
            r, segs = (g["dot_dia"] + 0.15) / 2, 64
        elif d["mat"] == "T":
            # The test hole is a plated pad now (copper both sides), so the
            # flood window clears the PAD, not the drill: pad diameter plus
            # 0.5mm keeps 0.25mm of graphic-to-pad clearance. 256 segments so
            # the polygon's sagitta error doesn't eat the margin (it did at 64).
            r, segs = (g["hole_dia"] + 0.6 + 0.5) / 2, 256
        else:
            continue
        hl = pcbnew.SHAPE_LINE_CHAIN()
        for px, py in _circle_pts(d["x"], d["y"], r, segs):
            hl.Append(mm(px), mm(py))
        hl.SetClosed(True)
        flood.AddHole(hl)
    for slash in fr4:
        xs = [x for x, y in slash["outline"]]
        ys = [y for x, y in slash["outline"]]
        x0, y0 = min(xs) - 0.2, min(ys) - 0.2
        x1, y1 = max(xs) + 0.2, max(ys) + 0.2
        hl = pcbnew.SHAPE_LINE_CHAIN()
        for x, y in [(x0, y0), (x1, y0), (x1, y1), (x0, y1)]:
            hl.Append(mm(x), mm(y))
        hl.SetClosed(True)
        flood.AddHole(hl)
    hole_count = flood.HoleCount(0)
    flood.Fracture()
    s = pcbnew.PCB_SHAPE(board, pcbnew.SHAPE_T_POLY)
    s.SetPolyShape(flood); s.SetFilled(True); s.SetWidth(0); s.SetLayer(pcbnew.B_Cu)
    board.Add(s)
    return hole_count

# Reflow standoff seats: blind SMT nuts (PEM SMTSO-M25-3ET or equivalent,
# verify stock at order) solder to the back-copper flood so M2.5 screws come
# up from the case floor through the main board's mounting holes. Coordinates
# are the main board's H1-H4 minus its (100, 60) origin - the two boards'
# holes must stay coaxial; change them together or not at all.
#
# Round 7 spread them to the corners: H1 and H2 now sit directly behind panel
# dots (0,0) and (0,6), both gold in Frame 9b, so the flood is already solid
# there and no exception is needed. A seat over a flood window would have no
# copper to reflow onto, which is what check_seats proves against the real
# hole list rather than by inspection.
STANDOFFS = [(5.6364, 5.6364), (56.3636, 5.6364), (5.6364, 91.93), (53.3636, 91.93)]
STANDOFF_SEAT_DIA = 6.0     # exposed ENIG seat; Wurth wants a 6.0mm land
STANDOFF_PASTE_OD = 5.8     # paste is a RING: solid paste would wick into the
STANDOFF_PASTE_ID = 3.9     # bottom-resident M2.5 thread (bore ~3.5)

# Wurth WA-SMSI 9774030151R: M2.5 x 3.0mm SMT steel spacer, FLAT bottom, no
# pilot boss, so it solders to the solid back flood with no hole in the art
# face -- the only M2.5x3 part in distribution that can (PEM SMTSO and all
# clones need a 4.2mm hole). Digi-Key ~48k stock, ~$0.60 @ reel. Screw spec
# for the case plan: tip must stop ~0.5mm short of the seat -- the thread
# lives in the bottom ~1.8mm of the bore.
STANDOFF_PART = "9774030151R"
STANDOFF_LCSC = ""          # not an LCSC part; PCBWay sources from Digi-Key

def standoff_pads(board):
    # Real footprints (MT1-MT4), not bare mask windows, so the standoff nuts
    # appear in the BOM and pick-and-place files and PCBWay can reflow them.
    # One SMD circle pad on the back: 5.0mm copper/mask seat, paste pulled in
    # to 4.6mm via a -0.2mm local paste margin. The pad overlaps the graphic
    # back-copper flood, which merges in the gerbers - no clearance carving,
    # the seat stays part of the light barrier.
    for i, (x, y) in enumerate(STANDOFFS, 1):
        fp = pcbnew.FOOTPRINT(board)
        fp.SetPosition(pcbnew.VECTOR2I(mm(x), mm(y)))
        fp.SetReference(f"MT{i}"); fp.Reference().SetVisible(False)
        fp.SetValue(STANDOFF_PART); fp.Value().SetVisible(False)
        fp.SetFPIDAsString("panel:SMT_Standoff_M2.5_D6.0mm")
        fp.SetAttributes(pcbnew.FP_SMD)
        if STANDOFF_LCSC:
            fp.SetField("LCSC", STANDOFF_LCSC)
        # Aperture pad: mask + paste only, NO copper layer. The copper seat is
        # the graphic back flood itself (already solid here, check_seats
        # asserts it); adding pad copper on top reads as a short to DRC.
        pad = pcbnew.PAD(fp)
        pad.SetAttribute(pcbnew.PAD_ATTRIB_SMD)
        pad.SetShape(pcbnew.PAD_SHAPE_CIRCLE)
        pad.SetSize(pcbnew.VECTOR2I(mm(STANDOFF_SEAT_DIA), mm(STANDOFF_SEAT_DIA)))
        pad.SetPosition(pcbnew.VECTOR2I(mm(x), mm(y)))
        ls = pcbnew.LSET()
        ls.AddLayer(pcbnew.B_Mask)
        pad.SetLayerSet(ls)
        fp.Add(pad)
        m = pcbnew.FP_3DMODEL()
        m.m_Filename = ("${KICAD10_3DMODEL_DIR}/Mounting_Wuerth.3dshapes/"
                        "Mounting_Wuerth_WA-SMSI-M2.5_H3mm_9774030151.step")
        fp.Models().push_back(m)
        # ring paste aperture as board graphics (a single pad cannot hold a
        # donut); the stencil gerber reads B.Paste either way
        filled_ring(board, pcbnew.B_Paste, x, y, STANDOFF_PASTE_OD, STANDOFF_PASTE_ID)
        fp.SetLayer(pcbnew.B_Cu)
        board.Add(fp)

def check_seats(p, dots, g, fr4):
    # Every seat must land on solid back copper: no dot window, no slash relief,
    # and far enough in from the outline that the flood's 0.55mm inset has not
    # eaten it.
    r = STANDOFF_SEAT_DIA / 2
    for x, y in STANDOFFS:
        assert (r + 0.55 <= x <= p["w"] - r - 0.55
                and r + 0.55 <= y <= p["h"] - r - 0.55), f"seat ({x},{y}) off the flood"
        for d in dots:
            if d["mat"] in ("P", "B", "L"):
                hole_r = (g["dot_dia"] + 0.15) / 2
            elif d["mat"] == "T":
                hole_r = (g["hole_dia"] + 0.6 + 0.5) / 2
            else:
                continue
            assert math.hypot(x - d["x"], y - d["y"]) > r + hole_r, \
                f"seat ({x},{y}) overlaps dot ({d['r']},{d['c']})'s flood window"
        for slash in fr4:
            xs = [q for q, _ in slash["outline"]]
            ys = [q for _, q in slash["outline"]]
            assert (x + r < min(xs) - 0.2 or x - r > max(xs) + 0.2
                    or y + r < min(ys) - 0.2 or y - r > max(ys) + 0.2), \
                f"seat ({x},{y}) overlaps the slash relief"

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
    check_seats(p, lay["dots"], g, fr4)
    silk, mask_knockouts = split_mask_knockouts(silk, lay["dots"], g["dot_dia"] / 2)

    # One decorative net ties the test dot's contacting copper (plated pad,
    # annulus ring, tab art) together, so DRC reads the contact as one node
    # instead of "items shorting two nets". Everything else stays netless.
    au = pcbnew.NETINFO_ITEM(board, "PANEL_AU")
    board.Add(au)
    tdot = next(d for d in lay["dots"] if d["mat"] == "T")

    for i, d in enumerate(lay["dots"]):
        cu, mf, mb = RECIPE[d["mat"]]
        dia = g["dot_dia"]
        if cu:
            if d["mat"] == "T":
                # The plated pad carries copper from the drill edge out to
                # hole_dia+0.6; the art annulus takes over from there to the
                # dot edge, meeting the pad rim with no gap. Gold is continuous
                # from the bore to the dot edge.
                filled_ring(board, pcbnew.F_Cu, d["x"], d["y"], dia,
                            g["hole_dia"] + 0.6, net=au)
            else:
                filled_circle(board, pcbnew.F_Cu, d["x"], d["y"], dia)
        if mf:
            if i in mask_knockouts:
                # keep_dia is gone: no black surround at the test hole any
                # more - mask opens all the way to the plated bore, so the
                # gold runs to the edge (decision, Wells 2026-08-28).
                mask_knockout(board, d["x"], d["y"], dia, mask_knockouts[i], None)
            else:
                filled_circle(board, pcbnew.F_Mask, d["x"], d["y"], dia)
        if mb: filled_circle(board, pcbnew.B_Mask, d["x"], d["y"], dia)
        if d["mat"] == "T":
            pth(board, d["x"], d["y"], g["hole_dia"], g["hole_dia"] + 0.6, net=au)

    silk_clip = panel_interior(p["w"], p["h"], p["corner_r"], 0.15)
    # Gold art may run over the test NPTH (Frame 26 squares the test dot off
    # with a solid tab); clip the COPPER around the drill so hole clearance
    # holds. The mask stays unclipped - the annulus is what the art shows.
    gold_cu_clip = panel_interior(p["w"], p["h"], p["corner_r"], 0.0)
    hole_relief = pcbnew.SHAPE_POLY_SET()
    hc = pcbnew.SHAPE_LINE_CHAIN()
    for px_, py_ in _circle_pts(tdot["x"], tdot["y"], (g["hole_dia"] + 0.6) / 2, 128):
        hc.Append(mm(px_), mm(py_))
    hc.SetClosed(True)
    hole_relief.AddOutline(hc)
    gold_cu_clip.BooleanSubtract(hole_relief)
    import math as _m
    t_gold = [q for q in gold
              if any(_m.hypot(x - tdot["x"], y - tdot["y"]) < g["dot_dia"]
                     for x, y in q["outline"])]
    rest_gold = [q for q in gold if q not in t_gold]
    poly_shape(board, pcbnew.F_Cu, rest_gold, clip=gold_cu_clip)  # gold = copper...
    poly_shape(board, pcbnew.F_Cu, t_gold, clip=gold_cu_clip, net=au)  # tab touches the pad
    # The gold-art mask opening must keep mask OVER the knockout glyph
    # strokes (they may sit on the tab, outside the disc that mask_knockout
    # already handles) - subtract every knockout glyph from the opening.
    mask_clip = panel_interior(p["w"], p["h"], p["corner_r"], 0.0)
    glyph_cut = pcbnew.SHAPE_POLY_SET()
    for glyphs_ in mask_knockouts.values():
        for glyph_ in glyphs_:
            gl = pcbnew.SHAPE_LINE_CHAIN()
            for x_, y_ in glyph_["outline"]:
                gl.Append(mm(x_), mm(y_))
            gl.SetClosed(True)
            glyph_cut.AddOutline(gl)
    mask_clip.BooleanSubtract(glyph_cut)
    poly_shape(board, pcbnew.F_Mask, gold, clip=mask_clip)   # ...plus mask opening
    poly_shape(board, pcbnew.F_SilkS, silk, clip=silk_clip)   # excludes mask knockouts
    poly_shape(board, pcbnew.F_Mask, fr4)    # bare FR4 = open both faces
    poly_shape(board, pcbnew.B_Mask, fr4)
    flood_holes = back_copper_flood(board, p, lay["dots"], g, fr4)
    standoff_pads(board)

    out = HERE / "panel.kicad_pcb"
    board.Save(str(out))
    derandomize_uuids(out)
    inject_stackup(out)
    print("wrote panel.kicad_pcb:", len(lay["dots"]), "dots,",
          flood_holes, "back-copper flood holes")

if __name__ == "__main__":
    main()
