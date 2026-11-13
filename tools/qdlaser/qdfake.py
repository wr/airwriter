#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""A Personal LaserWriter 300 in software, for testing AirWriter without one (W-1102).

It answers packets as the printer does (design: docs/superpowers/specs/
2026-11-12-quickdraw-laserwriters-design.md, "What the printer and the board allow"),
decodes the pages it is sent, and draws them. Its decoder follows the design's rules,
not any code of ours, so it can check the firmware's coder and line image.

    python3 tools/qdlaser/qdfake.py serve [--pty] [--engine 29|0d|23] [--mem BYTES]
                                          [--out DIR] [--no-png] [--log FILE]
    python3 tools/qdlaser/qdfake.py decode STREAM --out DIR [--no-png]
    python3 tools/qdlaser/qdfake.py decode-bits FILE --out DIR [--no-png]

The printer's link is synchronous on a clock it drives, which a pty cannot carry, so
`serve` takes packets framed: from the host, `P`, a u16 little-endian length, and the
packet's bytes (what follows the BREAK); from the printer, `R` and a reply byte, or `B`
for a BREAK of its own. `--pty` makes a pseudo-terminal and prints its path on the
first line of stdout. `decode` reads a file of the same `P` frames (what `rip --printer
plw300 --out-qd` writes); `decode-bits` reads the line itself as the board clocks it out
(`rip --out-qd-bits`): bit k is bit (k & 7) of byte k >> 3, 1 a mark; a packet is a BREAK
of at least 95 zero bits, at least 113 marks, then bytes 8N1 LSB first. Each page
printed goes to DIR/page-N.pbm (the image area as loaded, 1 = toner) and DIR/page-N.png
(the sheet). --log FILE writes `MS > HEX` for each packet heard, `MS < HEX` for each
reply and `MS ! text` for what happened.

Lines on stdin fault it: `paper` (the cassette is empty), `cassette` (the cassette is
out), `manual` (a sheet in the manual slot), `jam`, `cover` (open), `engine` (a fuser
fault), `silent` (it hears and says nothing: switched off), `badrow N` (row packet N of
the next page fails its check: dropped, and 8E's error bit set), `ok` (every fault gone,
paper in the cassette).

What it models, from the design's command table:

* Replies only to 82, 83 (and the value packet after 83 9D or 83 A7) and 8E, one byte.
  An 83 reply has odd parity in bit 0. 8E 00's bit 2 (page done) is cleared by the read.
* A packet whose last byte is not the XOR of the others is dropped. A row packet is
  `0x5000 - n` big-endian, n bytes of run codes (literal, repeat, white, black runs of 1
  to 62, a lone 80 for a white row), its check.
* 85/86, 87 and 88/89 (each negative) place the rows: top rows down, a left margin in
  bytes that carries Apple's 0x1A0 / 2 dots past the paper's edge, the row width in
  bytes. Every row must decode to that width.
* Memory: row codes count against --mem (513,287 by default, what board 1's printer
  took); past it 8E bit 0 and 82 bit 0, and rows are lost; 82 bit 1 past 300,000.
* 81 00 starts a new page with the next row (and keeps the last one for a reprint);
  8B 8B twice ends a page; 8A 01 prints a page so ended, if nothing holds it, and it is
  done 2 s later (a BREAK, then 8E bit 2). 8A 00 stops it.
* A jam or the cover stops a page; once cleared, it prints again only after 83 5D, as
  Apple's recovery sends. 83 2A: 20 paper in the cassette, 40 a sheet in the manual slot
  (83 4C and 83 4F choose).

Not modelled: the link's clock and timing, page memory's exact cost, density, copies.
"""
import argparse
import asyncio
import os
import pathlib
import sys
import time
import tty

import numpy as np

MEM = 513287
HALF = 300000
DONE_S = 2.0
EDGE = 0x1A0 // 2          # what 87's left margin carries past the paper's own (Apple's formula)
ENGINES = {"29": 0x29, "0d": 0x0D, "23": 0x23}


class Fault(Exception):
    """A row the printer could not take."""


def odd(v):
    """A reply's data bits 7 to 1 with bit 0 making the count of ones odd."""
    v &= 0xFE
    return v if bin(v).count("1") & 1 else v | 1


