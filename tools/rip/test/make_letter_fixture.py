#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Write a US Letter (8.5x11in @ 300dpi) 8-bit grayscale URF with a solid
black 0.25in margin on each side and a distinct vertical bar, for exercising
the horizontal margin-crop path in tools/rip/main.c. checker.urf (the
committed regression fixture) is only 2in wide, so it never triggers that
path -- this fixture is the one that does. Not itself a regression target;
rip it and check the crop arithmetic by hand (see main.c's stderr notice
and docs/superpowers/plans/2026-08-04-rip-host-pipeline.md's crop rule).
"""
import struct, sys

W, H, RES = 2550, 3300, 300        # 8.5in x 11in at 300dpi
MARGIN = RES // 4                  # 0.25in = 75 source px per side
BAR_X0, BAR_W = 575, 30            # vertical bar, well clear of both margins

out = bytearray(b"UNIRAST\0" + struct.pack(">I", 1))
hdr = bytearray(32)
hdr[0] = 8                          # bits per pixel
hdr[1] = 0                          # colorspace 0 = sGray
struct.pack_into(">I", hdr, 12, W)
struct.pack_into(">I", hdr, 16, H)
struct.pack_into(">I", hdr, 20, RES)
out += hdr

row = bytearray([255]) * W          # 255 = white, 0 = black (sGray, additive)
for x in range(MARGIN):
    row[x] = 0
    row[W - 1 - x] = 0
for x in range(BAR_X0, BAR_X0 + BAR_W):
    row[x] = 0


def encode_row(r):
    """Repeat-encode one line's pixel data: chunks of up to 128 identical
    bytes, each as a single (ctl, value) pair. ctl+1 copies of value follow."""
    enc = bytearray()
    x = 0
    while x < len(r):
        v = r[x]
        n = 1
        while x + n < len(r) and r[x + n] == v and n < 128:
            n += 1
        enc.append(n - 1)
        enc.append(v)
        x += n
    return bytes(enc)


payload = encode_row(row)

# Every row is identical, so use the line-repeat byte to avoid re-encoding
# it H times: each decode_line() call supplies one payload plus a repeat
# count of up to 256 (ctl 0..255 means ctl+1 copies).
rows_left = H
while rows_left > 0:
    n = min(256, rows_left)
    out.append(n - 1)               # line repeat: emit n times
    out += payload
    rows_left -= n

open(sys.argv[1], "wb").write(bytes(out))
print(f"wrote {sys.argv[1]}: {W}x{H} @ {RES}dpi, margin={MARGIN}px/side, "
      f"bar=[{BAR_X0},{BAR_X0 + BAR_W}), {len(out)} bytes")
