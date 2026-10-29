#!/usr/bin/env python3
# SPDX-License-Identifier: CERN-OHL-S-2.0
"""svg/lc.svg + panel_layout.json -> airwriter-lc-top.svg, the laser file
for the LC's clear acrylic top.

One sheet of 1.5 mm clear cast acrylic at the art panel's own outline, so it
seats in the tray's pocket exactly where the Pro's top case does. Two layers,
by colour, which LightBurn, xTool and Glowforge all read as separate jobs:

  black fill   engrave (frost): everything gold, white or tan in the art: the
               dots, the test dot, the wordmark and subtitle, the port labels
               and their rules, the power slash
  red hairline cut: the outline and the test button's hole

svg/lc.svg is the LC's own art (Wells's Figma Frame 17: "AirWriter LC",
rev 2026.11, serial and power only). Its gold dots are the Pro's, the ones
that never light; it draws the other 37 in the background's #111111, and they
stay clear, since the LC has no LEDs under them. The slash is frosted so the
power LED under it lights it, as it lights the Pro's bare-FR4 slash. The
outline and the test hole are the panel's (panel_layout.json): the board under
them is the same.

Everything is copied out of the Figma export as vector paths, scaled to mm, so
a re-export of the art flows through on the next run (generate.sh runs this).
"""
import json, math, pathlib, re

HERE = pathlib.Path(__file__).parent
ART = HERE / "svg" / "lc.svg"
OUT = HERE / "airwriter-lc-top.svg"
SLASH = HERE / "lc_slash.polys.json"
"""The frosted power slash's outline, in mm, for the LC frame's light tube
(enclosure/lc_frame.py), whose slit has to be exactly under it."""
MARGIN = 1.0  # mm of empty sheet round the part, so viewers don't clip the cut line
CUT = "#FF0000"
ENGRAVE = "#000000"
GOLD, WHITE, FR4, INK = "#dda857", "white", "#e7ba9b", "#111111"

_NUM = r"[-+]?(?:\d+\.?\d*|\.\d+)(?:[eE][-+]?\d+)?"
# Absolute commands only; Figma writes nothing else. One letter per argument:
# x or y a coordinate, r a radius (both scale), - an arc's rotation or flag.
_ARGS = {"M": "xy", "L": "xy", "H": "x", "V": "y", "C": "xyxyxy", "S": "xyxy",
         "Q": "xyxy", "T": "xy", "A": "rr---xy", "Z": ""}


def _attr(tag, name):
    m = re.search(rf'\b{name}="([^"]+)"', tag)
    return m.group(1) if m else None


def parse_path(d):
    """Path data -> [(cmd, [floats])], one entry per command repetition."""
    out = []
    for cmd, body in re.findall(r"([A-Za-z])([^A-Za-z]*)", d):
        assert cmd in _ARGS, f"relative or unknown path command {cmd!r} in {d[:40]}"
        nums = [float(n) for n in re.findall(_NUM, body)]
        n = len(_ARGS[cmd])
        if n == 0:
            out.append((cmd, []))
            continue
        assert nums and len(nums) % n == 0, f"{cmd} takes {n} numbers, got {nums}"
        for i in range(0, len(nums), n):
            out.append((cmd, nums[i:i + n]))
    return out


def fmt_path(cmds, s):
    """[(cmd, args)] in px -> path data in mm, scaled by s."""
    def num(v):
        return f"{v:.4f}".rstrip("0").rstrip(".")
    parts = []
    for cmd, args in cmds:
        vals = [a if kind == "-" else a * s for a, kind in zip(args, _ARGS[cmd])]
        parts.append(cmd + " ".join(num(v) for v in vals))
    return "".join(parts)


def bbox(cmds):
    """Bounds of a path's points and control points. An arc counts only its
    end points, which is all the tests below need: is it inside a dot, and
    the extent of a mask's rect."""
    xs = [a for c, args in cmds for a, k in zip(args, _ARGS[c]) if k == "x"]
    ys = [a for c, args in cmds for a, k in zip(args, _ARGS[c]) if k == "y"]
    return min(xs), min(ys), max(xs), max(ys)


