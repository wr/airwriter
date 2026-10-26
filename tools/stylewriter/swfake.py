#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""A StyleWriter in software, for testing AirWriter without one (W-1103).

It answers on a byte stream as the seven Apple StyleWriters do (design:
docs/superpowers/specs/2026-10-25-stylewriters-design.md, "What the printer and the
board allow"), decodes the pages it is sent, and draws them as ink on paper. The
decoder follows the wire format's rules from the design, not any code of ours, so
it can check the firmware's coder.

    python3 tools/stylewriter/swfake.py serve MODEL [--pty | --serial DEV | --tcp HOST:PORT |
                                         --listen PORT] [--cart black|colour]
                                         [--media letter|a4|legal] [--out DIR] [--no-png]
                                         [--log FILE] [--drain BYTES_PER_S]
    python3 tools/stylewriter/swfake.py decode MODEL STREAM.bin --out DIR [--media M] [--no-png]

MODEL is original, ii, 1200, 1500, 2200, 2400 or 2500. `serve` waits for bytes on
the transport: `--pty` makes a pseudo-terminal and prints its path on the first
line of stdout (open it as a serial port at 57600 8N1); `--serial DEV` opens a real
serial device raw at 57600 8N1; `--tcp HOST:PORT` connects out; `--listen PORT`
accepts connections on 127.0.0.1 (0 picks a port; the first line says which). No
flow control, as the printer has none. `--cart` is the cartridge fitted (a colour
one only in the 1500, 2200, 2400 and 2500). `--drain` is how fast the printer eats
its buffer, 32768 by default. Each page that ends with 0C goes to DIR/page-N.png
(ink on paper at 360 dots an inch, the sheet with its margins) and
DIR/page-N-{c,m,y,k}.pbm (the printable area, 1 = ink) when --out is given;
--no-png skips the pictures. --log FILE writes both directions, one line each,
`MS > HEX` for what the printer heard and `MS < HEX` for what it said, and
`MS ! bad REASON` when it ejected.

Lines on stdin fault it (as etpeer.py's fake LaserWriter does): `paper`, `jam`,
`ink`, `door`, `waste`, `cart` (cartridge missing; `cart black` or `cart colour`
swaps the cartridge in), `busy N` (status busy for N seconds), `silent` (it answers
and hears nothing, as if unplugged), `ok` (every fault gone), `overflow` (the next
time more arrives than its buffer holds, take it, and fault the time after).

What it models, from the design:

* `?` answers SW (II), SW3 (1200), IJ10 (original, only once reset), CS (1500, 2200,
  2400, 2500) and a CR; `FF FF FF p` after CS answers one byte: 01 2400, 02 2200,
  04 1500, 05 2500. `p` to any other model is a fault.
* `FF FF FF I` resets: the page in the printer is ejected, and for 2 s it hears
  nothing. A fault (below) leaves it deaf until that reset.
* Queries, `FF FF FF` and a letter: `1` status (01 busy, 02 error, 08 page done, 40
  door, 80 waste ink), `2` errors (01 cartridge jammed, 02 ink out, 04 no paper, 08
  paper jam, 20 cartridge missing; the 2400 and 2500 add a spurious 80), `B` the free
  buffer, `H` the cartridge (81 colour, 01 black, 00 missing), `S` resume after a
  fault, `D`, `N` (taken, no answer), `R` (answers 00).
* The buffer: free bytes are B x 256 - 2048 (the original's B x 64). `B` reads the
  model's empty value when it is empty and falls as page bytes (rects, G blocks, 0C)
  arrive; it refills at --drain, but not while a fault holds the page, and after a
  fault only `S` lets it go on. More than it holds is a fault.
* A page: `R l t r b` (black) or `c l t r b` (four inks), little-endian u16, then `G`
  blocks (`G`, u16 length, data, 00) of run-coded rows, then 0C. Rows are checked the
  way the design says: counts 1-62, `80` alone for a white row, each plane XORed with
  the row above inside a band (the original not), colour rows C, M, Y, K. A rect is
  inside the printable area (minus the +16 of the 1200 family), no taller than a band,
  and gets all its rows before the next rect or 0C.
* Setup letters (m + one byte, d n Z b s A u D N L H B F, E ends a page early, Y l,
  P once a cleaning on every model but the original) are taken and recorded in
  `setup`. Anything else is a fault.
* A fault (logged as bad, in `bad`): the page is ejected and the printer is deaf until
  the next reset. Faults are an unknown letter, a bad run code, a block longer than
  its length, a rect past the printable width or the page, a colour rect on a model
  without colour, `p` to a model that is not a colour one, an overflow, and the
  0x8000-byte block that stalls a colour printer.

Not modelled: the line speed (a byte arrives when it is fed), the fast clock, paper
sizes (the media only sets the sheet), cartridge jams, "paper too small".
"""
import argparse
import asyncio
import collections
import os
import pathlib
import struct
import sys
import termios
import time
import tty

import numpy as np

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent / "iwview"))
import iwview  # noqa: E402  the ink-on-paper model

DPI = 360                      # the StyleWriters print 360 dots an inch
DRAIN = 32768                  # bytes a second the engine eats, unless told
CLEAN_SECONDS = 2.0
SETTLE = 2.0                   # after FF FF FF I

# paper sheets in dots at 360 an inch
MEDIA = {"letter": (3060, 3960), "a4": (2976, 4209), "legal": (3060, 5040)}

Spec = collections.namedtuple(
    "Spec", "ident cs p across ml mr mt mb band dx colour delta cleans bdiv spurious80")

# 2880 across and margins 72/72, 72, 114 in the 1200 family; 2919 and 72/72, 45, 99 in the
# colour (CS) family; the rect's x values are 16 more in the 1200 family.
_F1200 = dict(across=2880, ml=72, mr=72, mt=72, mb=114, band=800, dx=16, delta=True, cleans=True,
              bdiv=256, spurious80=False)
_FCS = dict(across=2919, ml=72, mr=72, mt=45, mb=99, band=400, dx=0, delta=True, cleans=True,
            bdiv=256, colour=True)
SPECS = {
    "original": Spec(ident=b"IJ10\r", cs=False, p=0, colour=False,
                     **{**_F1200, "dx": 0, "delta": False, "cleans": False, "bdiv": 64}),
    "ii": Spec(ident=b"SW\r", cs=False, p=0, colour=False, **_F1200),
    "1200": Spec(ident=b"SW3\r", cs=False, p=0, colour=False, **_F1200),
    "1500": Spec(ident=b"CS\r", cs=True, p=4, colour=True, **_F1200),
    "2200": Spec(ident=b"CS\r", cs=True, p=2, spurious80=False, **_FCS),
    "2400": Spec(ident=b"CS\r", cs=True, p=1, spurious80=True, **_FCS),
    "2500": Spec(ident=b"CS\r", cs=True, p=5, spurious80=True, **_FCS),
}
# `B` when the buffer is empty. The 2200 reads A2 or A3 (`B & A0 == A0`): A2 here.
B_EMPTY = {"original": 0xA0, "ii": 0xF8, "1200": 0xF8, "2200": 0xA2, "2400": 0xF8, "2500": 0x80}

ERR = {"paper": 0x04, "jam": 0x08, "ink": 0x02, "cart": 0x20}
BARE = set(b"dnZbsAuDNLHBFYl")           # setup letters on their own; m takes one byte, E and P are below
ZERO_ROW = 0x80


class Fault(Exception):
    """The printer will eject and reset: the text says why."""


def b_empty(model, cart):
    if model == "1500":
        return 0x87 if cart == "colour" else 0x9F
    return B_EMPTY[model]


def printable(spec, media):
    """The printable area in dots: the engine's widest row or the sheet less both side
    margins, whichever is less; the sheet less the top and bottom margins."""
    return min(spec.across, media[0] - spec.ml - spec.mr), media[1] - spec.mt - spec.mb


class Band:
    """A rect being filled with rows."""

    def __init__(self, inks, x0, y0, w, h):
        self.inks, self.x0, self.y0, self.w, self.h = inks, x0, y0, w, h
        self.wb = (w + 7) // 8
        self.prev = {i: None for i in inks}     # each plane's last row, for the XOR
        self.part = bytearray()                 # the plane row being decoded
        self.plane = self.rows = 0
        self.done = h == 0


class Page:
    """Four planes of the printable area, a bit a dot, rows packed as in a PBM."""

    def __init__(self, spec, media):
        self.spec, self.paper = spec, media
        self.w, self.h = printable(spec, media)
        if self.w <= 0 or self.h <= 0:
            raise ValueError(f"a {media[0]} x {media[1]} dot sheet has no printable area")
        self.wb = (self.w + 7) // 8
        self.planes = {i: np.zeros((self.h, self.wb), np.uint8) for i in "cmyk"}
        self.rects = 0

    def put(self, ink, y, x0, w, row):
        """OR one rect row (bytes from the rect's left edge, most significant bit first) in."""
        plane = self.planes[ink]
        sh, b0 = x0 & 7, x0 >> 3
        if sh == 0 and w & 7 == 0:
            plane[y, b0:b0 + len(row)] |= row
            return
        bits = np.unpackbits(row)[:w]
        if sh:
            bits = np.concatenate((np.zeros(sh, np.uint8), bits))
        packed = np.packbits(bits)
        plane[y, b0:b0 + len(packed)] |= packed

    def bits(self, ink):
        """The plane as a bool array (rows, printable width)."""
        return np.unpackbits(self.planes[ink], axis=1)[:, :self.w].astype(bool)

    def pbm(self, ink):
        return f"P4\n{self.w} {self.h}\n".encode() + self.planes[ink].tobytes()

    def draw(self):
        """The sheet as ink on paper, a pixel a dot (iwview's press model)."""
        W, H = self.paper
        rng = np.random.default_rng(7)
        px = iwview.blank_paper(W, H, rng)
        r = iwview.dot_radius(DPI, DPI)
        R = iwview.pad_for(r)
        for ink, mask in (("y", iwview.Y), ("m", iwview.M), ("c", iwview.C), ("k", iwview.K)):
            bits = self.bits(ink)
            if not bits.any():
                continue
            cnt = np.zeros((H + 2 * R + 2, W + 2 * R + 2), np.float32)
            top, left = R + self.spec.mt, R + self.spec.ml
            cnt[top:top + self.h, left:left + self.w] = bits * (
                0.82 + 0.18 * rng.random(bits.shape, np.float32))
            iwview.lay_ink(px, iwview.spread(cnt, r), mask)
        return px.clip(0, 255).astype(np.uint8)

    def save(self, directory, n, png=True):
        d = pathlib.Path(directory)
        d.mkdir(parents=True, exist_ok=True)
        for ink in "cmyk":
            (d / f"page-{n}-{ink}.pbm").write_bytes(self.pbm(ink))
        if png:
            from PIL import Image
            Image.fromarray(self.draw()).save(d / f"page-{n}.png", compress_level=3)


class StyleWriter:
    """One printer. feed() is what the wire carries to it and returns what it says back."""

    def __init__(self, model, cart="black", media="letter", drain=DRAIN, clock=time.monotonic,
                 out=None, log=None, settle=SETTLE, say=print, png=True, unlimited=False):
        if model not in SPECS:
            raise ValueError(f"unknown model {model!r}: {', '.join(SPECS)}")
        self.model, self.spec = model, SPECS[model]
        if cart not in ("black", "colour"):
            raise ValueError("the cartridge is black or colour")
        if cart == "colour" and not self.spec.colour:
            raise ValueError(f"the {model} takes no colour cartridge")
        self.cart, self.cart_missing = cart, False
        self.media = MEDIA[media] if isinstance(media, str) else tuple(media)
        self.drain, self.clock, self.out_dir, self.say = drain, clock, out, say
        self.settle, self.png, self.unlimited = settle, png, unlimited
        self.logf, self.t0 = log, clock()
        self.pages, self.ejected, self.bad, self.setup = [], [], [], []
        self.cleanings = 0
        self.err = 0                        # the bits of `2` that faults have set
        self.door = self.waste = self.silent = False
        self.halted = False                 # a fault held a page: only S lets it go on
        self.tolerate = 0                   # `overflow` on stdin: overflows to forgive
        self.awake = model != "original"    # the original ignores `?` until it is reset
        self.busy_until = 0.0
        self.fill = 0.0                     # bytes in the buffer
        self.page = self.band = None
        self.page_pending = False           # a page ended: busy until it has printed, then 08
        self.state, self.ready_at, self.t_last = "idle", 0.0, clock()
        self._tail = b""
        self._over_call = False
        self.out = bytearray()
        self._gen = self._new_gen()

    # ---- the buffer and the clock
    @property
    def cap(self):
        """Bytes the empty buffer holds: what `B` says when empty."""
        return b_empty(self.model, self.cart) * self.spec.bdiv - 2048

    def _faulted(self):
        return bool(self.err or self.door or self.waste)

    def _tick(self):
        now = self.clock()
        if self.state == "settling" and now >= self.ready_at:
            self.state = "idle"
        dt, self.t_last = now - self.t_last, now
        if self.fill > 0 and not self.halted and not self._faulted():
            self.fill = 0.0 if self.drain == float("inf") else max(0.0, self.fill - dt * self.drain)
        return now

    def _count(self, n):
        self.fill += n
        if self.unlimited or self.fill <= self.cap:
            return
        if self._over_call:                       # an overflow forgiven this write goes on being so
            self.fill = self.cap
        elif self.tolerate > 0:
            self.tolerate -= 1
            self._over_call, self.fill = True, self.cap
            self._event("overflow forgiven")
        else:
            raise Fault(f"overflow: more than the {self.cap} bytes the buffer holds")

    def _b(self):
        free = self.cap - self.fill
        return max(0, min(255, int((free + 2048) // self.spec.bdiv)))

    # ---- the wire
    def _stamp(self):
        return int((self.clock() - self.t0) * 1000)

    def _log(self, arrow, data):
        if self.logf and data:
            self.logf.write(f"{self._stamp():8d} {arrow} {data.hex()}\n")
            self.logf.flush()

    def _event(self, text):
        if self.logf:
            self.logf.write(f"{self._stamp():8d} ! {text}\n")
            self.logf.flush()

    def feed(self, data):
        """Bytes arrive; returns what the printer answers (b"" for nothing)."""
        data = bytes(data)
        self._log(">", data)
        self._tick()
        self.out = bytearray()
        self._over_call = False
        if not self.silent:
            for b in data:
                if self.state != "idle":
                    self._deaf(b)
                    continue
                try:
                    self._gen.send(b)
                except Fault as f:
                    self._bad(str(f))
        reply = bytes(self.out)
        self._log("<", reply)
        return reply

    def _deaf(self, b):
        """Settling, or stuck after a fault: only the reset gets through."""
        self._tail = (self._tail + bytes([b]))[-4:]
        if self._tail == b"\xff\xff\xffI":
            self._reset()

    def _new_gen(self):
        g = self._proc()
        next(g)
        return g

    def _bad(self, why):
        self.bad.append(why)
        self._event(f"bad {why}")
        self.say(f"fake StyleWriter: bad: {why}")
        self._eject()
        self.state, self._tail, self._gen = "stuck", b"", self._new_gen()

    def _eject(self):
        if self.page is not None:
            self.ejected.append(self.page)
        self.page = self.band = None
        self.fill, self.halted, self.page_pending = 0.0, False, False

    def _reset(self):
        self._eject()
        self.awake, self.busy_until, self._tail = True, 0.0, b""
        self._gen = self._new_gen()
        if self.settle > 0:
            self.state, self.ready_at = "settling", self.clock() + self.settle
        else:
            self.state = "idle"

    # ---- parsing: one generator, a byte at a time
    def _take(self, n):
        buf = bytearray()
        while len(buf) < n:
            buf.append((yield))
            self._count(1)
        return buf

    def _proc(self):
        spec = self.spec
        while True:
            b = yield
            if b == 0x00:
                continue
            if b == 0xFF:                          # FF FF FF and a letter
                n = 1
                while True:
                    c = yield
                    if c != 0xFF:
                        break
                    n += 1
                if n < 3:
                    raise Fault(f"{n} sync byte(s) before {c:02x}, not FF FF FF")
                self._query(c)
            elif b == 0x3F:                        # ?
                if self.awake:
                    self.out += spec.ident
            elif b == 0x0C:
                self._count(1)
                self._page_end()
            elif b in (0x52, 0x63):                # R, c
                self._count(1)
                raw = yield from self._take(8)
                self._rect(b, raw)
            elif b == 0x47:                        # G
                self._count(1)
                ln = yield from self._take(2)
                n = ln[0] | ln[1] << 8
                if spec.cs and n == 0x8000:
                    raise Fault("a block of 0x8000 bytes stalls a colour printer")
                data = yield from self._take(n)
                end = yield from self._take(1)
                if end[0] != 0:
                    raise Fault(f"block of {n} bytes is not followed by 00 "
                                f"(its data is longer than its length)")
                self._block(data)
            elif b == 0x6D:                        # m and one more
                arg = yield
                self.setup.append(bytes([b, arg]))
            elif b == 0x45:                        # E
                self.setup.append(b"E")
                self._eject()
            elif b == 0x4C:                        # L
                self.setup.append(b"L")
                self._load()
            elif b == 0x50:                        # P
                if not spec.cleans:
                    raise Fault(f"the {self.model} has no cleaning (P)")
                self.setup.append(b"P")
                self.cleanings += 1
                self.busy_until = self.clock() + CLEAN_SECONDS
            elif b in BARE:
                self.setup.append(bytes([b]))
            else:
                raise Fault(f"unknown letter {b:02x} ({chr(b) if 32 <= b < 127 else '?'})")

    def _query(self, c):
        now = self._tick()
        ch, spec = chr(c), self.spec
        if ch == "I":
            self._reset()
        elif ch == "1":
            self.out.append(self._status(now))
        elif ch == "2":
            self.out.append(self.err | (0x80 if spec.spurious80 else 0))
        elif ch == "B":
            self.out.append(self._b())
        elif ch == "H":
            self.out.append(self._cart_byte())
        elif ch == "R":
            self.out.append(0)
        elif ch == "S":
            if not self._faulted():
                self.halted = False
        elif ch in "DN":
            pass
        elif ch == "p":
            if not spec.cs:
                raise Fault(f"p sent to a {self.model}, which does not answer it")
            self.out.append(spec.p)
        else:
            raise Fault(f"unknown query {ch!r}")

    def _status(self, now):
        s = 0
        if now < self.busy_until or (self.page_pending and self.fill > 0):
            s |= 0x01
        if self.err:
            s |= 0x02
        if self.page_pending and self.fill <= 0:
            s |= 0x08
        if self.door:
            s |= 0x40
        if self.waste:
            s |= 0x80
        return s

    def _cart_byte(self):
        if self.cart_missing:
            return 0x00
        return 0x81 if self.cart == "colour" else 0x01

    # ---- pages
    def _need_page(self):
        if self.page is None:
            self.page = Page(self.spec, self.media)
        self.page_pending = False

    def _load(self):
        if self.page is not None and self.page.rects:
            self._event("L inside a page: the page is ejected")
            self._eject()
        self.page = self.band = None
        self.page_pending = False

    def _rect(self, kind, raw):
        spec = self.spec
        l, t, r, b = struct.unpack("<HHHH", bytes(raw))
        colour = kind == 0x63
        if colour and not spec.colour:
            raise Fault(f"a colour rect (c) to a {self.model}, which prints black only")
        if self.band is not None and not self.band.done:
            raise Fault("a rect before the last one had all its rows")
        w, h = printable(spec, self.media)
        x0, x1 = l - spec.dx, r - spec.dx
        if l < spec.dx:
            raise Fault(f"rect left {l} is before the printable area (it starts at {spec.dx})")
        if l > r or t > b:
            raise Fault(f"rect {l},{t},{r},{b} runs backwards")
        if x1 >= w:
            raise Fault(f"rect right {r} is past the printable width ({w} dots, +{spec.dx})")
        if b >= h:
            raise Fault(f"rect bottom {b} is past the page ({h} printable rows)")
        if b - t + 1 > spec.band:
            raise Fault(f"rect of {b - t + 1} rows is taller than a {spec.band}-row band")
        self._need_page()
        self.page.rects += 1
        self.band = Band("cmyk" if colour else "k", x0, t, x1 - x0 + 1, b - t + 1)

    def _block(self, data):
        band, spec = self.band, self.spec
        if band is None or band.done:
            raise Fault("a G block with no rect to fill")
        wb, part, i, n = band.wb, band.part, 0, len(data)
        while i < n:
            if band.done:
                raise Fault("the block has more rows than its rect")
            code = data[i]
            i += 1
            kind, cnt = code >> 6, code & 63
            if code == ZERO_ROW:                   # 80 alone: a white plane row
                if part:
                    raise Fault("80 inside a row")
                self._row(bytes(wb))
                continue
            if cnt == 0 or cnt == 63:
                raise Fault(f"run code {code:02x} has a count of {cnt} (1 to 62)")
            if kind == 0:
                if i + cnt > n:
                    raise Fault(f"a literal of {cnt} bytes runs past the end of its block")
                part += data[i:i + cnt]
                i += cnt
            elif kind == 1:
                if i >= n:
                    raise Fault("a repeat code at the end of its block has no byte")
                part += bytes([data[i]]) * cnt
                i += 1
            elif kind == 2:
                part += bytes(cnt)
            else:
                part += b"\xff" * cnt
            if len(part) > wb:
                raise Fault(f"a run goes past the end of its row ({len(part)} of {wb} bytes)")
            if len(part) == wb:
                self._row(bytes(part))
                part.clear()

    def _row(self, coded):
        band, spec = self.band, self.spec
        ink = band.inks[band.plane]
        row = np.frombuffer(coded, np.uint8)
        if spec.delta and band.prev[ink] is not None:
            row = row ^ band.prev[ink]
        band.prev[ink] = row
        self.page.put(ink, band.y0 + band.rows, band.x0, band.w, row)
        band.plane += 1
        if band.plane == len(band.inks):
            band.plane = 0
            band.rows += 1
            if band.rows == band.h:
                band.done = True

    def _page_end(self):
        if self.band is not None and not self.band.done:
            raise Fault("0C before the rect had all its rows")
        self._need_page()
        page, self.page, self.band = self.page, None, None
        self.pages.append(page)
        self.page_pending = True
        if self.out_dir:
            page.save(self.out_dir, len(self.pages), self.png)
            self.say(f"fake StyleWriter: page {len(self.pages)} in {self.out_dir}")

    # ---- faults from the bench
    def command(self, line):
        """A fault from the bench (stdin) or a test."""
        w = line.split()
        if not w:
            return
        c, now = w[0], self.clock()
        if c in ERR and not (c == "cart" and len(w) > 1):
            self.err |= ERR[c]
            if c == "cart":
                self.cart_missing = True
        elif c == "cart":
            if w[1] == "colour" and not self.spec.colour:
                self.say(f"the {self.model} takes no colour cartridge")
                return
            if w[1] not in ("black", "colour"):
                self.say("cart black | cart colour")
                return
            self.cart, self.cart_missing = w[1], False
            self.err &= ~0x20
        elif c == "door":
            self.door = True
        elif c == "waste":
            self.waste = True
        elif c == "silent":
            self.silent = True
        elif c == "ok":
            self.err, self.door, self.waste, self.silent = 0, False, False, False
            self.cart_missing, self.busy_until = False, 0.0
        elif c == "busy":
            self.busy_until = now + float(w[1] if len(w) > 1 else 30)
        elif c == "overflow":
            self.tolerate = 1
        else:
            self.say(f"unknown command {line.strip()!r}: paper, jam, ink, door, waste, "
                     "cart [black|colour], busy N, silent, ok, overflow")
            return
        if self._faulted() and (self.fill > 0 or self.page is not None):
            self.halted = True
        self.say(f"fake StyleWriter: {line.strip()} -> status {self._status(now):02x}, "
                 f"errors {self.err:02x}")


# ---- transports: byte streams

def open_pty():
    master, slave = os.openpty()
    tty.setraw(slave)
    os.set_blocking(master, False)
    return master, slave, os.ttyname(slave)


def open_serial(path):
    fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    tty.setraw(fd)
    attrs = termios.tcgetattr(fd)
    attrs[2] = (attrs[2] & ~(termios.CSIZE | termios.PARENB | termios.CSTOPB | termios.CRTSCTS)) \
        | termios.CS8 | termios.CLOCAL | termios.CREAD
    attrs[4] = attrs[5] = termios.B57600
    termios.tcsetattr(fd, termios.TCSANOW, attrs)
    return fd


async def pump_fd(sw, fd):
    loop = asyncio.get_running_loop()
    done = asyncio.Event()

    def readable():
        try:
            data = os.read(fd, 65536)
        except BlockingIOError:
            return
        except OSError as e:
            print(f"fake StyleWriter: {e}", file=sys.stderr)
            loop.remove_reader(fd)
            done.set()
            return
        reply = sw.feed(data) if data else b""
        if reply:
            os.write(fd, reply)

    loop.add_reader(fd, readable)
    await done.wait()


async def pump_stream(sw, reader, writer):
    while True:
        data = await reader.read(65536)
        if not data:
            break
        reply = sw.feed(data)
        if reply:
            writer.write(reply)
            await writer.drain()
    writer.close()


async def stdin_commands(sw):
    loop = asyncio.get_running_loop()
    while True:
        line = await loop.run_in_executor(None, sys.stdin.readline)
        if not line:
            return
        sw.command(line)


async def serve(a):
    log = open(a.log, "a") if a.log else None
    sw = StyleWriter(a.model, cart=a.cart, media=a.media, drain=a.drain, out=a.out, log=log,
                     png=not a.no_png)
    tasks = [asyncio.create_task(stdin_commands(sw))]
    if a.pty:
        master, slave, path = open_pty()
        print(path, flush=True)
        tasks.append(asyncio.create_task(pump_fd(sw, master)))
    elif a.serial:
        tasks.append(asyncio.create_task(pump_fd(sw, open_serial(a.serial))))
    elif a.tcp:
        host, _, port = a.tcp.rpartition(":")
        reader, writer = await asyncio.open_connection(host, int(port))
        tasks.append(asyncio.create_task(pump_stream(sw, reader, writer)))
    else:
        server = await asyncio.start_server(lambda r, w: pump_stream(sw, r, w), "127.0.0.1",
                                            int(a.listen))
        print(f"listening on {server.sockets[0].getsockname()[1]}", flush=True)
        tasks.append(asyncio.create_task(server.serve_forever()))
    print(f"fake StyleWriter: {a.model}, {sw.cart} cartridge, buffer {sw.cap} bytes", flush=True)
    await asyncio.wait(tasks[1:], return_when=asyncio.FIRST_COMPLETED)


def decode(a):
    """A captured or ripped stream, through the same rules, into the same files."""
    sw = StyleWriter(a.model, media=a.media, drain=float("inf"), out=a.out, settle=0,
                     png=not a.no_png, unlimited=True, say=lambda s: print(s, file=sys.stderr))
    sw.feed(pathlib.Path(a.stream).read_bytes())
    if sw.bad:
        print(f"swfake: the stream is bad: {sw.bad[0]}", file=sys.stderr)
        return 1
    if sw.page is not None and sw.page.rects:
        print("swfake: the last page has no 0C; kept as it stands", file=sys.stderr)
        sw.pages.append(sw.page)
        sw.page.save(a.out, len(sw.pages), not a.no_png)
    print(f"{len(sw.pages)} page(s) in {a.out}")
    return 0


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0], add_help=True)
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("serve")
    s.add_argument("model", choices=list(SPECS))
    g = s.add_mutually_exclusive_group(required=True)
    g.add_argument("--pty", action="store_true")
    g.add_argument("--serial", metavar="DEV")
    g.add_argument("--tcp", metavar="HOST:PORT")
    g.add_argument("--listen", metavar="PORT")
    s.add_argument("--cart", choices=("black", "colour"), default="black")
    s.add_argument("--media", choices=list(MEDIA), default="letter")
    s.add_argument("--out")
    s.add_argument("--no-png", action="store_true")
    s.add_argument("--log")
    s.add_argument("--drain", type=float, default=DRAIN, metavar="BYTES_PER_S")
    d = sub.add_parser("decode")
    d.add_argument("model", choices=list(SPECS))
    d.add_argument("stream")
    d.add_argument("--out", required=True)
    d.add_argument("--media", choices=list(MEDIA), default="letter")
    d.add_argument("--no-png", action="store_true")
    a = ap.parse_args(argv)
    if a.cmd == "decode":
        return decode(a)
    try:
        asyncio.run(serve(a))
    except KeyboardInterrupt:
        pass
    except ValueError as e:
        sys.exit(f"swfake: {e}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
