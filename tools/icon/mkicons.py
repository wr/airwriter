#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Builds the printer-icons PNGs in firmware/components/ipp_server/icons/.

Each icon is a pixel grid, one character per pixel ('.' is transparent). All of
them are Apple's and are not covered by this repository's licences (LICENSE.md).

imagewriter.txt is Apple's colour ImageWriter icon, redrawn pixel for pixel.
stylewriter.txt is the Color StyleWriter 2500's Chooser icon, written by
tools/stylewriter/mktables.py from Apple's driver in Apple's own colours (the
Macintosh system palette's).
apple/ holds the rest, one file for each drawing Apple made: the LaserWriters' from
the model's own file in Mac OS 8.1's Printer Descriptions, the StyleWriters' and the
ImageWriter LQ's from their drivers, each a 32 x 32 icl8 cut out by its ICN# mask. A
file's name is the name of its PNGs and the name pr_icon (printers_core.c) gives it.

The first two keep the palettes they always had, in full, so their PNGs do not
change. An icon in apple/ has only the colours it uses in its PLTE.

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

# The Color StyleWriter's icon: PALETTE, and the colours its icl8 takes from the Macintosh
# system palette that PALETTE has no match for. The grays are the system palette's ramp,
# named by their hex pair (W is white, e EE, x DD, c CC, b BB, a AA, h 88, g 77, f 55, F 44, E 22).
STYLEWRITER_PALETTE = dict(PALETTE, **{
    "W": (255, 255, 255), "e": (238, 238, 238), "x": (221, 221, 221), "c": (204, 204, 204),
    "b": (187, 187, 187), "a": (170, 170, 170), "h": (136, 136, 136), "g": (119, 119, 119),
    "f": (85, 85, 85), "F": (68, 68, 68), "E": (34, 34, 34),
    "B": (0, 0, 221), "N": (0, 187, 0), "Q": (221, 0, 0), "Y": (255, 255, 0), "O": (255, 102, 0),
})

# Apple's other printer icons: the same system palette, the few more of it that they use.
APPLE_PALETTE = dict(STYLEWRITER_PALETTE, **{
    "A": (0, 170, 0), "C": (255, 102, 51), "H": (17, 17, 17), "I": (51, 255, 153),
    "J": (153, 153, 153), "L": (102, 102, 102), "M": (255, 0, 0), "P": (17, 0, 0),
    "S": (204, 0, 0), "T": (51, 51, 51), "U": (238, 0, 0), "V": (0, 136, 0),
})


def canvas(name, palette=PALETTE):
    rows = (HERE / name).read_text().split()
    h, w = len(rows), max(len(r) for r in rows)
    assert w <= CANVAS and h <= CANVAS
    oy, ox = (CANVAS - h) // 2, (CANVAS - w) // 2
    grid = [["."] * CANVAS for _ in range(CANVAS)]
    for j, r in enumerate(rows):
        for i, c in enumerate(r):
            assert c == "." or c in palette, f"unknown pixel {c!r}"
            grid[oy + j][ox + i] = c
    return grid


def png(grid, size, palette=PALETTE):
    """Indexed PNG, index 0 transparent."""
    keys = ["."] + sorted(palette)
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

    plte = b"".join(bytes(palette[k]) if k in palette else b"\0\0\0" for k in keys)
    trns = b"\0" + b"\xff" * (len(keys) - 1)
    return (b"\x89PNG\r\n\x1a\n"
            + chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 3, 0, 0, 0))
            + chunk(b"PLTE", plte)
            + chunk(b"tRNS", trns)
            + chunk(b"IDAT", zlib.compress(bytes(raw), 9))
            + chunk(b"IEND", b""))


def icons():
    """Every icon, as (name, grid, palette)."""
    yield "icon", canvas("imagewriter.txt", PALETTE), PALETTE
    yield "stylewriter", canvas("stylewriter.txt", STYLEWRITER_PALETTE), STYLEWRITER_PALETTE
    for path in sorted((HERE / "apple").glob("*.txt")):
        grid = canvas(f"apple/{path.name}", APPLE_PALETTE)
        used = {c for row in grid for c in row} - {"."}
        yield path.stem, grid, {k: APPLE_PALETTE[k] for k in used}


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    for name, grid, palette in icons():
        for size in SIZES:
            data = png(grid, size, palette)
            (OUT / f"{name}-{size}.png").write_bytes(data)
            print(f"{name}-{size}.png  {len(data)} bytes")


if __name__ == "__main__":
    main()