def circle_path(cx, cy, r):
    return [("M", [cx - r, cy]), ("A", [r, r, 0, 1, 0, cx + r, cy]),
            ("A", [r, r, 0, 1, 0, cx - r, cy]), ("Z", [])]


def flatten(sub, step=0.35):
    """One subpath (lines and cubics; no arcs) -> its ring of points, in px.
    `step` px between samples on a curve: 0.02 mm, a sagitta under 0.001 mm
    on the tightest serif."""
    pts, cur = [], None
    for cmd, a in sub:
        if cmd in "ML":
            cur = (a[0], a[1]); pts.append(cur)
        elif cmd == "H":
            cur = (a[0], cur[1]); pts.append(cur)
        elif cmd == "V":
            cur = (cur[0], a[0]); pts.append(cur)
        elif cmd == "C":
            p0, p1, p2, p3 = cur, (a[0], a[1]), (a[2], a[3]), (a[4], a[5])
            hull = sum(math.dist(u, v) for u, v in ((p0, p1), (p1, p2), (p2, p3)))
            n = max(2, math.ceil(hull / step))
            for k in range(1, n + 1):
                t = k / n; u = 1 - t
                pts.append(tuple(u**3 * p0[i] + 3 * u * u * t * p1[i] + 3 * u * t * t * p2[i]
                                 + t**3 * p3[i] for i in (0, 1)))
            cur = p3
        else:
            assert cmd == "Z", f"can't weld a shape drawn with {cmd}"
    if math.dist(pts[0], pts[-1]) < 1e-9:
        pts.pop()
    return pts


def _area(ring):
    return sum(x1 * y2 - x2 * y1 for (x1, y1), (x2, y2) in zip(ring, ring[1:] + ring[:1])) / 2


def _inside(ring, p):
    x, y = p; hit = False
    for (x1, y1), (x2, y2) in zip(ring, ring[1:] + ring[:1]):
        if (y1 > y) != (y2 > y) and x < (x2 - x1) * (y - y1) / (y2 - y1) + x1:
            hit = not hit
    return hit


def _crossings(p, q):
    """Every point where ring p's edges cross ring q's: (i, j, point)."""
    out = []
    qe = [(j, a, b) for j, (a, b) in enumerate(zip(q, q[1:] + q[:1]))]
    for i, (a, b) in enumerate(zip(p, p[1:] + p[:1])):
        lo, hi = (min(a[0], b[0]), min(a[1], b[1])), (max(a[0], b[0]), max(a[1], b[1]))
        for j, c, d in qe:
            if max(c[0], d[0]) < lo[0] or min(c[0], d[0]) > hi[0] or \
               max(c[1], d[1]) < lo[1] or min(c[1], d[1]) > hi[1]:
                continue
            den = (b[0] - a[0]) * (d[1] - c[1]) - (b[1] - a[1]) * (d[0] - c[0])
            if den == 0:
                continue
            t = ((c[0] - a[0]) * (d[1] - c[1]) - (c[1] - a[1]) * (d[0] - c[0])) / den
            u = ((c[0] - a[0]) * (b[1] - a[1]) - (c[1] - a[1]) * (b[0] - a[0])) / den
            if 0 <= t < 1 and 0 <= u < 1:
                out.append((i, j, (a[0] + t * (b[0] - a[0]), a[1] + t * (b[1] - a[1]))))
    return out


