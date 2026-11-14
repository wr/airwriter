#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""qdfake.py's printer held to the design's command table (W-1102) and to what board 1's
printer did with a second page loaded while the first printed (W-1144).

Packets here are built by hand from the design ("What the printer and the board allow"),
not by our C code, and the line image by a second writer in this file, so the fake is an
independent reader of both.

    python3.14 -m unittest tools/qdlaser/test_qdfake.py
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

LIT = bytes([62] + [0x55] * 62) * 4 + bytes([52] + [0x55] * 52)   # a row of 305 bytes of codes

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
        lit = LIT
        send(lw, LETTER + [row(lit)] * 3)
        self.assertEqual(ask(lw, 0x8E, 0x00) & 1, 0)
        self.assertEqual(ask(lw, 0x82, 0x00) & 1, 0)
        send(lw, [row(lit)])                                      # 1220 > 1000
        self.assertEqual(ask(lw, 0x8E, 0x00) & 1, 1)
        self.assertEqual(ask(lw, 0x82, 0x00) & 1, 0)              # 82 bit 0 never said so (A2b)

    def test_past_half(self):
        lw, _ = printer()
        send(lw, LETTER + [row(LIT)] * 859)                       # 261,995 bytes
        self.assertEqual(ask(lw, 0x82, 0x00) & 2, 0)
        send(lw, [row(LIT)])                                      # 262,300
        self.assertEqual(ask(lw, 0x82, 0x00) & 3, 2)


