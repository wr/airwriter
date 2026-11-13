#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""qdfake.py's printer held to the design's command table (W-1102).

Packets here are built by hand from the design ("What the printer and the board allow"),
not by our C code, and the line image by a second writer in this file, so the fake is an
independent reader of both.

    python3 -m unittest tools/qdlaser/test_qdfake.py
"""
import pathlib
import sys
import tempfile
import unittest

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import qdfake  # noqa: E402


def cmd(c, a):
    return bytes([c, a, c ^ a])


def value(v):
    b = (v << 1) & 0xFE
    if not bin(b).count("1") & 1:
        b |= 1
    return cmd(0x83, b)


def row(codes):
    n = len(codes)
    head = bytes([(0x5000 - n) >> 8, (0x5000 - n) & 0xFF])
    x = 0
    for b in head + codes:
        x ^= b
    return head + bytes(codes) + bytes([x])


def odd_ok(r):
    return not r & 0x80 and bin(r).count("1") & 1


LETTER = [cmd(0x81, 0), cmd(0x83, 0x40), cmd(0x83, 0x3E), cmd(0x82, 0), cmd(0x87, 0xDD),
          cmd(0x85, 0xC2), cmd(0x86, 0xFF), cmd(0x83, 0x9D), value(0x21), cmd(0x83, 0xA7),
          value(32), cmd(0x84, 0), cmd(0x88, 0xD4), cmd(0x89, 0xFE), cmd(0x83, 0x4C), cmd(0x84, 1)]
END = [cmd(0x8B, 0x8B), cmd(0x8B, 0x8B)]
PRINT = [cmd(0x8A, 1)]

# Three rows of 300 bytes: black, white, and AB CD then white.
BLACK = bytes([0xC0 | 62] * 4 + [0xC0 | 52])
WHITE = bytes([0x80])
ABCD = bytes([0x02, 0xAB, 0xCD] + [0x80 | 62] * 4 + [0x80 | 50])
ROWS = [BLACK, WHITE, ABCD]
WANT = [b"\xff" * 300, bytes(300), b"\xab\xcd" + bytes(298)]


class Clock:
    def __init__(self):
        self.t = 100.0

    def __call__(self):
        return self.t


def printer(**kw):
    clock = Clock()
    lw = qdfake.QuickDrawLaser(clock=clock, say=lambda s: None, **kw)
    return lw, clock


def send(lw, packets):
    return [lw.packet(p) for p in packets]


def ask(lw, c, a):
    return lw.packet(cmd(c, a))


def line(packets):
    """The line image, written here apart from the fake's reader."""
    bits = []
    for p in packets:
        bits += [0] * 100 + [1] * 120
        for b in p:
            bits += [0] + [(b >> i) & 1 for i in range(8)] + [1]
        bits += [1] * 180
    bits += [1] * (-len(bits) % 8)
    out = bytearray()
    for i in range(0, len(bits), 8):
        out.append(sum(bit << k for k, bit in enumerate(bits[i:i + 8])))
    return bytes(out)


def frames(packets):
    return b"".join(b"P" + bytes([len(p) & 0xFF, len(p) >> 8]) + p for p in packets)


class Identity(unittest.TestCase):
    def test_replies(self):
        lw, _ = printer()
        self.assertEqual(ask(lw, 0x83, 0x40), 0x01)
        self.assertEqual(ask(lw, 0x83, 0x3E), 0x29)
        self.assertEqual(ask(lw, 0x82, 0x00), 0xA4)
        self.assertIsNone(ask(lw, 0x81, 0x00))       # no reply

    def test_other_engines(self):
        for e in (0x0D, 0x23):
            lw, _ = printer(engine=e)
            self.assertEqual(ask(lw, 0x83, 0x3E), e)

    def test_83_replies_have_odd_parity(self):
        lw, _ = printer()
        for a in (0x40, 0x01, 0x02, 0x04, 0x07, 0x2A, 0x4C, 0x5D, 0x9D):
            lw.value_next = False
            self.assertTrue(odd_ok(ask(lw, 0x83, a)), f"83 {a:02x}")

    def test_only_82_83_8e_reply(self):
        lw, _ = printer()
        for c, a in ((0x81, 0), (0x84, 1), (0x85, 0xC2), (0x86, 0xFF), (0x87, 0xDD),
                     (0x88, 0xD4), (0x89, 0xFE), (0x8A, 0), (0x8B, 0x8B)):
            self.assertIsNone(ask(lw, c, a), f"{c:02x} {a:02x}")
        self.assertEqual(ask(lw, 0x8E, 0x00), 0xF8)

    def test_framing(self):
        lw, _ = printer()
        self.assertEqual(lw.feed(frames([cmd(0x83, 0x3E), cmd(0x81, 0)])), b"R\x29")


