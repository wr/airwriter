#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Takes the files out of Apple's old Installer packages, so that mktables.py can
read the Color StyleWriter drivers out of Apple's own installers.

Apple's drivers are not shipped as loose files. The Color StyleWriter 1500 and
2500 installers are a set of Disk Copy 4.2 floppy images (HFS volumes) each holding
an "Installer" script and one or more "Tome" files; the 2200's is a StuffIt archive
holding the Tomes directly. A Tome holds the files the script installs, each fork
compressed. This script reads all three layers, with no Apple code or data in it:

    unar Color_SW_1500_2.2.1-3of3.sea.bin        # StuffIt; leaves a Disk Copy image
    python3 untome.py OUT "Color SW 1500 Disk 3"  # HFS volume, then the Tome in it

and writes, for every file, NAME.data and NAME.rsrc (its two forks, when they are not
empty) under OUT/<volume>/<Tome>/ (OUT/<Tome>/ for a Tome given on its own). Resource forks come out whole, ready for
mktables.py, which reads a raw fork. Input is any mix of Disk Copy 4.2 images, raw HFS
volume images and Tome files. Nothing is executed and nothing outside OUT is written.

The Tome format, as worked out from the Installer's own decompressor:

  Header  'kc' 00 01, then at 0x18 a u16 offset of the file table and at 0x1A a u16
          file count. A file is 128 bytes: u16 1, u32 id, a Pascal name (32 bytes),
          type, creator, created, modified, then at +0x3C the data fork and at +0x4C
          the resource fork, each { u32 size, u32 offset in the Tome, u32 packed
          size, u32 checksum } (a checksum whose algorithm is not known; the packed
          fork is checked by its own structure instead).
  A fork  is a run of chunks, each the four bytes 00 01 00 00 and then a bit stream
          (most significant bit first). A chunk ends at the first token boundary where
          the fork's output reaches the next multiple of 64 KB, and is padded to a byte.
          The history carries over from one chunk to the next; the state does not.
  Tokens  alternate between a literal run and a match, and the choice is in the
          count: a count code of zero, when a literal run may start (at the start of
          a chunk, after a match, or after a run of 63), is a literal run. Otherwise
          it is a match of count + 2 bytes (+1 more straight after a shorter run).
          Literal bytes are 8 bits each. A match then has a distance, coded by how
          much history there is: the window grows through twelve code sets, doubling,
          and stops at 43008 bytes (0xA800).
