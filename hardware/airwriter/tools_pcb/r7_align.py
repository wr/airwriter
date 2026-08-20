"""Alignment proof: every panel dot against the part that must sit under it.

Blue circle  = panel dot centre mapped to the board (100 + x_p, 60 + y_p)
Green cross  = placed LED footprint centre
Red ring     = a led:true dot with no LED under it -- there must be none
Purple ring  = a mounting hole, drawn at its real 2.75mm drill, with the panel's
               standoff seat (5.0mm) dashed around it
Orange ring  = the test dot (panel 31,31 -> board 131,91)
Orange cross = SW3's PLUNGER centre, read off the footprint's F.Fab circle.
Earlier rounds plotted SW3's origin, which is pin 1 and sits 3.95mm away -- that
is exactly the defect this proof now has to be able to show.
"""
import sys, json, math
sys.path.insert(0, "/Users/wells/Projects/airwriter/hardware/airwriter/tools_pcb")
import pcbnew, r6_lib as L

SVG = "/private/tmp/claude-501/-Users-wells-Projects-airwriter/7bc1e39f-0a8b-4cad-8d10-e7d55ee438ae/scratchpad/r7-align.svg"
PANEL = json.load(open("/Users/wells/Projects/airwriter/hardware/panel/panel_layout.json"))
b = L.load()
fps = {f.GetReference(): f for f in b.Footprints()}

dots = PANEL["dots"] if isinstance(PANEL, dict) else PANEL
S, M = 14.0, 26.0            # px per mm, margin
X0, Y0, X1, Y1 = 100.0, 60.0, 162.0, 164.27
W = (X1 - X0) * S + 2 * M + 150  # room for the H-labels and the caption lines
H = (Y1 - Y0) * S + 2 * M + 96   # room for the caption below the board
def px(x): return M + (x - X0) * S
def py(y): return M + (y - Y0) * S

o = ['<svg xmlns="http://www.w3.org/2000/svg" width="%.0f" height="%.0f">' % (W, H),
     '<rect x="0" y="0" width="%.0f" height="%.0f" fill="#ffffff"/>' % (W, H),
     '<rect x="%.1f" y="%.1f" width="%.1f" height="%.1f" rx="%.1f" fill="none" stroke="#334155" stroke-width="2"/>'
     % (px(X0), py(Y0), (X1 - X0) * S, (Y1 - Y0) * S, 2.0 * S)]

# placed LED centres
placed = {}
for r, f in fps.items():
    if r.startswith("D") and r[1:].isdigit() and int(r[1:]) >= 10:
        p = f.GetPosition(); placed[r] = (L.mm(p.x), L.mm(p.y))

worst = 0.0
n = 0
for d in dots:
    bx, by = 100.0 + d["x"], 60.0 + d["y"]
    if d.get("led"):
        o.append('<circle cx="%.2f" cy="%.2f" r="%.1f" fill="none" stroke="#1d6fd6" stroke-width="2.2"/>'
                 % (px(bx), py(by), 0.9 * S))
    else:
        o.append('<circle cx="%.2f" cy="%.2f" r="%.1f" fill="none" stroke="#c0c8d4" stroke-width="1.4"/>'
                 % (px(bx), py(by), 0.6 * S))

# a led:true dot with nothing under it is the round's headline change, so it is
# drawn as a miss rather than left to be inferred from an absent green cross
unlit = []
for d in dots:
    if not d.get("led"):
        continue
    bx, by = 100.0 + d["x"], 60.0 + d["y"]
    if not any(math.hypot(x - bx, y - by) < 0.01 for x, y in placed.values()):
        unlit.append((d["r"], d["c"], bx, by))

for r, (x, y) in placed.items():
    best = min(dots, key=lambda d: math.hypot(100 + d["x"] - x, 60 + d["y"] - y))
    e = math.hypot(100 + best["x"] - x, 60 + best["y"] - y)
    worst = max(worst, e); n += 1
    k = 0.55 * S
    o.append('<path d="M%.2f %.2f L%.2f %.2f M%.2f %.2f L%.2f %.2f" stroke="#0f9d58" stroke-width="2.4"/>'
             % (px(x) - k, py(y), px(x) + k, py(y), px(x), py(y) - k, px(x), py(y) + k))

# the four mounting holes, at their real drill, with the panel's seat around them
for hr in ("H1", "H2", "H3", "H4"):
    hp = fps[hr].GetPosition()
    hx, hy = L.mm(hp.x), L.mm(hp.y)
    o.append('<circle cx="%.2f" cy="%.2f" r="%.1f" fill="#efe6fb" stroke="#7c3aed" stroke-width="2.4"/>'
             % (px(hx), py(hy), 1.375 * S))
    o.append('<circle cx="%.2f" cy="%.2f" r="%.1f" fill="none" stroke="#7c3aed" stroke-width="1.2" stroke-dasharray="4 4"/>'
             % (px(hx), py(hy), 2.5 * S))
    o.append('<text x="%.2f" y="%.2f" fill="#5b21b6" font-family="monospace" font-size="15">%s</text>'
             % (px(hx) + 2.8 * S, py(hy) + 5, hr))
