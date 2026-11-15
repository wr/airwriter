#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Reads the Personal LaserWriter 300's halftone screen, tone curve and icon out of Apple's
LaserWriter 300 driver (W-1102), the Personal LaserWriter LS's icon out of its own driver
(W-1145), and the PGRD table a LaserWriter Select 300 is sent out of the 300's driver
(W-1146), and writes them as source.

Apple's drivers are never committed (they live in ~/.config/airwriter/lw300-driver/ and
plw-ls-driver/); what comes out is the tables and the icons, which are Apple's (LICENSE.md). The input is the driver's
resource fork in any form tools/stylewriter/mktables.py reads: MacBinary, AppleDouble, a raw
fork, or a file on macOS with a resource fork.

    python3 tools/qdlaser/mkqdtables.py --driver "LaserWriter 300 v1.1.bin" \
                                        --ls-driver "Personal LW LS v1.2.bin"
    python3 tools/icon/mkicons.py

What it takes, and from where:
  HTSC 2, an 8 x 8 clustered-dot screen with a four byte header: apple_lw_screen8.
  GAMA 3, 256 bytes from 0 to 33, the curve the driver puts each gray through before that
  screen: apple_lw_tone.
  PGRD 1, a count of 32 then 32 entries of two big-endian words, which the driver sends a
  printer on page memory at the start of every job: apple_lw_pgrd, the 128 bytes after the
  count.
  icl8 128 cut out by ICN# 128, the printer's Chooser icon, as a pixel grid
  (tools/icon/apple/plw300.txt, which tools/icon/mkicons.py turns into the PNGs), in the
  Macintosh system palette's colours mapped to mkicons.py's APPLE_PALETTE.
  From the LS's driver (Personal LW LS 7.1), its icl8 128 cut out by ICN# 128 the same way:
  tools/icon/apple/plwls.txt. Either driver may be given alone.

Before it writes anything: HTSC 2 is 68 bytes with the header 00 08 00 08, and its 64
thresholds are 1 to 32, each twice; GAMA 3 is 256 bytes, never falls, starts at 0 and ends at
33; PGRD 1 is 130 bytes with the count 32. --sizes prints PREC 3's paper names with their sizes in 1/120 in.
"""
import argparse
import importlib.util
import pathlib
import struct
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
HEADER = ROOT / "firmware/components/halftone/include/apple_tables_lw.h"
SOURCE = ROOT / "firmware/components/halftone/apple_tables_lw.c"
GRID = ROOT / "tools/icon/apple/plw300.txt"
LS_GRID = ROOT / "tools/icon/apple/plwls.txt"
MKTABLES = ROOT / "tools/stylewriter/mktables.py"
MKICONS = ROOT / "tools/icon/mkicons.py"

ICON_ID = 128
DRIVER = "LaserWriter 300"


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


mk = load("mktables", MKTABLES)


def screen(res):
    t = res["HTSC"][2]
    if len(t) != 4 + 64 or t[:4] != b"\x00\x08\x00\x08":
        raise ValueError("HTSC 2 is not an 8 x 8 screen")
    v = list(t[4:])
    if sorted(v) != sorted(list(range(1, 33)) * 2):
        raise ValueError("HTSC 2's thresholds are not 1 to 32, each twice")
    return v


def tone(res):
    t = res["GAMA"][3]
    if len(t) != 256:
        raise ValueError("GAMA 3 is %d bytes, not 256" % len(t))
    if any(b < a for a, b in zip(t, t[1:])):
        raise ValueError("GAMA 3 falls somewhere; a tone curve never does")
    if t[0] != 0 or t[-1] != 33:
        raise ValueError("GAMA 3 runs %d to %d, not 0 to 33" % (t[0], t[-1]))
    return list(t)


def pgrd(res):
    t = res["PGRD"][1]
    if len(t) != 2 + 32 * 4 or struct.unpack(">H", t[:2])[0] != 32:
        raise ValueError("PGRD 1 is not a count of 32 and 32 entries of two words")
    return list(t[2:])


def sizes(res):
    """PREC 3: a count, then (height, width) in 1/120 in for each, then the names."""
    p = res["PREC"][3]
    n = struct.unpack(">H", p[:2])[0]
    dims = [struct.unpack(">HH", p[2 + 4 * i:6 + 4 * i]) for i in range(n)]
    at, names = 2 + 4 * n, []
    for _ in range(n):
        ln = p[at]
        names.append(p[at + 1:at + 1 + ln].decode("mac_roman"))
        at += 1 + ln
    return list(zip(names, dims))


def icon_grid(res):
    try:
        icl8, icn = res["icl8"][ICON_ID], res["ICN#"][ICON_ID]
    except KeyError:
        raise ValueError("no icl8 and ICN# %d" % ICON_ID)
    if len(icl8) != 1024 or len(icn) != 256:
        raise ValueError("icon %d is not 32 x 32" % ICON_ID)
    palette = load("mkicons", MKICONS).APPLE_PALETTE
    system = mk.mac_system_palette()
    rows, mapped = [], {}
    for y in range(32):
        row = ""
        for x in range(32):
            if not icn[128 + y * 4 + x // 8] & (0x80 >> (x % 8)):
                row += "."
                continue
            rgb = system[icl8[y * 32 + x]]
            key = min(sorted(palette), key=lambda k: sum((a - b) ** 2 for a, b in zip(palette[k], rgb)))
            mapped[(rgb, key, palette[key] == rgb)] = mapped.get((rgb, key, palette[key] == rgb), 0) + 1
            row += key
        rows.append(row)
    return rows, mapped


PREAMBLE = """\
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Generated by tools/qdlaser/mkqdtables.py: do not edit.
// © Apple; extracted from Apple's LaserWriter 300 driver %(v)s by tools/qdlaser/mkqdtables.py;
// not licensed under this repository's licences (LICENSE.md).
"""


def header_text(version):
    return PREAMBLE % {"v": version} + """\