def big(kb):
    """A page's rows of about kb thousand bytes of codes."""
    return [row(LIT)] * (kb * 1000 // len(LIT))


NEXT = [cmd(0x83, 0x4C)]                    # all that goes before the next page's rows


class Overlap(unittest.TestCase):
    """W-1144's bench, board 1's printer, 2026-11-13: runs A1 to A5."""

    def test_the_next_page_loads_while_one_prints(self):
        # A1c: three pages, each loaded while the last printed, no 81 00 between.
        lw, clock = printer()
        second = [BLACK, BLACK, WHITE]
        send(lw, LETTER + [row(r) for r in ROWS] + END + PRINT)
        clock.t += 0.5
        send(lw, NEXT + [row(r) for r in second])                 # while page 1 prints
        send(lw, END + PRINT)                                     # too soon: page 1 still prints
        self.assertIn("8A 01 while a page prints", lw.events)
        clock.t += 1.6
        self.assertEqual(lw.breaks(), b"B")
        self.assertEqual(ask(lw, 0x8E, 0x00), 0xFC)
        self.assertEqual(len(lw.pages), 1)
        send(lw, END + PRINT)
        clock.t += 0.5
        send(lw, NEXT + [row(r) for r in ROWS])                   # the third, behind the second
        clock.t += 1.6
        self.assertEqual(ask(lw, 0x8E, 0x00), 0xFC)
        send(lw, END + PRINT)
        clock.t += 2.1
        self.assertEqual(ask(lw, 0x8E, 0x00), 0xFC)
        self.assertEqual([p.rows for p in lw.pages],
                         [WANT, [WANT[0], WANT[0], WANT[1]], WANT])
        self.assertEqual(lw.blanks, [])

    def test_a_pages_room_is_free_at_its_done(self):
        # A2c: three 221 KB pages, the third loaded while the second printed, never filled it;
        # past half 41 KB into the third.
        lw, clock = printer()
        send(lw, LETTER + big(221) + END + PRINT)
        send(lw, NEXT + big(221))
        self.assertEqual(ask(lw, 0x82, 0x00) & 2, 2)              # 442 KB held
        clock.t += 2.1
        self.assertEqual(ask(lw, 0x8E, 0x00), 0xFC)               # page 1 done: its room is free
        self.assertEqual(ask(lw, 0x82, 0x00) & 2, 0)
        send(lw, END + PRINT + NEXT + big(40))
        self.assertEqual(ask(lw, 0x82, 0x00) & 2, 0)
        send(lw, big(2))
        self.assertEqual(ask(lw, 0x82, 0x00) & 2, 2)
        send(lw, big(179))
        self.assertEqual(ask(lw, 0x8E, 0x00) & 1, 0)
        clock.t += 2.1
        lw.tick()
        send(lw, END + PRINT)
        clock.t += 2.1
        lw.tick()
        self.assertEqual(len(lw.pages), 3)
        self.assertEqual(lw.blanks, [])

    def test_one_big_page(self):
        # A2a: a 364 KB page alone: past half while it is in, nothing waiting once it is done.
        lw, clock = printer()
        send(lw, LETTER + big(364) + END + PRINT)
        self.assertEqual(ask(lw, 0x82, 0x00) & 6, 2)
        clock.t += 2.1
        self.assertEqual(ask(lw, 0x8E, 0x00), 0xFC)
        self.assertEqual(ask(lw, 0x82, 0x00) & 6, 4)

    def test_memory_full_while_a_page_prints_blanks_both(self):
        # A2b: two 364 KB pages. Full 158 KB into the second: 8E bit 0 and a BREAK, 82 bit 0
        # never, page 1 done early, and both sheets blank.
        lw, clock = printer()
        send(lw, LETTER + big(364) + END + PRINT)
        clock.t += 0.5
        send(lw, NEXT + big(140))
        self.assertEqual(ask(lw, 0x8E, 0x00) & 1, 0)
        self.assertEqual(lw.breaks(), b"")
        send(lw, big(20))                                         # 524 KB in all
        self.assertEqual(lw.breaks(), b"B")
        self.assertEqual(ask(lw, 0x8E, 0x00), 0xF9)
        self.assertEqual(ask(lw, 0x82, 0x00) & 1, 0)
        send(lw, big(204))                                        # the rows go on, lost
        clock.t += 0.3                                            # done early, not at 2 s
        self.assertEqual(lw.breaks(), b"B")
        self.assertEqual(ask(lw, 0x8E, 0x00), 0xFD)
        send(lw, END + PRINT)
        clock.t += 2.1
        self.assertEqual(ask(lw, 0x8E, 0x00) & 4, 4)              # it says done all the same
        self.assertEqual(lw.pages, [])
        self.assertEqual(len(lw.blanks), 2)

    def test_81_00_while_a_page_prints_blanks_it(self):
        # A3: 82 says empty at once, no page done ever comes, and the sheet is blank.
        lw, clock = printer(exit_s=2.0)
        send(lw, LETTER + [row(r) for r in ROWS] + END + PRINT)
        clock.t += 0.6
        send(lw, [cmd(0x81, 0)])
        self.assertEqual(ask(lw, 0x82, 0x00) & 4, 4)
        self.assertEqual(ask(lw, 0x83, 0x01) & 0x20, 0x20)        # the sheet still goes out
        clock.t += 10
        self.assertEqual(lw.breaks(), b"")
        self.assertEqual(ask(lw, 0x8E, 0x00), 0xF8)
        self.assertEqual(ask(lw, 0x83, 0x01) & 0x20, 0)
        self.assertEqual(lw.pages, [])
        self.assertEqual(len(lw.blanks), 1)

    def test_offsets_changed_while_a_page_prints_move_it(self):
        # A5: page 2's offsets sent 3 s into page 1's print: both sheets moved.
        lw, clock = printer()
        send(lw, LETTER + [row(r) for r in ROWS] + END + PRINT)
        clock.t += 0.4
        send(lw, [cmd(0x87, 0xE5), cmd(0x85, 0x80), cmd(0x86, 0xFF)] + NEXT + [row(r) for r in ROWS])
        clock.t += 1.7
        lw.tick()
        send(lw, END + PRINT)
        clock.t += 2.1
        lw.tick()
        self.assertEqual([(p.top, p.left) for p in lw.pages], [(128, 27), (128, 27)])
        self.assertTrue(any(e.startswith("moved") for e in lw.events))

    def test_the_sheet_is_on_its_way_out_after_done(self):
        lw, clock = printer(exit_s=1.5)
        send(lw, LETTER + [row(r) for r in ROWS] + END + PRINT)
        clock.t += 2.1
        self.assertEqual(ask(lw, 0x8E, 0x00), 0xFC)
        self.assertEqual(ask(lw, 0x83, 0x01) & 0x20, 0x20)
        clock.t += 1.6
        self.assertEqual(ask(lw, 0x83, 0x01) & 0x20, 0)

    RECOVERY = [cmd(0x81, 0), cmd(0x84, 8), cmd(0x84, 1), cmd(0x83, 0x40), cmd(0x83, 0x3E),
                cmd(0x82, 0), cmd(0x83, 0x5D), cmd(0x83, 0x4C), cmd(0x81, 0), cmd(0x8A, 1)]

    def test_the_cover_mid_page_and_apples_recovery(self):
        # A4c: the cover opened as page 1 printed, page 2 loaded behind it. The recovery
        # printed page 1 again, and page 2's own print sequence then printed page 2.
        lw, clock = printer(warm_s=3.0)
        second = [WHITE, BLACK, BLACK]
        send(lw, LETTER + [row(r) for r in ROWS] + END + PRINT + NEXT + [row(r) for r in second])
        clock.t += 0.7
        lw.command("cover")
        self.assertEqual(ask(lw, 0x83, 0x01), 0x13)
        self.assertEqual(ask(lw, 0x83, 0x02), 0x0D)               # a jam and the cover
        send(lw, [cmd(0x8A, 0)])
        clock.t += 30
        lw.command("ok")
        self.assertIsNone(ask(lw, 0x83, 0x02))                    # warming: no answer
        self.assertEqual(ask(lw, 0x82, 0x00) & 0xF0, 0xA0)        # 82 still answers
        clock.t += 3.1
        self.assertEqual(ask(lw, 0x83, 0x02) & 0x0C, 0)
        send(lw, self.RECOVERY[:3])
        clock.t += 3.1                                            # 84 08: no answers for a while
        send(lw, self.RECOVERY[3:])
        self.assertEqual(ask(lw, 0x82, 0x00) & 4, 4)              # "empty", but nothing is gone
        clock.t += 2.1
        self.assertEqual(ask(lw, 0x8E, 0x00), 0xFC)
        send(lw, END + PRINT)                                     # page 2's own sequence
        clock.t += 2.1
        self.assertEqual(ask(lw, 0x8E, 0x00), 0xFC)
        self.assertEqual([p.rows for p in lw.pages], [WANT, [WANT[1], WANT[0], WANT[0]]])
        self.assertEqual(lw.blanks, [])
        self.assertFalse(any(e.startswith("wrong page") for e in lw.events))

    def test_the_recovery_prints_the_first_page_in_memory(self):
        # The page a jam stopped is the second since the last 81 00: the recovery's 81 00
        # goes back to the first, which is not the page wanted.
        lw, clock = printer()
        second = [WHITE, BLACK, BLACK]
        send(lw, LETTER + [row(r) for r in ROWS] + END + PRINT)
        clock.t += 2.1
        lw.tick()
        send(lw, NEXT + [row(r) for r in second] + END + PRINT)
        clock.t += 0.5
        lw.command("jam")
        send(lw, [cmd(0x8A, 0)])
        lw.command("ok")
        send(lw, self.RECOVERY)
        clock.t += 2.1
        lw.tick()
        self.assertEqual([p.rows for p in lw.pages], [WANT, WANT])
        self.assertTrue(any(e.startswith("wrong page") for e in lw.events))

    def test_a_second_sheet_in_the_path_jams_again(self):
        # A4d: the cover opened as page 2 printed, page 1's sheet still on its way out. After
        # it closed, 83 02 read clear once, then jam again until the second sheet was out.
        lw, clock = printer(exit_s=2.0)
        send(lw, LETTER + [row(r) for r in ROWS] + END + PRINT + NEXT + [row(r) for r in ROWS])
        clock.t += 2.1
        lw.tick()
        send(lw, END + PRINT)
        clock.t += 0.8
        lw.command("cover")
        self.assertEqual(ask(lw, 0x83, 0x02), 0x0D)
        send(lw, [cmd(0x8A, 0)])
        lw.command("ok")
        self.assertEqual(ask(lw, 0x83, 0x02) & 0x0C, 0)           # clear, once
        for _ in range(3):
            self.assertEqual(ask(lw, 0x83, 0x02) & 0x0C, 0x08)    # and a jam again
            clock.t += 300
        lw.command("clear")                                       # every sheet out
        for _ in range(3):
            self.assertEqual(ask(lw, 0x83, 0x02) & 0x0C, 0)

    def test_one_sheet_stays_clear(self):
        lw, clock = printer()
        send(lw, LETTER + [row(r) for r in ROWS] + END + PRINT)
        clock.t += 0.5
        lw.command("jam")
        self.assertEqual(ask(lw, 0x83, 0x02) & 0x0C, 0x08)
        lw.command("ok")
        for _ in range(3):
            self.assertEqual(ask(lw, 0x83, 0x02) & 0x0C, 0)

    def test_8a_01_during_a_jam_is_kept(self):
        # A4e: the printer kept an 8A 01 sent while it said jam, and fed by itself when the
        # jam was cleared; 81 00 and 84 08 sent into that feed jammed it again.
        lw, clock = printer()
        send(lw, LETTER + [row(r) for r in ROWS] + END + PRINT)
        clock.t += 0.5
        lw.command("jam")
        ask(lw, 0x83, 0x02)
        send(lw, [cmd(0x8A, 0), cmd(0x83, 0x5D), cmd(0x8A, 1)])
        self.assertTrue(any(e.startswith("8A 01 while") for e in lw.events))
        clock.t += 5
        lw.tick()
        self.assertEqual(lw.pages, [])
        lw.command("ok")
        self.assertEqual(ask(lw, 0x83, 0x01) & 0x20, 0x20)        # feeding, by itself
        self.assertTrue(any(e.startswith("fed by itself") for e in lw.events))
        send(lw, [cmd(0x81, 0), cmd(0x84, 8)])
        self.assertEqual(ask(lw, 0x83, 0x02) & 0x08, 0x08)
        self.assertTrue(any(e.startswith("jammed again") for e in lw.events))
        clock.t += 5
        lw.tick()
        self.assertEqual(lw.pages, [])

    def test_84_08_is_followed_by_silence(self):
        lw, clock = printer(warm_s=3.0)
        send(lw, [cmd(0x84, 8)])
        clock.t += 0.3
        send(lw, [cmd(0x84, 1)])
        self.assertIsNone(ask(lw, 0x83, 0x40))
        self.assertEqual(ask(lw, 0x82, 0x00) & 0xF0, 0xA0)
        clock.t += 2.8
        self.assertEqual(ask(lw, 0x83, 0x40), 0x01)

    def test_nothing_but_questions_once_a_jam_is_told(self):
        # Rows, 81 00 and 84 08 after an 83 02 has said jam: logged, and the page loading
        # never prints. Before the host has been told, rows are taken.
        lw, clock = printer()
        send(lw, LETTER + [row(r) for r in ROWS] + END + PRINT + NEXT + [row(ROWS[0])])
        clock.t += 0.5
        lw.command("jam")
        send(lw, [row(ROWS[1])])                                  # not told yet
        self.assertFalse(any(e.startswith("rows while") for e in lw.events))
        self.assertEqual(ask(lw, 0x83, 0x02) & 0x08, 0x08)
        send(lw, [cmd(0x8A, 0), cmd(0x84, 4)])                    # the stop and the lights: fine
        self.assertFalse(any(" while a jam" in e for e in lw.events))
        send(lw, [row(ROWS[2]), cmd(0x81, 0), cmd(0x84, 8)])
        for start in ("rows while", "81 00 while", "84 08 while"):
            self.assertTrue(any(e.startswith(start) for e in lw.events), start)
        self.assertEqual(ask(lw, 0x8E, 0x00) & 2, 2)
        lw.command("ok")
        ask(lw, 0x83, 0x02)
        send(lw, [row(ROWS[2])])                                  # told clear: rows again
        self.assertEqual(sum(e.startswith("rows while") for e in lw.events), 1)

    def test_rows_that_start_the_memory_over_blank_the_page_printing(self):
        lw, clock = printer()
        send(lw, LETTER + [row(r) for r in ROWS] + END + PRINT)
        clock.t += 2.1
        lw.tick()
        send(lw, NEXT + [row(r) for r in ROWS])
        send(lw, [cmd(0x81, 0), cmd(0x8A, 1)])                    # page 1 again, from the start
        clock.t += 0.5
        send(lw, [row(r) for r in ROWS])                          # over it
        clock.t += 2.1
        lw.tick()
        self.assertEqual(len(lw.pages), 1)
        self.assertEqual(len(lw.blanks), 1)


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
