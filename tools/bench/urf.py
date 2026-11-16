# SPDX-License-Identifier: GPL-3.0-or-later
"""One page of 8-bit gray URF, the format AirWriter takes over AirPrint.

The layout is urf_decode's (firmware/components/urf_decode): "UNIRAST\\0", a
page count, a 32-byte page header, then each row as a line-repeat byte and
runs (0x00-0x7F repeat the next pixel n+1 times, 0x81-0xFF are 257-n literal
pixels).
"""
import struct


def _row(row):
    out = bytearray()
    w = len(row)
    x = 0
    while x < w:
        v = row[x]
        n = 1
        while x + n < w and n < 128 and row[x + n] == v:
            n += 1
        if n >= 3 or x + n == w:
            out += bytes([n - 1, v])
            x += n
            continue
        start = x
        while x < w and x - start < 128:
            if x + 2 < w and row[x] == row[x + 1] == row[x + 2]:
                break
            x += 1
        if x - start == 1:
            out += bytes([0, row[start]])
        else:
            out.append(257 - (x - start))
            out += row[start:x]
    return out


def encode_page(rows, width, dpi):
    """rows: bytes of `width` gray pixels each (0 ink, 255 paper)."""
    out = bytearray(b"UNIRAST\0" + struct.pack(">I", 1))
    hdr = bytearray(32)
    hdr[0] = 8                                       # bits per pixel
    hdr[1] = 0                                       # sGray
    struct.pack_into(">III", hdr, 12, width, len(rows), dpi)
    out += hdr
    y = 0
    while y < len(rows):
        n = 1
        while y + n < len(rows) and n < 256 and rows[y + n] == rows[y]:
            n += 1
        out.append(n - 1)
        out += _row(rows[y])
        y += n
    return bytes(out)