class Pages(unittest.TestCase):
    def test_letter_page_of_three_rows(self):
        lw, clock = printer()
        send(lw, LETTER + [row(r) for r in ROWS] + END + PRINT)
        self.assertEqual(lw.pages, [])
        self.assertEqual(ask(lw, 0x83, 0x01) & 0x20, 0x20)       # paper moving
        clock.t += 2.1
        lw.tick()
        self.assertEqual(len(lw.pages), 1)
        p = lw.pages[0]
        self.assertEqual(p.rows, WANT)
        self.assertEqual(p.top, 62)                               # 85 C2 86 FF
        self.assertEqual(p.x0, 35 * 8 - 0x1A0 // 2)               # 87 DD: 72 dots in
        self.assertEqual(p.width, 300)                            # 88 D4 89 FE
        self.assertEqual(lw.breaks(), b"B")                       # the printer's BREAK
        self.assertEqual(ask(lw, 0x8E, 0x00), 0xFC)               # page done, cleared by the read
        self.assertEqual(ask(lw, 0x8E, 0x00), 0xF8)

    def test_bad_check_dropped(self):
        lw, clock = printer()
        bad = bytearray(row(ROWS[1]))
        bad[-1] ^= 1
        send(lw, LETTER + [row(ROWS[0]), bytes(bad), row(ROWS[2])] + END + PRINT)
        clock.t += 2.1
        lw.tick()
        self.assertEqual(lw.pages[0].rows, [WANT[0], WANT[2]])

    def test_print_needs_8b_8b_twice(self):
        for end in ([], END[:1]):
            lw, clock = printer()
            send(lw, LETTER + [row(r) for r in ROWS] + end + PRINT)
            clock.t += 5
            lw.tick()
            self.assertEqual(lw.pages, [])

    def test_81_00_reprints_the_page_in_memory(self):
        lw, clock = printer()
        send(lw, LETTER + [row(r) for r in ROWS] + END + PRINT)
        clock.t += 2.1
        lw.tick()
        send(lw, [cmd(0x81, 0), cmd(0x8A, 1)])
        clock.t += 2.1
        lw.tick()
        self.assertEqual(len(lw.pages), 2)
        self.assertEqual(lw.pages[1].rows, WANT)

    def test_memory_full(self):
        lw, _ = printer(mem=1000)
        lit = bytes([62] + [0x55] * 62) * 4 + bytes([52] + [0x55] * 52)   # 305 bytes
        send(lw, LETTER + [row(lit)] * 3)
        self.assertEqual(ask(lw, 0x8E, 0x00) & 1, 0)
        self.assertEqual(ask(lw, 0x82, 0x00) & 1, 0)
        send(lw, [row(lit)])                                      # 1220 > 1000
        self.assertEqual(ask(lw, 0x8E, 0x00) & 1, 1)
        self.assertEqual(ask(lw, 0x82, 0x00) & 1, 1)

    def test_past_half(self):
        lw, _ = printer()
        lit = bytes([62] + [0x55] * 62) * 4 + bytes([52] + [0x55] * 52)
        send(lw, LETTER + [row(lit)] * 980)                       # 298,900 bytes
        self.assertEqual(ask(lw, 0x82, 0x00) & 2, 0)
        send(lw, [row(lit)] * 4)                                  # 300,120
        self.assertEqual(ask(lw, 0x82, 0x00) & 3, 2)


class Faults(unittest.TestCase):
    def reg(self, lw, a):
        return ask(lw, 0x83, a) & 0xFE

    def test_each_fault_sets_its_bits(self):
        lw, _ = printer()
        self.assertEqual(self.reg(lw, 0x2A), 0x20)
        # No paper at the source chosen: 83 02 10 and 83 01's fault bit with it, as board 1's
        # printer answered (2026-11-13), whether the cassette is empty or out.
        for fault in ("paper", "cassette"):
            lw.command(fault)
            self.assertEqual(self.reg(lw, 0x2A), 0x00)
            self.assertEqual(self.reg(lw, 0x02), 0x10)
            self.assertEqual(self.reg(lw, 0x01) & 0x02, 0x02)
            self.assertEqual(ask(lw, 0x82, 0) & 0x08, 0x08)       # engine not ready
            lw.command("ok")
        self.assertEqual(self.reg(lw, 0x02), 0x00)
        # The manual slot chosen and empty is the same, the cassette full or not.
        ask(lw, 0x83, 0x4F)
        self.assertEqual(self.reg(lw, 0x02), 0x10)
        self.assertEqual(self.reg(lw, 0x01) & 0x02, 0x02)
        ask(lw, 0x83, 0x4C)
        self.assertEqual(self.reg(lw, 0x02), 0x00)
        self.assertEqual(self.reg(lw, 0x01) & 0x02, 0x00)
        lw.command("manual")
        self.assertEqual(self.reg(lw, 0x2A), 0x60)
        lw.command("jam")
        self.assertEqual(self.reg(lw, 0x02) & 0x08, 0x08)
        lw.command("ok")
        lw.command("cover")
        self.assertEqual(self.reg(lw, 0x02), 0x04)
        lw.command("ok")
        lw.command("engine")
        self.assertEqual(self.reg(lw, 0x01) & 0x02, 0x02)
        lw.command("ok")
        self.assertEqual(self.reg(lw, 0x01), 0x00)
        self.assertEqual(self.reg(lw, 0x02), 0x00)
        lw.command("silent")
        self.assertIsNone(ask(lw, 0x83, 0x3E))
        self.assertIsNone(ask(lw, 0x82, 0x00))
        lw.command("ok")
        self.assertEqual(ask(lw, 0x83, 0x3E), 0x29)

    def test_jam_needs_apples_recovery(self):
        lw, clock = printer()
        send(lw, LETTER + [row(r) for r in ROWS] + END + PRINT)
        clock.t += 0.5
        lw.command("jam")
        clock.t += 3
        lw.tick()
        self.assertEqual(lw.pages, [])                            # stopped
        self.assertEqual(ask(lw, 0x83, 0x01) & 0x10, 0x10)        # send the page again
        lw.command("ok")
        send(lw, [cmd(0x81, 0), cmd(0x8A, 1)])                    # not yet: no 83 5D
        clock.t += 3
        lw.tick()
        self.assertEqual(lw.pages, [])
        send(lw, [cmd(0x81, 0), cmd(0x84, 8), cmd(0x84, 1), cmd(0x83, 0x40), cmd(0x83, 0x3E),
                  cmd(0x82, 0), cmd(0x83, 0x5D), cmd(0x83, 0x4C), cmd(0x81, 0), cmd(0x8A, 1)])
        clock.t += 2.1
        lw.tick()
        self.assertEqual(len(lw.pages), 1)
        self.assertEqual(lw.pages[0].rows, WANT)

    def test_paper_out_holds_the_page(self):
        lw, clock = printer()
        send(lw, LETTER + [row(r) for r in ROWS] + END)
        lw.command("paper")
        send(lw, PRINT)
        clock.t += 3
        lw.tick()
        self.assertEqual(lw.pages, [])
        lw.command("ok")
        send(lw, PRINT)
        clock.t += 2.1
        lw.tick()
        self.assertEqual(len(lw.pages), 1)

    def test_manual_slot(self):
        lw, clock = printer()
        setup = [cmd(0x83, 0x4F) if p == cmd(0x83, 0x4C) else p for p in LETTER]
        send(lw, setup + [row(r) for r in ROWS] + END + PRINT)
        clock.t += 3
        lw.tick()
        self.assertEqual(lw.pages, [])                            # the slot is empty
        lw.command("manual")
        send(lw, PRINT)
        clock.t += 2.1
        lw.tick()
        self.assertEqual(len(lw.pages), 1)
        self.assertEqual(ask(lw, 0x83, 0x2A) & 0x40, 0)           # the sheet went

    def test_badrow(self):
        lw, clock = printer()
        lw.command("badrow 2")
        send(lw, LETTER + [row(r) for r in ROWS] + END)
        self.assertEqual(ask(lw, 0x8E, 0x00) & 2, 2)
        send(lw, PRINT)
        clock.t += 3
        lw.tick()
        self.assertEqual(lw.pages, [])


class Decoders(unittest.TestCase):
    def test_bits_and_frames_decode_alike(self):
        packets = LETTER + [row(r) for r in ROWS] + END + PRINT + [cmd(0x8E, 0)]
        self.assertEqual(qdfake.packets_from_bits(line(packets)), packets)
        self.assertEqual(qdfake.packets_from_frames(frames(packets)), packets)
        with tempfile.TemporaryDirectory() as t:
            t = pathlib.Path(t)
            (t / "s.bin").write_bytes(frames(packets))
            (t / "b.bin").write_bytes(line(packets))
            self.assertEqual(qdfake.main(["decode", str(t / "s.bin"), "--out", str(t / "a"), "--no-png"]), 0)
            self.assertEqual(qdfake.main(["decode-bits", str(t / "b.bin"), "--out", str(t / "b"), "--no-png"]), 0)
            a, b = (t / "a" / "page-1.pbm").read_bytes(), (t / "b" / "page-1.pbm").read_bytes()
            self.assertEqual(a, b)
            self.assertEqual(a, b"P4\n2400 3\n" + b"".join(WANT))

    def test_a_short_low_run_is_no_break(self):
        # Zero bytes on the line: 9 zero bits at most between stop bits.
        packets = [cmd(0x81, 0), row(bytes([0x00]))]
        self.assertEqual(qdfake.packets_from_bits(line(packets)), packets)


if __name__ == "__main__":
    unittest.main()
