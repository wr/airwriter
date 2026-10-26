#!/usr/bin/env python3.14
# SPDX-License-Identifier: GPL-3.0-or-later
"""The StyleWriter page stage held to a software StyleWriter and a second coder (W-1103).

For each page and model below, the rip's own dots are compared with what the wire
stream turns into on swfake.py's side, twice:

  1. `rip --printer stylewriter --model M --out-sw s.bin --out-pbm p.pbm` writes the
     page as the firmware's stage frames it (the rects and G blocks as the printer
     gets them, 0C at the end) and the dots it halftoned; `swfake.py decode M s.bin`
     reads the stream by the design's wire rules, written apart from our coder, and
     draws the page. Its dots must be the rip's.
  2. `lpstyl_stream` codes the same dots with lpstyl's coder (lpstyl_encode.c, BSD-2,
     © Monroe Williams, ported for these tests only) into a stream of its own, which
     swfake decodes the same way, and its dots must be the rip's too. lpstyl codes
     each block as a rect of its own and starts the XOR delta again with every block,
     where the page stage carries the delta across the blocks of a band, so the two
     streams differ byte for byte. What is compared is the decoded dots, never the
     bytes.

What is compared: the printable area, the region the page stage keeps and the printer
prints. swfake's page-N-{c,m,y,k}.pbm are that area of the sheet (a Letter page is
2880 x 3774 dots on the 1200 family and 2916 x 3816 on the colour 2200, 2400 and 2500:
the sheet less the margins, the rects' +16 offset removed). `rip --out-pbm p.pbm` writes
p-{c,m,y,k}.pbm of the same area as it fed the stage: the page's rows between the
engine's top and bottom margins, and the columns it kept from the printable area's
left edge. A page that is narrower or shorter than the sheet fills the top left of
swfake's area; what swfake has outside it must be white. Four planes are always
compared: a gray page, or a black cartridge, leaves cyan, magenta and yellow white.

Pages: the checker (fixtures/checker.urf, 144 dpi, so scaled up to 360), a Letter text
page and a photo-like page of gray at 360 dpi and a Letter sRGB colour page
(make_300dpi_fixture.py --dpi 360), and A4 and Legal text pages, on the seven models,
with a black cartridge and, for the four colour models, a colour one. Screens: Atkinson
(the default), and Apple's, blue noise and Floyd-Steinberg on some. Not done: the
brief's optional second oracle, TailTalk's encoder (R13): lpstyl's coder and swfake
are two decoders and two coders already, and Apple's own captures are the stronger
check (a later task).

    make -C tools/rip swcheck          (builds build/rip and build/lpstyl_stream first)
    python3.14 tools/rip/test/swcheck.py [-j N] [--quick]
"""
import argparse
import concurrent.futures
import pathlib
import subprocess
import sys
import tempfile
import time

import numpy as np

HERE = pathlib.Path(__file__).resolve().parent
RIP = HERE.parent / "build" / "rip"
LPSTYL = HERE.parent / "build" / "lpstyl_stream"
SWFAKE = HERE.parents[1] / "stylewriter" / "swfake.py"
CHECKER = HERE.parents[2] / "fixtures" / "checker.urf"
MAKER = HERE / "make_300dpi_fixture.py"

MODELS = ("original", "ii", "1200", "1500", "2200", "2400", "2500")
COLOUR_MODELS = ("1500", "2200", "2400", "2500")
PLANES = "cmyk"


def run(cmd, **kw):
    r = subprocess.run([str(c) for c in cmd], capture_output=True, text=True, **kw)
    if r.returncode:
        raise RuntimeError(f"{' '.join(str(c) for c in cmd)}\n{r.stdout}{r.stderr}")
    return r


def read_pbm(path):
    """A raw PBM (P4) as a bool array of rows x width."""
    data = pathlib.Path(path).read_bytes()
    parts = data.split(b"\n", 2)
    if parts[0] != b"P4":
        raise ValueError(f"{path} is not a raw PBM")
    w, h = (int(v) for v in parts[1].split())
    rb = (w + 7) // 8
    raw = np.frombuffer(parts[2], np.uint8, count=rb * h).reshape(h, rb) if h else np.zeros((0, rb), np.uint8)
    return np.unpackbits(raw, axis=1)[:, :w].astype(bool)


def planes(prefix, sep):
    return {p: read_pbm(f"{prefix}{sep}{p}.pbm") for p in PLANES}


def compare(mine, theirs):
    """The rip's plane against swfake's: equal inside the rip's area, white outside it.
    Returns a description of the first difference, or None."""
    h, w = mine.shape
    if theirs.shape[0] < h or theirs.shape[1] < w:
        return f"the rip kept {w} x {h} dots, swfake's printable area is {theirs.shape[1]} x {theirs.shape[0]}"
    inside = theirs[:h, :w]
    if not np.array_equal(inside, mine):
        ys, xs = np.nonzero(inside != mine)
        return f"{len(ys)} dots differ, the first at x {xs[0]}, y {ys[0]}"
    if theirs.sum() != mine.sum():
        return f"swfake has {int(theirs.sum() - mine.sum())} dots outside the rip's area"
    return None


