#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Holds the LaserWriter's PostScript to the rip's own dots (W-1082).

For each page, in binary and in hex: rip it for a LaserWriter (`rip --printer
laser --ps --out-pbm`), render the PostScript with Ghostscript at 300 dpi on
Letter, and require Ghostscript's dots to be the rip's, bit for bit, with
nothing else on the paper. Then the same with Ghostscript held to Level 1, and
a check that the prolog uses only Level 1 operators (ps_level1_ops.txt).

Ghostscript is a host test tool only (AGPL): nothing of it is linked or
shipped. Called by path, since `gs` is a git alias in some shells.

    python3 tools/rip/test/pscheck.py          (or: make -C tools/rip pscheck)
"""
import os, pathlib, re, subprocess, sys, tempfile

HERE = pathlib.Path(__file__).resolve().parent
RIP = HERE.parent / "build" / "rip"
GS = os.environ.get("GS", "/opt/homebrew/bin/gs")
LETTER = (612, 792)
# Level 2 names that are procedures, not operators, in Ghostscript's systemdict.
LEVEL2_PROCS = ("languagelevel", "currentpagedevice", "setpagedevice", "findresource",
                "defineresource", "setcolorspace", "setcolor", "selectfont")
TIMEOUT = 120


def pbm(path):
    """(w, h, rows) of a raw PBM, comments allowed in the header."""
    data = pathlib.Path(path).read_bytes()
    fields, pos = [], 0
    while len(fields) < 3:
        while data[pos:pos + 1].isspace():
            pos += 1
        if data[pos:pos + 1] == b"#":
            pos = data.index(b"\n", pos) + 1
            continue
        end = pos
        while not data[end:end + 1].isspace():
            end += 1
        fields.append(data[pos:end])
        pos = end
    if fields[0] != b"P4":
        sys.exit(f"{path}: not a raw PBM")
    w, h = int(fields[1]), int(fields[2])
    pos += 1
    stride = (w + 7) // 8
    return w, h, [data[pos + y * stride: pos + (y + 1) * stride] for y in range(h)]


def compare(rip_pbm, gs_pbm):
    """None when Ghostscript's paper holds exactly the rip's page at its
    top-left and nothing else, else where they first differ."""
    w, h, mine = pbm(rip_pbm)
    W, H, theirs = pbm(gs_pbm)
    if (W, H) != (LETTER[0] * 300 // 72, LETTER[1] * 300 // 72):
        return f"Ghostscript's page is {W} x {H}"
    stride = (w + 7) // 8
    tail = (0xFF << (8 - w % 8)) & 0xFF if w % 8 else 0xFF
    for y in range(H):
        row = theirs[y]
        want = mine[y] if y < h else b""
        got = row[:stride] if y < h else b""
        if y < h:
            if w > W:
                got, want = row, want[:len(row)]
            elif got[:-1] != want[:-1] or (got[-1] & tail) != (want[-1] & tail):
                x = next(i for i in range(stride) if got[i] != want[i])
                return f"row {y}, byte {x}: Ghostscript {got[x]:08b}, rip {want[x]:08b}"
            rest = row[stride:] if w <= W else b""
            if any(rest) or (w % 8 and w <= W and row[stride - 1] & ~tail & 0xFF):
                return f"row {y}: Ghostscript has dots right of the page"
        elif any(row):
            return f"row {y}: Ghostscript has dots below the page"
    return None


def level1_ops():
    text = (HERE / "ps_level1_ops.txt").read_text()
    return set(" ".join(l for l in text.splitlines() if not l.startswith("#")).split())


def level1_trap(path):
    """A prefix that hides every operator outside Level 1 behind one that
    fails (and the Level 2 names Ghostscript defines as procedures, which
    trapping every procedure would break Ghostscript itself over), in a dictionary on top of the stack. Ghostscript 10 has no Level 1
    mode (.setlanguagelevel is gone), so this is how a file that strays past
    Level 1 is caught when it runs, binding included."""
    # As strings made names: "/[" would not read as one.
    names = " ".join(f"({w}) cvn true" for w in sorted(level1_ops()))
    pathlib.Path(path).write_text(f"""