def _outside_arc(ring, x1, x2, other):
    """The run of `ring` between crossings x1 and x2 (each (edge, point)) that
    lies outside `other`, as points from one crossing to the other."""
    n = len(ring)
    fwd = lambda s, e: [ring[k % n] for k in range(s[0] + 1, e[0] + 1 + (n if e[0] < s[0] else 0))]
    for s, e in ((x1, x2), (x2, x1)):
        run = fwd(s, e)
        if run and not _inside(other, run[len(run) // 2]):
            return [s[1]] + run + [e[1]]
    raise AssertionError("weld: neither side of the crossing is outside")


def weld(shapes):
    """Join shapes whose outlines cross into one outline.

    Laser software fills a layer even-odd, so where two marks overlap it leaves
    the overlap clear. In the art the wordmark's A and i touch, at their feet.
    The joined outline goes out as straight segments 0.02 mm long."""
    shapes = list(shapes)
    while True:
        # Dots are drawn with arcs and sit alone on the grid; only art can touch.
        rings = [None if any(c == "A" for s in sh for c, _ in s) else [flatten(s) for s in sh]
                 for sh in shapes]
        outer = [r and max(range(len(r)), key=lambda k: abs(_area(r[k]))) for r in rings]
        box = [r and (min(x for x, _ in r[o]), min(y for _, y in r[o]),
                      max(x for x, _ in r[o]), max(y for _, y in r[o]))
               for r, o in zip(rings, outer)]
        pair = None
        for i in range(len(shapes)):
            for j in range(i + 1, len(shapes)):
                if not (box[i] and box[j]) or box[i][2] < box[j][0] or box[j][2] < box[i][0] \
                        or box[i][3] < box[j][1] or box[j][3] < box[i][1]:
                    continue
                ra, rb = rings[i][outer[i]], rings[j][outer[j]]
                xs = _crossings(ra, rb)
                if xs:
                    pair = i, j, ra, rb, xs
                    break
                assert not _inside(ra, rb[0]) and not _inside(rb, ra[0]), \
                    "one engraved shape sits on another without crossing it"
            if pair:
                break
        if not pair:
            return shapes
        i, j, ra, rb, xs = pair
        assert len(xs) == 2, f"weld handles one overlap per pair, found {len(xs)} crossings"
        (ia, ib, p1), (ja, jb, p2) = xs
        arc_a = _outside_arc(ra, (ia, p1), (ja, p2), rb)
        arc_b = _outside_arc(rb, (ib, p1), (jb, p2), ra)
        if math.dist(arc_a[-1], arc_b[0]) > 1e-6:
            arc_b.reverse()
        ring = arc_a + arc_b[1:-1]
        joined = [("M", list(ring[0]))] + [("L", list(p)) for p in ring[1:]] + [("Z", [])]
        shapes[i] = [joined] + [s for k, s in enumerate(shapes[i]) if k != outer[i]] \
            + [s for k, s in enumerate(shapes[j]) if k != outer[j]]
        del shapes[j]


def read_art(svg):
    """The art's shapes, sorted into what this file needs."""
    masks = {}
    for mid, body in re.findall(r'<mask\b[^>]*\bid="([^"]+)"[^>]*>(.*?)</mask>', svg, re.S):
        masks[mid] = bbox(parse_path(_attr(re.search(r"<path[^>]*>", body).group(0), "d")))
    svg = re.sub(r"<mask\b.*?</mask>", "", svg, flags=re.S)

    circles, paths = [], []
    for tag in re.findall(r"<circle[^>]*>", svg):
        cx, cy, r = (float(_attr(tag, k)) for k in ("cx", "cy", "r"))
        m = _attr(tag, "transform")
        if m:
            # Figma places a flipped circle by matrix(a b c d e f).
            a, b, c, d, e, f = (float(v) for v in re.search(
                r"matrix\(([^)]*)\)", m).group(1).replace(",", " ").split())
            cx, cy = a * cx + c * cy + e, b * cx + d * cy + f
            r *= math.sqrt(abs(a * d - b * c))
        circles.append(dict(cx=cx, cy=cy, r=r, fill=_attr(tag, "fill").lower()))
    for tag in re.findall(r"<path[^>]*>", svg):
        assert not _attr(tag, "transform"), "a path with a transform; flatten it in Figma"
        cmds = parse_path(_attr(tag, "d"))
        mask = _attr(tag, "mask")
        if mask:
            # Figma insets each label's rule by masking a 4 px bar to its cell;
            # laser software ignores masks, so the bar becomes the rect it shows.
            x0, y0, x1, y1 = bbox(cmds)
            mx0, my0, mx1, my1 = masks[re.search(r"#([^)]+)", mask).group(1)]
            x0, y0, x1, y1 = max(x0, mx0), max(y0, my0), min(x1, mx1), min(y1, my1)
            assert x1 > x0 and y1 > y0, f"mask {mask} hides its whole path"
            cmds = [("M", [x0, y0]), ("H", [x1]), ("V", [y1]), ("H", [x0]), ("Z", [])]
        paths.append(dict(cmds=cmds, fill=(_attr(tag, "fill") or "").lower()))
    return circles, paths


def test_dot(dot, circles, paths):
    """The test dot as one even-odd path: the gold disc joined to its square tab,
    less the hole's black marker and any dark art inside the dot.

    Laser fills treat overlapping shapes even-odd, so disc and tab can't go out
    as two shapes: where they overlap would come out clear."""
    cx, cy, r = dot["cx"], dot["cy"], dot["r"]
    inside = lambda b: (b[0] >= cx - r and b[2] <= cx + r and b[1] >= cy - r and b[3] <= cy + r)
    tabs = [p for p in paths if p["fill"] == GOLD and inside(bbox(p["cmds"]))]
    assert len(tabs) == 1, f"expected one tab on the test dot, found {len(tabs)}"
    tab = tabs[0]["cmds"]
    # The tab is the dot's south-east quadrant with its outer corner
    # rounded: M c,c H c+r <outer edge, ending H c> V c Z. Its outer edge plus
    # the disc's other three quadrants is the union's outline.
    assert bbox(tab) == (cx, cy, cx + r, cy + r), "test tab is no longer the SE quadrant"
    assert tab[0] == ("M", [cx, cy]) and tab[1] == ("H", [cx + r]), "test tab drawn differently"
    end = max(i for i, (c, a) in enumerate(tab) if c == "H" and a == [cx])
    assert [c for c, _ in tab[end + 1:]] in (["V", "Z"], ["Z"]), "test tab drawn differently"
    outline = [("M", [cx, cy + r]), ("A", [r, r, 0, 1, 1, cx + r, cy])] + tab[2:end + 1] + [("Z", [])]

    holes = [circle_path(c["cx"], c["cy"], c["r"]) for c in circles
             if c["fill"] in ("black", INK) and inside((c["cx"] - c["r"], c["cy"] - c["r"],
                                                        c["cx"] + c["r"], c["cy"] + c["r"]))]
    holes += [p["cmds"] for p in paths if p["fill"] in ("black", INK) and inside(bbox(p["cmds"]))]
    assert holes, "test dot has no hole marker"
    return [outline] + holes


def engraved(welded=True):
    """The engraved layer: ([shape], s), each shape its subpaths in Figma px
    (fill even-odd), s mm per px. Welded, as the laser file has it: where marks
    overlap, one outline, its counters and the dots of its i's bridged in. Not
    welded, each mark on its own, with the art's own subpaths: what a renderer
    that triangulates (the shop's 3D model) wants."""
    lay = json.loads((HERE / "panel_layout.json").read_text())
    svg = ART.read_text()
    vw, vh = (float(v) for v in re.search(r'viewBox="0 0 ([\d.]+) ([\d.]+)"', svg).groups())
    s = lay["panel"]["w"] / vw  # mm per Figma px
    # Frame 13 was 1801 px, 3 mm short: its slash stood behind the power LED.
    assert abs(vh * s - lay["panel"]["h"]) < 0.05, \
        f"the art is {vh * s:.2f} mm tall, the top case {lay['panel']['h']}: re-export at 1100 x 1850"
    circles, paths = read_art(svg)
    for c in circles:
        assert c["fill"] in (GOLD, INK, "black"), f"a circle in {c['fill']} has no layer"

    # The test dot is where the panel's is, whatever the art does round it:
    # SW3's plunger comes up through it. Figma puts every dot on whole pixels.
    tdot = next(d for d in lay["dots"] if d["mat"] == "T")
    at = (round(tdot["x"] / s), round(tdot["y"] / s))
    (t,) = [c for c in circles if c["fill"] == GOLD and (round(c["cx"]), round(c["cy"])) == at]

    engrave = [[circle_path(c["cx"], c["cy"], c["r"])] for c in circles
               if c["fill"] == GOLD and c is not t]
    engrave.append(test_dot(t, circles, paths))
    tab_box = (t["cx"] - t["r"], t["cy"] - t["r"], t["cx"] + t["r"], t["cy"] + t["r"])
    for p in paths:
        b = bbox(p["cmds"])
        if b[0] >= tab_box[0] and b[2] <= tab_box[2] and b[1] >= tab_box[1] and b[3] <= tab_box[3]:
            continue  # part of the test dot, already composed
        assert p["fill"] in (GOLD, WHITE, "#ffffff", FR4), f"art in {p['fill']} has no layer"
        engrave.append([p["cmds"]])
    return (weld(engrave) if welded else engrave), s


def build():
    lay = json.loads((HERE / "panel_layout.json").read_text())
    tdot = next(d for d in lay["dots"] if d["mat"] == "T")
    engrave, s = engraved()

    w, h, cr = lay["panel"]["w"], lay["panel"]["h"], lay["panel"]["corner_r"]
    hole_r = lay["grid"]["hole_dia"] / 2
    num = lambda v: f"{v:.4f}".rstrip("0").rstrip(".")
    shapes = "\n".join(
        f'    <path d="{"".join(fmt_path(sub, s) for sub in subs)}"'
        + (' fill-rule="evenodd"' if len(subs) > 1 else "") + "/>"
        for subs in engrave)
    return f"""<?xml version="1.0" encoding="UTF-8"?>
<!-- Generated by hardware/panel/lc_top.py from svg/lc.svg; do not edit.
     AirWriter LC top: 1.5 mm clear CAST acrylic, {num(w)} x {num(h)} mm.
     Black fill: engrave the top face. Red hairline: cut. Engrave first. -->
<svg xmlns="http://www.w3.org/2000/svg" width="{num(w + 2 * MARGIN)}mm" height="{num(h + 2 * MARGIN)}mm" viewBox="{num(-MARGIN)} {num(-MARGIN)} {num(w + 2 * MARGIN)} {num(h + 2 * MARGIN)}">
  <title>AirWriter LC top</title>
  <g id="engrave" fill="{ENGRAVE}" stroke="none">
{shapes}
  </g>
  <g id="cut" fill="none" stroke="{CUT}" stroke-width="0.05">
    <rect x="0" y="0" width="{num(w)}" height="{num(h)}" rx="{num(cr)}" ry="{num(cr)}"/>
    <circle cx="{num(tdot["x"])}" cy="{num(tdot["y"])}" r="{num(hole_r)}"/>
  </g>
</svg>
"""


def slash():
    """The art's power slash (its one tan shape) as polys.json: [{outline,
    holes}] in mm, y down, flattened to 0.02 mm."""
    svg = ART.read_text()
    s = json.loads((HERE / "panel_layout.json").read_text())["panel"]["w"] / \
        float(re.search(r'viewBox="0 0 ([\d.]+) ', svg).group(1))
    (p,) = [p for p in read_art(svg)[1] if p["fill"] == FR4]
    ring = flatten(p["cmds"])
    return [dict(outline=[[round(x * s, 4), round(y * s, 4)] for x, y in ring], holes=[])]


if __name__ == "__main__":
    OUT.write_text(build())
    SLASH.write_text(json.dumps(slash()) + "\n")
    print(f"wrote {OUT.relative_to(HERE)}, {SLASH.relative_to(HERE)}")
