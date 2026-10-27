#!/usr/bin/env python3.14
# SPDX-License-Identifier: GPL-3.0-or-later
"""What Apple's StyleWriter drivers send, held to what the firmware sends (W-1103, Task 13).

Apple's drivers run in an emulated Mac whose printer port is swfake.py's pseudo-terminal
(tools/stylewriter/capture.md). The captures are swfake's own logs, kept on this computer
only, never committed:

    ~/.config/airwriter/sw-captures/<model>/<page>[-colour|-black][-q3|-q4|-q5][-a4|-legal].log

<model> is original, ii, 1200, 1500, 2200, 2400 or 2500. <page> is blank, checker, text or
photo. The cartridge defaults to black and the quality to normal (4). Without any capture
this prints a skip line and exits 0. AIRWRITER_SW_CAPTURES or --dir names another place.

For every capture:

  * decodes: the log reads; swfake never ejected the page (`! bad` in the log would be a
    stream, or a model of the printer, that is wrong); the bytes the Mac sent decode to at
    least one page. The decoded pages go to <model>/<name>.pages/ (PBM planes, a PNG).
  FAIL on any of these.
  * the command order: the stream's commands with the page data taken out (a rect is one
    token, a G block none, the polling queries 1, 2 and B none) against the same for what
    `sw_session` sends for the same model, cartridge and quality, by running swhost against
    a swfake. Every difference is listed as `DIFF`. Differences do not fail: they are the
    evidence for the controller (docs: where Apple's driver and TailTalk's reading part).
  * the geometry: the rect offsets, band height and block length against the model's
    (swfake.SPECS and the block caps the design gives). DIFF lines again.
  * the checker, with --dither apple: AirWriter's page (`rip --dither apple`) and Apple's
    decode, both cropped to their ink, agree to within TOLERANCE below. FAIL if not. The
    figures it measured are printed whether they pass or not.

Always (when there are captures) the three questions the reviews left open are answered
from them, or "not captured":

  1. what the 2400 and 2500 drivers do when `2` answers with bit 80 set (swfake answers it
     always on those two): the job goes on, or it does not
  2. the page-mode letter (H or B) the 2200, 2400 and 2500 drivers send for a gray page on
     a colour cartridge
  3. whether the row delta carries across `G` blocks within a band: both readings of each
     capture are decoded and the one whose block joins look like the rest of the page wins

    make -C tools/rip swcaptures           (builds build/rip and build/swhost first)
    python3.14 tools/rip/test/swcaptures.py [--dir DIR] [--model M]... [--no-png] [--selftest]

`--selftest` makes captures of its own from the firmware's session and holds them to the
same checks, to show the checks work (and that a changed stream is found): it is the only
mode that needs no Apple data and says nothing about Apple's drivers.
"""
import argparse
import collections
import difflib
import math
import os
import pathlib
import re
import shutil
import signal
import subprocess
import sys
import tempfile

import numpy as np

HERE = pathlib.Path(__file__).resolve().parent
RIP_DIR = HERE.parent
ROOT = HERE.parents[2]
RIP = RIP_DIR / "build" / "rip"
SWHOST = RIP_DIR / "build" / "swhost"
SWFAKE = ROOT / "tools" / "stylewriter" / "swfake.py"
CHECKER = ROOT / "fixtures" / "checker.urf"
MAKER = HERE / "make_300dpi_fixture.py"

sys.path.insert(0, str(ROOT / "tools" / "stylewriter"))
sys.path.insert(0, str(HERE))
import swfake  # noqa: E402  the printer's side: the decoder and its model of each StyleWriter
import swcheck  # noqa: E402  its PBM reader

DEFAULT_DIR = "~/.config/airwriter/sw-captures"
MODELS = tuple(swfake.SPECS)
COLOUR_MODELS = ("1500", "2200", "2400", "2500")
CS_MODELS = ("2200", "2400", "2500")
PAGES = ("blank", "checker", "text", "photo")
PLANES = "cmyk"
ROWS = 1200               # rows of the text and photo pages swhost is given for the reference

# The checker is held to Apple's, cropped to its ink (Apple's page puts it where the Mac
# document does, so only its shape and its dots are compared):
#   size: the ink's width and height agree to this fraction;
#   dots: the dots in one and not the other, as a fraction of the ink dots of the page that
#         has more.
# Provisional: nobody has measured an Apple page yet. The first capture's figures replace
# them, and capture.md records the figures.
TOLERANCE = {"size": 0.02, "dots": 0.05}

