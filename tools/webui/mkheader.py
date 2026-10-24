#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Builds firmware/components/webui/webui_page.h from the sources here.

The config page is written as ordinary HTML, CSS and JavaScript so it can be
opened in a browser and worked on, then compiled into one C string literal for
the firmware. Edit style.css / body.html / app.js and run this; never edit the
generated header.

    python3 tools/webui/mkheader.py
"""
import base64, json, pathlib, re, sys

HERE = pathlib.Path(__file__).parent
OUT = HERE / "../../firmware/components/webui/webui_page.h"
MASK = HERE / "../../firmware/components/halftone/bluenoise64.h"
ICON = HERE / "../../firmware/components/ipp_server/icons/icon-128.png"
LOGO = HERE / "wells-workshop.svg"
# AirWriter itself, from the shop's render (Wells's own; LICENSE.md, name and logo)
AIRWRITER = HERE / "airwriter.webp"
# the status panel (W-1092): its dots, the LEDs' order behind them, and the
# colours board 1's panel was measured showing
LAYOUT = HERE / "../../hardware/panel/panel_layout.json"
LEDMAP = HERE / "../../firmware/components/led_grid/led_map.c"
COLOURS = HERE / "../lights/panel_colours.json"
# the AppleTalk cards' two emulator icons (LICENSE.md, third-party material)
BASILISK = HERE / "basilisk-ii.png"
INFINITEMAC = HERE / "infinite-mac.png"

def bluenoise_js():
    """The firmware's 64 x 64 blue-noise tile, as a base64 string for the
    preview: the same bytes, so the sheet screens as the printer does."""
    body = MASK.read_text().split("{", 1)[1].split("}", 1)[0]
    vals = bytes(int(v) for v in body.replace("\n", "").split(",") if v.strip())
    assert len(vals) == 64 * 64
    return "const BLUENOISE='%s';\n" % base64.b64encode(vals).decode()

def panel_js():
    """The panel for the sidebar's picture of it: the dot grid (one letter a
    dot, row by row: G gold, T the test button, P FR4, B and L mask), the dot
    behind each LED in chain order, and per material the 24 measured hues and
    the white, as tools/lights/render.py draws them."""
    lay = json.loads(LAYOUT.read_text())
    g = lay["grid"]
    grid = {(d["r"], d["c"]): d["mat"] for d in lay["dots"]}
    dots = "".join(grid[r, c] for r in range(g["rows"]) for c in range(g["cols"]))
    chain = [int(r) * g["cols"] + int(c) for r, c, _ in
             re.findall(r"\{(\d+), (\d+), '(\w)'\}", LEDMAP.read_text())]
    assert len(chain) == 37 and all(dots[i] in "PBL" for i in chain)
    col = json.loads(COLOURS.read_text())
    out = {"w": lay["panel"]["w"], "pitch": g["pitch"], "dia": g["dot_dia"], "origin": g["origin"][0],
           "hole": g["hole_dia"], "cols": g["cols"], "rows": g["rows"], "dots": dots, "chain": chain,
           "ring": {m: [e[m] for e in col[:24]] for m in "PBL"},
           "white": {m: next(e[m] for e in col if e["name"] == "neutral") for m in "PBL"}}
    return "const PANEL=%s;\n" % json.dumps(out, separators=(",", ":"))

def favicon():
    """The printer's own icon (tools/icon), the one AirPrint shows, inline:
    the 128 px one is an exact 4x of the 32 px grid, so a tab draws it sharp."""
    return ('<link rel=icon type=image/png sizes=128x128 href="data:image/png;base64,%s">'
            % base64.b64encode(ICON.read_bytes()).decode())

def png_uri(path):
    return "data:image/png;base64," + base64.b64encode(path.read_bytes()).decode()

def c_string(text):
    """One C string literal per source line, so the header stays diffable."""
    out = []
    for line in text.split("\n"):
        esc = line.replace("\\", "\\\\").replace('"', '\\"')
        out.append('"%s\\n"' % esc)
    return "\n".join(out)

def build_page():
    """The whole page as the board serves it (preview.py serves it too)."""
    css = (HERE / "style.css").read_text().replace(
        "{{airwriter}}", "data:image/webp;base64," + base64.b64encode(AIRWRITER.read_bytes()).decode())
    body = (HERE / "body.html").read_text()
    body = body.replace("{{ww-logo}}", LOGO.read_text().strip())
    body = (body.replace("{{icon}}", png_uri(ICON)).replace("{{basilisk}}", png_uri(BASILISK))
                .replace("{{infinitemac}}", png_uri(INFINITEMAC)))
    assert "{{" not in body, "a placeholder in body.html has no source"
    js = bluenoise_js() + panel_js() + (HERE / "app.js").read_text()

    # Comments are for whoever edits the sources, not for the device.
    css = re.sub(r"/\*.*?\*/", "", css, flags=re.S)
    css = re.sub(r"\n\s*\n", "\n", css)
    body = re.sub(r"<!--.*?-->", "", body, flags=re.S)
    js = re.sub(r"^\s*/\*.*?\*/\s*$", "", js, flags=re.S | re.M)

    page = ("<!doctype html><html lang=en><head><meta charset=utf-8>"
            '<meta name=viewport content="width=device-width,initial-scale=1">'
            "<title>AirWriter</title>" + favicon() + "<style>" + css + "</style></head><body>"
            + body + "<script>" + js + "</script></body></html>")
    return page

def main():
    page = build_page()
    header = ("// SPDX-License-Identifier: GPL-3.0-or-later\n"
              "//\n"
              "// GENERATED by tools/webui/mkheader.py -- do not edit.\n"
              "// Sources: tools/webui/{style.css,body.html,app.js} and the\n"
              "// halftone's bluenoise64.h, the printer icon, the Wells Workshop mark,\n"
              "// tools/webui/{basilisk-ii,infinite-mac}.png and the panel's layout,\n"
              "// LED map and measured colours\n"
              "//\n"
              "// The whole page in one string: nothing it needs comes from outside,\n"
              "// because a printer's config page has to work on a phone that has\n"
              "// just joined the printer's own access point and has no internet at\n"
              "// all. Apple Garamond is subset and embedded for the same\n"
              "// reason. The one request out, GitHub's star count, is shown only\n"
              "// when it comes back.\n"
              "#pragma once\n\n"
              "static const char WEBUI_HTML[] =\n" + c_string(page) + ";\n")

    OUT.resolve().write_text(header)
    print("%s: %d bytes of page, %d bytes of header"
          % (OUT.resolve().name, len(page), len(header)))

if __name__ == "__main__":
    sys.exit(main())
