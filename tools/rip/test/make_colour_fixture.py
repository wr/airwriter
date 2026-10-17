#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Write a small 24-bit sRGB URF for the colour regression (W-1064).

Top half: bars of white, yellow, magenta, cyan, red, green, blue and black,
the inks alone and in pairs. Bottom half: a gray ramp, which should print in
the black band only, over a red-to-blue ramp, which should not."""
import struct, sys

W, H, RES = 576, 288, 144           # 4 x 2 inches at 144 dpi
BARS = [(255, 255, 255), (255, 255, 0), (255, 0, 255), (0, 255, 255),
        (255, 0, 0), (0, 255, 0), (0, 0, 255), (0, 0, 0)]


def pixel(x, y):
    if y < H // 2:
        return BARS[x * len(BARS) // W]
    v = 255 - x * 255 // (W - 1)
    if y < 3 * H // 4:
        return (v, v, v)
    return (v, 0, 255 - v)


out = bytearray(b"UNIRAST\0" + struct.pack(">I", 1))
hdr = bytearray(32)
hdr[0] = 24                         # bits per pixel
hdr[1] = 1                          # colorspace 1 = sRGB
struct.pack_into(">I", hdr, 12, W)
struct.pack_into(">I", hdr, 16, H)
struct.pack_into(">I", hdr, 20, RES)
out += hdr

for y in range(H):
    row = [pixel(x, y) for x in range(W)]
    out.append(0)                   # line repeat: emit once
    x = 0
    while x < W:                    # literal runs of up to 128 pixels
        n = min(128, W - x)
        if n == 1:                  # a literal run of one is not encodable
            out.append(0x00); out += bytes(row[x])
        else:
            out.append(257 - n)
            for p in row[x:x + n]:
                out += bytes(p)
        x += n

open(sys.argv[1], "wb").write(bytes(out))
print(f"wrote {sys.argv[1]}: {W}x{H} sRGB @ {RES}dpi, {len(out)} bytes")