# The most a G block may hold (global constraints; TailTalk's reading of the drivers' ENGC).
CAP_CS = {"black": 0x7E77, "colour": 0x79FA}
CAP_1200 = 0x76ED         # less the planes' row bytes
CAP_ORIGINAL = 0x2000

POP = np.array([bin(i).count("1") for i in range(256)], np.int32)
LINE = re.compile(r"^\s*(\d+) ([<>!]) ?(.*)$")
SETTERS = set(b"dnZbsAuDNLHBFYlEP")       # the letters sent on their own (m takes one more)
POLLS = {"1", "2", "B"}                    # the queries a job asks over and over


# ---- the capture

class Capture:
    def __init__(self, path):
        self.path = pathlib.Path(path)
        self.model = self.path.parent.name
        self.name = self.path.stem
        words = self.name.split("-")
        self.page = words[0]
        self.cart = "colour" if "colour" in words[1:] else "black"
        self.quality = next((int(w[1]) for w in words[1:] if re.fullmatch(r"q[345]", w)), 4)
        self.media = next((w for w in words[1:] if w in ("a4", "legal")), "letter")
        self.host = b""            # what the Mac sent
        self.printer = b""         # what swfake answered
        self.events = []           # (ms, text) of swfake's `!` lines
        self.chunks = []           # (ms, arrow, bytes) in order

    def read(self):
        for n, line in enumerate(self.path.read_text().splitlines(), 1):
            if not line.strip():
                continue
            m = LINE.match(line)
            if not m:
                raise ValueError(f"line {n} is not a swfake log line: {line[:60]!r}")
            ms, arrow, rest = int(m[1]), m[2], m[3].strip()
            if arrow == "!":
                self.events.append((ms, rest))
                continue
            if rest.endswith("..."):
                raise ValueError(f"line {n} is cut short: this is swhost's log, not swfake's")
            try:
                data = bytes.fromhex(rest)
            except ValueError:
                raise ValueError(f"line {n} has no hex bytes: {rest[:40]!r}") from None
            self.chunks.append((ms, arrow, data))
        self.host = b"".join(d for _, a, d in self.chunks if a == ">")
        self.printer = b"".join(d for _, a, d in self.chunks if a == "<")
        if not self.host:
            raise ValueError("nothing was sent to the printer")
        return self


def captures(root, models):
    root = pathlib.Path(root).expanduser()
    found = sorted(p for p in root.glob("*/*.log") if p.parent.name in MODELS) if root.is_dir() else []
    return [Capture(p) for p in found if not models or p.parent.name in models]


# ---- the wire, as commands

def tokens(stream):
    """The Mac-to-printer bytes as commands: ('q', letter), ('ident',), ('ff',), ('rect', kind,
    l, t, r, b), ('block', length, ends_ok), ('set', text). Returns (tokens, problems)."""
    i, n, out, bad = 0, len(stream), [], []
    while i < n:
        b = stream[i]
        if b == 0x00:
            i += 1
        elif b == 0xFF:
            j = i
            while j < n and stream[j] == 0xFF:
                j += 1
            if j - i < 3 or j >= n:
                bad.append(f"byte {i}: {j - i} FF byte(s), then {'nothing' if j >= n else hex(stream[j])}")
                i = j
                continue
            out.append(("q", chr(stream[j])))
            i = j + 1
        elif b == 0x3F:
            out.append(("ident",))
            i += 1
        elif b == 0x0C:
            out.append(("ff",))
            i += 1
        elif b in (0x52, 0x63):
            if i + 9 > n:
                bad.append(f"byte {i}: a rect cut short")
                break
            l, t, r, bt = (int.from_bytes(stream[i + 1 + 2 * k:i + 3 + 2 * k], "little") for k in range(4))
            out.append(("rect", chr(b), l, t, r, bt))
            i += 9
        elif b == 0x47:
            if i + 3 > n:
                bad.append(f"byte {i}: a block cut short")
                break
            ln = int.from_bytes(stream[i + 1:i + 3], "little")
            end = i + 3 + ln
            ok = end < n and stream[end] == 0
            if end >= n:
                bad.append(f"byte {i}: a block of {ln} bytes runs past the end")
            out.append(("block", ln, ok))
            i = end + 1
        elif b == 0x6D:
            out.append(("set", "m" + (chr(stream[i + 1]) if i + 1 < n else "")))
            i += 2
        elif b in SETTERS:
            out.append(("set", chr(b)))
            i += 1
        else:
            bad.append(f"byte {i}: {b:02x} is no command")
            i += 1
    return out, bad


