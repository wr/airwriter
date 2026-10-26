#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""swfake.py answers, decodes and draws as the StyleWriter spec says (W-1103).

    python3.14 -m unittest tools/stylewriter/test_swfake.py

The expected values are typed here from the design's facts (docs/superpowers/specs/
2026-10-25-stylewriters-design.md, "What the printer and the board allow"), not read
back from swfake. Time is the tests' own: the printer's clock only moves when a test
moves it, so the 2 s settle costs nothing. The pages are small (a sheet 1144 dots
wide) except where a test says so.
"""
import io
import math
import os
import pathlib
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import time
import tty
import unittest

import numpy as np

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import swfake  # noqa: E402

SWFAKE = HERE / "swfake.py"
SHEET = (1144, 700)        # dots: 1000 printable across, 314 / 556 rows (1200 family / CS family)
WIDTH = 1000

IDENT = {"original": b"IJ10\r", "ii": b"SW\r", "1200": b"SW3\r", "1500": b"CS\r",
         "2200": b"CS\r", "2400": b"CS\r", "2500": b"CS\r"}
P = {"2400": 1, "2200": 2, "1500": 4, "2500": 5}
DX = {"original": 0, "ii": 16, "1200": 16, "1500": 16, "2200": 0, "2400": 0, "2500": 0}
COLOUR = ("1500", "2200", "2400", "2500")
SPURIOUS80 = ("2400", "2500")
# B reads this when the buffer is empty; free bytes are B x 256 - 2048 (the original's B x 64)
EMPTY_B = {"original": 0xA0, "ii": 0xF8, "1200": 0xF8, "2400": 0xF8, "2500": 0x80}


class Clock:
    def __init__(self):
        self.now = 1000.0

    def __call__(self):
        return self.now

    def advance(self, s):
        self.now += s


def quiet(*a, **k):
    pass


def mk(model, cart="black", media=SHEET, **kw):
    clock = Clock()
    sw = swfake.StyleWriter(model, cart=cart, media=media, clock=clock, say=quiet, **kw)
    return sw, clock


def q(letter):
    return b"\xff\xff\xff" + letter.encode()


def reset(sw, clock, secs=2.0):
    sw.feed(b"\x00\xff\xff\xffI")
    clock.advance(secs)


def b_of(sw):
    r = sw.feed(q("B"))
    assert len(r) == 1, r
    return r[0]


# ---- a page made by hand, with a coder of its own ----

def code_row(row):
    """One plane row (bytes, already XORed with the row before) as run codes."""
    if not any(row):
        return b"\x80"
    out, i, n = bytearray(), 0, len(row)
    while i < n:
        b, j = row[i], i
        while j < n and row[j] == b and j - i < 62:
            j += 1
        run = j - i
        if b == 0:
            out.append(0x80 | run)
        elif b == 0xFF:
            out.append(0xC0 | run)
        elif run >= 3:
            out += bytes([0x40 | run, b])
        else:                                       # a literal, up to the next run of 3
            j = i
            while j < n and j - i < 62:
                if row[j] in (0, 0xFF) or (j + 2 < n and row[j] == row[j + 1] == row[j + 2]):
                    break
                j += 1
            j = max(j, i + 1)
            out += bytes([j - i]) + bytes(row[i:j])
            run = j - i
        i += run
    return bytes(out)


def band_stream(model, planes, x, y, inks, split=None, delta=True):
    """The wire bytes of one band: its rect and its G blocks. planes: ink -> bool
    array (h, w); inks 'k' (an R rect) or 'cmyk' (a c rect). split: the most data
    bytes a block may carry (blocks end between plane rows)."""
    h, w = planes[inks[0]].shape
    dx = DX[model]
    rect = (b"c" if len(inks) == 4 else b"R") + struct.pack("<HHHH", x + dx, y, x + dx + w - 1, y + h - 1)
    packed = {i: np.packbits(planes[i], axis=1) for i in inks}
    rows = []
    for r in range(h):
        for i in inks:
            row = packed[i][r]
            if delta and r > 0:
                row = row ^ packed[i][r - 1]
            rows.append(code_row(bytes(row)))
    blocks, cur = [], b""
    for c in rows:
        if split and cur and len(cur) + len(c) > split:
            blocks.append(cur)
            cur = b""
        cur += c
    blocks.append(cur)
    return rect + b"".join(b"G" + struct.pack("<H", len(d)) + d + b"\x00" for d in blocks)


def pattern_planes(inks, h, w, seed):
    """Rows chosen to use every code: random, a repeat of the row above (a zero
    delta), solid, blank, one repeated byte, and sparse."""
    rng = np.random.default_rng(seed)
    wb = (w + 7) // 8
    planes = {}
    for n, ink in enumerate(inks):
        rows = []
        for r in range(h):
            kind = (r + n) % 6
            if kind == 0:
                row = rng.integers(0, 256, wb, dtype=np.uint8)
            elif kind == 1 and rows:
                row = rows[-1].copy()
            elif kind == 2:
                row = np.full(wb, 0xFF, np.uint8)
            elif kind == 3:
                row = np.zeros(wb, np.uint8)
            elif kind == 4:
                row = np.full(wb, 0xAA, np.uint8)
            else:
                row = np.zeros(wb, np.uint8)
                row[rng.integers(0, wb, 4)] = rng.integers(1, 256, 4, dtype=np.uint8)
            rows.append(row)
        planes[ink] = np.unpackbits(np.array(rows), axis=1)[:, :w].astype(bool)
    return planes


def code_kinds(coded):
    """Which kinds of code a coded plane row uses, walking it as the format says."""
    kinds, i = set(), 0
    while i < len(coded):
        c = coded[i]
        kinds.add("zero row" if c == 0x80 else ("literal", "repeat", "white", "black")[c >> 6])
        i += 1 + (c & 63 if c >> 6 == 0 else 1 if c >> 6 == 1 else 0) if c != 0x80 else 1
    return kinds


class TestCoder(unittest.TestCase):
    def test_the_hand_made_pages_use_every_code(self):
        planes = pattern_planes("k", 12, WIDTH, 1)
        rows = np.packbits(planes["k"], axis=1)
        kinds = set()
        for r in range(12):
            kinds |= code_kinds(code_row(bytes(rows[r] ^ rows[r - 1] if r else rows[r])))
        self.assertEqual(kinds, {"literal", "repeat", "white", "black", "zero row"})


class Answers(unittest.TestCase):
    def test_the_question_mark(self):
        for model in ("ii", "1200", "1500", "2200", "2400", "2500"):
            sw, _ = mk(model)
            self.assertEqual(sw.feed(b"?"), IDENT[model], model)

    def test_a_byte_at_a_time(self):
        sw, _ = mk("2500")
        self.assertEqual(b"".join(sw.feed(bytes([b])) for b in b"?\xff\xff\xffp"), b"CS\r\x05")

    def test_p_names_the_colour_model(self):
        for model, p in P.items():
            sw, _ = mk(model)
            self.assertEqual(sw.feed(b"?"), b"CS\r")
            self.assertEqual(sw.feed(q("p")), bytes([p]), model)

    def test_p_to_anything_else_ejects_and_resets(self):
        for model in ("original", "ii", "1200"):
            sw, clock = mk(model)
            if model == "original":
                reset(sw, clock)
            self.assertEqual(sw.feed(q("p")), b"", model)
            self.assertEqual(len(sw.bad), 1, model)
            clock.advance(60)
            self.assertEqual(sw.feed(b"?"), b"", f"{model}: nothing until the reset")
            self.assertEqual(sw.feed(b"\x00\xff\xff\xffI"), b"")
            self.assertEqual(sw.feed(b"?"), b"", f"{model}: still settling")
            clock.advance(1.9)
            self.assertEqual(sw.feed(b"?"), b"", f"{model}: 1.9 s is not 2 s")
            clock.advance(0.2)
            self.assertEqual(sw.feed(b"?"), IDENT[model], model)

    def test_the_original_ignores_the_question_until_reset(self):
        sw, clock = mk("original")
        self.assertEqual(sw.feed(b"?"), b"")
        clock.advance(30)
        self.assertEqual(sw.feed(b"?"), b"")
        reset(sw, clock)
        self.assertEqual(sw.feed(b"?"), b"IJ10\r")

    def test_the_reset_takes_two_seconds(self):
        sw, clock = mk("2200")
        sw.feed(b"\x00\xff\xff\xffI")
        self.assertEqual(sw.feed(b"?"), b"")
        clock.advance(1.99)
        self.assertEqual(sw.feed(b"?"), b"")
        clock.advance(0.02)
        self.assertEqual(sw.feed(b"?"), b"CS\r")

    def test_silent_answers_nothing_and_ok_ends_it(self):
        sw, _ = mk("ii")
        sw.command("silent")
        self.assertEqual(sw.feed(b"?"), b"")
        self.assertEqual(sw.feed(q("1")), b"")
        sw.command("ok")
        self.assertEqual(sw.feed(b"?"), b"SW\r")

    def test_paper_reads_04_until_ok(self):
        sw, _ = mk("ii")
        self.assertEqual(sw.feed(q("2")), b"\x00")
        sw.command("paper")
        self.assertEqual(sw.feed(q("2")), b"\x04")
        self.assertEqual(sw.feed(q("1"))[0] & 0x02, 0x02, "status says there is an error")
        sw.command("ok")
        self.assertEqual(sw.feed(q("2")), b"\x00")
        self.assertEqual(sw.feed(q("1")), b"\x00")

    def test_each_fault(self):
        for word, err, status in (("jam", 0x08, 0x02), ("ink", 0x02, 0x02), ("cart", 0x20, 0x02),
                                  ("paper", 0x04, 0x02), ("door", 0x00, 0x40), ("waste", 0x00, 0x80)):
            sw, _ = mk("ii")
            sw.command(word)
            self.assertEqual(sw.feed(q("2")), bytes([err]), word)
            self.assertEqual(sw.feed(q("1")), bytes([status]), word)
            sw.command("ok")
            self.assertEqual((sw.feed(q("1")), sw.feed(q("2"))), (b"\x00", b"\x00"), word)

    def test_the_2400_and_2500_say_80_for_nothing(self):
        for model in ("original", "ii", "1200", "1500", "2200", "2400", "2500"):
            sw, _ = mk(model)
            want = b"\x80" if model in SPURIOUS80 else b"\x00"
            self.assertEqual(sw.feed(q("2")), want, model)
            self.assertEqual(sw.feed(q("1")), b"\x00", f"{model}: 80 in `2` is not a fault")

    def test_busy(self):
        sw, clock = mk("2200")
        sw.command("busy 5")
        self.assertEqual(sw.feed(q("1")), b"\x01")
        clock.advance(4.9)
        self.assertEqual(sw.feed(q("1")), b"\x01")
        clock.advance(0.2)
        self.assertEqual(sw.feed(q("1")), b"\x00")

    def test_the_cartridge(self):
        for model, cart, want in (("2200", "colour", 0x81), ("2500", "black", 0x01),
                                  ("1500", "colour", 0x81), ("1500", "black", 0x01),
                                  ("ii", "black", 0x01)):
            sw, _ = mk(model, cart=cart)
            self.assertEqual(sw.feed(q("D") + q("H")), bytes([want]), f"{model} {cart}")
        sw, _ = mk("2200", cart="colour")
        sw.command("cart")
        self.assertEqual(sw.feed(q("2")), b"\x20")
        sw.command("cart black")
        self.assertEqual(sw.feed(q("D") + q("H")), b"\x01")
        self.assertEqual(sw.feed(q("2")), b"\x00")
        with self.assertRaises(ValueError):
            mk("ii", cart="colour")

    def test_unknown_commands_are_said_not_ignored(self):
        said = []
        sw = swfake.StyleWriter("ii", clock=Clock(), say=said.append)
        sw.command("frobnicate")
        sw.command("")
        self.assertEqual(len(said), 1)
        self.assertIn("frobnicate", said[0])


class Buffer(unittest.TestCase):
    def test_b_reads_empty_at_each_models_value(self):
        for model, b in EMPTY_B.items():
            sw, _ = mk(model)
            self.assertEqual(b_of(sw), b, model)
        sw, _ = mk("2200")
        self.assertEqual(b_of(sw) & 0xA0, 0xA0)
        for cart, b in (("colour", 0x87), ("black", 0x9F)):
            sw, _ = mk("1500", cart=cart)
            self.assertEqual(b_of(sw), b, f"1500 {cart}")

    def test_b_falls_by_the_bytes_received_and_refills_at_the_drain_rate(self):
        for model in ("original", "ii", "2500"):
            planes = pattern_planes("k", 30, 800, 3)
            data = band_stream(model, planes, 0, 0, "k", delta=model != "original")
            sw, clock = mk(model, drain=1000)
            empty = b_of(sw)
            sw.feed(data)
            div = 64 if model == "original" else 256
            self.assertEqual(b_of(sw), empty - math.ceil(len(data) / div), f"{model} {len(data)} bytes")
            clock.advance(1.0)
            self.assertEqual(b_of(sw), empty - math.ceil((len(data) - 1000) / div), f"{model} after 1 s")
            clock.advance(100.0)
            self.assertEqual(b_of(sw), empty, f"{model} drained")

    def test_a_fault_holds_the_buffer_until_s(self):
        planes = pattern_planes("k", 30, 800, 4)
        data = band_stream("ii", planes, 0, 0, "k")
        sw, clock = mk("ii", drain=1000)
        sw.feed(data)
        low = b_of(sw)
        sw.command("paper")
        clock.advance(50)
        self.assertEqual(b_of(sw), low, "paper out: nothing drains")
        sw.command("ok")
        clock.advance(50)
        self.assertEqual(b_of(sw), low, "paper back, but the printer waits for S")
        sw.feed(q("S"))
        clock.advance(50)
        self.assertEqual(b_of(sw), 0xF8)

    def test_overflow_is_a_fault_and_the_command_tolerates_one(self):
        planes = {"k": np.random.default_rng(5).integers(0, 2, (400, 1000)).astype(bool)}
        data = band_stream("2500", planes, 0, 0, "k")
        self.assertGreater(len(data), 30720)
        sw, _ = mk("2500", drain=0)
        sw.feed(data)
        self.assertTrue(any("overflow" in b for b in sw.bad), sw.bad)
        self.assertEqual(sw.feed(b"?"), b"", "ejected and reset: it answers nothing until the reset")

        sw, clock = mk("2500", drain=0)
        sw.command("overflow")
        sw.feed(b"L" + data + b"\x0c")
        self.assertEqual(sw.bad, [], "one overflow is tolerated")
        self.assertEqual(len(sw.pages), 1)
        self.assertTrue((sw.pages[0].bits("k")[:400, :1000] == planes["k"]).all())
        sw.feed(data)
        self.assertTrue(any("overflow" in b for b in sw.bad), "then it faults")


class Pages(unittest.TestCase):
    def check_page(self, model, inks, x, y, h, w, split=None, seed=1):
        planes = pattern_planes(inks, h, w, seed)
        stream = b"L" + band_stream(model, planes, x, y, inks, split, delta=model != "original") + b"\x0c"
        sw, _ = mk(model, drain=float("inf"))
        sw.feed(stream)
        self.assertEqual(sw.bad, [], f"{model}")
        self.assertEqual(len(sw.pages), 1, f"{model}")
        page = sw.pages[0]
        for ink in "cmyk":
            got = page.bits(ink)
            want = np.zeros_like(got)
            if ink in planes:
                want[y:y + h, x:x + w] = planes[ink]
            self.assertTrue((got == want).all(), f"{model} {ink}")
        return sw

    def test_black_page_every_model(self):
        for model in DX:
            self.check_page(model, "k", 0, 0, 41, WIDTH)

    def test_colour_page_the_colour_models(self):
        for model in COLOUR:
            self.check_page(model, "cmyk", 0, 0, 31, WIDTH)

    def test_unaligned_left_edge_and_a_width_that_is_not_whole_bytes(self):
        self.check_page("ii", "k", 3, 5, 20, 701)
        self.check_page("2500", "cmyk", 5, 9, 12, 989)

    def test_blocks_may_be_many_and_the_delta_carries_across_them(self):
        sw = self.check_page("2200", "k", 0, 0, 60, WIDTH, split=300)
        self.assertEqual(sw.bad, [])
        self.check_page("1200", "k", 0, 0, 60, WIDTH, split=250)

    def test_a_rect_without_L_still_makes_a_page_and_two_pages_are_two(self):
        planes = pattern_planes("k", 8, 100, 9)
        one = band_stream("ii", planes, 0, 0, "k") + b"\x0c"
        sw, _ = mk("ii", drain=float("inf"))
        sw.feed(one + b"L" + one)
        self.assertEqual((len(sw.pages), sw.bad), (2, []))

    def test_a_blank_page(self):
        sw, _ = mk("2400")
        sw.feed(b"L\x0c")
        self.assertEqual(len(sw.pages), 1)
        self.assertFalse(sw.pages[0].bits("k").any())

    def test_setup_letters_are_taken_and_remembered(self):
        sw, _ = mk("2200")
        sw.feed(b"m2" + b"nZA" + b"L")
        self.assertEqual(sw.bad, [])
        self.assertEqual(sw.setup, [b"m2", b"n", b"Z", b"A", b"L"])

    def test_the_page_is_done_when_it_has_drained(self):
        planes = pattern_planes("k", 8, 100, 9)
        sw, clock = mk("ii", drain=1000)
        sw.feed(b"L" + band_stream("ii", planes, 0, 0, "k") + b"\x0c")
        self.assertEqual(sw.feed(q("1")), b"\x01", "still printing what it holds")
        clock.advance(10)
        self.assertEqual(sw.feed(q("1")), b"\x08", "page done")
        sw.feed(b"L")
        self.assertEqual(sw.feed(q("1")), b"\x00")

    def test_cleaning_is_counted_and_the_original_has_none(self):
        sw, clock = mk("2200")
        sw.feed(b"P")
        self.assertEqual((sw.cleanings, sw.bad), (1, []))
        self.assertEqual(sw.feed(q("1")), b"\x01", "busy while it cleans")
        clock.advance(60)
        self.assertEqual(sw.feed(q("1")), b"\x00")
        sw, _ = mk("original")
        sw.feed(b"P")
        self.assertEqual((sw.cleanings, len(sw.bad)), (0, 1))

    def test_E_ends_a_page_in_the_middle_of_a_band_without_complaint(self):
        data = band_stream("ii", pattern_planes("k", 30, 100, 2), 0, 0, "k")
        sw, _ = mk("ii")
        # a 30-row rect that has had three white rows, and then the job is cancelled
        sw.feed(b"L" + data[:9] + b"G\x03\x00\x80\x80\x80\x00" + b"E")
        self.assertEqual((sw.bad, sw.pages, len(sw.ejected)), ([], [], 1))
        self.assertEqual(sw.feed(q("2")), b"\x00", "and it answers")


class Bad(unittest.TestCase):
    def wedged(self, sw, clock, why):
        self.assertTrue(sw.bad, why)
        self.assertEqual(sw.feed(b"?"), b"", f"{why}: it answers nothing until the reset")
        reset(sw, clock)
        self.assertEqual(sw.feed(b"?"), IDENT[sw.model], f"{why}: and after the reset it does")

    def test_a_block_longer_than_its_length(self):
        sw, clock = mk("ii")
        # three bytes declared, a fourth where the 00 should be
        sw.feed(b"R" + struct.pack("<HHHH", 16, 0, 16 + 79, 1) + b"G\x03\x00" + b"\x80\x80\x80\x80\x00")
        self.assertTrue(any("length" in b for b in sw.bad), sw.bad)
        self.assertEqual(len(sw.ejected), 1)
        self.wedged(sw, clock, "long block")

    def test_a_code_that_runs_past_its_block(self):
        sw, clock = mk("2200")
        sw.feed(b"R" + struct.pack("<HHHH", 0, 0, 79, 1) + b"G\x02\x00" + b"\x05\xaa\x00")
        self.assertTrue(sw.bad)
        self.wedged(sw, clock, "literal past its block")

    def test_a_rect_past_the_printable_width(self):
        for model in ("ii", "2500"):
            sw, clock = mk(model)
            dx = DX[model]
            sw.feed(b"R" + struct.pack("<HHHH", dx, 0, dx + WIDTH, 1))
            self.assertTrue(any("width" in b for b in sw.bad), (model, sw.bad))
            self.wedged(sw, clock, model)
            sw, _ = mk(model)
            sw.feed(b"R" + struct.pack("<HHHH", dx, 0, dx + WIDTH - 1, 1))
            self.assertEqual(sw.bad, [], f"{model}: the last printable dot is fine")

    def test_a_rect_at_a_wrong_place_or_size(self):
        for what, rect in (("left of the area", (0, 0, 99, 1)),
                           ("backwards", (116, 0, 20, 1)),
                           ("past the page", (16, 0, 99, 1014)),
                           ("taller than a band", (16, 0, 99, 800))):
            sw, _ = mk("ii", media=(1144, 1200))
            sw.feed(b"R" + struct.pack("<HHHH", *rect))
            self.assertTrue(sw.bad, what)
        sw, _ = mk("2200", media=(1144, 1200))
        sw.feed(b"R" + struct.pack("<HHHH", 0, 0, 99, 400))         # 401 rows: past a 400-row band
        self.assertTrue(sw.bad)
        sw, _ = mk("2200", media=(1144, 1200))
        sw.feed(b"R" + struct.pack("<HHHH", 0, 0, 99, 399))
        self.assertEqual(sw.bad, [])

    def test_colour_rect_on_a_black_only_model(self):
        sw, _ = mk("ii")
        sw.feed(b"c" + struct.pack("<HHHH", 16, 0, 99, 1))
        self.assertTrue(sw.bad)

    def test_an_unknown_letter(self):
        for letter in (b"Q", b"\x01", b"="):
            sw, clock = mk("1200")
            sw.feed(letter)
            self.assertTrue(sw.bad, letter)
            self.wedged(sw, clock, f"letter {letter!r}")
        sw, _ = mk("ii")
        sw.feed(q("Q"))
        self.assertTrue(sw.bad, "an unknown query")

    def test_page_end_with_rows_missing_and_data_with_no_rect(self):
        sw, _ = mk("ii")
        sw.feed(b"R" + struct.pack("<HHHH", 16, 0, 16 + 79, 3) + b"G\x01\x00\x80\x00\x0c")
        self.assertTrue(sw.bad, "0C before the rect had its rows")
        sw, _ = mk("ii")
        sw.feed(b"G\x01\x00\x80\x00")
        self.assertTrue(sw.bad, "a block with no rect")
        sw, _ = mk("ii")
        sw.feed(b"R" + struct.pack("<HHHH", 16, 0, 16 + 79, 0) + b"G\x02\x00\x80\x80\x00")
        self.assertTrue(sw.bad, "more rows than the rect")

    def test_bad_run_codes(self):
        for code in (b"\x00", b"\x3f", b"\x80\x3f", b"\xc0", b"\x40"):
            sw, _ = mk("ii")
            sw.feed(b"R" + struct.pack("<HHHH", 16, 0, 16 + 79, 1) + b"G" + bytes([len(code), 0]) + code + b"\x00")
            self.assertTrue(sw.bad, code.hex())
        for code in (b"\xbf", b"\xff", b"\x7f\xaa"):     # a count of 63 where the row has room for it
            sw, _ = mk("ii")
            sw.feed(b"R" + struct.pack("<HHHH", 16, 0, 16 + WIDTH - 1, 1)
                    + b"G" + bytes([len(code), 0]) + code + b"\x00")
            self.assertTrue(sw.bad, code.hex())
        sw, _ = mk("ii")      # 80 is a whole row only on its own
        sw.feed(b"R" + struct.pack("<HHHH", 16, 0, 16 + 79, 0) + b"G\x03\x00\x01\xaa\x80\x00")
        self.assertTrue(sw.bad)
        sw, _ = mk("ii")      # a run that overshoots its row
        sw.feed(b"R" + struct.pack("<HHHH", 16, 0, 16 + 79, 0) + b"G\x01\x00\xcb\x00")
        self.assertTrue(sw.bad)

    def test_a_block_of_8000_stalls_a_cs_printer(self):
        for model in ("2200", "2400", "2500", "1500", "ii", "1200", "original"):
            sw, _ = mk(model)
            sw.feed(b"R" + struct.pack("<HHHH", DX[model], 0, DX[model] + 79, 1) + b"G\x00\x80")
            self.assertEqual(bool(sw.bad), model in COLOUR, model)       # the ones that answer CS

    def test_bad_is_in_the_log(self):
        log = io.StringIO()
        sw, _ = mk("ii", log=log)
        sw.feed(b"Q")
        self.assertRegex(log.getvalue(), r"(?m)^\s*\d+ ! bad")


class Draw(unittest.TestCase):
    def test_ink_lands_inside_the_margins_and_mixes(self):
        sw, _ = mk("2500", media=(544, 400), drain=float("inf"))
        # cyan and yellow over the same dot make green; magenta alone sits elsewhere
        c = np.zeros((4, 8), bool)
        c[1, 2] = True
        m = np.zeros((4, 8), bool)
        m[1, 5] = True
        y = c.copy()
        k = np.zeros((4, 8), bool)
        stream = band_stream("2500", {"c": c, "m": m, "y": y, "k": k}, 3, 2, "cmyk")
        sw.feed(b"L" + stream + b"\x0c")
        self.assertEqual(sw.bad, [])
        px = sw.pages[0].draw()
        self.assertEqual(px.shape, (400, 544, 3))
        ml, mt = 72, 45                                  # the CS family's left and top margins
        paper = px[0, 0].astype(int)
        green = px[mt + 3, ml + 5].astype(int)           # row 2 + 1, column 3 + 2
        pink = px[mt + 3, ml + 8].astype(int)
        self.assertLess(green[0], 60)                    # no red in cyan + yellow
        self.assertGreater(green[1], 120)
        self.assertGreater(pink[0], 180)                 # magenta keeps its red
        self.assertLess(pink[1], 80)
        blank = px[mt + 10, ml + 10].astype(int)
        self.assertLess(abs(blank - paper).max(), 12)    # only the paper's grain

    def test_files(self):
        with tempfile.TemporaryDirectory() as t:
            sw, _ = mk("2500", media=(544, 400), drain=float("inf"), out=t)
            k = np.zeros((5, 16), bool)
            k[2, 3] = k[4, 15] = True
            sw.feed(b"L" + band_stream("2500", {"k": k}, 0, 0, "k") + b"\x0c" + b"L\x0c")
            names = sorted(os.listdir(t))
            self.assertEqual(names, ["page-1-c.pbm", "page-1-k.pbm", "page-1-m.pbm", "page-1-y.pbm",
                                     "page-1.png", "page-2-c.pbm", "page-2-k.pbm", "page-2-m.pbm",
                                     "page-2-y.pbm", "page-2.png"])
            pbm = (pathlib.Path(t) / "page-1-k.pbm").read_bytes()
            w, h = 544 - 144, 400 - 45 - 99
            head = f"P4\n{w} {h}\n".encode()
            self.assertTrue(pbm.startswith(head))
            rows = np.unpackbits(np.frombuffer(pbm[len(head):], np.uint8).reshape(h, -1), axis=1)[:, :w]
            self.assertEqual(sorted(zip(*np.nonzero(rows))), [(2, 3), (4, 15)])
            self.assertEqual(len(pbm), len(head) + h * ((w + 7) // 8))
            from PIL import Image
            with Image.open(pathlib.Path(t) / "page-1.png") as im:
                self.assertEqual(im.size, (544, 400))


class Log(unittest.TestCase):
    def test_both_directions_with_milliseconds(self):
        log = io.StringIO()
        sw, clock = mk("ii", log=log)
        clock.advance(1.25)
        sw.feed(b"?")
        lines = log.getvalue().splitlines()
        self.assertEqual([l.split(None, 1)[1] for l in lines], ["> 3f", "< 53570d"])
        self.assertEqual(lines[0].split()[0], "1250")


class Transports(unittest.TestCase):
    def start(self, *args, stdin=subprocess.PIPE):
        p = subprocess.Popen([sys.executable, "-u", str(SWFAKE), "serve", *args],
                             stdin=stdin, stdout=subprocess.PIPE, text=True)

        def reap():
            p.kill()
            p.wait()
            for f in (p.stdin, p.stdout):
                if f:
                    f.close()
        self.addCleanup(reap)
        return p

    @staticmethod
    def read(fd, n, secs=5):
        import select
        end, got = time.monotonic() + secs, b""
        while len(got) < n and time.monotonic() < end:
            if select.select([fd], [], [], 0.05)[0]:
                got += os.read(fd, n - len(got))
        return got

    def test_pty(self):
        p = self.start("ii", "--pty")
        path = p.stdout.readline().strip()
        self.assertTrue(path.startswith("/dev/"), path)
        fd = os.open(path, os.O_RDWR | os.O_NOCTTY)
        self.addCleanup(os.close, fd)
        tty.setraw(fd)
        os.write(fd, b"?")
        self.assertEqual(self.read(fd, 3), b"SW\r")
        os.write(fd, q("B"))
        self.assertEqual(self.read(fd, 1), b"\xf8")
        p.stdin.write("silent\n")
        p.stdin.flush()
        said = ""
        while "silent" not in said:                      # the banner first, then it owns up
            said = p.stdout.readline()
            self.assertTrue(said, "swfake went away")
        os.write(fd, b"?")
        self.assertEqual(self.read(fd, 3, 0.3), b"")

    def test_serial_device(self):
        # a pty slave stands in for the USB serial adapter: the termios setup must take it
        master, slave = os.openpty()
        self.addCleanup(os.close, master)
        self.addCleanup(os.close, slave)
        p = self.start("2400", "--serial", os.ttyname(slave))
        self.assertIn("2400", p.stdout.readline())       # its banner: the port is open and raw
        os.write(master, b"?")
        self.assertEqual(self.read(master, 3), b"CS\r")
        os.write(master, q("p"))
        self.assertEqual(self.read(master, 1), b"\x01")

    def test_tcp_out_and_listen(self):
        srv = socket.socket()
        srv.bind(("127.0.0.1", 0))
        srv.listen(1)
        self.addCleanup(srv.close)
        self.start("2500", "--tcp", f"127.0.0.1:{srv.getsockname()[1]}")
        srv.settimeout(10)
        conn, _ = srv.accept()
        self.addCleanup(conn.close)
        conn.settimeout(10)
        conn.sendall(b"?")
        self.assertEqual(conn.recv(3), b"CS\r")

        p = self.start("1200", "--listen", "0")
        port = int(p.stdout.readline().split()[-1])
        c = socket.create_connection(("127.0.0.1", port), timeout=10)
        self.addCleanup(c.close)
        c.sendall(b"?")
        self.assertEqual(c.recv(4), b"SW3\r")

    # A swfake that outlives its job is a hung test run for whatever scripts it (the
    # first version waited on a thread blocked reading stdin: 6 s and more after Ctrl-C).
    def test_ctrl_c_ends_it_whatever_stdin_is(self):
        tty_master, tty_slave = os.openpty()
        self.addCleanup(os.close, tty_master)
        self.addCleanup(os.close, tty_slave)
        for what, stdin in (("an open pipe", subprocess.PIPE), ("a tty", tty_slave),
                            ("/dev/null", subprocess.DEVNULL)):
            with self.subTest(stdin=what):
                p = self.start("ii", "--pty", stdin=stdin)
                p.stdout.readline()
                p.stdout.readline()                        # path, then banner: it is serving
                t = time.monotonic()
                p.send_signal(signal.SIGINT)
                p.wait(timeout=2)
                self.assertLess(time.monotonic() - t, 2)

    def test_it_ends_when_its_tcp_peer_closes(self):
        srv = socket.socket()
        srv.bind(("127.0.0.1", 0))
        srv.listen(1)
        self.addCleanup(srv.close)
        p = self.start("ii", "--tcp", f"127.0.0.1:{srv.getsockname()[1]}")
        srv.settimeout(10)
        conn, _ = srv.accept()
        conn.settimeout(10)
        conn.sendall(b"?")
        self.assertEqual(conn.recv(3), b"SW\r")
        conn.close()
        p.wait(timeout=2)

    def test_it_ends_when_the_serial_peer_goes_away(self):
        master, slave = os.openpty()
        self.addCleanup(os.close, slave)
        p = self.start("ii", "--serial", os.ttyname(slave))
        p.stdout.readline()
        os.close(master)                                   # the adapter is unplugged
        p.wait(timeout=2)                                  # not 97% of a core until killed

    def test_a_listener_outlasts_its_peers(self):
        p = self.start("ii", "--listen", "0")
        port = int(p.stdout.readline().split()[-1])
        for _ in range(2):
            c = socket.create_connection(("127.0.0.1", port), timeout=10)
            c.sendall(b"?")
            self.assertEqual(c.recv(3), b"SW\r")
            c.close()
        self.assertIsNone(p.poll())

    def test_decode_a_stream_and_refuse_a_bad_one(self):
        planes = pattern_planes("k", 9, 300, 6)
        stream = b"L" + band_stream("1200", planes, 0, 0, "k") + b"\x0c"
        with tempfile.TemporaryDirectory() as t:
            t = pathlib.Path(t)
            (t / "s.bin").write_bytes(stream)
            r = subprocess.run([sys.executable, str(SWFAKE), "decode", "1200", str(t / "s.bin"),
                                "--out", str(t / "o"), "--no-png"], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertEqual(sorted(os.listdir(t / "o")),
                             [f"page-1-{i}.pbm" for i in "ckmy"])
            (t / "bad.bin").write_bytes(b"Q")
            r = subprocess.run([sys.executable, str(SWFAKE), "decode", "1200", str(t / "bad.bin"),
                                "--out", str(t / "o2"), "--no-png"], capture_output=True, text=True)
            self.assertEqual(r.returncode, 1)
            self.assertIn("bad", r.stderr + r.stdout)


if __name__ == "__main__":
    unittest.main()