def decode_row(codes, width):
    """One row's run codes into `width` bytes."""
    if len(codes) == 1 and codes[0] == 0x80:
        return bytes(width)
    out, i = bytearray(), 0
    while i < len(codes):
        c = codes[i]
        i += 1
        kind, n = c >> 6, c & 63
        if n == 0 or n == 63:
            raise Fault(f"run code {c:02x} has a count of {n}")
        if kind == 0:
            if i + n > len(codes):
                raise Fault("a literal runs past the end of its row")
            out += codes[i:i + n]
            i += n
        elif kind == 1:
            if i >= len(codes):
                raise Fault("a repeat with no byte")
            out += bytes([codes[i]]) * n
            i += 1
        elif kind == 2:
            out += bytes(n)
        else:
            out += b"\xff" * n
    if len(out) != width:
        raise Fault(f"a row of {len(out)} bytes where the width is {width}")
    return bytes(out)


def packets_from_frames(data):
    """`P` frames into packets."""
    i, out = 0, []
    while i + 3 <= len(data):
        if data[i] != ord("P"):
            i += 1
            continue
        n = data[i + 1] | data[i + 2] << 8
        if i + 3 + n > len(data):
            break
        out.append(bytes(data[i + 3:i + 3 + n]))
        i += 3 + n
    return out


def packets_from_bits(data):
    """A line image into packets: BREAK (95 zero bits or more), 113 marks or more, bytes 8N1."""
    bits = np.unpackbits(np.frombuffer(data, np.uint8), bitorder="little")
    n, k, out = len(bits), 0, []
    while k < n:
        while k < n and bits[k]:
            k += 1
        z = k
        while k < n and not bits[k]:
            k += 1
        if k - z < 95:
            continue
        m = k
        while k < n and bits[k]:
            k += 1
        if k - m < 113 or k >= n:
            continue
        pkt = bytearray()
        while k + 10 <= n and not bits[k]:
            if not bits[k + 9]:
                break                                  # no stop bit: noise
            v = 0
            for b in range(8):
                v |= int(bits[k + 1 + b]) << b
            pkt.append(v)
            k += 10
        if pkt:
            out.append(bytes(pkt))
    return out


class Page:
    """The image area as loaded, a row of `width` bytes at a time."""

    def __init__(self, top, left, width):
        self.top, self.left, self.width = top, left, width
        self.rows = []

    @property
    def x0(self):
        return max(0, self.left * 8 - EDGE)

    def bits(self):
        if not self.rows:
            return np.zeros((0, self.width * 8), bool)
        a = np.frombuffer(b"".join(self.rows), np.uint8).reshape(len(self.rows), self.width)
        return np.unpackbits(a, axis=1).astype(bool)

    def pbm(self):
        return f"P4\n{self.width * 8} {len(self.rows)}\n".encode() + b"".join(self.rows)

    def save(self, directory, n, png=True):
        d = pathlib.Path(directory)
        d.mkdir(parents=True, exist_ok=True)
        (d / f"page-{n}.pbm").write_bytes(self.pbm())
        if png:
            from PIL import Image
            b = self.bits()
            h, w = b.shape
            sheet = np.full((self.top * 2 + h, self.x0 * 2 + w), 255, np.uint8)
            sheet[self.top:self.top + h, self.x0:self.x0 + w][b] = 0
            Image.fromarray(sheet).save(d / f"page-{n}.png", compress_level=3)