def skeleton(toks):
    """The commands with the page data out: polls dropped, a block dropped, runs of one rect
    kind one token."""
    out = []
    for t in toks:
        k = t[0]
        if k == "q":
            if t[1] in POLLS:
                continue
            s = {"I": "RESET", "p": "p", "H": "H?", "D": "ffD", "N": "ffN", "S": "S", "R": "R?"}.get(t[1], "q" + t[1])
        elif k == "ident":
            s = "?"
        elif k == "ff":
            s = "0C"
        elif k == "rect":
            s = "RECT:" + t[1]
        elif k == "set":
            s = t[1]
        else:
            continue
        if not (s.startswith("RECT") and out and out[-1] == s):
            out.append(s)
    return out


def geometry(toks, model, cart):
    """What the rects and blocks say about the page: their offsets, heights, block lengths."""
    rects = [t for t in toks if t[0] == "rect"]
    blocks = []
    cur = None
    for t in toks:
        if t[0] == "rect":
            cur = t
        elif t[0] == "block":
            rb = ((cur[4] - cur[2] + 8) // 8) if cur else 0
            planes = 4 if cur and cur[1] == "c" else 1
            blocks.append((t[1], planes * rb))
    spec = swfake.SPECS[model]
    if model == "original":
        cap = CAP_ORIGINAL
    elif spec.cs:
        cap = CAP_CS["colour" if cart == "colour" else "black"]
    else:
        cap = None
    return {
        "rects": len(rects), "blocks": len(blocks),
        "colour_rects": sum(1 for t in rects if t[1] == "c"),
        "l_min": min((t[2] for t in rects), default=None),
        "r_max": max((t[4] for t in rects), default=None),
        "rows_max": max((t[5] - t[3] + 1 for t in rects), default=None),
        "block_max": max((b[0] for b in blocks), default=None),
        "over_cap": [b[0] for b in blocks if (cap if cap else CAP_1200 - b[1]) < b[0]],
        "cap": cap,
    }


# ---- decoding, both readings of the delta

class Reading:
    """A StyleWriter decoding a stream, noting where each G block starts."""

    def __init__(self, *a, **kw):
        super().__init__(*a, **kw)
        self.edges, self.starts = [], []

    def _rect(self, kind, raw):
        super()._rect(kind, raw)
        self.starts.append(self.band.y0)

    def _block(self, data):
        band = self.band
        if band is not None and not band.done and band.plane == 0 and not band.part and band.rows > 0:
            self.edges.append(band.y0 + band.rows)
        self._before_block()
        super()._block(data)

    def _before_block(self):
        pass


class Carry(Reading, swfake.StyleWriter):
    """The delta runs on across blocks: the page stage's reading."""


class Restart(Reading, swfake.StyleWriter):
    """The delta starts again with every block: lpstyl's reading (a rect of its own each)."""

    def _before_block(self):
        band = self.band
        if band is not None and not band.done:
            band.prev = {i: None for i in band.inks}


def decode(model, cart, media, stream, cls=Carry):
    sw = cls(model, cart=cart, media=media, drain=float("inf"), settle=0, png=False, unlimited=True,
             say=lambda s: None)
    sw.feed(stream)
    if sw.page is not None and sw.page.rects and not sw.bad:
        sw.pages.append(sw.page)
    return sw


def join_ratio(sw):
    """How different the dot rows are across a block join from the rows around it: near 1
    when the delta was read right, well away from it when a row was XORed with the wrong
    one. Each join is set against the rows within five of it in the same rect. None when no
    block begins in the middle of a rect, or there is nothing to say."""
    if not sw.pages or not sw.edges:
        return None
    page = sw.pages[0]
    dist = None
    for p in page.planes.values():
        d = POP[p[1:] ^ p[:-1]].sum(axis=1)
        dist = d if dist is None else dist + d
    skip = set(sw.edges) | set(sw.starts)
    join = near = 0.0
    n = 0
    for y in sw.edges:
        if not 0 < y < page.h:
            continue
        around = [dist[k - 1] for k in range(y - 5, y + 6) if k != y and 0 < k < page.h and k not in skip]
        if len(around) >= 4:
            join += dist[y - 1]
            near += float(np.mean(around))
            n += 1
    return join / near if n and near else None


# ---- the firmware's conversation for the same job

def run(cmd, **kw):
    r = subprocess.run([str(c) for c in cmd], capture_output=True, text=True, **kw)
    if r.returncode:
        raise RuntimeError(f"{' '.join(str(c) for c in cmd)}\n{r.stdout}{r.stderr}")
    return r


def page_file(cap, work):
    """A page of the kind the capture printed, for swhost to print."""
    work = pathlib.Path(work)
    colour = cap.cart == "colour" and cap.model in COLOUR_MODELS
    key = cap.page if cap.page != "photo" else ("colour" if colour else "photo")
    path = work / f"{key}-{cap.media}.{'png' if key == 'blank' else 'urf'}"
    if path.exists() or key == "checker":
        return CHECKER if key == "checker" else path
    if key == "blank":
        from PIL import Image
        w, h = swfake.MEDIA[cap.media]
        Image.new("L", (w, h), 255).save(path, dpi=(360, 360))
    else:
        run([sys.executable, MAKER, key, path, "--dpi", "360", "--paper", cap.media, "--rows", str(ROWS)])
    return path


def converse(model, cart, page, quality, dither, log, media="letter"):
    """swhost prints `page` to a swfake of the same model on a pty and the fake logs the
    conversation to `log`. Returns swhost's output."""
    fake = subprocess.Popen(
        [sys.executable, str(SWFAKE), "serve", model, "--pty", "--cart", cart, "--media", media,
         "--no-png", "--log", str(log), "--drain", "4000000"],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, stdin=subprocess.PIPE, text=True,
        env={**os.environ, "PYTHONUNBUFFERED": "1"}, preexec_fn=lambda: signal.signal(signal.SIGINT, signal.SIG_DFL))
    try:
        dev = fake.stdout.readline().strip()
        r = subprocess.run([str(SWHOST), "--dev", dev, "--model", model, "--quality", str(quality),
                            "--dither", dither, str(page)], capture_output=True, text=True, timeout=300)
        if r.returncode:
            raise RuntimeError(f"swhost {model}: {r.stdout}{r.stderr}")
        return r.stdout + r.stderr
    finally:
        fake.send_signal(signal.SIGINT)
        try:
            fake.wait(10)
        except subprocess.TimeoutExpired:
            fake.kill()
            fake.wait()


_reference = {}


def reference(cap, work):
    """What sw_session sends for this capture's model, cartridge, quality and kind of page."""
    colour = cap.cart == "colour" and cap.model in COLOUR_MODELS
    key = (cap.model, cap.cart, cap.page, cap.quality, cap.media)
    if key not in _reference:
        log = pathlib.Path(work) / f"ref-{'-'.join(map(str, key))}.log"
        converse(cap.model, "colour" if colour else "black", page_file(cap, work), cap.quality, "atkinson", log, cap.media)
        _reference[key] = Capture(log).read()
    return _reference[key]


# ---- the checks

def order_diffs(apple, ours):
    """The differences of two skeletons, one readable line each."""
    out = []
    sm = difflib.SequenceMatcher(None, apple, ours, autojunk=False)
    for op, a0, a1, b0, b1 in sm.get_opcodes():
        if op == "equal":
            continue
        where = f"after {' '.join(apple[max(0, a0 - 3):a0]) or 'the start'}"
        out.append(f"{where}: Apple sends [{' '.join(apple[a0:a1]) or '-'}], sw_session [{' '.join(ours[b0:b1]) or '-'}]")
    return out


def geometry_diffs(cap, g):
    """Where the rects and blocks break the model's geometry. The page decides how tall a
    rect is and how far left it starts, so those two only count when they cannot be right."""
    spec = swfake.SPECS[cap.model]
    out = []
    if g["l_min"] is not None and g["l_min"] < spec.dx:
        out.append(f"a rect starts at x {g['l_min']}, before the +{spec.dx} offset the {cap.model} family's rects carry")
    if g["rows_max"] and g["rows_max"] > spec.band:
        out.append(f"a rect of {g['rows_max']} rows is taller than the {spec.band}-row band")
    if g["over_cap"]:
        out.append(f"{len(g['over_cap'])} block(s) longer than the cap (longest {max(g['over_cap'])}, cap {g['cap']})")
    return out


def all_inks(sw, page):
    """All the inks of a decoded page together, as a bool array."""
    pg = sw.pages[page]
    ink = np.zeros((pg.h, pg.w), bool)
    for p in PLANES:
        ink |= pg.bits(p)
    return ink


def cropped(ink):
    ys, xs = np.nonzero(ink)
    if not len(ys):
        return None
    return ink[ys.min():ys.max() + 1, xs.min():xs.max() + 1]


def checker_check(cap, sw, work):
    """AirWriter's checker with --dither apple against Apple's decode. Returns (problems, line)."""
    d = pathlib.Path(work) / f"rip-{cap.model}-{cap.cart}"
    d.mkdir(parents=True, exist_ok=True)
    run([RIP, "--in", CHECKER, "--printer", "stylewriter", "--model", cap.model, "--cart", cap.cart,
         "--dither", "apple", "--out-pbm", d / "p.pbm"])
    mine_planes = swcheck.planes(d / "p", "-")
    mine = np.zeros_like(mine_planes["k"])
    for p in PLANES:
        mine |= mine_planes[p]
    a, b = cropped(all_inks(sw, 0)), cropped(mine)
    if a is None or b is None:
        return ["one of the checker pages has no ink at all"], None
    dh, dw = abs(a.shape[0] - b.shape[0]) / b.shape[0], abs(a.shape[1] - b.shape[1]) / b.shape[1]
    h, w = min(a.shape[0], b.shape[0]), min(a.shape[1], b.shape[1])
    ca, cb = a[:h, :w], b[:h, :w]
    differ = int(np.count_nonzero(ca != cb))
    base = max(int(a.sum()), int(b.sum()))
    frac = differ / base
    line = (f"checker, ink cropped: Apple {a.shape[1]} x {a.shape[0]} ({int(a.sum())} dots), AirWriter "
            f"{b.shape[1]} x {b.shape[0]} ({int(b.sum())} dots); size differs {dw:.1%} wide {dh:.1%} tall, "
            f"{differ} dots differ ({frac:.2%} of the ink)")
    problems = []
    if max(dw, dh) > TOLERANCE["size"]:
        problems.append(f"the checker's size differs more than {TOLERANCE['size']:.0%}: {line}")
    elif frac > TOLERANCE["dots"]:
        problems.append(f"more than {TOLERANCE['dots']:.0%} of the ink differs: {line}")
    return problems, line


def check(cap, work, png):
    """One capture. Returns (problems, diffs, notes, facts)."""
    problems, diffs, notes = [], [], []
    facts = {}
    bad = [t for _, t in cap.events if t.startswith("bad ")]
    if bad:
        problems.append(f"swfake ejected the page: {bad[0][4:]}")
    toks, tbad = tokens(cap.host)
    problems += [f"stream: {t}" for t in tbad]
    colour_cart = cap.cart if cap.model in COLOUR_MODELS else "black"
    sw = decode(cap.model, colour_cart, cap.media, cap.host)
    if sw.bad:
        problems.append(f"decode: {sw.bad[0]}")
    if not sw.pages:
        problems.append("no page came out of the stream")
    if sw.pages:
        out = cap.path.with_suffix(".pages")
        if out.exists():
            shutil.rmtree(out)
        for n, pg in enumerate(sw.pages, 1):
            pg.save(out, n, png)
    facts["pages"] = len(sw.pages)
    g = geometry(toks, cap.model, colour_cart)
    facts["geometry"] = g
    notes.append(f"{g['rects']} rects ({g['colour_rects']} colour), {g['blocks']} blocks, "
                 f"longest block {g['block_max']}, tallest rect {g['rows_max']} rows, x {g['l_min']}..{g['r_max']}")

    # the command order and the geometry, against sw_session's
    try:
        if cap.page not in PAGES:
            raise ValueError(f"the file name does not say what page it printed: {cap.page!r} (one of {', '.join(PAGES)})")
        ref = reference(cap, work)
        rtoks, _ = tokens(ref.host)
        ours = geometry(rtoks, cap.model, colour_cart)
        diffs += [f"order: {d}" for d in order_diffs(skeleton(toks), skeleton(rtoks))]
        diffs += [f"geometry: {d}" for d in geometry_diffs(cap, g)]
        notes.append(f"sw_session on a page of its own: tallest rect {ours['rows_max']} rows, longest block "
                     f"{ours['block_max']} bytes (the cap here is {g['cap'] or 'the row bytes less ' + hex(CAP_1200)})")
        facts["skeleton"], facts["ours"] = skeleton(toks), skeleton(rtoks)
    except (ValueError, RuntimeError, OSError, subprocess.TimeoutExpired) as e:
        problems.append(f"the firmware's conversation could not be run: {e}")

    # the checker's dots
    if cap.page == "checker" and sw.pages:
        try:
            p, line = checker_check(cap, sw, work)
            problems += p
            if line:
                notes.append(line)
        except (RuntimeError, OSError) as e:
            problems.append(f"the checker comparison could not be run: {e}")

    # the open questions
    twos = collections.Counter()
    chunks = cap.chunks
    for (_, a1, d1), nxt in zip(chunks, chunks[1:] + [(0, "", b"")]):
        if a1 == ">" and tokens(d1)[0] == [("q", "2")] and nxt[1] == "<" and len(nxt[2]) == 1:
            twos[nxt[2][0]] += 1
    facts["twos"] = twos
    facts["reset"] = sum(1 for t in toks if t == ("q", "I"))
    skel = skeleton(toks)
    first_a = skel.index("A") if "A" in skel else None
    mode = []
    if first_a is not None:
        for s in skel[first_a + 1:]:
            if s.startswith("RECT") or s in ("L", "0C"):
                break
            if s in ("H", "B"):
                mode.append(s)
    facts["mode"] = mode
    # the row delta across blocks: only a model with a delta, and a block that begins inside a rect
    facts["join"] = (None, None)
    if swfake.SPECS[cap.model].delta and sw.edges and not sw.bad:
        r = decode(cap.model, colour_cart, cap.media, cap.host, Restart)
        facts["join"] = (join_ratio(sw), join_ratio(r))
    return problems, diffs, notes, facts


def away(ratio):
    """How far a join ratio is from 1 (the same as the rest of the page), on a log scale."""
    return math.inf if ratio is None else abs(math.log(max(ratio, 1e-6)))


def questions(results):
    """The three open questions, from what the captures showed."""
    out = []
    spur = [(c, f) for c, f in results if c.model in ("2400", "2500")]
    if not spur:
        out.append("1. `2` bit 80 on the 2400 and 2500: not captured")
    for c, f in spur:
        n80 = sum(v for k, v in f["twos"].items() if k & 0x80)
        out.append(f"1. `2` bit 80, {c.model}/{c.name}: swfake answered `2` with bit 80 {n80} of {sum(f['twos'].values())} "
                   f"time(s); {f['pages']} page(s) came out and the driver reset the printer {f['reset']} time(s)"
                   + ("; the driver ignores it" if n80 and f["pages"] else ""))
    modes = [(c, f) for c, f in results if c.model in CS_MODELS and c.cart == "colour" and c.page in ("blank", "checker", "text")]
    if not modes:
        out.append("2. page-mode letter on a gray page with a colour cartridge: not captured")
    for c, f in modes:
        out.append(f"2. page-mode letter, gray {c.page} page, colour cartridge, {c.model}: "
                   f"{''.join(f['mode']) or 'none sent'} (sw_session sends B)")
    seen = [(c, f) for c, f in results if f["join"][0] is not None]
    if not seen:
        out.append("3. the row delta across G blocks: not captured (no capture has a block that begins inside a rect)")
    for c, f in seen:
        carry, restart = f["join"]
        r = "-" if restart is None else f"{restart:.2f}x"
        if restart is not None and abs(carry - restart) < 0.05 * max(carry, restart):
            verdict = "no evidence: the two readings look the same on this page"
        else:
            verdict = "the delta carries across blocks" if away(carry) <= away(restart) else "the delta starts again with every block"
        out.append(f"3. row delta, {c.model}/{c.name}: a row across a block join differs from its neighbour {carry:.2f}x as much as "
                   f"rows inside a block do when read as carried, {r} when read as restarting: {verdict}")
    return out


def check_all(root, models, png=True, tag=""):
    caps = captures(root, models)
    if not caps:
        print(f"skip swcaptures: no captures in {pathlib.Path(root).expanduser()} (the rig is in "
              f"tools/stylewriter/capture.md; no Apple data is committed)")
        return 0
    failed = ndiff = 0
    results = []
    with tempfile.TemporaryDirectory() as work:
        for cap in caps:
            label = f"{cap.model}/{cap.name}"
            try:
                cap.read()
            except (ValueError, OSError) as e:
                failed += 1
                print(f"FAIL swcaptures {label}: {e}")
                continue
            problems, diffs, notes, facts = check(cap, work, png)
            ndiff += len(diffs)
            print(f"{'FAIL' if problems else 'ok  '} swcaptures {label} "
                  f"({cap.cart} cartridge, quality {cap.quality}, {facts['pages']} page(s), {len(cap.host)} bytes)")
            for p in problems:
                print(f"     FAIL {p}")
            for d in diffs:
                print(f"     DIFF {d}")
            for n in notes:
                print(f"     note {n}")
            failed += bool(problems)
            results.append((cap, facts))
    print()
    for q in questions(results):
        print(f"swcaptures question {tag}{q}")
    print(f"{'FAIL' if failed else 'ok  '} swcaptures: {len(caps) - failed}/{len(caps)} captures decode and agree; "
          f"{ndiff} difference(s) listed from sw_session")
    return 1 if failed else 0


# ---- the self test

def selftest(png):
    """The firmware's own conversations as captures, the same checks over them, then one
    changed. Returns 0 when the checks do what they should."""
    global ROWS
    ROWS = 700             # two bands of the 400-row colour family: blocks enough to join
    cases = [("2500", "colour", "checker", 4), ("2500", "colour", "text", 4), ("2200", "colour", "blank", 4),
             ("2500", "colour", "photo", 4), ("1200", "black", "photo", 3),
             ("ii", "black", "checker", 5), ("original", "black", "checker", 4)]
    with tempfile.TemporaryDirectory() as t:
        root = pathlib.Path(t) / "caps"
        work = pathlib.Path(t) / "work"
        work.mkdir()
        for model, cart, page, q in cases:
            cap = Capture(root / model / f"{page}-{cart}-q{q}.log")
            cap.path.parent.mkdir(parents=True, exist_ok=True)
            converse(model, cart, page_file(cap, work), q, "apple" if page == "checker" else "atkinson", cap.path)
        print(f"swcaptures selftest: {len(cases)} captures from the firmware's own session")
        if check_all(root, [], png, tag="(selftest: the firmware's own captures, nothing of Apple's) "):
            print("FAIL swcaptures selftest: the firmware's own captures did not pass its own checks")
            return 1
        # a changed setup string must be found, and a changed block must fail the decode
        log = root / "2500" / "checker-colour-q4.log"
        text = log.read_text()
        mutated = text.replace("6d326e5a41", "6d326e41", 1)
        if mutated == text:
            print("FAIL swcaptures selftest: no setup string to change in the 2500 capture")
            return 1
        log.write_text(mutated)
        _reference.clear()
        cap = Capture(log).read()
        problems, diffs, _, _ = check(cap, work, False)
        if not any("order:" in d and "Z" in d for d in diffs):
            print(f"FAIL swcaptures selftest: a setup string with no Z was not found ({diffs})")
            return 1
        print("ok   swcaptures selftest: a changed setup string is listed as a difference")
        bad = re.sub(r"(\d+ > )(47)", lambda m: m[1] + "5a", text, count=1)
        log.write_text(bad)
        cap = Capture(log).read()
        problems, _, _, _ = check(cap, work, False)
        if not problems:
            print("FAIL swcaptures selftest: a block with a changed letter was not refused")
            return 1
        print("ok   swcaptures selftest: a changed block is refused")
    return 0


def main():
    ap = argparse.ArgumentParser(description="Apple's StyleWriter driver captures against the firmware's session")
    ap.add_argument("--dir", default=os.environ.get("AIRWRITER_SW_CAPTURES", DEFAULT_DIR))
    ap.add_argument("--model", action="append", default=[], choices=MODELS, help="only this model (repeatable)")
    ap.add_argument("--no-png", action="store_true", help="skip drawing the decoded pages")
    ap.add_argument("--selftest", action="store_true", help="check the checks on the firmware's own captures")
    a = ap.parse_args()
    if a.selftest or captures(a.dir, a.model):
        for tool in (RIP, SWHOST):
            if not tool.exists():
                sys.exit(f"swcaptures: build {tool.name} first: make -C tools/rip swcaptures")
    if a.selftest:
        return selftest(not a.no_png)
    return check_all(a.dir, a.model, not a.no_png)


if __name__ == "__main__":
    sys.exit(main())
