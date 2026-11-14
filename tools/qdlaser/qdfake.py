#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""A Personal LaserWriter 300 in software, for testing AirWriter without one (W-1102).

It answers packets as the printer does (design: docs/superpowers/specs/
2026-11-12-quickdraw-laserwriters-design.md, "What the printer and the board allow"),
decodes the pages it is sent, and draws them. Its decoder follows the design's rules,
not any code of ours, so it can check the firmware's coder and line image.

    python3 tools/qdlaser/qdfake.py serve [--pty] [--engine 29|0d|23] [--mem BYTES]
                                          [--done S] [--exit S] [--warm S]
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
out), `manual` (a sheet in the manual slot), `jam`, `jam2` (a jam with two sheets in the
paper path), `cover` (open), `engine` (a fuser fault), `silent` (it hears and says
nothing: switched off), `badrow N` (row packet N of the next page fails its check:
dropped, and 8E's error bit set), `ok` (the cover closed, the sheet that jammed pulled,
paper in the cassette: with a second sheet still in the path, 83 02 reads clear once
and then jam again), `clear` (`ok`, and every sheet out of the path).

What it models, from the design's command table:

* Replies only to 82, 83 (and the value packet after 83 9D or 83 A7) and 8E, one byte.
  An 83 reply has odd parity in bit 0. 8E 00's bit 2 (page done) is cleared by the read.
* A packet whose last byte is not the XOR of the others is dropped. A row packet is
  `0x5000 - n` big-endian, n bytes of run codes (literal, repeat, white, black runs of 1
  to 62, a lone 80 for a white row), its check.
* 85/86, 87 and 88/89 (each negative) place the rows: top rows down, a left margin in
  bytes that carries Apple's 0x1A0 / 2 dots past the paper's edge, the row width in
  bytes. Every row must decode to that width.
