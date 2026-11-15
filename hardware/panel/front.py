#!/usr/bin/env python3
# SPDX-License-Identifier: CERN-OHL-S-2.0
"""The art as seen from the front -> the art in the board's frame.

AirWriter sits with its sockets toward the wall, so the side away from them is
its front. Wells's Figma frames ("Frame 16 · from the front", "Frame 17 · from
the front") draw the panel as read from there, sockets along the top, and
svg/*.svg are their exports. The boards, the case and the LED map are laid out
with the sockets along the bottom, so everything that reads the art reads it
through to_board(): the same SVG turned 180 degrees about its centre, as text.

Only what Figma writes for this art is handled: absolute path data, circles,
rects, masks, and circles Figma places with a matrix. Anything else (any other
transform, a relative command, a line, an image) raises, so an export it can't turn exactly never turns into a panel.
"""
import pathlib, re

W, H = 1100.0, 1850.0

_NUM = r"[-+]?(?:\d+\.?\d*|\.\d+)(?:[eE][-+]?\d+)?"
# Absolute commands only; Figma writes nothing else. One letter per argument:
# x or y a coordinate, r a radius (both scale), - an arc's rotation or flag.
_ARGS = {"M": "xy", "L": "xy", "H": "x", "V": "y", "C": "xyxyxy", "S": "xyxy",
         "Q": "xyxy", "T": "xy", "A": "rr---xy", "Z": ""}

_REFUSE = ("<line", "<polyline", "<polygon", "<ellipse", "<image",
           "<use", "<text", "<pattern", "Gradient")


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


def _num(v):
    s = f"{v:.4f}".rstrip("0").rstrip(".")
    return "0" if s == "-0" else s


def _turn_d(d):
    try:
        cmds = parse_path(d)
    except AssertionError as e:
        raise ValueError(str(e)) from None
    parts = []
    for cmd, args in cmds:
        vals = [W - a if k == "x" else H - a if k == "y" else a
                for a, k in zip(args, _ARGS[cmd])]
        parts.append(cmd + " ".join(_num(v) for v in vals))
    return "".join(parts)


def _attr(tag, name):
    m = re.search(rf'\s{name}="([^"]*)"', tag)
    return float(m.group(1)) if m else None


def _set(tag, name, value):
    if re.search(rf'\s{name}="[^"]*"', tag):
        return re.sub(rf'(\s{name}=")[^"]*(")', lambda m: m.group(1) + _num(value) + m.group(2), tag)
    return tag[:-2] + f' {name}="{_num(value)}"/>' if tag.endswith("/>") else \
        tag[:-1] + f' {name}="{_num(value)}">'


_MATRIX = re.compile(r'(\stransform="matrix\()([^)]*)(\)")')


def _turn_circle(m):
    tag = m.group(0)
    placed = _MATRIX.search(tag)
    if placed:
        # Figma places a flipped circle by matrix(a b c d e f): turn the matrix,
        # (-a -b -c -d W-e H-f), and leave the circle inside it as it is.
        a, b, c, d, e, f = (float(v) for v in placed.group(2).replace(",", " ").split())
        turned = " ".join(_num(v) for v in (-a, -b, -c, -d, W - e, H - f))
        return _MATRIX.sub(lambda m: m.group(1) + turned + m.group(3), tag)
    tag = _set(tag, "cx", W - (_attr(tag, "cx") or 0.0))
    return _set(tag, "cy", H - (_attr(tag, "cy") or 0.0))


def _turn_rect(m):
    tag = m.group(0)
    x, y = _attr(tag, "x") or 0.0, _attr(tag, "y") or 0.0
    w, h = _attr(tag, "width"), _attr(tag, "height")
    if w is None or h is None:
        raise ValueError(f"a rect with no size: {tag[:60]}")
    # Written as Figma writes a rect: x, y, width, height first, an x or y of
    # 0 left out, then the rest as they were.
    rest = re.sub(r'\s(?:x|y|width|height)="[^"]*"', "", tag[len("<rect"):])
    head = "".join(f' {k}="{_num(v)}"' for k, v in (("x", W - x - w), ("y", H - y - h)) if v != 0)
    return f'<rect{head} width="{_num(w)}" height="{_num(h)}"{rest}'


def to_board(svg_text):
    """The export, drawn from the front, turned into the board's frame."""
    m = re.search(r'viewBox="([^"]*)"', svg_text)
    if not m or [float(v) for v in m.group(1).split()] != [0, 0, W, H]:
        raise ValueError(f"expected viewBox 0 0 1100 1850, got {m.group(1) if m else None}")
    for bad in _REFUSE:
        if bad in svg_text:
            raise ValueError(f"the art has {bad!r}, which this turn doesn't handle; "
                             "flatten it in Figma and export again")
    for tag in re.findall(r"<[a-zA-Z][^>]*\stransform=[^>]*>", svg_text):
        if not (tag.startswith("<circle") and _MATRIX.search(tag)):
            raise ValueError(f"a transform this turn doesn't handle: {tag[:80]}; "
                             "flatten it in Figma and export again")
    out = re.sub(r'(\sd=")([^"]*)(")', lambda m: m.group(1) + _turn_d(m.group(2)) + m.group(3), svg_text)
    out = re.sub(r"<circle\b[^>]*>", _turn_circle, out)
    return re.sub(r"<rect\b[^>]*>", _turn_rect, out)


def board_art(path):
    """An art file, read and turned into the board's frame."""
    return to_board(pathlib.Path(path).read_text())