class QuickDrawLaser:
    """One printer. packet() is a packet heard; it returns the reply byte or None."""

    def __init__(self, engine=0x29, mem=MEM, out=None, png=True, log=None, clock=time.monotonic,
                 say=print, done_s=DONE_S):
        self.engine, self.mem, self.out_dir, self.png, self.log = engine, mem, out, png, log
        self.clock, self.say, self.done_s = clock, say, done_s
        self.t0 = clock()
        # the faults and the paper
        self.no_paper = self.no_cassette = self.manual_sheet = False
        self.jam = self.cover = self.engine_fault = self.silent = False
        self.badrow = 0                         # row N of the next page fails its check
        self.manual = False                     # 83 4F chose the slot
        # the page
        self.top = self.left = self.width = 0
        self.lo85 = self.lo88 = 0
        self.fresh = True                       # 81 00: the next row starts a new page
        self.page = None
        self.loaded = 0                         # bytes of codes in memory
        self.rows_heard = 0
        self.ended = 0                          # 8B 8B packets since the last row
        self.ready = False                      # ended with 8B 8B twice: 8A 01 prints it
        self.error = False                      # 8E bit 1
        self.again = False                      # 83 01 bit 4: a jam's page to send again
        self.printing = None                    # when 8A 01 started it
        self.done = False                       # 8E bit 2
        self.brk = 0                            # BREAKs to send
        self.value_next = False
        self.lights = 0
        self.pages = []
        self.heard = []                         # packets, for tests
        self.buf = bytearray()                  # framed bytes not yet whole
        self.events = []

    # ---- the record
    def _ms(self):
        return int((self.clock() - self.t0) * 1000)

    def _log(self, arrow, text):
        if self.log:
            self.log.write(f"{self._ms():8d} {arrow} {text}\n")
            self.log.flush()

    def _event(self, text):
        self.events.append(text)
        self._log("!", text)

    # ---- state
    def _paper(self):
        v = 0
        if not self.no_paper and not self.no_cassette:
            v |= 0x20
        if self.manual_sheet:
            v |= 0x40
        return v

    def _has_paper(self):
        return bool(self._paper() & (0x40 if self.manual else 0x20))

    def _held(self):
        return self.jam or self.cover or self.no_cassette or self.engine_fault or not self._has_paper()

    def _s01(self):
        return (0x20 if self.printing is not None else 0) | (0x10 if self.again else 0) | \
               (0x02 if self.engine_fault else 0)

    def _s02(self):
        return (0x08 if self.jam else 0) | (0x04 if self.cover else 0) | (0x20 if self.no_cassette else 0)

    def _c82(self):
        v = 0xA0
        if self.page is None or not self.page.rows:
            v |= 0x04
        if not self._has_paper():
            v |= 0x08
        if self.loaded > HALF:
            v |= 0x02
        if self.loaded > self.mem:
            v |= 0x01
        return v

    def _e8e(self):
        v = 0xF8 | (0x04 if self.done else 0) | (0x02 if self.error else 0) | \
            (0x01 if self.loaded > self.mem else 0)
        self.done = False
        return v

    # ---- time
    def tick(self):
        """The page printing comes out; a fault stops it."""
        if self.printing is None:
            return
        if self._held():
            self.printing = None
            self._event("stopped")
            return
        if self.clock() - self.printing >= self.done_s:
            self.printing = None
            self.done = True
            self.brk += 1
            page = self.page
            self.pages.append(page)
            if self.manual:
                self.manual_sheet = False
            self._event(f"done {len(self.pages)}")
            if self.out_dir:
                page.save(self.out_dir, len(self.pages), self.png)
                self.say(f"fake LaserWriter 300: page {len(self.pages)} in {self.out_dir}")

    # ---- packets
    def packet(self, pkt):
        """A packet heard (the bytes after its BREAK). Returns the reply byte, or None."""
        self.tick()
        if self.silent:
            return None
        self.heard.append(bytes(pkt))
        x = 0
        for b in pkt[:-1]:
            x ^= b
        text = pkt.hex() if len(pkt) <= 16 else f"{pkt[:16].hex()}... ({len(pkt)} bytes)"
        self._log(">", text)
        if len(pkt) < 3 or x != pkt[-1]:
            self._event(f"bad check: {pkt[:8].hex()}")
            return None
        if pkt[0] < 0x80:
            self._row(pkt)
            return None
        r = self._command(pkt[0], pkt[1])
        if r is not None:
            self._log("<", f"{r:02x}")
        return r

    def _row(self, pkt):
        n = 0x5000 - (pkt[0] << 8 | pkt[1])
        if n != len(pkt) - 3:
            self._event(f"row length {n} in a packet of {len(pkt)}")
            return
        if self.fresh:
            self.fresh = False
            self.page = Page(-self.top, -self.left, -self.width)
            self.loaded = 0
            self.rows_heard = 0
            self.ready = False
            self.error = False
            self.ended = 0
        self.rows_heard += 1
        if self.badrow and self.rows_heard == self.badrow:
            self.badrow = 0
            self.error = True
            self._event(f"row {self.rows_heard}: check failed")
            return
        if self.loaded + n > self.mem:
            self.loaded = self.mem + 1
            return
        self.loaded += n
        if self.page.width <= 0:
            self._event("a row before the width")
            self.error = True
            return
        try:
            self.page.rows.append(decode_row(pkt[2:-1], self.page.width))
        except Fault as e:
            self._event(f"bad row {self.rows_heard}: {e}")
            self.error = True

    @staticmethod
    def _neg(lo, hi=0xFF):
        v = lo | hi << 8
        return v - 0x10000 if v & 0x8000 else v

    def _command(self, c, a):
        if self.value_next and c == 0x83:
            self.value_next = False
            return odd(0)
        if c == 0x82:
            return self._c82()
        if c == 0x8E:
            return self._e8e()
        if c == 0x83:
            if a in (0x9D, 0xA7):
                self.value_next = True
            elif a == 0x4C:
                self.manual = False
            elif a == 0x4F:
                self.manual = True
            elif a == 0x5D:
                self.again = False
            regs = {0x40: 0, 0x01: self._s01(), 0x02: self._s02(), 0x2A: self._paper(),
                    0x04: 0x10 if self.engine_fault else 0, 0x07: 0, 0x25: 0x80}
            if a == 0x3E:
                return self.engine
            if a == 0x25:
                return 0x80
            return odd(regs.get(a, 0))
        if c == 0x81:
            self.fresh = True
            self.ended = 0
        elif c == 0x84:
            self.lights = a
        elif c == 0x85:
            self.lo85 = a
        elif c == 0x86:
            self.top = self._neg(self.lo85, a)
        elif c == 0x87:
            self.left = a - 0x100 if a & 0x80 else a
        elif c == 0x88:
            self.lo88 = a
        elif c == 0x89:
            self.width = self._neg(self.lo88, a)
        elif c == 0x8B and a == 0x8B:
            self.ended += 1
            if self.ended >= 2 and self.page is not None:
                self.ready = True
                self.fresh = True
        elif c == 0x8A:
            if a == 0x00:
                if self.printing is not None:
                    self._event("8A 00: stopped")
                self.printing = None
            elif a == 0x01:
                self._print()
        return None

    def _print(self):
        if not self.ready or self.page is None:
            self._event("8A 01 with no page ended")
            return
        if self.error:
            self._event("8A 01 refused: a row was bad")
            return
        if self.again or self._held():
            self._event("8A 01 refused: " + ("waiting for 83 5D" if self.again else "held"))
            return
        self.printing = self.clock()
        self._event("print")
        if self.done_s <= 0:
            self.tick()

    # ---- faults from the bench
    def command(self, line):
        w = line.split()
        if not w:
            return
        cmd = w[0]
        if cmd == "paper":
            self.no_paper = True
        elif cmd == "cassette":
            self.no_cassette = True
        elif cmd == "manual":
            self.manual_sheet = True
        elif cmd == "jam":
            self.jam = True
            if self.printing is not None or self.ready:
                self.again = True
        elif cmd == "cover":
            self.cover = True
            if self.printing is not None or self.ready:
                self.again = True
        elif cmd == "engine":
            self.engine_fault = True
        elif cmd == "silent":
            self.silent = True
        elif cmd == "badrow" and len(w) == 2 and w[1].isdigit():
            self.badrow = int(w[1])            # of the next page
        elif cmd == "ok":
            self.no_paper = self.no_cassette = self.jam = self.cover = False
            self.engine_fault = self.silent = False
        else:
            self.say(f"fake LaserWriter 300: unknown command {line.strip()!r}: paper, cassette, "
                     "manual, jam, cover, engine, silent, badrow N, ok")
            return
        self.tick()
        self.say(f"fake LaserWriter 300: {line.strip()} -> 83 01 {self._s01():02x}, "
                 f"83 02 {self._s02():02x}, 83 2A {self._paper():02x}")

    # ---- the pty's framing
    def feed(self, data):
        """`P` frames in, `R` replies out (and a `B` for each BREAK owed)."""
        self.buf += data
        out = bytearray()
        while len(self.buf) >= 3:
            if self.buf[0] != ord("P"):
                del self.buf[0]
                continue
            n = self.buf[1] | self.buf[2] << 8
            if len(self.buf) < 3 + n:
                break
            pkt = bytes(self.buf[3:3 + n])
            del self.buf[:3 + n]
            r = self.packet(pkt)
            if r is not None:
                out += b"R" + bytes([r])
        out += self.breaks()
        return bytes(out)

    def breaks(self):
        self.tick()
        out = b"B" * self.brk if not self.silent else b""
        self.brk = 0
        return out