* Memory (W-1144, board 1's printer on the bench): the pages in it are kept in the order
  they were loaded, and --mem (513,287 by default, what the printer took) holds the codes
  of every page not yet done, so the next page's rows go in while one prints and a page's
  room is free again at its done. 82 bit 1 says more than 262,144 bytes are held, bit 2
  that nothing waits to print; 82 bit 0 is never set.
* Past --mem, 8E bit 0. While a page prints that is the end of both: a BREAK, the sheet
  printing comes out blank and is done early, and the page loading prints blank too.
* 8B 8B twice ends the page loading; the next row starts another behind it. 8A 01 prints
  the next page not yet printed, if it is ended and nothing holds it; it is done --done
  seconds later (2 by default: a BREAK, then 8E bit 2), and its sheet is on its way out
  for --exit seconds more (83 01 bit 5 through both). 8A 00 stops it.
* 81 00 goes back to the start of the memory and clears nothing: 8A 01 then prints the
  first page loaded since the 81 00 before, and the pages behind it after that; the next
  row starts the memory over. 81 00 while a page prints: that sheet comes out blank and
  no page done comes for it. Rows that start the memory over while a page prints blank
  that sheet too (the real printer may hold them; nobody has tried).
* 85/86, 87 and 88/89 are read as a page prints: changed while one prints, they move that
  sheet (its page records where it ended up, and the log says so).
* A jam or the cover stops a page (83 01 13; 83 02 0D for both, as the cover opened
  mid-page gives, 04 for the cover alone); once cleared, it prints again only after
  83 5D, as Apple's recovery sends, and for --warm seconds after the cover closes the 83
  questions go unanswered. The recovery's 81 00 means the page printed again is the first
  since the 81 00 before: when another was stopped, the log says the wrong page printed.
* A jam as a page prints while the sheet before it is still on its way out has two sheets
  in the path (bench A4d): after `ok`, 83 02 reads clear once, then jam again, until
  `clear`. An 8A 01 heard while a jam or the cover shows is kept, and the printer feeds by
  itself when the jam clears; 81 00 or 84 08 into that feed jams it again (A4e). 84 08
  is followed by --warm seconds of unanswered 83s, as the cover closing is.
* Once an 83 02 has said jam or cover, and until one says clear, the host is to send
  nothing but questions, 8A 00 and the lights. A row, 81 00, 84 08 or 8A 01 then is
  logged (`rows while…`, `81 00 while…`, `84 08 while…`, `8A 01 while…`), and the page
  loading never prints: nobody has tried the real printer with them, so a test fails.
  83 2A: 20 paper in the cassette, 40 a sheet in the manual slot (83 4C and 83 4F
  choose). No paper at the source chosen answers as board 1's printer did: 83 02 sets 10
  and 83 01 says a fault is stored, the cassette empty or out, or the manual slot chosen
  and empty.

Not modelled: the link's clock and timing, page memory's exact cost, the memory
emptying as a page prints, a sheet jammed on its way out, density, copies.
"""
import argparse
import asyncio
import copy
import os
import pathlib
import sys
import time
import tty

import numpy as np

MEM = 513287
HALF = 262144
DONE_S = 2.0
EXIT_S = 2.0
WARM_S = 0.3
FULL_DONE = 0.08           # a sheet lost to memory full is done this much of DONE_S later
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


class Slot:
    """A page in the printer's memory."""

    def __init__(self, page):
        self.page = page
        self.bytes = 0                          # its codes
        self.rows_heard = 0
        self.ended = False                      # 8B 8B twice came after its rows
        self.error = False                      # a row was bad: it never prints
        self.ruined = False                     # memory filled as it loaded: it prints blank


class QuickDrawLaser:
    """One printer. packet() is a packet heard; it returns the reply byte or None."""

    def __init__(self, engine=0x29, mem=MEM, out=None, png=True, log=None, clock=time.monotonic,
                 say=print, done_s=DONE_S, exit_s=0.0, warm_s=0.0):
        self.engine, self.mem, self.out_dir, self.png, self.log = engine, mem, out, png, log
        self.clock, self.say = clock, say
        self.done_s, self.exit_s, self.warm_s = done_s, exit_s, warm_s
        self.t0 = clock()
        # the faults and the paper
        self.no_paper = self.no_cassette = self.manual_sheet = False
        self.jam = self.cover = self.engine_fault = self.silent = False
        self.badrow = 0                         # row N of the next page fails its check
        self.manual = False                     # 83 4F chose the slot
        self.quiet_until = None                 # the 83 questions go unanswered until then
        # the registers
        self.top = self.left = self.width = 0
        self.lo85 = self.lo88 = 0
        # the memory
        self.slots = []                         # the pages in it, in the order loaded
        self.rd = 0                             # the next to print
        self.freed = 0                          # the pages before this one are done: their room is free
        self.fresh = True                       # 81 00: the next row starts the memory over
        self.full = False                       # 8E bit 0
        self.ended = 0                          # 8B 8B packets since the last row
        self.error = False                      # 8E bit 1
        self.again = False                      # 83 01 bit 4: a jam's page to send again
        self.stopped = None                     # the page a jam or the cover stopped
        self.sheets = 0                         # sheets in the paper path at a jam
        self.rejam = False                      # one clear 83 02, then the jam again
        self.told = False                       # the last 83 02 said jam or cover
        self.kept_print = False                 # an 8A 01 heard during a jam
        self.fed = False                        # the page printing started by itself
        # the page printing
        self.printing = None                    # its slot
        self.print_at = self.done_at = 0.0
        self.blank = None                       # why its sheet comes out blank
        self.moving_until = None                # a sheet on its way out
        self.done = False                       # 8E bit 2
        self.brk = 0                            # BREAKs to send
        self.value_next = False
        self.lights = 0
        self.pages = []                         # the sheets that came out whole
        self.blanks = []                        # and why each blank one was
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

    def _moving(self):
        return self.printing is not None or \
            (self.moving_until is not None and self.clock() < self.moving_until)

    def _faulted(self):
        """A jam or the cover shows, and the host has been told so."""
        return self.told and (self.jam or self.cover)

    def _next_ended(self):
        return self.rd < len(self.slots) and self.slots[self.rd].ended

    def _used(self):
        return sum(s.bytes for s in self.slots[self.freed:])

    def _s01(self):
        # No paper at the source chosen also says a fault is stored, as board 1's printer did;
        # so do a jam and the cover (83 01 02 with the cover open at rest, 13 with a page
        # stopped, on the bench).
        return (0x20 if self._moving() else 0) | (0x10 if self.again else 0) | \
               (0x02 if self.engine_fault or self.jam or self.cover or not self._has_paper() else 0)

    def _s02(self):
        # 10: no paper at the source chosen (83 4C the cassette, 83 4F the manual slot).
        return (0x08 if self.jam else 0) | (0x04 if self.cover else 0) | \
               (0x10 if not self._has_paper() else 0)

    def _c82(self):
        v = 0xA0
        if self.fresh or not any(s.page.rows for s in self.slots[self.freed:]):
            v |= 0x04
        if not self._has_paper():
            v |= 0x08
        if self._used() > HALF:
            v |= 0x02
        return v

    def _e8e(self):
        v = 0xF8 | (0x04 if self.done else 0) | (0x02 if self.error else 0) | \
            (0x01 if self.full else 0)
        self.done = False
        return v

    # ---- time
    def _sheet_blank(self, why):
        self.blanks.append(why)
        self._event(f"blank sheet: {why}")

    def tick(self):
        """The page printing comes out; a fault stops it."""
        if self.printing is None:
            return
        if self._held():
            if self.jam or self.cover:
                self.stopped = self.printing
            self.printing = None
            self._event("stopped")
            return
        if self.clock() < self.done_at:
            return
        slot, self.printing = self.printing, None
        self.done = True
        self.brk += 1
        self.moving_until = self.done_at + self.exit_s
        self.rd += 1
        self.freed = self.rd
        if self.stopped is slot:
            self.stopped = None
        if self.manual:
            self.manual_sheet = False
        if self.blank or slot.ruined:
            self._sheet_blank(self.blank or "memory filled as it loaded")
            self._event("done, blank")
            return
        self.pages.append(copy.copy(slot.page))
        self._event(f"done {len(self.pages)}")
        if self.out_dir:
            slot.page.save(self.out_dir, len(self.pages), self.png)
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
        if self._faulted():
            self._event("rows while a jam or the cover shows")
            self.error = True
            if self.slots and not self.slots[-1].ended:
                self.slots[-1].error = True
            return
        if self.fresh:
            self.fresh = False
            if self.printing is not None:
                self.blank = "rows started the memory over while it printed"
            self.slots = []
            self.rd = self.freed = 0
            self.full = self.error = False
            self.stopped = None
        if not self.slots or self.slots[-1].ended:
            self.slots.append(Slot(Page(-self.top, -self.left, -self.width)))
        slot = self.slots[-1]
        self.ended = 0
        slot.rows_heard += 1
        if self.badrow and slot.rows_heard == self.badrow:
            self.badrow = 0
            self.error = slot.error = True
            self._event(f"row {slot.rows_heard}: check failed")
            return
        if self.full:
            return
        if self._used() + n > self.mem:
            self.full = True
            slot.ruined = True
            self._event(f"memory full: {self._used()} bytes held and {n} more")
            if self.printing is not None:
                # Bench A2b: a BREAK, the page printing is done early, and both sheets are blank.
                self.brk += 1
                self.blank = "memory filled while it printed"
                self.done_at = min(self.done_at, self.clock() + FULL_DONE * self.done_s)
            return
        slot.bytes += n
        if slot.page.width <= 0:
            self._event("a row before the width")
            self.error = slot.error = True
            return
        try:
            slot.page.rows.append(decode_row(pkt[2:-1], slot.page.width))
        except Fault as e:
            self._event(f"bad row {slot.rows_heard}: {e}")
            self.error = slot.error = True

    @staticmethod
    def _neg(lo, hi=0xFF):
        v = lo | hi << 8
        return v - 0x10000 if v & 0x8000 else v

    def _moved(self):
        """85 to 89 are read as a page prints: the sheet printing goes where they now say."""
        if self.printing is None:
            return
        page = self.printing.page
        if (page.top, page.left) != (-self.top, -self.left):
            page.top, page.left = -self.top, -self.left
            self._event("moved: the offsets changed while a page printed")

    def _command(self, c, a):
        if self.value_next and c == 0x83:
            self.value_next = False
            return odd(0)
        if c == 0x82:
            return self._c82()
        if c == 0x8E:
            return self._e8e()
        if c == 0x83:
            if self.quiet_until is not None:
                if self.clock() < self.quiet_until:
                    return None
                self.quiet_until = None
            if a in (0x9D, 0xA7):
                self.value_next = True
            elif a == 0x4C:
                self.manual = False
            elif a == 0x4F:
                self.manual = True
            elif a == 0x5D:
                self.again = False
            if a == 0x02:
                self.told = bool(self._s02() & 0x0C)
                if not self.told and self.rejam:
                    self.rejam = False
                    reply = odd(self._s02())
                    self.jam = True             # the second sheet: a jam again at the next look
                    self._event("jam again: a second sheet is in the path")
                    return reply
            regs = {0x40: 0, 0x01: self._s01(), 0x02: self._s02(), 0x2A: self._paper(),
                    0x04: 0x10 if self.engine_fault else 0, 0x07: 0, 0x25: 0x80}
            if a == 0x3E:
                return self.engine
            if a == 0x25:
                return 0x80
            return odd(regs.get(a, 0))
        if c == 0x81 or (c == 0x84 and a == 0x08):
            name = "81 00" if c == 0x81 else "84 08"
            if self._faulted():
                self._event(f"{name} while a jam or the cover shows")
            if self.printing is not None and self.fed:
                # Bench A4e: sent into the printer's own feed, it jammed again.
                self.stopped = self.printing
                self.printing = None
                self.jam = self.again = True
                self.sheets = 1
                self._event(f"jammed again: {name} into a feed")
        if c == 0x81:
            if self.printing is not None:
                # Bench A3: the sheet goes on out, blank, and no page done comes for it.
                self.printing = None
                self.moving_until = self.clock() + self.done_s + self.exit_s
                self._sheet_blank("81 00 while it printed")
            self.fresh = True
            self.ended = 0
            self.rd = self.freed = 0
        elif c == 0x84:
            self.lights = a
            if a == 0x08:
                self.quiet_until = self.clock() + self.warm_s
        elif c == 0x85:
            self.lo85 = a
        elif c == 0x86:
            self.top = self._neg(self.lo85, a)
            self._moved()
        elif c == 0x87:
            self.left = a - 0x100 if a & 0x80 else a
            self._moved()
        elif c == 0x88:
            self.lo88 = a
        elif c == 0x89:
            self.width = self._neg(self.lo88, a)
            if self.printing is not None and -self.width != self.printing.page.width:
                self._event("moved: the width changed while a page printed")
        elif c == 0x8B and a == 0x8B:
            self.ended += 1
            if self.ended >= 2 and self.slots and self.slots[-1].page.rows:
                self.slots[-1].ended = True
        elif c == 0x8A:
            if a == 0x00:
                if self.printing is not None:
                    self._event("8A 00: stopped")
                self.printing = None
            elif a == 0x01:
                self._print()
        return None

    def _print(self):
        if self.printing is not None:
            self._event("8A 01 while a page prints")
            return
        if not self._next_ended():
            self._event("8A 01 with no page ended")
            return
        slot = self.slots[self.rd]
        if slot.error:
            self._event("8A 01 refused: a row was bad")
            return
        if self.jam or self.cover:
            # Bench A4e: kept, and run when the jam clears.
            self.kept_print = True
            self._event("8A 01 while a jam or the cover shows: kept")
            return
        if self.again or self._held():
            self._event("8A 01 refused: " + ("waiting for 83 5D" if self.again else "held"))
            return
        if self.stopped is not None and self.stopped is not slot:
            self._event("wrong page: the first in the memory printed again, not the one that stopped")
            self.stopped = None
        self._start(slot)

    def _start(self, slot, fed=False):
        self.printing = slot
        self.fed = fed
        self.blank = None
        slot.page.top, slot.page.left = -self.top, -self.left
        self.print_at = self.clock()
        self.done_at = self.print_at + self.done_s
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
        elif cmd in ("jam", "jam2", "cover"):
            self.tick()
            if cmd == "cover":
                self.cover = True
            if cmd != "cover" or self.printing is not None:
                self.jam = True                 # the cover mid-page: the sheet inside is a jam too
                # The sheet before, still on its way out, is caught as well (bench A4d).
                behind = self.moving_until is not None and self.clock() < self.moving_until
                self.sheets = 2 if cmd == "jam2" or (self.printing is not None and behind) else 1
            if self.printing is not None or self._next_ended():
                self.again = True
        elif cmd == "engine":
            self.engine_fault = True
        elif cmd == "silent":
            self.silent = True
        elif cmd == "badrow" and len(w) == 2 and w[1].isdigit():
            self.badrow = int(w[1])            # of the next page
        elif cmd in ("ok", "clear"):
            self.tick()
            if self.cover:
                self.quiet_until = self.clock() + self.warm_s
            was_jam = self.jam or self.cover
            self.no_paper = self.no_cassette = self.jam = self.cover = False
            self.engine_fault = self.silent = False
            if cmd == "ok" and was_jam and self.sheets > 1:
                self.sheets -= 1
                self.rejam = True               # clear at the next look, then a jam again
            else:
                self.sheets = 0
                self.rejam = False
                if self.kept_print:
                    self.kept_print = False
                    if self._next_ended() and not self._held():
                        self._start(self.slots[self.rd], fed=True)
                        self.again = False
                        self._event("fed by itself: the 8A 01 heard during the jam")
        else:
            self.say(f"fake LaserWriter 300: unknown command {line.strip()!r}: paper, cassette, "
                     "manual, jam, jam2, cover, engine, silent, badrow N, ok, clear")
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
    lw = QuickDrawLaser(engine=ENGINES[a.engine], mem=a.mem, out=a.out, png=not a.no_png, log=log,
                        done_s=a.done, exit_s=a.exit, warm_s=a.warm)
    stdin_commands(lw)
    master, slave, path = open_pty()
    print(path, flush=True)
    print(f"fake LaserWriter 300: engine {lw.engine:02x}, memory {lw.mem} bytes", flush=True)
    await pump_fd(lw, master)


def decode(a, packets):
    lw = QuickDrawLaser(out=a.out, png=not a.no_png, done_s=0, exit_s=0, say=lambda s: None)
    for p in packets:
        lw.packet(p)
    bad = [e for e in lw.events if e.startswith(("bad", "row ", "a row", "8A 01 refused", "blank",
                                                  "memory full", "moved", "wrong page", "rows while",
                                                  "81 00 while", "84 08 while", "8A 01 while",
                                                  "fed by itself", "jammed again"))]
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
    s.add_argument("--done", type=float, default=DONE_S, help="seconds a page takes to print")
    s.add_argument("--exit", type=float, default=EXIT_S, help="seconds its sheet then takes to come out")
    s.add_argument("--warm", type=float, default=WARM_S,
                   help="seconds the 83 questions go unanswered after the cover closes")
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