def one(job, tmp):
    """One page on one model: the rip's dots against swfake's reading of both streams."""
    name, urf, model, cart, dither, media, *extra = job
    d = pathlib.Path(tmp)
    sw, pbm = d / "s.bin", d / "p.pbm"
    cmd = [RIP, "--in", urf, "--printer", "stylewriter", "--model", model, "--cart", cart,
           "--dither", dither, "--out-sw", sw, "--out-pbm", pbm, *extra]
    run(cmd)
    mine = planes(d / "p", "-")
    inks = {p: int(mine[p].sum()) for p in PLANES}
    problems = []
    if not inks["k"] and not (cart == "colour" and any(inks.values())):
        problems.append("the rip printed no black at all: nothing to compare")
    if cart == "black" and any(inks[p] for p in "cmy"):
        problems.append("a black cartridge got cyan, magenta or yellow")
    if cart == "colour" and name.startswith("colour") and not all(inks[p] for p in "cmy"):
        problems.append("a colour page in a colour cartridge printed no cyan, magenta or yellow")

    streams = [("page stage", sw)]
    ls = d / "l.bin"
    run([LPSTYL, model, "1" if cart == "colour" else "0", d / "p", ls])
    streams.append(("lpstyl's coder", ls))
    for who, stream in streams:
        out = d / f"dec-{stream.stem}"
        r = subprocess.run([sys.executable, str(SWFAKE), "decode", model, str(stream), "--out", str(out),
                            "--media", media, "--no-png"], capture_output=True, text=True)
        if r.returncode:
            problems.append(f"{who}: swfake refused the stream: {(r.stdout + r.stderr).strip()}")
            continue
        theirs = planes(out / "page-1", "-")
        for p in PLANES:
            diff = compare(mine[p], theirs[p])
            if diff:
                problems.append(f"{who}, {p} plane: {diff}")
    return job, inks, problems


def main():
    ap = argparse.ArgumentParser(description="StyleWriter page stage against swfake and lpstyl's coder")
    ap.add_argument("-j", type=int, default=4, help="runs at once")
    ap.add_argument("--quick", action="store_true", help="the checker and the text page only")
    a = ap.parse_args()
    for tool in (RIP, LPSTYL):
        if not tool.exists():
            sys.exit(f"swcheck: build {tool.name} first: make -C tools/rip swcheck")

    t0 = time.monotonic()
    failed = ran = 0
    with tempfile.TemporaryDirectory() as t:
        tmp = pathlib.Path(t)

        def make(kind, name, *opts):
            p = tmp / f"{name}.urf"
            run([sys.executable, MAKER, kind, p, "--dpi", "360", *opts])
            return p

        text = make("text", "text360")
        work = []
        for model in MODELS:
            work.append(("checker", CHECKER, model, "black", "atkinson", "letter"))
            work.append(("text", text, model, "black", "atkinson", "letter"))
        work.append(("text apple", text, "ii", "black", "apple", "letter"))
        work.append(("text apple", text, "2500", "colour", "apple", "letter"))
        if not a.quick:
            photo = make("photo", "photo360", "--rows", "1500")
            colour = make("colour", "colour360")
            a4 = make("text", "text360a4", "--paper", "a4")
            legal = make("text", "text360legal", "--paper", "legal")
            for model in MODELS:
                work.append(("photo", photo, model, "black", "atkinson", "letter"))
                if model in COLOUR_MODELS:
                    work.append(("colour", colour, model, "colour", "atkinson", "letter"))
                else:
                    work.append(("colour as gray", colour, model, "black", "atkinson", "letter"))
            work.append(("colour as gray", colour, "2200", "black", "bluenoise", "letter"))
            work.append(("colour apple", colour, "2500", "colour", "apple", "letter"))
            work.append(("colour floyd", colour, "1500", "colour", "floyd", "letter"))
            work.append(("photo apple", photo, "1200", "black", "apple", "letter"))
            # Apple's tone curve is on by default and --tone none takes it off: the dots change.
            work.append(("photo, no tone curve", photo, "ii", "black", "atkinson", "letter", "--tone", "none"))
            work.append(("photo, no tone curve", photo, "2500", "black", "atkinson", "letter", "--tone", "none"))
            for model in ("ii", "2500", "original"):
                work.append(("text a4", a4, model, "black", "atkinson", "a4"))
                work.append(("text legal", legal, model, "black", "atkinson", "legal"))
            work.append(("text a4", a4, "2400", "colour", "atkinson", "a4"))

        def task(job):
            with tempfile.TemporaryDirectory(dir=tmp) as d:
                return one(job, d)

        with concurrent.futures.ThreadPoolExecutor(max_workers=a.j) as pool:
            results = {}
            for job, inks, problems in pool.map(task, work):
                ran += 1
                name, _, model, cart, dither, media, *_ = job
                results[(name, model)] = inks
                label = f"{name} / {model} / {cart} cartridge / {dither} / {media}"
                if problems:
                    failed += 1
                    print(f"FAIL swcheck {label}")
                    for p in problems:
                        print(f"     {p}")
                else:
                    dots = " ".join(f"{k}{v}" for k, v in inks.items() if v)
                    print(f"ok  swcheck {label} ({dots} dots)")
        for model in ("ii", "2500"):
            on, off = results.get(("photo", model)), results.get(("photo, no tone curve", model))
            if on and off and on["k"] == off["k"]:
                failed += 1
                print(f"FAIL swcheck {model}: --tone none changed nothing ({on['k']} dots either way)")
    print(f"{'FAIL' if failed else 'ok '} swcheck: {ran - failed}/{ran} runs agree "
          f"({time.monotonic() - t0:.0f} s)")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
