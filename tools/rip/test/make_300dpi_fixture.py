#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Write a page at 300 dpi as 8-bit gray URF, for the LaserWriter's rip
(W-1082). The only iOS capture is at 144 dpi; these stand in for an iPhone
page at RS300 until one is captured. With --dpi 360 they stand in for a
StyleWriter's (W-1103; swcheck.py), and `colour` is the same in 24-bit sRGB.

    make_300dpi_fixture.py text OUT.urf    rows of glyph-like strokes, 1 in margins
    make_300dpi_fixture.py photo OUT.urf   full-bleed continuous tone
    make_300dpi_fixture.py colour OUT.urf  sRGB: bars, ramps and a gradient, full bleed
        [--dpi N]                          the page's resolution (default 300)
        [--paper letter|a4|legal]          the sheet (default letter)
        [--rows N]                         only the first N rows of it, for a quick page

Deterministic: the same bytes every run. Not committed; pscheck.py and swcheck.py
make them. The defaults are the 300 dpi Letter pages pscheck.py has always used.
"""
import argparse, math, random, struct, sys

# The sheet in inches; W and H are its pixels at --dpi (A4 is 2976 x 4209 at 360 dpi,
# the size swfake.py gives it).
PAPERS = {"letter": (8.5, 11.0), "a4": (210 / 25.4, 297 / 25.4), "legal": (8.5, 14.0)}
W, H, RES = 2550, 3300, 300          # 8.5 x 11 in


def setup(dpi, paper, rows):
    global W, H, RES
    w_in, h_in = PAPERS[paper]
    W, H, RES = round(w_in * dpi), round(h_in * dpi), dpi
    if rows:
        H = min(H, rows)


def header(bits=8, space=0):
    out = bytearray(b"UNIRAST\0" + struct.pack(">I", 1))
    hdr = bytearray(32)
    hdr[0] = bits                    # bits per pixel
    hdr[1] = space                   # colorspace: 0 sGray, 1 sRGB
    struct.pack_into(">I", hdr, 12, W)
    struct.pack_into(">I", hdr, 16, H)
    struct.pack_into(">I", hdr, 20, RES)
    return out + hdr


def encode(row):
    """One row's runs: repeats for runs of a value, literals for the rest."""
    out = bytearray()
    x = 0
    while x < W:
        v = row[x]
        n = 1
        while x + n < W and n < 128 and row[x + n] == v:
            n += 1
        if n >= 3 or x + n == W:
            out.append(n - 1)
            out.append(v)
            x += n
            continue
        start = x
        while x < W and x - start < 128:
            if x + 2 < W and row[x] == row[x + 1] == row[x + 2]:
                break
            x += 1
        if x - start == 1:
            out.append(0)
            out.append(row[start])
        else:
            out.append(257 - (x - start))
            out += row[start:x]
    return out


def write(rows, path):
    """rows: H rows of W gray bytes. Identical neighbours share a line-repeat."""
    out = header()
    y = 0
    while y < H:
        n = 1
        while y + n < H and n < 256 and rows[y + n] == rows[y]:
            n += 1
        out.append(n - 1)
        out += encode(rows[y])
        y += n
    open(path, "wb").write(bytes(out))
    return len(out)


def text_page():
    """Lines of 'words' between 1 in margins: each glyph a 24 px cell with a
    stem and a bar, 30 px of ink a line and 20 px of paper between."""
    rnd = random.Random(1082)
    white = bytes([255]) * W
    rows = [white] * H
    y = 300
    while y + 30 <= H - 300:
        line = [bytearray(white) for _ in range(30)]
        x = 300
        while x < W - 300 - 24:
            letters = rnd.randint(2, 9)
            for _ in range(letters):
                if x + 24 > W - 300:
                    break
                stem = x + rnd.choice((2, 10, 18))
                tall = rnd.random() < 0.3
                for r in range(0 if tall else 10, 30):
                    line[r][stem:stem + 4] = b"\0" * 4
                bar = rnd.choice((10, 19, 26))
                line[bar][x + 2:x + 20] = b"\0" * 18
                line[bar + 1][x + 2:x + 20] = b"\0" * 18
                x += 24
            x += 25
        for r in range(30):
            rows[y + r] = bytes(line[r])
        y += 50
    return rows