/awl1 << {names} >> def
/awl2 << {" ".join(f"({w}) true" for w in LEVEL2_PROCS)} >> def
/awtrap 400 dict def
systemdict {{
  dup type /operatortype eq 2 index 128 string cvs awl2 exch known or {{
    pop dup 128 string cvs dup length 0 gt {{ 0 get 46 ne }} {{ pop false }} ifelse
    {{ dup awl1 exch known not {{ awtrap exch {{ /not_level_1 cvx exec }} put }} {{ pop }} ifelse }}
    {{ pop }} ifelse
  }} {{ pop pop }} ifelse
}} forall
awtrap begin
""")


def gs_render(ps, out, level1=False):
    cmd = [GS, "-q", "-dSAFER", "-dBATCH", "-dNOPAUSE", "-sDEVICE=pbmraw", "-r300",
           f"-dDEVICEWIDTHPOINTS={LETTER[0]}", f"-dDEVICEHEIGHTPOINTS={LETTER[1]}",
           "-dFIXEDMEDIA", f"-sOutputFile={out}"]
    if level1:
        trap = pathlib.Path(out).with_suffix(".l1.ps")
        level1_trap(trap)
        cmd += [trap, ps]
    else:
        cmd += [ps]
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=TIMEOUT)
    if r.returncode or r.stdout.strip() or r.stderr.strip():
        return f"Ghostscript: exit {r.returncode} {r.stdout.strip()} {r.stderr.strip()}"
    return None


def level1_names(ps_path):
    """Executable names in the prolog and page setup not in Level 1 and not
    defined by the prolog."""
    text = pathlib.Path(ps_path).read_bytes().decode("latin-1")
    prolog = text[text.index("%%BeginProlog"):text.index("%%EndProlog")]
    setup = text[text.index("%%Page:"):].split("\n")[1:3]
    ops = level1_ops()
    defined = set(re.findall(r"/([A-Za-z]\w*)", prolog))
    code = re.sub(r"%[^\n]*", "", prolog + "\n" + "\n".join(setup))
    names = re.findall(r"(?<![/\w.])([A-Za-z][\w.]*)", code)
    return sorted({n for n in names if n not in ops and n not in defined})


def query_check(tmp):
    """The query job through Ghostscript as a LaserWriter would run it, and
    its answer through the board's parser (test_lw --parse): every question
    answered, binary intact, Letter; Level 1 with every later operator trapped."""
    job, out = tmp / "query.ps", tmp / "query.pbm"
    subprocess.run([RIP, "--query-job", job], check=True)
    parse = RIP.parent / "test_lw"
    fails = 0
    for level1, level in ((False, 3), (True, 1)):
        cmd = [GS, "-q", "-dSAFER", "-dBATCH", "-dNOPAUSE", "-sDEVICE=pbmraw", "-r300",
               f"-dDEVICEWIDTHPOINTS={LETTER[0]}", f"-dDEVICEHEIGHTPOINTS={LETTER[1]}",
               "-dFIXEDMEDIA", f"-sOutputFile={out}"]
        if level1:
            trap = tmp / "trap.ps"
            level1_trap(trap)
            cmd.append(trap)
        r = subprocess.run(cmd + [job], capture_output=True, timeout=TIMEOUT)
        got = subprocess.run([parse, "--parse"], input=r.stdout,
                             capture_output=True).stdout.decode().strip()
        want = [f"complete=1", f"level={level}", "binary=1", "page=letter"]
        what = f"query{' level 1' if level1 else ''}"
        if r.returncode or not all(w in got.split() for w in want):
            print(f"FAIL pscheck {what}: {got or r.stderr.decode(errors='replace')[:200]}")
            fails += 1
        else:
            print(f"ok  pscheck {what}: {got}")
    return fails


def pages(tmp):
    """The pages to check: the checker, and the 300 dpi text and photo pages
    once make_300dpi_fixture.py exists."""
    out = []
    checker = tmp / "checker.urf"
    subprocess.run([sys.executable, HERE / "make_fixture.py", checker], check=True,
                   capture_output=True)
    out.append(checker)
    maker = HERE / "make_300dpi_fixture.py"
    if maker.exists():
        for kind in ("text", "photo"):
            p = tmp / f"{kind}300.urf"
            subprocess.run([sys.executable, maker, kind, p], check=True, capture_output=True)
            out.append(p)
    return out


def main():
    if not pathlib.Path(GS).exists():
        sys.exit(f"pscheck: no Ghostscript at {GS}: brew install ghostscript")
    if not RIP.exists():
        sys.exit("pscheck: build the rip first: make -C tools/rip")
    failed = 0
    with tempfile.TemporaryDirectory() as t:
        tmp = pathlib.Path(t)
        failed += query_check(tmp)
        for urf in pages(tmp):
            for mode in ("binary", "hex"):
                ps, mine, theirs = tmp / "a.ps", tmp / "a.pbm", tmp / "g.pbm"
                cmd = [RIP, "--in", urf, "--printer", "laser", "--ps", ps, "--out-pbm", mine]
                if mode == "hex":
                    cmd.append("--hex")
                subprocess.run(cmd, check=True, capture_output=True)
                for level1 in (False, True):
                    what = f"{urf.stem} {mode}{' level 1' if level1 else ''}"
                    err = gs_render(ps, theirs, level1) or compare(mine, theirs)
                    if err:
                        print(f"FAIL pscheck {what}: {err}")
                        failed += 1
                    else:
                        print(f"ok  pscheck {what}")
                w, h, _ = pbm(mine)
                if mode == "binary":
                    bitmap = (w + 7) // 8 * h
                    print(f"    {urf.stem}: {ps.stat().st_size} bytes of PostScript for a "
                          f"{bitmap}-byte bitmap ({100 * ps.stat().st_size // bitmap}%)")
                bad = level1_names(ps)
                if bad:
                    print(f"FAIL pscheck {urf.stem} {mode}: not Level 1: {' '.join(bad)}")
                    failed += 1
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