"""
import argparse
import os
import struct
import sys

CHUNK = 65536
WINDOW = 0xA800
# Where each distance code set takes over, in bytes of history.
LIMITS = (0x0A, 0x14, 0x28, 0x50, 0xA0, 0x2A0, 0x3E8, 0xA80, 0x1500, 0x2A00, 0x5400, 0xA800)
# The longest tail code sets add a bit where history reaches base + 2^k - 1, doubling each
# time, with one exception in Apple's table: the set for 1000 to 2688 bytes adds its 11th
# bit at 1644 and not 1664.
TAIL_AT = {(7, 10): 1644}
# Count codes with three or more leading ones: (extra bits, base) by number of ones - 3.
COUNT_TAIL = ((3, 11), (3, 19), (5, 27), (6, 59), (7, 123), (8, 251), (9, 507), (10, 1019))


class Bits:
    """A most-significant-bit-first reader over bytes."""

    def __init__(self, data, at):
        self.data, self.at, self.acc, self.n = data, at, 0, 0

    def __call__(self, n):
        while self.n < n:
            if self.at >= len(self.data):
                raise ValueError("packed data ends mid-token")
            self.acc = (self.acc << 8) | self.data[self.at]
            self.at += 1
            self.n += 8
        self.n -= n
        v = (self.acc >> self.n) & ((1 << n) - 1)
        self.acc &= (1 << self.n) - 1
        return v


def count_code(b):
    n = 0
    while n < 10 and b(1):
        n += 1
    if n == 0:
        return b(1)
    if n == 1:
        return b(1) + 3 if b(1) else 2
    if n == 2:
        return b(2) + 7 if b(1) else b(1) + 5
    bits, base = COUNT_TAIL[n - 3]
    return b(bits) + base


def run_code(b):
    if not b(1):
        return 1
    n = b(2)
    if n < 2:
        return 2 + n
    if n == 2:
        return b(2) + 4
    n = b(4)
    if n < 8:
        return n + 8
    if n < 12:
        return (n - 8) * 4 + 16 + b(2)
    return (n - 12) * 8 + 32 + b(3)


def distance_code(pos, b):
    pos = min(pos, WINDOW)
    i = next(k for k, limit in enumerate(LIMITS) if pos <= limit)
    if not b(1):
        return 1 + b(i) if i else 1
    if not b(1):
        return 1 + (1 << i) + b(i + 2)
    base = 1 + (1 << i) + (1 << (i + 2))
    k = 1
    while TAIL_AT.get((i, k), base + (1 << k) - 1) < pos:
        k += 1
    return base + b(k)


def unpack_fork(packed, size, chunk=CHUNK):
    """The `size` bytes a packed fork holds."""
    out = bytearray()
    at = 0
    while len(out) < size:
        if packed[at:at + 4] != b"\x00\x01\x00\x00":
            raise ValueError("no chunk header at %d (output %d)" % (at, len(out)))
        b = Bits(packed, at + 4)
        stop = min(size, (len(out) // chunk + 1) * chunk)
        free = True                       # may a literal run start here?
        while len(out) < stop:
            n = count_code(b)
            if n == 0 and free:
                run = run_code(b)
                out += bytes(b(8) for _ in range(run))
                free = run >= 63
                continue
            n += 2 if free else 3
            free = True
            d = distance_code(len(out), b)
            if d > len(out):
                raise ValueError("distance %d reaches before the fork at %d" % (d, len(out)))
            for _ in range(n):
                out.append(out[-d])
        at = b.at
    return bytes(out[:size])


def tome_files(tome):
    """(name, type, creator, [data fork, resource fork]) for each file in a Tome."""
    if tome[:4] != b"kc\x00\x01":
        raise ValueError("not a Tome")
    table, count = struct.unpack(">HH", tome[0x18:0x1C])
    for i in range(count):
        r = table + 128 * i
        name = tome[r + 7:r + 7 + tome[r + 6]].decode("mac_roman")
        forks = []
        for at in (r + 0x3C, r + 0x4C):
            size, off, packed, _ = struct.unpack(">IIII", tome[at:at + 16])
            forks.append(unpack_fork(tome[off:off + packed], size) if size else b"")
        yield name, tome[r + 0x26:r + 0x2A], tome[r + 0x2A:r + 0x2E], forks


# ---- Disk Copy 4.2 and HFS ------------------------------------------------

def disk_image(data):
    """The sectors of a Disk Copy 4.2 file (84 bytes of header), or the data as it is."""
    if len(data) > 84 and data[0] <= 63 and data[82:84] == b"\x01\x00":
        size = struct.unpack(">I", data[64:68])[0]
        return data[84:84 + size]
    return data


class Hfs:
    """Reads files out of a classic HFS volume image."""

    def __init__(self, image):
        self.d = image
        mdb = image[1024:1536]
        if mdb[:2] != b"BD":
            raise ValueError("not an HFS volume")
        self.name = mdb[37:37 + mdb[36]].decode("mac_roman")
        self.block = struct.unpack(">I", mdb[20:24])[0]
        self.first = struct.unpack(">H", mdb[28:30])[0] * 512
        self.catalog = self.read(struct.unpack(">6H", mdb[150:162]),
                                 struct.unpack(">I", mdb[146:150])[0])
        self.overflow = self.read(struct.unpack(">6H", mdb[134:146]),
                                  struct.unpack(">I", mdb[130:134])[0])
        self.extents = {}
        for rec in self.leaves(self.overflow):
            fid, start = struct.unpack(">IH", rec[2:8])   # rec[1] is the fork: 0 data, 0xFF resource
            self.extents[(fid, rec[1], start)] = struct.unpack(">6H", rec[8:20])
        self.kids = {}                    # parent id -> [(name, kind, id or record)]
        for rec in self.leaves(self.catalog):
            parent = struct.unpack(">I", rec[2:6])[0]
            name = rec[7:7 + rec[6]].decode("mac_roman")
            body = rec[1 + rec[0] + ((1 + rec[0]) & 1):]
            if body[0] == 1:
                self.kids.setdefault(parent, []).append((name, "dir", struct.unpack(">I", body[6:10])[0]))
            elif body[0] == 2:
                self.kids.setdefault(parent, []).append((name, "file", body))

    def read(self, extents, length):
        out = b""
        for start, count in zip(extents[0::2], extents[1::2]):
            at = self.first + start * self.block
            out += self.d[at:at + count * self.block]
        return out[:length]

    def leaves(self, tree):
        node = struct.unpack(">I", tree[24:28])[0]       # the header record's first leaf
        while node:
            raw = tree[node * 512:(node + 1) * 512]
            link, _, _, _, nrec = struct.unpack(">IIbBH", raw[:12])
            offs = struct.unpack(">%dH" % (nrec + 1), raw[512 - 2 * (nrec + 1):])[::-1]
            for i in range(nrec):
                yield raw[offs[i]:offs[i + 1]]
            node = link

    def fork(self, rec, which):
        """A file record's data (0) or resource (1) fork."""
        fid = struct.unpack(">I", rec[20:24])[0]
        length, at = (26, 74) if which == 0 else (36, 86)
        size = struct.unpack(">I", rec[length:length + 4])[0]
        extents = list(struct.unpack(">6H", rec[at:at + 12]))
        have = sum(extents[1::2])
        while (fid, 0xFF * which, have) in self.extents:
            more = self.extents[(fid, 0xFF * which, have)]
            extents += more
            have += sum(more[1::2])
        return self.read(extents, size)

    def walk(self, parent=2, path=()):
        for name, kind, what in sorted(self.kids.get(parent, []), key=lambda k: k[0]):
            if kind == "dir":
                yield from self.walk(what, path + (name,))
            else:
                yield path + (name,), what[4:8], what[8:12], [self.fork(what, 0), self.fork(what, 1)]