def photo_page():
    """A full-bleed ramp with soft waves and seeded grain: hardly two
    neighbouring pixels alike, as in a photograph."""
    rnd = random.Random(1083)
    grain = [rnd.randint(0, 24) for _ in range(W + 97)]
    ramp = [x * 160 // W for x in range(W)]
    wave = [int(40 * math.sin(x / 90.0)) for x in range(W)]
    rows = []
    for y in range(H):
        lift = 40 + y * 50 // H + int(30 * math.sin(y / 140.0))
        g = (7 * y) % len(grain)
        row = grain[g:] + grain[:g]
        rows.append(bytes(max(0, min(255, lift + r + w + n))
                          for r, w, n in zip(ramp, wave, row)))
    return rows


BARS = [(255, 255, 255), (255, 255, 0), (255, 0, 255), (0, 255, 255),
        (255, 0, 0), (0, 255, 0), (0, 0, 255), (0, 0, 0)]


def colour_page():
    """24-bit sRGB, full bleed. A third of the page is the eight bars of white, the
    three inks, red, green, blue and black; a third a gray ramp over a red-to-blue
    one, each across the width; the last a smooth gradient in all three channels,
    stepped every 8 pixels across so a row is runs, not noise. Returns H arrays of
    (W, 3) uint8. The only page that needs numpy (the gray ones run on any python3)."""
    import numpy as np
    xs = np.arange(W)
    rows = []
    third = H // 3
    bar = np.array([BARS[x * len(BARS) // W] for x in range(W)], np.uint8)
    ramp = np.clip(255 - xs * 255 // (W - 1), 0, 255).astype(np.uint8)
    gray = np.stack([ramp, ramp, ramp], axis=1)
    redblue = np.stack([ramp, np.zeros(W, np.uint8), 255 - ramp], axis=1)
    step = (xs // 8) * 8
    for y in range(H):
        if y < third:
            rows.append(bar)
        elif y < 2 * third:
            rows.append(gray if y < third + third // 2 else redblue)
        else:
            t = (y - 2 * third) / max(1, H - 2 * third)
            r = (step * 255 // (W - 1)).astype(np.int64)
            g = np.full(W, int(255 * t), np.int64)
            b = 255 - r // 2 - int(80 * t)
            rows.append(np.stack([r, g, np.clip(b, 0, 255)], axis=1).astype(np.uint8))
    return rows


def encode_rgb(px):
    """One RGB row's runs: repeats for runs of a pixel, literals for the rest."""
    import numpy as np
    n = len(px)
    same = np.all(px[1:] == px[:-1], axis=1) if n > 1 else np.zeros(0, bool)
    bounds = np.flatnonzero(~same) + 1                  # where a new pixel value starts
    starts = [0, *bounds.tolist()]
    ends = [*bounds.tolist(), n]
    out = bytearray()
    lit = []                                            # pixel indices of a literal in progress

    def flush_lit():
        i = 0
        while i < len(lit):
            k = min(128, len(lit) - i)
            if k == 1:
                out.append(0)
            else:
                out.append(257 - k)
            for x in lit[i:i + k]:
                out.extend(px[x].tobytes())
            i += k
        lit.clear()

    for a, b in zip(starts, ends):
        if b - a >= 3:
            flush_lit()
            while a < b:
                k = min(128, b - a)
                out.append(k - 1)
                out.extend(px[a].tobytes())
                a += k
        else:
            lit.extend(range(a, b))
    flush_lit()
    return out


def write_rgb(rows, path):
    out = header(24, 1)
    y = 0
    while y < H:
        n = 1
        key = rows[y].tobytes()
        while y + n < H and n < 256 and rows[y + n].tobytes() == key:
            n += 1
        out.append(n - 1)
        out += encode_rgb(rows[y])
        y += n
    open(path, "wb").write(bytes(out))
    return len(out)


def main():
    ap = argparse.ArgumentParser(description="a synthetic URF page for the rip's tests")
    ap.add_argument("kind", choices=("text", "photo", "colour"))
    ap.add_argument("out")
    ap.add_argument("--dpi", type=int, default=300)
    ap.add_argument("--paper", choices=sorted(PAPERS), default="letter")
    ap.add_argument("--rows", type=int, default=0)
    a = ap.parse_args()
    setup(a.dpi, a.paper, a.rows)
    if a.kind == "colour":
        n = write_rgb(colour_page(), a.out)
        fmt = "sRGB"
    else:
        n = write(text_page() if a.kind == "text" else photo_page(), a.out)
        fmt = "gray"
    print(f"wrote {a.out}: {a.kind}, {W}x{H} {fmt} @ {RES}dpi, {n} bytes")


if __name__ == "__main__":
    main()