# ---- transports

def open_pty():
    master, slave = os.openpty()
    tty.setraw(slave)
    os.set_blocking(master, False)
    return master, slave, os.ttyname(slave)


async def pump_fd(lw, fd):
    loop = asyncio.get_running_loop()
    done = asyncio.Event()

    def readable():
        try:
            data = os.read(fd, 65536)
        except BlockingIOError:
            return
        except OSError:
            data = b""
        if not data:
            loop.remove_reader(fd)
            done.set()
            return
        reply = lw.feed(data)
        if reply:
            os.write(fd, reply)

    async def clock():
        while not done.is_set():
            await asyncio.sleep(0.05)
            b = lw.breaks()
            if b:
                try:
                    os.write(fd, b)
                except OSError:
                    pass

    loop.add_reader(fd, readable)
    t = asyncio.create_task(clock())
    await done.wait()
    t.cancel()


def stdin_commands(lw):
    loop = asyncio.get_running_loop()
    buf = bytearray()
    try:
        fd = sys.stdin.fileno()
    except (AttributeError, ValueError, OSError):
        return

    def readable():
        try:
            data = os.read(fd, 4096)
        except OSError:
            data = b""
        if not data:
            loop.remove_reader(fd)
            return
        buf.extend(data)
        while (n := buf.find(b"\n")) >= 0:
            lw.command(bytes(buf[:n]).decode(errors="replace"))
            del buf[:n + 1]

    try:
        loop.add_reader(fd, readable)
    except (OSError, ValueError, NotImplementedError):
        pass


