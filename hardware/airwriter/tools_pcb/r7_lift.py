"""Round 7, second pass: lift the module so H4 can take the designer's corner.

The designer wants the two lower mounts LEVEL, on the same columns as the upper
pair: H3 where J2 allows it and H4 on H2's column at the same y. Nothing fits
there while U1 sits where it does, so the module moves north by the minimum that
opens the seat -- not by an estimate, by arithmetic:

    H4's y = H3's y, read off H3. Level is the requirement, so it is derived
    rather than typed, and J2 (which caps H3) ends up setting both.

    U1's lift is then whatever puts its courtyard 0.2mm clear of H4's:
        lift = U1_courtyard_bottom - (H4_top - 0.2)

    J1 still caps any hole overlapping it in x at y <= 154.214, but the level
    pair sits north of that, so J1 stops being the binding constraint.

The WROOM ANTENNA KEEPOUT travels with the module, and then two edits:

  north  clamped to the grid-zone boundary at y=127.6 rather than riding the
         full lift, which keeps it out of the LED field and clear of dot
         (7,6)'s LED. Costs a little of the module's north RF margin.
  south-east  notched around H4's courtyard. THIS IS THE RF-SIGNIFICANT ONE and
         the report carries the number: it takes copper clearance off the
         antenna's south edge on a ~3mm-wide strip. The old J1 notch is dropped
         -- after the lift the keepout no longer reaches J1 at all.

Nothing here re-solves the parts the lift displaces; r7_lift_fit.py does that.
"""
import sys
sys.path.insert(0, "/Users/wells/Projects/airwriter/hardware/airwriter/tools_pcb")
import pcbnew
import r6_lib as L

GAP = 0.2
GRID_SOUTH = 127.6
H4_X = 156.3636          # panel dot (0,6)'s column, the same as H2
# The lower pair sits level, which is the designer's drawn intent, so H4's y is
# READ OFF H3 rather than typed here -- if J2 ever moves and H3 with it, the two
# stay level by construction instead of by someone remembering to edit both.
H4_SHARE_Y_WITH = "H3"

b = L.load()
fps = {f.GetReference(): f for f in b.Footprints()}


def cy(ref):
    return L.cy_bbox(fps[ref])


def half(ref):
    x0, y0, x1, y1 = cy(ref)
    p = fps[ref].GetPosition()
    return (L.mm(p.x) - x0, x1 - L.mm(p.x), L.mm(p.y) - y0, y1 - L.mm(p.y))


j1, u1 = cy("J1"), cy("U1")
_, h4_rt, h4_up, h4_dn = half("H4")
h4_y = round(L.mm(fps[H4_SHARE_Y_WITH].GetPosition().y), 3)
lift = round(u1[3] - (h4_y - h4_up - GAP), 3)

print(f"H4 shares {H4_SHARE_Y_WITH}'s y -> ({H4_X:.3f}, {h4_y:.3f}); the lower pair is level")
print(f"  J1 alone would have allowed y <= {j1[1] - GAP - h4_dn:.3f}, so J1 is no "
      f"longer the binding constraint -- U1 is, and U1 can move")
print(f"U1 courtyard bottom {u1[3]:.3f}  ->  lift {lift:.3f}mm north")

# --- the module
u1fp = fps["U1"]
p = u1fp.GetPosition()
u1_y = round(L.mm(p.y) - lift, 3)
fab_before = [(L.mm(i.GetStart().y), L.mm(i.GetEnd().y)) for i in u1fp.GraphicalItems()
              if i.GetClass() == "PCB_SHAPE" and i.GetLayer() == pcbnew.F_Fab]
u1fp.SetPosition(pcbnew.VECTOR2I(p.x, L.nm(u1_y)))
u1n = cy("U1")
print(f"U1 ({L.mm(p.x):.3f}, {u1_y:.3f})  cy [{u1n[0]:.3f},{u1n[1]:.3f} - "
      f"{u1n[2]:.3f},{u1n[3]:.3f}]")

