#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Write a small 8-bit grayscale URF with an uncompressed body."""
import struct, sys

W, H, RES = 288, 144, 144          # 2 inches at 144 dpi
out = bytearray(b"UNIRAST\0" + struct.pack(">I", 1))
hdr = bytearray(32)
hdr[0] = 8                          # bits per pixel
hdr[1] = 0                          # colorspace 0 = sGray
struct.pack_into(">I", hdr, 12, W)
struct.pack_into(">I", hdr, 16, H)
struct.pack_into(">I", hdr, 20, RES)
out += hdr

for y in range(H):
    row = bytes(0 if ((x // 16) + (y // 16)) % 2 else 255 for x in range(W))
    out.append(0)                   # line repeat: emit once
    x = 0
    while x < W:                    # encode as literal runs of up to 128
        n = min(128, W - x)
        if n == 1:                  # a literal run of one is not encodable
            out.append(0x00); out.append(row[x])
        else:
            out.append(257 - n); out += row[x:x + n]
        x += n

open(sys.argv[1], "wb").write(bytes(out))
print(f"wrote {sys.argv[1]}: {W}x{H} @ {RES}dpi, {len(out)} bytes")
