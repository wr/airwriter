"""Alignment proof: every panel dot against the part that must sit under it.

Blue circle  = panel dot centre mapped to the board (100 + x_p, 60 + y_p)
Green cross  = placed LED footprint centre
Orange ring  = the test dot (panel 31,31 -> board 131,91)
Orange cross = SW3's PLUNGER centre, read off the footprint's F.Fab circle.
Earlier rounds plotted SW3's origin, which is pin 1 and sits 3.95mm away -- that
is exactly the defect this proof now has to be able to show.
"""
import sys, json, math
sys.path.insert(0, "/Users/wells/Projects/airwriter/hardware/airwriter/tools_pcb")
import pcbnew, r6_lib as L

SVG = "/tmp/claude-501/-Users-wells-Projects-airwriter/7bc1e39f-0a8b-4cad-8d10-e7d55ee438ae/scratchpad/r6-align.svg"
PANEL = json.load(open("/Users/wells/Projects/airwriter/hardware/panel/panel_layout.json"))
b = L.load()
fps = {f.GetReference(): f for f in b.Footprints()}

dots = PANEL["dots"] if isinstance(PANEL, dict) else PANEL
S, M = 14.0, 26.0            # px per mm, margin
X0, Y0, X1, Y1 = 100.0, 60.0, 162.0, 164.27
W = (X1 - X0) * S + 2 * M
H = (Y1 - Y0) * S + 2 * M
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

for r, (x, y) in placed.items():
    best = min(dots, key=lambda d: math.hypot(100 + d["x"] - x, 60 + d["y"] - y))
    e = math.hypot(100 + best["x"] - x, 60 + best["y"] - y)
    worst = max(worst, e); n += 1
    k = 0.55 * S
    o.append('<path d="M%.2f %.2f L%.2f %.2f M%.2f %.2f L%.2f %.2f" stroke="#0f9d58" stroke-width="2.4"/>'
             % (px(x) - k, py(y), px(x) + k, py(y), px(x), py(y) - k, px(x), py(y) + k))

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
        "LED field: %d/%d placed, worst error %.4f mm" % (n, sum(1 for d in dots if d.get("led")), worst),
        "SW3 plunger at (%.4f, %.4f), test dot (131.0000, 91.0000), error %.4f mm" % (pl[0], pl[1], err),
        "SW3 origin at (%.2f, %.2f) -- 3.95 mm from the plunger, which is why it must not be the check" % (ox, oy)]):
    o.append('<text x="%.1f" y="%.1f" fill="#1a202c" font-family="monospace" font-size="17">%s</text>'
             % (M, H - M - 44 + i * 21, line))
o.append("</svg>")
open(SVG, "w").write("\n".join(o))
print("LED worst error %.4f mm over %d parts; SW3 plunger error %.4f mm" % (worst, n, err))
print("wrote", SVG)