# the antenna's own extent, read off F.Fab rather than assumed
fab = [i for i in u1fp.GraphicalItems()
       if i.GetClass() == "PCB_SHAPE" and i.GetLayer() == pcbnew.F_Fab]
fx = sorted({round(v, 3) for i in fab for v in (L.mm(i.GetStart().x), L.mm(i.GetEnd().x))})
fy = sorted({round(v, 3) for i in fab for v in (L.mm(i.GetStart().y), L.mm(i.GetEnd().y))})
ant_x0, ant_y0, ant_y1 = fx[-2], fy[0], fy[-1]
print(f"antenna section: x {ant_x0:.3f}..{fx[-1]:.3f}, y {ant_y0:.3f}..{ant_y1:.3f}")

# --- H4 into the corner
fps["H4"].SetPosition(pcbnew.VECTOR2I(L.nm(H4_X), L.nm(h4_y)))
h4 = cy("H4")
print(f"H4 cy [{h4[0]:.3f},{h4[1]:.3f} - {h4[2]:.3f},{h4[3]:.3f}] ; "
      f"gap to U1 {h4[1] - u1n[3]:.3f}mm, to J1 {j1[1] - h4[3]:.3f}mm")

# --- the keepout. It is owned by U1's footprint, so SetPosition has ALREADY
# carried it north with the module -- the coordinates read back here are the
# translated ones, and subtracting the lift again is the bug this line exists to
# name. What is left to do is clamp its north edge and notch its south-east.
zone = next(z for f in b.Footprints() for z in f.Zones() if z.GetIsRuleArea())
o = zone.Outline().Outline(0)
old = [(L.mm(o.CPoint(i).x), L.mm(o.CPoint(i).y)) for i in range(o.PointCount())]
ox0, ox1 = min(p[0] for p in old), max(p[0] for p in old)
oy0, oy1 = min(p[1] for p in old), max(p[1] for p in old)
south = round(oy1, 3)
notch_x = round(h4[2] + GAP, 3)
notch_y = round(h4[1] - GAP, 3)
new = [(ox0, GRID_SOUTH), (ox1, GRID_SOUTH), (ox1, south),
       (notch_x, south), (notch_x, notch_y), (ox0, notch_y)]
poly = pcbnew.SHAPE_POLY_SET()
poly.NewOutline()
for x, y in new:
    poly.Append(L.nm(x), L.nm(y))
zone.Outline().RemoveAllContours()
zone.Outline().NewOutline()
for x, y in new:
    zone.Outline().Append(L.nm(x), L.nm(y))

print(f"keepout after riding the module north: "
      f"{tuple(round(v, 3) for v in (ox0, oy0, ox1, oy1))}, still notched for J1")
print(f"keepout now  {new}")
print(f"  north edge clamped to the grid boundary {GRID_SOUTH}: "
      f"{GRID_SOUTH - oy0:.3f}mm given up, leaving {ant_y0 - GRID_SOUTH:.3f}mm "
      f"of RF margin north of the antenna (rule of thumb is 5)")
print(f"  south-east notched for H4 to x<={notch_x:.3f}, y>={notch_y:.3f}: "
      f"on that {notch_x - ox0:.3f}mm-wide strip the antenna's SOUTH clearance "
      f"goes {oy1 - ant_y1:.3f}mm -> {notch_y - ant_y1:.3f}mm. THIS IS THE "
      f"RF-SIGNIFICANT EDIT -- see the report.")
print(f"  J1 notch dropped -- the keepout now stops {j1[1] - south:.3f}mm north of J1")

# --- what the lift displaced
hit = []
for ref in fps:
    if ref in ("U1", "H4"):
        continue
    for r in (L.courtyard_rects(fps[ref]) or [L.cy_bbox(fps[ref])]):
        if not (u1n[0] >= r[2] or r[0] >= u1n[2] or u1n[1] >= r[3] or r[1] >= u1n[3]):
            hit.append(ref)
print(f"displaced by the lift: {sorted(set(hit))}")

pcbnew.SaveBoard(L.BOARD, b)
print("saved")