hull = [(L.mm(fps[h].GetPosition().x), L.mm(fps[h].GetPosition().y))
        for h in ("H1", "H2", "H4", "H3")]
o.append('<polygon points="%s" fill="none" stroke="#7c3aed" stroke-width="1.4" stroke-dasharray="8 6"/>'
         % " ".join("%.2f,%.2f" % (px(x), py(y)) for x, y in hull))

for _r, _c, bx, by in unlit:   # drawn last so a mounting hole cannot hide one
    o.append('<circle cx="%.2f" cy="%.2f" r="%.1f" fill="none" stroke="#d92b2b" stroke-width="3"/>'
             % (px(bx), py(by), 1.95 * S))

# SW3: courtyard, origin, and the plunger the panel actually has to line up with
sw3 = fps["SW3"]
cy = L.cy_bbox(sw3)
o.append('<rect x="%.2f" y="%.2f" width="%.2f" height="%.2f" fill="none" stroke="#e07b00" stroke-width="1.6" stroke-dasharray="5 4"/>'
         % (px(cy[0]), py(cy[1]), (cy[2] - cy[0]) * S, (cy[3] - cy[1]) * S))
op = sw3.GetPosition(); ox, oy = L.mm(op.x), L.mm(op.y)
o.append('<circle cx="%.2f" cy="%.2f" r="4" fill="#7a4a10" stroke="#e07b00" stroke-width="1.4"/>' % (px(ox), py(oy)))
o.append('<text x="%.2f" y="%.2f" fill="#b06000" font-family="monospace" font-size="15">SW3 origin (pin 1)</text>'
         % (px(ox) + 8, py(oy) + 5))
pl = None
for it in sw3.GraphicalItems():
    if it.GetClass() == "PCB_SHAPE" and it.GetLayer() == pcbnew.F_Fab and it.GetShape() == pcbnew.SHAPE_T_CIRCLE:
        c = it.GetCenter(); pl = (L.mm(c.x), L.mm(c.y), L.mm(it.GetRadius()))
o.append('<circle cx="%.2f" cy="%.2f" r="%.1f" fill="none" stroke="#e07b00" stroke-width="2.2"/>'
         % (px(131), py(91), 0.9 * S))
k = 0.55 * S
o.append('<path d="M%.2f %.2f L%.2f %.2f M%.2f %.2f L%.2f %.2f" stroke="#e07b00" stroke-width="3"/>'
         % (px(pl[0]) - k, py(pl[1]), px(pl[0]) + k, py(pl[1]), px(pl[0]), py(pl[1]) - k, px(pl[0]), py(pl[1]) + k))
o.append('<circle cx="%.2f" cy="%.2f" r="%.1f" fill="none" stroke="#e07b00" stroke-width="1.2" stroke-dasharray="3 3"/>'
         % (px(pl[0]), py(pl[1]), pl[2] * S))

err = math.hypot(pl[0] - 131, pl[1] - 91)
for i, line in enumerate([
        "LED field: %d/%d placed on Frame 9b's map, worst error %.4f mm"
        % (n, sum(1 for d in dots if d.get("led")), worst),
        "SW3 plunger at (%.4f, %.4f), test dot (131.0000, 91.0000), error %.4f mm" % (pl[0], pl[1], err),
        "SW3 origin at (%.2f, %.2f) -- 3.95 mm from the plunger, which is why it must not be the check" % (ox, oy),
        "Mounting holes " + "  ".join("%s (%.3f, %.3f)" % (h, L.mm(fps[h].GetPosition().x), L.mm(fps[h].GetPosition().y)) for h in ("H1", "H2", "H3", "H4")),
        "led:true dots with no LED (red): " + (", ".join("(%d,%d)" % (u[0], u[1]) for u in unlit) or "none"),
        "Gold under the holes: (0,0) and (0,6); gold under the DIP: (7,0)"]):
    o.append('<text x="%.1f" y="%.1f" fill="#1a202c" font-family="monospace" font-size="17">%s</text>'
             % (M, H - 96 + 6 + i * 21, line))
o.append("</svg>")
open(SVG, "w").write("\n".join(o))
assert not unlit, unlit
assert n == 37 and worst < 0.005, (n, worst)
print("LED worst error %.4f mm over %d parts; SW3 plunger error %.4f mm; unlit %s"
      % (worst, n, err, sorted(unlit)))
print("wrote", SVG)
