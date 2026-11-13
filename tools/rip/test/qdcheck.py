#!/usr/bin/env python3.14
# SPDX-License-Identifier: GPL-3.0-or-later
"""The Personal LaserWriter 300's page held to a software printer, twice (W-1102).

For each page, `rip --printer plw300` runs the firmware's page stage and session and writes
three things: the packets in qdfake.py's framing (--out-qd), the line the board clocks out
(--out-qd-bits: BREAK, idle, 8N1 bytes, marks) and the dots it screened (--out-pbm).
qdfake.py decodes both streams by the design's rules, written apart from our coder and our
line image, and the three sets of dots must be the same over the sheet's image area: the
page's rows from (page height - image height) / 2 down, the columns the rip kept; what the
printer has past the page is white.

Pages: the checker (fixtures/checker.urf, 144 dpi, a small page that lands on Letter's area)
and the iOS capture (a Letter page from an iPhone), in Atkinson and in Apple's 300 screen.

    make -C tools/rip qdcheck          (builds build/rip first)
"""
import pathlib
import re
import subprocess
import sys
import tempfile

import numpy as np

HERE = pathlib.Path(__file__).resolve().parent
RIP = HERE.parent / "build" / "rip"
QDFAKE = HERE.parents[1] / "qdlaser" / "qdfake.py"
FIX = HERE.parents[2] / "fixtures"
PAGES = (FIX / "checker.urf", FIX / "ios-20260804-232652.urf")
SCREENS = ("atkinson", "apple")
# The image area's height for each of the 300's sheets, and the page height a client sends.
SHEETS = {3175: 3300, 3750: 4200, 3025: 3150, 2725: 2850, 2125: 2250, 3375: 3508}


def read_pbm(path):
    data = pathlib.Path(path).read_bytes()
    m = re.match(rb"P4\s+(\d+)\s+(\d+)\s", data)
    w, h = int(m.group(1)), int(m.group(2))
    raw = np.frombuffer(data[m.end():], np.uint8).reshape(h, (w + 7) // 8)
    return np.unpackbits(raw, axis=1)[:, :w].astype(bool)


def run(cmd):
    r = subprocess.run([str(c) for c in cmd], capture_output=True, text=True)
    if r.returncode:
        raise RuntimeError(f"{' '.join(map(str, cmd))}\n{r.stdout}{r.stderr}")


def check(page, screen, tmp):
    d = pathlib.Path(tmp) / f"{page.stem}-{screen}"
    d.mkdir()
    run([RIP, "--in", page, "--printer", "plw300", "--dither", screen, "--out-qd", d / "s.bin",
         "--out-qd-bits", d / "b.bin", "--out-pbm", d / "p.pbm"])
    run([sys.executable, QDFAKE, "decode", d / "s.bin", "--out", d / "a", "--no-png"])
    run([sys.executable, QDFAKE, "decode-bits", d / "b.bin", "--out", d / "b", "--no-png"])
    mine = read_pbm(d / "p.pbm")
    a, b = read_pbm(d / "a" / "page-1.pbm"), read_pbm(d / "b" / "page-1.pbm")
    problems = []
    if a.shape != b.shape or (a != b).any():
        problems.append("the packet stream and the line image decode to different dots")
    h, w = a.shape
    top = (SHEETS.get(h, h) - h) // 2
    want = np.zeros((h, w), bool)
    part = mine[top:top + h, :w]
    want[:part.shape[0], :part.shape[1]] = part
    bad = int((want != a).sum())
    if bad:
        problems.append(f"{bad} dots differ from the rip's")
    if not a.any():
        problems.append("the printer's page is blank")
    return problems


def main():
    if not RIP.exists():
        sys.exit("qdcheck: build rip first: make -C tools/rip qdcheck")
    failed = 0
    with tempfile.TemporaryDirectory() as t:
        for page in PAGES:
            for screen in SCREENS:
                problems = check(page, screen, t)
                what = f"{page.name} in {screen}"
                if problems:
                    failed += 1
                    for p in problems:
                        print(f"FAIL qdcheck {what}: {p}")
                else:
                    print(f"ok  qdcheck {what}: packets, line image and the rip's dots agree")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
