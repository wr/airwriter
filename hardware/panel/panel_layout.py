#!/usr/bin/env python3
"""Panel geometry: dotmap.txt + the config block -> panel_layout.json."""
import json, pathlib

CFG = dict(w=62.0, h=100.0, corner_r=2.0,
           margin_side=3.2, margin_top=2.8, margin_bottom=1.8,
           cols=7, dot_ratio=0.66, hole_dia=3.5)
LED_MATS = set("PBL")

def load_layout():
    here = pathlib.Path(__file__).parent
    rows = (here / "dotmap.txt").read_text().split()
    cols = CFG["cols"]
    assert all(len(r) == cols for r in rows), "ragged dotmap"
    pitch = (CFG["w"] - 2 * CFG["margin_side"]) / cols
    dia = pitch * CFG["dot_ratio"]
    dots = []
    for r, row in enumerate(rows):
        for c, ch in enumerate(row):
            if ch == ".":
                continue
            assert ch in "GTPBL", f"bad material {ch!r} at {r},{c}"
            dots.append(dict(r=r, c=c, mat=ch,
                             x=round(CFG["margin_side"] + (c + 0.5) * pitch, 4),
                             y=round(CFG["margin_top"] + (r + 0.5) * pitch, 4),
                             led=ch in LED_MATS))
    assert sum(d["mat"] == "T" for d in dots) == 1, "exactly one test dot"
    return dict(panel=dict(w=CFG["w"], h=CFG["h"], corner_r=CFG["corner_r"]),
                grid=dict(cols=cols, rows=len(rows), pitch=round(pitch, 4),
                          dot_dia=round(dia, 4),
                          origin=[dots[0]["x"], dots[0]["y"]],
                          hole_dia=CFG["hole_dia"]),
                dots=dots)

if __name__ == "__main__":
    out = pathlib.Path(__file__).parent / "panel_layout.json"
    out.write_text(json.dumps(load_layout(), indent=1) + "\n")
    print("wrote", out)
