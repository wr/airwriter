# SPDX-License-Identifier: GPL-3.0-or-later
"""A sheet of labels as one 300 dpi black-and-white page.

Drawn here, not by the Mac, so nothing is smoothed into gray: the printer
would screen gray into dots, and the label's 4 pt small print shows it.
"""
from PIL import Image, ImageDraw, ImageFont

DPI = 300
MM = DPI / 25.4


def _px(inch):
    return round(inch * DPI)


def page(sheet):
    w, h = sheet["page_in"]
    return Image.new("1", (_px(w), _px(h)), 1)


def corner(sheet, position):
    """The top-left dot of label `position` (1 is top left, across then down)."""
    i = position - 1
    col, row = i % sheet["cols"], i // sheet["cols"]
    ox, oy = (v * MM for v in sheet.get("offset_mm", [0, 0]))
    return (_px(sheet["first_in"][0] + col * sheet["pitch_in"][0]) + round(ox),
            _px(sheet["first_in"][1] + row * sheet["pitch_in"][1]) + round(oy))


def sheet(geometry, rows, art, fields):
    """rows: labels.Row; art: {kind: mode "1" image at 300 dpi}; fields: where the serial and
    password go on a label ({"serial": {"x_mm", "y_mm" (the baseline), "pt", "font"}, ...})."""
    im = page(geometry)
    draw = ImageDraw.Draw(im)
    fonts = {k: ImageFont.truetype(f["font"], round(f["pt"] * DPI / 72)) for k, f in fields.items()}
    for r in rows:
        x, y = corner(geometry, r.position)
        im.paste(art[r.kind].convert("1"), (x, y))
        for k, text in (("serial", r.serial), ("password", r.password)):
            f = fields[k]
            draw.text((x + round(f["x_mm"] * MM), y + round(f["y_mm"] * MM)), text, font=fonts[k],
                      fill=0, anchor="ls")
    return im


def alignment(geometry):
    """Every label's outline and a cross at its middle, to hold over a label sheet."""
    im = page(geometry)
    draw = ImageDraw.Draw(im)
    lw, lh = (_px(v) for v in geometry["label_in"])
    for p in range(1, geometry["cols"] * geometry["rows"] + 1):
        x, y = corner(geometry, p)
        draw.rectangle([x, y, x + lw - 1, y + lh - 1], outline=0)
        cx, cy = x + lw // 2, y + lh // 2
        draw.line([cx - 30, cy, cx + 30, cy], fill=0)
        draw.line([cx, cy - 30, cx, cy + 30], fill=0)
    return im