async def serve(a):
    log = open(a.log, "a") if a.log else None
    lw = QuickDrawLaser(engine=ENGINES[a.engine], mem=a.mem, out=a.out, png=not a.no_png, log=log)
    stdin_commands(lw)
    master, slave, path = open_pty()
    print(path, flush=True)
    print(f"fake LaserWriter 300: engine {lw.engine:02x}, memory {lw.mem} bytes", flush=True)
    await pump_fd(lw, master)


def decode(a, packets):
    lw = QuickDrawLaser(out=a.out, png=not a.no_png, done_s=0, say=lambda s: None)
    for p in packets:
        lw.packet(p)
    bad = [e for e in lw.events if e.startswith(("bad", "row ", "a row", "8A 01 refused"))]
    if bad:
        print(f"qdfake: {bad[0]}", file=sys.stderr)
        return 1
    print(f"{len(lw.pages)} page(s) in {a.out}")
    return 0


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("serve")
    s.add_argument("--pty", action="store_true", required=True)
    s.add_argument("--engine", choices=list(ENGINES), default="29")
    s.add_argument("--mem", type=int, default=MEM)
    s.add_argument("--out")
    s.add_argument("--no-png", action="store_true")
    s.add_argument("--log")
    for name in ("decode", "decode-bits"):
        d = sub.add_parser(name)
        d.add_argument("file")
        d.add_argument("--out", required=True)
        d.add_argument("--no-png", action="store_true")
    a = ap.parse_args(argv)
    if a.cmd == "decode":
        return decode(a, packets_from_frames(pathlib.Path(a.file).read_bytes()))
    if a.cmd == "decode-bits":
        return decode(a, packets_from_bits(pathlib.Path(a.file).read_bytes()))
    try:
        asyncio.run(serve(a))
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
