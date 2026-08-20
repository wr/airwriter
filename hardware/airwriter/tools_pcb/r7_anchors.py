"""Round 7 step 1: the anchors the rest of the round is solved around.

Designer mandate: spread the four mounting holes to the board's corners on two
shared columns, and move the west edge controls north into the room Frame 9b's
gold (7,0) corner freed, so the south-west hole has somewhere to live.

  H1  under panel dot (0,0), gold in Frame 9b
  H2  under panel dot (0,6), gold
  H3  the same column as H1, as far south as it goes
  H4  east, in the pocket between J4's and J1's courtyards

H1 and H2 are read straight out of panel_layout.json plus the board origin, so
they are coaxial with the panel's standoff seats by construction rather than by
transcription.

THE LOWER PAIR CANNOT SIT ON THE LABEL RULE AT y=155, AND CANNOT SHARE A y.
That is arithmetic, not preference, and the numbers are printed at the end of
this script every time it runs:

  west column, x=105.636 -> hole courtyard x 103.189..108.081.
      J2's courtyard starts at y=154.675 and runs to the board edge, and it
      spans x 104.205..121.795, so it is under this whole column. A 4.882mm
      courtyard clearing it by 0.2mm caps the hole at y <= 152.034. y=155 puts
      the drill inside the printer connector. The designer's +-1.5mm is not
      enough; the shortfall is 2.97mm.

  east column, x=156.364 -> hole courtyard x 153.916..158.809.
      U1's courtyard covers y 134.855..153.845 there, so the hole must sit at
      y >= 156.486 to clear it. J1's courtyard starts at y=156.855, so the hole
      must sit at y <= 154.214 to clear that. The band is empty: no y works at
      any point on this column.

      Nudging east does not rescue it. J1 reaches x=157.315, so any hole whose
      courtyard overlaps J1 in x is still capped at y <= 154.214 -- and the
      WROOM's antenna occupies x 156.0..162.0, y 135.35..153.35 (read off U1's
      F.Fab). A 2.75mm drill at y=154.214 reaches y=152.84, which is 0.51mm
      UNDER the radiating element. Clearing J1 in x instead needs the courtyard
      east of 157.515, i.e. a centre at x >= 159.96, whose courtyard ends at
      162.408 -- off the board.

      The controller's authorisation was for metal under the antenna's
      OVERHANG. There is no overhang: Wells ruled the module flush in round 6
      and the antenna now sits over solid board. A screw and a metal standoff
      0.5mm under the radiating element is a different RF question, so H4 goes
      to the J4/J1 pocket and the choice goes back to the designer.

The west edge is one vertical stack -- S1, SW1, H3 -- pinned at the bottom by
J2's courtyard and packed upward at a uniform 0.3mm, which is this board's
established gap in the crowded rows. Every y below is derived from that pin and
each part's real courtyard half-height, printed at the end so the arithmetic is
checkable rather than asserted. x is untouched for S1 and SW1: the mandate keeps
the DIP's body face 0.2mm off x=100 and the reset actuator flush at x=100.
"""
import json
import sys
sys.path.insert(0, "/Users/wells/Projects/airwriter/hardware/airwriter/tools_pcb")
import pcbnew
import r6_lib as L

LAYOUT = "/Users/wells/Projects/airwriter/hardware/panel/panel_layout.json"
ORIGIN = (100.0, 60.0)
GAP = 0.3          # the packing gap this board already uses in its tight rows
EDGE_INSET = 1.0   # a mounting hole's courtyard stays this far inside the outline
SOUTH_EDGE = 164.27

b = L.load()
fps = {f.GetReference(): f for f in b.Footprints()}
dots = {(d["r"], d["c"]): d for d in json.loads(open(LAYOUT).read())["dots"]}


def cy(ref):
    return L.cy_bbox(fps[ref])


def half(ref):
    x0, y0, x1, y1 = cy(ref)
    p = fps[ref].GetPosition()
    return (L.mm(p.x) - x0, x1 - L.mm(p.x), L.mm(p.y) - y0, y1 - L.mm(p.y))


def move(ref, x, y, rot=None):
    f = fps[ref]
    if rot is not None:
        f.SetOrientationDegrees(rot)
    f.SetPosition(pcbnew.VECTOR2I(L.nm(x), L.nm(y)))


def dot_board(rc):
    d = dots[rc]
    return ORIGIN[0] + d["x"], ORIGIN[1] + d["y"]


# --- the two grid holes, straight off the panel grid
h1 = dot_board((0, 0))
h2 = dot_board((0, 6))
assert not any(f.GetReference().startswith("D") and
               abs(L.mm(f.GetPosition().x) - h1[0]) < 0.01 and
               abs(L.mm(f.GetPosition().y) - h1[1]) < 0.01 for f in b.Footprints()), \
    "(0,0) still carries an LED -- run the retirement first"