# ---- Output ---------------------------------------------------------------

def check_resource_fork(name, fork):
    """A resource fork's header says where its map is and how long: a packed fork that
    came out wrong shows there. Anything too small to be one is not checked."""
    if len(fork) < 300:
        return
    data, mapat, data_len, map_len = struct.unpack(">IIII", fork[:16])
    if data + data_len != mapat or mapat + map_len != len(fork):
        raise ValueError("%s: the resource fork came out damaged" % name)


def safe(name):
    return name.replace("/", ":").replace("\0", "").lstrip(".") or "_"


def save(out, path, forks):
    os.makedirs(out, exist_ok=True)
    stem = os.path.join(out, safe(path))
    for suffix, fork in zip((".data", ".rsrc"), forks):
        if fork:
            with open(stem + suffix, "wb") as f:
                f.write(fork)
    return stem


def unpack(out, label, data, log):
    """Writes what is in `data`, a Tome or a volume image, under `out`."""
    if data[:4] == b"kc\x00\x01":
        into = os.path.join(out, safe(label))
        for name, ftype, creator, forks in tome_files(data):
            check_resource_fork(name, forks[1])
            save(into, name, forks)
            log("  %s  (%s %s)  %d + %d bytes" % (name, ftype.decode("mac_roman"),
                creator.decode("mac_roman"), len(forks[0]), len(forks[1])))
        return
    volume = Hfs(disk_image(data))
    log("volume %s" % volume.name)
    for names, ftype, creator, forks in volume.walk():
        if forks[0][:4] == b"kc\x00\x01":
            log("Tome %s" % names[-1])
            unpack(os.path.join(out, safe(volume.name)), names[-1], forks[0], log)
        else:
            save(os.path.join(out, safe(volume.name), *map(safe, names[:-1])), names[-1], forks)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("out", help="directory to write under")
    ap.add_argument("inputs", nargs="+", help="Disk Copy images, HFS images or Tome files")
    args = ap.parse_args()
    for path in args.inputs:
        with open(path, "rb") as f:
            unpack(args.out, os.path.basename(path), f.read(), print)


if __name__ == "__main__":
    sys.exit(main())
