#!/usr/bin/env python3
"""svg/panel.svg circles -> panel_layout.json (+ dotmap.txt, derived)."""
import json, pathlib, re

VIEW_W_PX = 1100.0
SCALE = 62.0 / VIEW_W_PX          # mm per px
CFG = dict(w=62.0, corner_r=2.0)
# Frame 9b's palette. The two teal classes are the two mask recipes, and the
# distinction is which faces open, not what the dot is made of:
#   B  bright teal -- single mask, the viewer's face masked, the far face open
#   L  deep teal   -- mask on both sides, so it reads darker until it is lit
# Both are LED-backed; gold and the test dot are not. #111111 is the artboard
# background and the wordmark strokes, never a dot, so it is not mapped here.
MAT_BY_FILL = {"#dda857": "G", "#e7ba9b": "P", "#14baba": "B", "#176767": "L"}
LED_MATS = set("PBL")
GRID0_PX, PITCH_PX = 100.0, 150.0
# The test NPTH is a mechanical dimension, not artwork: SW3's plunger has to poke
# up through it. The SVG's black marker circle only locates the test dot; Wells
# sets the drill here so it survives a Figma re-export. 3.6mm clears the button.
TEST_HOLE_DIA = 3.8

def _circles(svg_text):
    for tag in re.findall(r"<circle[^>]*>", svg_text):
        def attr(name):
            m = re.search(rf'\b{name}="([^"]+)"', tag)
            return m.group(1) if m else None
        yield float(attr("cx")), float(attr("cy")), float(attr("r")), attr("fill").lower()

def load_layout():
    here = pathlib.Path(__file__).parent
    svg = (here / "svg" / "panel.svg").read_text()
    m = re.search(r'viewBox="0 0 (\d+) (\d+)"', svg)
    assert m and float(m.group(1)) == VIEW_W_PX, "unexpected viewBox"
    panel_h = round(float(m.group(2)) * SCALE, 2)

    cells, hole = {}, None
    for cx, cy, r, fill in _circles(svg):
        col = (cx - GRID0_PX) / PITCH_PX
        row = (cy - GRID0_PX) / PITCH_PX
        assert abs(col - round(col)) < 0.01 and abs(row - round(row)) < 0.01, \
            f"circle off-grid at ({cx},{cy})"
        rc = (int(round(row)), int(round(col)))
        if fill == "black" and r < 40:  # hole marker (r34 since the 3.8mm hole)
            hole = (rc, round(2 * r * SCALE, 2)); continue
        cells[rc] = MAT_BY_FILL[fill]
    assert hole, "no test hole circle found"
    assert cells[hole[0]] == "G", "test hole must sit on a gold dot"
    cells[hole[0]] = "T"

    rows = 1 + max(rc[0] for rc in cells)
    cols = 1 + max(rc[1] for rc in cells)
    assert (rows, cols) == (8, 7) and len(cells) == 56, "grid shape drifted"
    dots = [dict(r=rc[0], c=rc[1], mat=mat,
                 x=round((GRID0_PX + rc[1] * PITCH_PX) * SCALE, 4),
                 y=round((GRID0_PX + rc[0] * PITCH_PX) * SCALE, 4),
                 led=mat in LED_MATS)
            for rc, mat in sorted(cells.items()) if mat != "."]
    return dict(panel=dict(w=CFG["w"], h=panel_h, corner_r=CFG["corner_r"]),
                grid=dict(cols=cols, rows=rows,
                          pitch=round(PITCH_PX * SCALE, 4),
                          dot_dia=round(100.0 * SCALE, 4),
                          origin=[round(GRID0_PX * SCALE, 4)] * 2,
                          hole_dia=TEST_HOLE_DIA),
                dots=dots, _cells={f"{r},{c}": m for (r, c), m in sorted(cells.items())})

if __name__ == "__main__":
    here = pathlib.Path(__file__).parent
    lay = load_layout()
    cells = lay.pop("_cells")
    (here / "panel_layout.json").write_text(json.dumps(lay, indent=1) + "\n")
    grid = [["?"] * lay["grid"]["cols"] for _ in range(lay["grid"]["rows"])]
    for key, m in cells.items():
        r, c = map(int, key.split(","))
        grid[r][c] = m
    (here / "dotmap.txt").write_text(
        "# generated from svg/panel.svg - do not edit\n"
        + "\n".join("".join(r) for r in grid) + "\n")
    print("wrote panel_layout.json and dotmap.txt")
