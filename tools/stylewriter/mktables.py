#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Reads Apple's StyleWriter tone curves, 16 x 16 halftone screen and printer icon out of
Apple's own Color StyleWriter drivers (W-1103), and writes them as source.

Apple's driver files are never committed (they live in ~/.config/airwriter/sw-drivers/);
what comes out is the tables, which are Apple's (LICENSE.md). Inputs are the resource
forks of three driver files, in any form that holds one: a raw fork (what untome.py
writes), a MacBinary file, an AppleDouble file, or a file on macOS with a resource fork
(read as FILE/..namedfork/rsrc). untome.py gets the forks out of Apple's installers:

    unar Color_SW_1500_2.2.1-3of3.sea.bin          # a Disk Copy image; the 2500's disk 3 of 4 likewise
    unar Color_SW_2200.sit                         # the Tome files themselves
    python3 tools/stylewriter/untome.py OUT "Color SW 1500 Disk 3" "Color SW 2500 Disk 3" \\
        "Color SW 2200 2.1.2/Color SW 2200 Install/Printing Tools 1 Tome"
    python3 tools/stylewriter/mktables.py \\
        --cs2200 "OUT/Printing Tools 1 Tome/Color SW 2200.rsrc" \\
        --sw1500 "OUT/Color SW 1500 Disk 3/Printer SW Tome 3/Color SW 1500.rsrc" \\
        --cs2500 "OUT/Color SW 2500 Disk 3/Printer SW Tome 3/Color SW 2500.rsrc"

What it takes, and from where:
  GAMA 1 and 2, 256 bytes each, from the 2200 driver (the Color StyleWriter 2200, 2400 and
  2500) and from the 1500 driver (the original, II, 1200 and 1500): apple_tone_cs and
  apple_tone_sw. HTSC 1, a 16 x 16 screen with a four byte header, from both: apple_screen16.
  icl8 and ICN# -8193 of the 2500 driver, the printer's Chooser icon, as a pixel grid
  (tools/icon/stylewriter.txt, which tools/icon/mkicons.py turns into the PNGs).
  The drivers hold no clut: the icon's colours are the Macintosh system palette (clut 8),
  which this script writes out from its definition, and then maps to the nearest of the
  icon palette's colours, since that palette must not grow (the ImageWriter's PNGs would
  change). It lists every colour that had no exact match.

