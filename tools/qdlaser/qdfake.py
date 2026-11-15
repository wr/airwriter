#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""A Personal LaserWriter 300 in software, for testing AirWriter without one (W-1102),
(W-1145) a Personal LaserWriter LS as its own driver has it, and (W-1146) a LaserWriter
Select 300 as its own driver has it, on page memory or the 300's path.

It answers packets as the printer does (design: docs/superpowers/specs/
2026-11-12-quickdraw-laserwriters-design.md, "What the printer and the board allow"),
decodes the pages it is sent, and draws them. Its decoder follows the design's rules,
not any code of ours, so it can check the firmware's coder and line image.

    python3 tools/qdlaser/qdfake.py serve [--pty] [--engine 29|0d|23|31] [--plain] [--mem BYTES]
                                          [--done S] [--exit S] [--warm S]
                                          [--out DIR] [--no-png] [--log FILE]
    python3 tools/qdlaser/qdfake.py decode STREAM --out DIR [--no-png] [--engine 29|0d|31] [--plain]
    python3 tools/qdlaser/qdfake.py decode-bits FILE --out DIR [--no-png] [--engine 29|0d|31] [--plain]

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

Lines on stdin fault it: `paper` (the cassette is empty; an LS's tray), `cassette` (the cassette is
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

`--engine 0d` is an LS. Nobody here has one: it is the LS's own driver (Personal LW LS 7.1)
read as a printer, and AirWriter's choices for it held as rules. What differs from the 300:

* 83 51 and 83 52 choose the paper cassette and the multipurpose tray. The tray holds a
  stack and is chosen at power-up (the LS Owner's Guide: only the Ready light on). This LS
  has no cassette, an optional feeder most never had: chosen, it has no paper. No paper is
  83 02's 10.
* Rows carry a 9-bit length: `0x4000 | (0x200 - n)`.
* 87's left margin carries 0xB7 / 2 dots past the paper's edge, not 0x1A0 / 2.
* The value after 83 9D is the paper's number as it is (Letter 04, A4 01, Legal 0C, B5 09,
  else 21), logged; the reply is never read.
* After a jam it needs no 83 5D: 81 00 and the page again.
* What its driver never sends is logged as `not the LS's: ...` and fails a test: 83 2A, 83 A7,
  83 5D, 83 4C, 83 4F, 84 08. 83 40 is answered and logged plainly, since the board asks it of
  every printer before it knows which. `--refuse-40` answers it 80 (refused).
* Rows while a page prints are logged as `rows while a page prints`: AirWriter gives an LS one
  page at a time.

`--engine 31` is a Select 300 (any answer but 29, 0D or 23). Nobody here has one: it is
its own driver (LW Select 300 1.0, with LaserWriter 300 1.2's 83 68 and 83 67) read as a
printer, and AirWriter's choices for it held as rules. Every row is the 300's. What differs:

* 83 51 the cassette, 83 4F the manual feed (a sheet with `manual`), 83 52 the lower cassette,
  which this one hasn't got (chosen, it has no paper). No paper is 83 02's 10; `cassette`
  sets 20; `front` (its front cover or toner cartridge) is 83 02's 40, a cover.
* 87's left margin carries 0x16A / 2 dots past the paper's edge; the top 0x27 rows more.
* 83 5B and 83 58 take a value; 83 68 and 83 67 open and end a job; all logged.
* What AirWriter never sends a Select is logged as `not sent to a Select: ...` and fails a test:
  83 A7, 83 9D, 83 4C, 83 2A, 83 25, 83 0B, 83 0D, 84 08, 8A 02. 83 40 as for an LS.
* Page memory (unless `--plain`): 82 00's bits 7-6 are 01. --mem bytes (512 KB by default)
  at 24-bit addresses: E0-E2 set the write address and E3-E5 the read address, a byte each,
  low first, and C0-C2 and C3-C5 read them; 8B v writes v at the write address, CF 00 answers
  the byte at the read address. An address set past the memory holds nothing (it reads FF),
  unless the memory is 1 MB or more, when it wraps; writing on from the memory's last byte
  goes on at 0. 92 takes the probe's result. 8C carries the left margin's high byte (87 and
  8C are dots on page memory, as 88/89 are), A2/A3 set registers (18 and 19 must be 11 less
  the margin) and A0/A1 the PGRD table, which must have come before any page prints.
  8B 8B twice with no page open starts one at the write address, and twice after its rows
  ends it; rows go to memory at the write address (no page open: logged, dropped). 8A 01
  prints the ended page that starts at the read address (none there: logged); its done
  brings the read address to the write address. 81 00 puts the write address back to 0
  (what a real one does nobody knows). A page past the memory is memory full.

Not modelled: the link's clock and timing, page memory's exact cost, the memory
emptying as a page prints, a sheet jammed on its way out, density, copies, an LS's or a
Select's real memory and status bytes (the 300's).
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
LS_EDGE = 0xB7 // 2        # the LS's
LS_NEVER = {0x2A: "83 2A", 0xA7: "83 A7", 0x5D: "83 5D", 0x4C: "83 4C", 0x4F: "83 4F"}
SEL_EDGE = 0x16A // 2      # the Select's
SEL_TOP = 0x27             # rows its driver adds to the top
SEL_MEM = 512 * 1024
SEL_NEVER = {0xA7: "83 A7", 0x9D: "83 9D", 0x4C: "83 4C", 0x2A: "83 2A", 0x25: "83 25",
             0x0B: "83 0B", 0x0D: "83 0D"}
ENGINES = {"29": 0x29, "0d": 0x0D, "23": 0x23, "31": 0x31}


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

    def __init__(self, top, left, width, edge=EDGE):
        self.top, self.left, self.width, self.edge = top, left, width, edge
        self.rows = []

    @property
    def x0(self):
        return max(0, int(self.left * 8) - self.edge)

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


class PmPage:
    """A page in a Select's page memory: where it starts, where it ends once ended."""

    def __init__(self, start, page):
        self.start, self.end, self.page = start, None, page
        self.bytes = 0
        self.ruined = False


class QuickDrawLaser:
    """One printer. packet() is a packet heard; it returns the reply byte or None."""

    def __init__(self, engine=0x29, mem=None, out=None, png=True, log=None, clock=time.monotonic,
                 say=print, done_s=DONE_S, exit_s=0.0, warm_s=0.0, refuse_40=False, plain=False):
        self.ls = engine == 0x0D                # a Personal LaserWriter LS
        self.sel = engine not in (0x29, 0x0D, 0x23)   # a LaserWriter Select 300
        self.pm = self.sel and not plain        # on page memory
        if mem is None:
            mem = SEL_MEM if self.sel else MEM
        self.engine, self.mem, self.out_dir, self.png, self.log = engine, mem, out, png, log
        self.refuse_40 = refuse_40
        self.name = ("fake LaserWriter Select 300" if self.sel else
                     "fake LaserWriter LS" if self.ls else "fake LaserWriter 300")
        # page memory
        self.ram = bytearray(min(mem, 1 << 22)) if self.pm else None
        self.wa = self.ra = 0
        self.pm_pages = []                      # PmPage, the latest last
        self.pm_open = None                     # the page being written
        self.marks = 0                          # 8B 8B packets in a row
        self.left_hi = 0                        # 8C
        self.regs = {}                          # A2/A3
        self.a0 = 0
        self.pgrd = {}                          # A0/A1: the PGRD table
        self.probe_told = None                  # what 92 last carried
        self.lower = False                      # 83 52: a Select's lower cassette, not fitted
        self.front = False                      # a Select's 83 02 bit 6
        self.clock, self.say = clock, say
        self.done_s, self.exit_s, self.warm_s = done_s, exit_s, warm_s
        self.t0 = clock()
        # the faults and the paper
        self.no_paper = self.no_cassette = self.manual_sheet = False
        self.jam = self.cover = self.engine_fault = self.silent = False
        self.badrow = 0                         # row N of the next page fails its check
        self.manual = self.ls                   # 83 4F chose the slot (an LS: 83 52, its tray)
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
        self.value_for = 0                      # the 83 the value follows
        self.size = None                        # an LS's paper number, from 83 9D
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
        if self.ls:                             # the tray's stack; no cassette fitted
            return self.manual and not self.no_paper
        if self.sel:
            if self.lower:
                return False
            return self.manual_sheet if self.manual else not (self.no_paper or self.no_cassette)
        return bool(self._paper() & (0x40 if self.manual else 0x20))

    def _held(self):
        return self.jam or self.cover or self.front or self.no_cassette or self.engine_fault or \
            not self._has_paper()

    def _moving(self):
        return self.printing is not None or \
            (self.moving_until is not None and self.clock() < self.moving_until)

    def _faulted(self):
        """A jam or the cover shows, and the host has been told so."""
        return self.told and (self.jam or self.cover or self.front)

    def _next_ended(self):
        return self.rd < len(self.slots) and self.slots[self.rd].ended

    def _used(self):
        return sum(s.bytes for s in self.slots[self.freed:])

    def _s01(self):
        # No paper at the source chosen also says a fault is stored, as board 1's printer did;
        # so do a jam and the cover (83 01 02 with the cover open at rest, 13 with a page
        # stopped, on the bench).
        return (0x20 if self._moving() else 0) | (0x10 if self.again else 0) | \
               (0x02 if self.engine_fault or self.jam or self.cover or self.front or
                not self._has_paper() else 0)

    def _s02(self):
        # 10: no paper at the source chosen (83 4C the cassette, 83 4F the manual slot; an LS's
        # 83 52 tray, or 83 51 the cassette it doesn't have).
        v = (0x08 if self.jam else 0) | (0x04 if self.cover else 0) | \
            (0x10 if not self._has_paper() else 0)
        if self.sel:
            v |= (0x40 if self.front else 0) | (0x20 if self.no_cassette and not self.manual else 0)
        return v

    def _c82(self):
        v = 0x40 if self.pm else 0xA0           # bits 7-6: 01 page memory
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
            if self.jam or self.cover or self.front:
                self.stopped = self.printing
            self.printing = None
            self._event("stopped")
            return
        if self.clock() < self.done_at:
            return
        slot, self.printing = self.printing, None
        self.done = True
        self.brk += 1
        if self.pm:
            self.ra = self.wa                   # the addresses meet
        self.moving_until = self.done_at + self.exit_s
        self.rd += 1
        self.freed = self.rd
        if self.stopped is slot:
            self.stopped = None
        if self.manual and not self.ls:
            self.manual_sheet = False
        if self.blank or slot.ruined:
            self._sheet_blank(self.blank or "memory filled as it loaded")
            self._event("done, blank")
            return
        self.pages.append(copy.copy(slot.page))
        self._event(f"done {len(self.pages)}")
        if self.out_dir:
            slot.page.save(self.out_dir, len(self.pages), self.png)
            self.say(f"{self.name}: page {len(self.pages)} in {self.out_dir}")

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
            if self.pm:
                self._pm_row(pkt)
            else:
                self._row(pkt)
            return None
        r = self._command(pkt[0], pkt[1])
        if r is not None:
            self._log("<", f"{r:02x}")
        return r

    def _row(self, pkt):
        if self.ls:
            if pkt[0] & 0xFE != 0x40:
                self._event(f"row length {pkt[:2].hex()}: not the LS's 9-bit length")
                return
            n = 0x200 - ((pkt[0] << 8 | pkt[1]) & 0x1FF)
        else:
            n = 0x5000 - (pkt[0] << 8 | pkt[1])
        if n != len(pkt) - 3:
            self._event(f"row length {n} in a packet of {len(pkt)}")
            return
        if self.ls and self.printing is not None:
            self._event("rows while a page prints: an LS takes one page at a time")
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
            self.slots.append(Slot(Page(-self.top - (SEL_TOP if self.sel else 0), -self.left,
                                        -self.width,
                                        LS_EDGE if self.ls else SEL_EDGE if self.sel else EDGE)))
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

    # ---- page memory (W-1146)
    def _phys(self, a):
        """The cell an address names, or None: past the memory, nothing under 1 MB."""
        if a < self.mem:
            return a
        if self.mem >= 0x100000 and not self.mem & (self.mem - 1):
            return a & (self.mem - 1)
        return None

    def _pm_put(self, data):
        """Bytes at the write address, which moves on, from the memory's end to 0."""
        for b in data:
            p = self._phys(self.wa)
            if p is not None and p < len(self.ram):
                self.ram[p] = b
            self.wa += 1
            if self.wa == self.mem:
                self.wa = 0

    def _pm_row(self, pkt):
        n = 0x5000 - (pkt[0] << 8 | pkt[1])
        if n != len(pkt) - 3:
            self._event(f"row length {n} in a packet of {len(pkt)}")
            return
        self.marks = 0
        if self.printing is not None:
            self._event("rows while a page prints: a Select takes one page at a time")
        if self._faulted():
            self._event("rows while a jam or the cover shows")
            self.error = True
            return
        pg = self.pm_open
        if pg is None:
            self._event("a row before its page's start (8B 8B twice)")
            self.error = True
            return
        self._pm_put(pkt[2:-1])
        pg.bytes += n
        if pg.bytes + 4 > self.mem and not pg.ruined:
            pg.ruined = self.full = True
            self._event(f"memory full: a page of {pg.bytes} bytes")
        if pg.page.width <= 0:
            self._event("a row before the width")
            self.error = True
            return
        try:
            pg.page.rows.append(decode_row(pkt[2:-1], pg.page.width))
        except Fault as e:
            self._event(f"bad row {len(pg.page.rows) + 1}: {e}")
            self.error = True

    def _pm_mark(self):
        """8B 8B: twice with no page open starts one; twice after its rows ends it."""
        self.marks += 1
        if self.marks < 2:
            return
        self.marks = 0
        pg = self.pm_open
        if pg is not None and pg.page.rows:
            pg.end = self.wa
            self.pm_open = None
            return
        if pg is not None:
            return                              # an empty page's second pair: still open
        dots = -self._neg(self.left & 0xFF, self.left_hi)
        width = -self.width
        if width % 8:
            self._event(f"a width of {width} dots, not whole bytes")
        page = Page(-self.top - SEL_TOP, dots / 8, width // 8, SEL_EDGE)
        self.pm_open = PmPage(self.wa, page)
        self.pm_pages = [p for p in self.pm_pages if p.end is not None][-3:] + [self.pm_open]
        self.error = self.full = False
        want = (11 - dots) & 0xFFFF
        if self.regs.get(0x19) != want >> 8 or self.regs.get(0x18) != want & 0xFF:
            self._event(f"A2 18/19 don't match the margin: {self.regs.get(0x19)} {self.regs.get(0x18)} "
                        f"for {dots} dots")

    def _pm_print(self):
        pg = next((p for p in reversed(self.pm_pages) if p.start == self.ra and p.end is not None), None)
        if pg is None:
            self._event(f"8A 01 at {self.ra:06x}: no page starts there")
            return
        if len(self.pgrd) < 96:
            self._event(f"printed with no PGRD table ({len(self.pgrd)} of 96 entries)")
        if self.printing is not None:
            self._event("8A 01 while a page prints")
            return
        if self.jam or self.cover or self.front:
            self._event("8A 01 while a jam or the cover shows")
            return
        if self.again or self._held():
            self._event("8A 01 refused: " + ("waiting for 83 5D" if self.again else "held"))
            return
        slot = Slot(pg.page)
        slot.ruined = pg.ruined
        slot.ended = True
        self._start(slot)

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
            if self.ls and self.value_for == 0x9D:
                self.size = a
                self._event(f"paper number {a:02x}")
            if self.sel:
                self._event(("density " if self.value_for == 0x58 else "83 5B's value ") + f"{a:02x}")
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
            if self.ls and a in LS_NEVER:
                self._event(f"not the LS's: {LS_NEVER[a]}")
            if self.sel and a in SEL_NEVER:
                self._event(f"not sent to a Select: {SEL_NEVER[a]}")
            if self.sel and a in (0x67, 0x68):
                self._event("job ends (83 67)" if a == 0x67 else "job opens (83 68)")
            if (self.ls or self.sel) and a == 0x40:
                self._event("83 40, asked before the printer is known")
                if self.refuse_40:
                    return 0x80
            if a in ((0x5B, 0x58) if self.sel else (0x9D, 0xA7)):
                self.value_next = True
                self.value_for = a
            elif self.sel and a in (0x4F, 0x51, 0x52):
                self.manual, self.lower = a == 0x4F, a == 0x52
            elif a == (0x51 if self.ls else 0x4C):
                self.manual = False
            elif a == (0x52 if self.ls else 0x4F):
                self.manual = True
            elif a == 0x5D:
                self.again = False
            if a == 0x02:
                self.told = bool(self._s02() & (0x4C if self.sel else 0x0C))
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
        if c == 0x81 and self.ls:
            self.again = False                  # its driver's recovery: 81 00, the page again
        if c == 0x81 and self.pm:
            self.wa = 0
            self.marks = 0
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
                if self.ls:
                    self._event("not the LS's: 84 08")
                if self.sel:
                    self._event("not sent to a Select: 84 08")
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
        elif c == 0x8B and self.pm:
            self._pm_put(bytes([a]))
            if a == 0x8B:
                self._pm_mark()
            else:
                self.marks = 0
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
                self._pm_print() if self.pm else self._print()
            elif a == 0x02 and self.sel:
                self._event("not sent to a Select: 8A 02")
        elif self.pm and 0xE0 <= c <= 0xE5:
            k = 8 * ((c - 0xE0) % 3)
            if c < 0xE3:
                self.wa = self.wa & ~(0xFF << k) | a << k
            else:
                self.ra = self.ra & ~(0xFF << k) | a << k
        elif self.pm and 0xC0 <= c <= 0xC5:
            return ((self.wa if c < 0xC3 else self.ra) >> (8 * ((c - 0xC0) % 3))) & 0xFF
        elif self.pm and c == 0xCF:
            p = self._phys(self.ra)
            return self.ram[p] if p is not None and p < len(self.ram) else 0xFF
        elif self.pm and c == 0x92:
            self.probe_told = a
            self._event(f"92 {a:02x}")
        elif self.pm and c == 0x8C:
            self.left_hi = a
        elif self.pm and c == 0xA2:
            self.reg = a
        elif self.pm and c == 0xA3:
            self.regs[getattr(self, "reg", 0)] = a
        elif self.pm and c == 0xA0:
            self.a0 = a
        elif self.pm and c == 0xA1:
            if self.a0 < 0x60:
                self.pgrd[self.a0] = a
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
        again = self.again and not self.ls     # the LS's driver never sends 83 5D
        if again or self._held():
            self._event("8A 01 refused: " + ("waiting for 83 5D" if again else "held"))
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
        if self.sel:                            # its driver's 0x27 rows; page memory's dots
            slot.page.top -= SEL_TOP
            if self.pm:
                slot.page.left = -self._neg(self.left & 0xFF, self.left_hi) / 8
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
        elif cmd in ("jam", "jam2", "cover", "front"):
            self.tick()
            if cmd == "cover":
                self.cover = True
            if cmd == "front":
                self.front = True
            if cmd not in ("cover", "front") or self.printing is not None:
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
            was_jam = self.jam or self.cover or self.front
            self.no_paper = self.no_cassette = self.jam = self.cover = self.front = False
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
            self.say(f"{self.name}: unknown command {line.strip()!r}: paper, cassette, "
                     "manual, jam, jam2, cover, front, engine, silent, badrow N, ok, clear")
            return
        self.tick()
        self.say(f"{self.name}: {line.strip()} -> 83 01 {self._s01():02x}, "
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
                        done_s=a.done, exit_s=a.exit, warm_s=a.warm, refuse_40=a.refuse_40,
                        plain=a.plain)
    stdin_commands(lw)
    master, slave, path = open_pty()
    print(path, flush=True)
    print(f"{lw.name}: engine {lw.engine:02x}, memory {lw.mem} bytes", flush=True)
    await pump_fd(lw, master)


# What a test fails on, in the printer's log.
BAD = ("bad", "row ", "a row", "8A 01 refused", "blank", "memory full", "moved", "wrong page",
       "rows while", "81 00 while", "84 08 while", "8A 01 while", "fed by itself", "jammed again",
       "not the LS's", "not sent to a Select", "8A 01 at", "printed with no PGRD", "A2 18/19",
       "a width of")


def decode(a, packets):
    lw = QuickDrawLaser(engine=ENGINES[a.engine], out=a.out, png=not a.no_png, done_s=0, exit_s=0,
                        say=lambda s: None, plain=a.plain)
    for p in packets:
        lw.packet(p)
    bad = [e for e in lw.events if e.startswith(BAD)]
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
    s.add_argument("--mem", type=int, default=None,
                   help=f"bytes of memory ({MEM} for a 300 or an LS, {SEL_MEM} for a Select)")
    s.add_argument("--plain", action="store_true", help="a Select on the 300's path, not page memory")
    s.add_argument("--done", type=float, default=DONE_S, help="seconds a page takes to print")
    s.add_argument("--exit", type=float, default=EXIT_S, help="seconds its sheet then takes to come out")
    s.add_argument("--warm", type=float, default=WARM_S,
                   help="seconds the 83 questions go unanswered after the cover closes")
    s.add_argument("--out")
    s.add_argument("--no-png", action="store_true")
    s.add_argument("--log")
    s.add_argument("--refuse-40", action="store_true", help="answer 83 40 refused (an LS)")
    for name in ("decode", "decode-bits"):
        d = sub.add_parser(name)
        d.add_argument("file")
        d.add_argument("--engine", choices=list(ENGINES), default="29")
        d.add_argument("--out", required=True)
        d.add_argument("--no-png", action="store_true")
        d.add_argument("--plain", action="store_true")
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
