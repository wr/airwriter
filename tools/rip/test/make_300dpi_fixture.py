#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Write a Letter page at 300 dpi as 8-bit gray URF, for the LaserWriter's
rip (W-1082). The only iOS capture is at 144 dpi; these stand in for an
iPhone page at RS300 until one is captured.

    make_300dpi_fixture.py text OUT.urf    rows of glyph-like strokes, 1 in margins
    make_300dpi_fixture.py photo OUT.urf   full-bleed continuous tone

Deterministic: the same bytes every run. Not committed; pscheck.py makes them.
"""
import math, random, struct, sys

W, H, RES = 2550, 3300, 300          # 8.5 x 11 in


def header():
    out = bytearray(b"UNIRAST\0" + struct.pack(">I", 1))
    hdr = bytearray(32)
    hdr[0] = 8                       # bits per pixel
    hdr[1] = 0                       # sGray
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


def main():
    if len(sys.argv) != 3 or sys.argv[1] not in ("text", "photo"):
        sys.exit("usage: make_300dpi_fixture.py text|photo OUT.urf")
    rows = text_page() if sys.argv[1] == "text" else photo_page()
    n = write(rows, sys.argv[2])
    print(f"wrote {sys.argv[2]}: {sys.argv[1]}, {W}x{H} @ {RES}dpi, {n} bytes")


if __name__ == "__main__":
    main()