# --- the west stack, packed up from J2. H3 is as far south as J2 allows, on
# H1's column exactly; S1 and SW1 stack above it at the same 0.3mm.
j2_top = cy("J2")[1]
_, _, h3_up, h3_dn = half("H3")
_, _, sw1_up, sw1_dn = half("SW1")
_, _, s1_up, s1_dn = half("S1")

h3_y = j2_top - GAP - h3_dn
sw1_y = h3_y - h3_up - GAP - sw1_dn
s1_y = sw1_y - sw1_up - GAP - s1_dn

# --- the east pocket. H4's x is not a choice: J4's and J1's courtyards leave a
# 5.39mm slot for a 4.89mm courtyard, so the hole is centred in it and the
# 0.25mm either side is the whole budget. U5 goes as far north as U1's courtyard
# allows (the mandate's "small amount"), which is what buys H4 its southward
# room; H4 then sits as far south as the outline inset permits.
j4_e, j1_w = cy("J4")[2], cy("J1")[0]
h4_lf, h4_rt, h4_up, h4_dn = half("H4")
h4_x = (j4_e + j1_w) / 2 - (h4_lf - h4_rt) / 2
u1_bot = cy("U1")[3]
_, _, u5_up, u5_dn = half("U5")
u5_y = u1_bot + 0.22 + u5_up
h4_y = SOUTH_EDGE - EDGE_INSET - h4_dn

# Solved values get snapped to 0.01mm. The derivation is what matters, not its
# last digit, and round numbers are what the case plan and the panel generator
# have to carry -- the two boards' holes must stay coaxial, so a 0.0002mm tail
# is noise in two files instead of one. Each snap is checked below, not assumed.
def snap(v):
    return round(v, 2)


MOVES = [
    ("H1", h1[0], h1[1], None),      # exactly on the panel dot, not snapped
    ("H2", h2[0], h2[1], None),
    ("H3", h1[0], snap(h3_y), None),      # same x column as H1
    ("H4", snap(h4_x), snap(h4_y), None),
    ("S1", L.mm(fps["S1"].GetPosition().x), snap(s1_y), None),
    ("SW1", L.mm(fps["SW1"].GetPosition().x), snap(sw1_y), None),
    ("U5", L.mm(fps["U5"].GetPosition().x), snap(u5_y), None),
]

TARGET_LOWER_Y = 155.0     # the designer's label rule, for the shortfall report
print(f"J2 courtyard top   {j2_top:.3f}   -> the west stack is packed up from here")
for ref, x, y, rot in MOVES:
    before = cy(ref)
    move(ref, x, y, rot)
    a = cy(ref)
    print(f"  {ref:4s} ({x:8.3f}, {y:8.3f})  cy [{a[0]:7.3f},{a[1]:7.3f} - "
          f"{a[2]:7.3f},{a[3]:7.3f}]   dy {y - (before[1] + before[3]) / 2:+7.3f}")

for a, c in (("S1", "SW1"), ("SW1", "H3"), ("H3", "J2"), ("U5", "H4")):
    g = cy(c)[1] - cy(a)[3]
    assert g >= GAP - 0.02, (a, c, g)
    print(f"  gap {a:4s} -> {c:4s} {g:6.3f}mm")
assert SOUTH_EDGE - cy("H4")[3] >= EDGE_INSET - 0.02, cy("H4")
print(f"H3 is {TARGET_LOWER_Y - snap(h3_y):.3f}mm north of the label rule at "
      f"y={TARGET_LOWER_Y} -- J2's courtyard is the whole of it")
_u1, _j1 = cy("U1"), cy("J1")
print(f"east column x=156.364: clear of U1 needs y>={_u1[3] + GAP + h3_up:.3f}, "
      f"clear of J1 needs y<={_j1[1] - GAP - h3_dn:.3f} -- empty band, no hole fits")
print(f"H4 pocket: J4 east {j4_e:.3f} .. J1 west {j1_w:.3f} = {j1_w - j4_e:.3f}mm "
      f"for a {h4_lf + h4_rt:.3f}mm courtyard -> {(j1_w - j4_e) - (h4_lf + h4_rt):.3f}mm of slack")
print(f"U5 to U1 courtyard: {cy('U5')[1] - u1_bot:.3f}mm ; "
      f"H4 to U5: {cy('H4')[1] - cy('U5')[3]:.3f}mm ; "
      f"H4 inside the south edge: {SOUTH_EDGE - cy('H4')[3]:.3f}mm")

pcbnew.SaveBoard(L.BOARD, b)
print("saved")