//
// Where each table is from: '%(d)s' %(v)s, the driver for the Personal LaserWriter 300.
//   apple_lw_screen8  HTSC 2, 8 x 8 thresholds 1 to 32, row by row, after its four byte header.
//   apple_lw_tone     GAMA 3, which takes a gray (0 black, 255 white) to 0 to 33.
//   apple_lw_pgrd     PGRD 1 after its count: 32 entries of two big-endian words, which the
//                     driver sends a printer on page memory at each job's start (qd_session.c).
// How the driver uses them: a pixel is ink where the screen's threshold is over the toned gray
// (halftone.c, HT_APPLE_LW).
#pragma once
#include <stdint.h>

extern const uint8_t apple_lw_screen8[64];
extern const uint8_t apple_lw_tone[256];
extern const uint8_t apple_lw_pgrd[128];
""" % {"d": DRIVER, "v": version}


def source_text(version, scr, tn, pg):
    return (PREAMBLE % {"v": version} + """\
//
// The tables declared in apple_tables_lw.h, where the driver they are from is named.
#include "apple_tables_lw.h"

""" + mk.c_array("apple_lw_screen8", scr, 8) + "\n\n" + mk.c_array("apple_lw_tone", tn) + "\n\n"
            + mk.c_array("apple_lw_pgrd", pg, 8) + "\n")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--driver", help="Apple's LaserWriter 300 driver (its resource fork)")
    ap.add_argument("--ls-driver", help="Apple's Personal LW LS driver (its resource fork)")
    ap.add_argument("--header", default=str(HEADER))
    ap.add_argument("--source", default=str(SOURCE))
    ap.add_argument("--icon-grid", default=str(GRID))
    ap.add_argument("--ls-icon-grid", default=str(LS_GRID))
    ap.add_argument("--dry-run", action="store_true", help="check; write nothing")
    ap.add_argument("--sizes", action="store_true", help="print PREC 3's paper sizes, 1/120 in")
    a = ap.parse_args()
    if not a.driver and not a.ls_driver:
        ap.error("give --driver, --ls-driver or both")

    writes = []
    if a.driver:
        res = mk.resources(mk.resource_fork(a.driver))
        version = mk.short_version(res)
        scr, tn, pg = screen(res), tone(res), pgrd(res)
        rows, mapped = icon_grid(res)
        report(DRIVER, version, rows, mapped)
        if a.sizes:
            for name, (h, w) in sizes(res):
                print("  %-18s %4d x %4d (height x width, 1/120 in)" % (name, h, w))
        writes += [(a.header, header_text(version)), (a.source, source_text(version, scr, tn, pg)),
                   (a.icon_grid, "\n".join(rows) + "\n")]
    if a.ls_driver:
        res = mk.resources(mk.resource_fork(a.ls_driver))
        rows, mapped = icon_grid(res)
        report("Personal LW LS", mk.short_version(res), rows, mapped)
        writes.append((a.ls_icon_grid, "\n".join(rows) + "\n"))
    if a.dry_run:
        return 0
    for path, text in writes:
        pathlib.Path(path).write_text(text)
    print("wrote " + ", ".join(str(path) for path, _ in writes))
    return 0


def report(driver, version, rows, mapped):
    print("%s %s: icon %d is %d pixels" % (driver, version, ICON_ID,
                                           sum(32 - r.count(".") for r in rows)))
    for (rgb, key, exact), n in sorted(mapped.items(), key=lambda kv: -kv[1]):
        if not exact:
            print("  icon: %d pixels of #%02X%02X%02X drawn as '%s', the nearest" % ((n,) + rgb + (key,)))

if __name__ == "__main__":
    sys.exit(main())