Before it writes anything: every GAMA is 256 bytes and never falls; every HTSC has the 16 x 16
header and 256 values; the two HTSC 1 agree (and the 2500's, which it only reports).

--compare raster.rs checks the result against another reading of the same drivers, TailTalk's
stylewriter/raster.rs, and lists every value that differs. That file is GPL-3.0 without an
"or later", so it is only ever compared against: nothing is copied from it.
"""
import argparse
import importlib.util
import pathlib
import re
import struct
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
HEADER = ROOT / "firmware/components/halftone/include/apple_tables.h"
SOURCE = ROOT / "firmware/components/halftone/apple_tables.c"
GRID = ROOT / "tools/icon/stylewriter.txt"
MKICONS = ROOT / "tools/icon/mkicons.py"

ICON_ID = -8193                 # the Chooser icon; -8190 is it selected and -8189 is it crossed out
TAILTALK = "7f48bf5"            # TailTalk commit whose raster.rs the notes in the header cite


# ---- Resource forks -------------------------------------------------------

def crc16_xmodem(data):
    crc = 0
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def looks_like_fork(d):
    if len(d) < 30:
        return False
    data, mapat, dlen, mlen = struct.unpack(">IIII", d[:16])
    return data >= 16 and data + dlen <= mapat and mapat + mlen <= len(d) and mlen >= 30


def resource_fork(path):
    """The resource fork `path` holds, whatever shape the file is in."""
    d = pathlib.Path(path).read_bytes()
    if d[:4] in (b"\x00\x05\x16\x07", b"\x00\x05\x16\x00"):               # AppleDouble / AppleSingle
        for i in range(struct.unpack(">H", d[24:26])[0]):
            eid, off, ln = struct.unpack(">III", d[26 + 12 * i:38 + 12 * i])
            if eid == 2:
                return d[off:off + ln]
        raise ValueError("%s: no resource fork in the AppleDouble file" % path)
    if len(d) >= 128 and d[0] == 0 and 1 <= d[1] <= 63 and d[74] == 0 and d[82] == 0:
        dlen, rlen = struct.unpack(">II", d[83:87] + d[87:91])             # MacBinary
        if 128 + -(-dlen // 128) * 128 + rlen <= len(d) + 127 and (
                d[124:126] == b"\0\0" or crc16_xmodem(d[:124]) == struct.unpack(">H", d[124:126])[0]):
            at = 128 + -(-dlen // 128) * 128
            return d[at:at + rlen]
    if looks_like_fork(d):
        return d
    try:
        d = pathlib.Path(str(path) + "/..namedfork/rsrc").read_bytes()
    except OSError:
        d = b""
    if looks_like_fork(d):
        return d
    raise ValueError("%s: no resource fork found" % path)


def resources(fork):
    """{type: {id: bytes}} from a resource fork's map."""
    data, mapat, dlen, mlen = struct.unpack(">IIII", fork[:16])
    m = fork[mapat:mapat + mlen]
    types, names = struct.unpack(">HH", m[24:28])
    out = {}
    for i in range(struct.unpack(">H", m[types:types + 2])[0] + 1):
        t, n, ref = struct.unpack(">4sHH", m[types + 2 + 8 * i:types + 10 + 8 * i])
        for j in range(n + 1):
            rid, _, off, _ = struct.unpack(">hHII", m[types + ref + 12 * j:types + ref + 12 * j + 12])
            at = data + (off & 0xFFFFFF)
            size = struct.unpack(">I", fork[at:at + 4])[0]
            if at + 4 + size > data + dlen:
                raise ValueError("resource %s %d runs past the data" % (t.decode("mac_roman"), rid))
            out.setdefault(t.decode("mac_roman"), {})[rid] = fork[at + 4:at + 4 + size]
    return out


def short_version(res):
    v = res["vers"][1]
    return v[7:7 + v[6]].decode("mac_roman")


# ---- Tables ---------------------------------------------------------------

class Driver:
    def __init__(self, label, path):
        self.label = label                      # the driver's file name, as Apple names it
        self.res = resources(resource_fork(path))
        self.version = short_version(self.res)

    def gama(self, rid):
        t = self.res["GAMA"][rid]
        if len(t) != 256:
            raise ValueError("%s GAMA %d is %d bytes, not 256" % (self.label, rid, len(t)))
        if any(b < a for a, b in zip(t, t[1:])):
            raise ValueError("%s GAMA %d falls somewhere; a tone curve never does" % (self.label, rid))
        return list(t)

    def htsc(self, rid):
        t = self.res["HTSC"][rid]
        if len(t) != 4 + 256 or t[:4] != b"\x00\x10\x00\x10":
            raise ValueError("%s HTSC %d is not a 16 x 16 screen" % (self.label, rid))
        return list(t[4:])


def mac_system_palette():
    """The Macintosh system 8-bit palette (clut 8): the 6 x 6 x 6 colour cube from white,
    black left out, then ten steps each of red, green, blue and gray, then black."""
    cube = [0xFF, 0xCC, 0x99, 0x66, 0x33, 0x00]
    pal = [(r, g, b) for r in cube for g in cube for b in cube][:215]
    ramp = [0xEE, 0xDD, 0xBB, 0xAA, 0x88, 0x77, 0x55, 0x44, 0x22, 0x11]
    pal += [(v, 0, 0) for v in ramp] + [(0, v, 0) for v in ramp] + [(0, 0, v) for v in ramp]
    pal += [(v, v, v) for v in ramp] + [(0, 0, 0)]
    assert len(pal) == 256
    return pal


def icon_grid(driver):
    """The icon as the rows of tools/icon/stylewriter.txt, and what the palette change cost."""
    icl8, icn = driver.res["icl8"][ICON_ID], driver.res["ICN#"][ICON_ID]
    if len(icl8) != 1024 or len(icn) != 256:
        raise ValueError("icon %d is not 32 x 32" % ICON_ID)
    spec = importlib.util.spec_from_file_location("mkicons", MKICONS)
    mkicons = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mkicons)
    palette = mkicons.PALETTE
    system = mac_system_palette()
    rows, mapped = [], {}
    for y in range(32):
        row = ""
        for x in range(32):
            if not icn[128 + y * 4 + x // 8] & (0x80 >> (x % 8)):
                row += "."
                continue
            rgb = system[icl8[y * 32 + x]]
            key = min(sorted(palette), key=lambda k: sum((a - b) ** 2 for a, b in zip(palette[k], rgb)))
            exact = palette[key] == rgb
            mapped.setdefault((rgb, key, exact), 0)
            mapped[(rgb, key, exact)] += 1
            row += key
        rows.append(row)
    return rows, mapped


# ---- Output ---------------------------------------------------------------

def c_array(name, values, per_row=16):
    out = ["const uint8_t %s[%d] = {" % (name, len(values))]
    for i in range(0, len(values), per_row):
        out.append("    " + ", ".join("0x%02X" % v for v in values[i:i + per_row]) + ",")
    out.append("};")
    return "\n".join(out)


PREAMBLE = """\
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Generated by tools/stylewriter/mktables.py: do not edit.
// © Apple; extracted from Apple's Color StyleWriter 1500, 2200 and 2500 drivers by
// tools/stylewriter/mktables.py; not licensed under this repository's licences (LICENSE.md).
"""


def header_text(cs, sw, sc, same):
    return PREAMBLE + """\
//
// Where each table is from: the driver file, its version, and the resource.
//   apple_screen16    HTSC 1, 16 x 16 thresholds, row by row, after its four byte header. In '%(cs)s'
//                     %(csv)s and '%(sw)s' %(swv)s alike%(also)s.
//   apple_tone_cs[0]  GAMA 1 of '%(cs)s' %(csv)s.
//   apple_tone_cs[1]  GAMA 2 of '%(cs)s' %(csv)s.
//   apple_tone_sw[0]  GAMA 1 of '%(sw)s' %(swv)s.
//   apple_tone_sw[1]  GAMA 2 of '%(sw)s' %(swv)s.
//
// Which curve a job takes, and how. Read from TailTalk's stylewriter/raster.rs (commit %(tt)s;
// GPL-3.0, so read for these facts and compared against, never copied from):
//   By model: the Color StyleWriter 2200, 2400 and 2500 take apple_tone_cs; the original
//     StyleWriter, the II, the 1200 and the Color 1500 take apple_tone_sw (raster.rs 163-171,
//     tone_curve(); the doc comments on the four curves, 35-36, 56-57, 77-79 and 99-101).
//   By screen, not by colour or gray: [0], GAMA 1, goes with the 16 x 16 pattern screen
//     (apple_screen16, HT_APPLE, flat colour and text); [1], GAMA 2, with the error-diffusion
//     scatter the drivers use for photographs (raster.rs 163-171 again, and 247-253).
//   How: a lightness curve, 0 black to 255 white, put on each gray pixel and on each of R, G and
//     B before a colour pixel is split into inks (raster.rs 173-182, 365-366, 385-386).
//   The screen: a colour ink prints where its amount (255 less its lightness) is over the
//     threshold, and black where it is over the threshold mirrored about 254, the largest
//     threshold; a gray pixel prints where its toned lightness is under the threshold (raster.rs
//     120-125, 428-449). A page's first halftoned row reads the screen's second row, and the
//     rows after it the rows after that (raster.rs 238-239, 421-426).
#pragma once
#include <stdint.h>

extern const uint8_t apple_screen16[256];
extern const uint8_t apple_tone_cs[2][256];
extern const uint8_t apple_tone_sw[2][256];
""" % {"cs": cs.label, "csv": cs.version, "sw": sw.label, "swv": sw.version, "tt": TAILTALK,
       "also": (", and in '%s' %s" % (sc.label, sc.version)) if same else ""}


def source_text(screen, cs, sw):
    return (PREAMBLE + """\
//
// The tables declared in apple_tables.h, where the drivers they are from are named.
#include "apple_tables.h"

""" + c_array("apple_screen16", screen) + "\n\n"
            + "const uint8_t apple_tone_cs[2][256] = {\n"
            + "".join("    {\n" + "\n".join("        " + ", ".join("0x%02X" % v for v in t[i:i + 16]) + ","
                                            for i in range(0, 256, 16)) + "\n    },\n" for t in cs)
            + "};\n\n"
            + "const uint8_t apple_tone_sw[2][256] = {\n"
            + "".join("    {\n" + "\n".join("        " + ", ".join("0x%02X" % v for v in t[i:i + 16]) + ","
                                            for i in range(0, 256, 16)) + "\n    },\n" for t in sw)
            + "};\n")


def compare(path, ours):
    """Reports every value that differs from TailTalk's; returns how many did."""
    text = pathlib.Path(path).read_text()
    wrong = 0
    for name, table in ours.items():
        m = re.search(r"pub const %s: \[u8; 256\] = \[(.*?)\];" % name, text, re.S)
        if not m:
            print("compare: %s is not in %s" % (name, path))
            wrong += 1
            continue
        theirs = [int(v, 16) for v in re.findall(r"0x([0-9A-Fa-f]{2})", m.group(1))]
        if len(theirs) != 256:
            print("compare: %s has %d values in %s" % (name, len(theirs), path))
            wrong += 1
            continue
        diff = [(i, a, b) for i, (a, b) in enumerate(zip(table, theirs)) if a != b]
        print("compare: %-22s %s" % (name, "256 of 256 agree" if not diff else "%d differ" % len(diff)))
        for i, a, b in diff:
            print("    [%3d] ours 0x%02X, theirs 0x%02X" % (i, a, b))
        wrong += len(diff)
    return wrong


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--cs2200", required=True, help="the Color SW 2200 driver's resource fork")
    ap.add_argument("--sw1500", required=True, help="the Color SW 1500 driver's resource fork")
    ap.add_argument("--cs2500", required=True, help="the Color SW 2500 driver's resource fork")
    ap.add_argument("--header", default=str(HEADER))
    ap.add_argument("--source", default=str(SOURCE))
    ap.add_argument("--icon-grid", default=str(GRID))
    ap.add_argument("--compare", metavar="RASTER_RS", help="TailTalk's raster.rs, to compare with")
    ap.add_argument("--dry-run", action="store_true", help="check and compare; write nothing")
    args = ap.parse_args()

    cs = Driver("Color SW 2200", args.cs2200)
    sw = Driver("Color SW 1500", args.sw1500)
    sc = Driver("Color SW 2500", args.cs2500)

    screen = cs.htsc(1)
    if sw.htsc(1) != screen:
        raise SystemExit("mktables: HTSC 1 differs between the 2200 and 1500 drivers")
    same = sc.htsc(1) == screen
    print("HTSC 1: the %s and %s drivers agree; the 2500's %s" %
          (cs.label, sw.label, "agrees too" if same else "DIFFERS (not used)"))
    tone_cs = [cs.gama(1), cs.gama(2)]
    tone_sw = [sw.gama(1), sw.gama(2)]
    for i in (1, 2):
        print("GAMA %d: the 2500 driver's %s the 2200's" %
              (i, "matches" if sc.gama(i) == tone_cs[i - 1] else "DIFFERS from"))
    print("GAMA: 4 curves of 256, none falling")

    rows, mapped = icon_grid(sc)
    print("icon %d: %d pixels, %d colours" % (ICON_ID, sum(mapped.values()), len({k[0] for k in mapped})))
    for (rgb, key, exact), n in sorted(mapped.items(), key=lambda kv: -kv[1]):
        print("    %3d px  #%02X%02X%02X -> %s%s" % (n, *rgb, key, "" if exact else "  (nearest; no exact match)"))

    bad = 0
    if args.compare:
        bad = compare(args.compare, {
            "TONE_PATTERN_2200": tone_cs[0], "TONE_DIFFUSION_2200": tone_cs[1],
            "TONE_PATTERN_1500": tone_sw[0], "TONE_DIFFUSION_1500": tone_sw[1],
            "HALFTONE_PATTERN": screen})
    if not args.dry_run:
        pathlib.Path(args.header).write_text(header_text(cs, sw, sc, same))
        pathlib.Path(args.source).write_text(source_text(screen, tone_cs, tone_sw))
        pathlib.Path(args.icon_grid).write_text("\n".join(rows) + "\n")
        print("wrote %s, %s, %s" % (args.header, args.source, args.icon_grid))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
