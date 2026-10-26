#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Builds the printer-icons PNGs in firmware/components/ipp_server/icons/.

imagewriter.txt is the icon as a pixel grid, one character per pixel ('.' is
transparent). It is Apple's colour ImageWriter icon, redrawn pixel for pixel,
and is not covered by this repository's licences (LICENSE.md).
laserwriter.txt is a LaserWriter II for a shared LaserWriter (W-1082), drawn
for AirWriter in the same style and palette (the palette must not grow: the
ImageWriter's PNGs would change), under this repository's licence.
stylewriter.txt is the Color StyleWriter's Chooser icon, written by
tools/stylewriter/mktables.py from Apple's driver and mapped to the nearest of this
palette's colours, so, like the ImageWriter's, it is Apple's and is not covered by
this repository's licences (LICENSE.md).

IPP Everywhere wants 48, 128 and 512 pixel icons. The grid is centred on a
32 x 32 canvas; 128 and 512 are exact 4x and 16x enlargements of it, and 48 is
the canvas at 1x with a margin, since 1.5x would draw some pixels wider than
others. No dependencies, so the output is the same on every machine.

    python3 tools/icon/mkicons.py
"""
import pathlib, struct, zlib

HERE = pathlib.Path(__file__).parent
OUT = HERE / "../../firmware/components/ipp_server/icons"
CANVAS = 32
SIZES = (48, 128, 512)

PALETTE = {
    "K": (0, 0, 0), "j": (18, 18, 18), "k": (31, 31, 31), "d": (60, 60, 60),
    "D": (75, 75, 75), "m": (125, 125, 125), "s": (160, 160, 160),
    "l": (197, 197, 197), "w": (235, 235, 235),
    "G": (0, 178, 0), "R": (217, 0, 0),
}


def canvas(name):
    rows = (HERE / name).read_text().split()
    h, w = len(rows), max(len(r) for r in rows)
    assert w <= CANVAS and h <= CANVAS
    oy, ox = (CANVAS - h) // 2, (CANVAS - w) // 2
    grid = [["."] * CANVAS for _ in range(CANVAS)]
    for j, r in enumerate(rows):
        for i, c in enumerate(r):
            assert c == "." or c in PALETTE, f"unknown pixel {c!r}"
            grid[oy + j][ox + i] = c
    return grid


def png(grid, size):
    """Indexed PNG, index 0 transparent."""
    keys = ["."] + sorted(PALETTE)
    index = {k: i for i, k in enumerate(keys)}
    scale = size // CANVAS if size % CANVAS == 0 else 1
    pad = (size - CANVAS * scale) // 2
    raw = bytearray()
    for y in range(size):
        raw.append(0)                          # filter: none
        gy = (y - pad) // scale
        for x in range(size):
            gx = (x - pad) // scale
            inside = 0 <= gx < CANVAS and 0 <= gy < CANVAS and y >= pad and x >= pad
            raw.append(index[grid[gy][gx]] if inside else 0)

    def chunk(tag, data):
        c = tag + data
        return struct.pack(">I", len(data)) + c + struct.pack(">I", zlib.crc32(c))

    plte = b"".join(bytes(PALETTE[k]) if k in PALETTE else b"\0\0\0" for k in keys)
    trns = b"\0" + b"\xff" * (len(keys) - 1)
    return (b"\x89PNG\r\n\x1a\n"
            + chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 3, 0, 0, 0))
            + chunk(b"PLTE", plte)
            + chunk(b"tRNS", trns)
            + chunk(b"IDAT", zlib.compress(bytes(raw), 9))
            + chunk(b"IEND", b""))


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    for grid_file, prefix in (("imagewriter.txt", "icon"), ("laserwriter.txt", "laser"),
                              ("stylewriter.txt", "stylewriter")):
        grid = canvas(grid_file)
        for size in SIZES:
            data = png(grid, size)
            (OUT / f"{prefix}-{size}.png").write_bytes(data)
            print(f"{prefix}-{size}.png  {len(data)} bytes")


if __name__ == "__main__":
    main()
