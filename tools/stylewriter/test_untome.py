#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""untome.py's reading of a Tome's packed forks, against streams written by hand (W-1103).

    python3.14 -m unittest tools/stylewriter/test_untome.py

These streams are made here from the format's rules, so they show that the reader does
what its docstring says, not that the rules are Apple's. That is shown by running it on
Apple's installers (the forks that come out parse as resource forks) and by mktables.py's
--compare, which finds every table value equal to another reading of the same drivers.
Neither can be a test here, because Apple's files are never committed.
"""
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import untome  # noqa: E402

HEADER = b"\x00\x01\x00\x00"


class Writer:
    def __init__(self):
        self.bits = ""

    def put(self, bits):
        self.bits += bits
        return self

    def byte(self, v):
        return self.put(format(v, "08b"))

    def packed(self):
        """A chunk: the header, then the bits padded with zeros to a whole byte."""
        bits = self.bits + "0" * (-len(self.bits) % 8)
        return HEADER + bytes(int(bits[i:i + 8], 2) for i in range(0, len(bits), 8))


def reader(bits):
    bits += "0" * (-len(bits) % 8)
    return untome.Bits(bytes(int(bits[i:i + 8], 2) for i in range(0, len(bits), 8)), 0)


RUN_63 = "00" + "1" + "11" + "1111" + "111"     # count 0, then a run of 63
MATCH_10 = "110" + "1" + "01"                    # a count of 8: a match of 10 straight after a match
DIST_1_AT_63 = "0" + "000"                       # the code set for up to 80 bytes of history: 3 bits after the 0


class Codes(unittest.TestCase):
    def test_counts(self):
        for bits, want in (("00", 0), ("01", 1), ("100", 2), ("1010", 3), ("1011", 4),
                           ("1100" + "0", 5), ("1100" + "1", 6), ("1101" + "00", 7), ("1101" + "11", 10),
                           ("1110" + "000", 11), ("1110" + "111", 18),
                           ("1111" + "0" + "000", 19),
                           ("1" * 10 + "0000000000", 1019), ("1" * 10 + "0000000001", 1020)):
            self.assertEqual(untome.count_code(reader(bits)), want, bits)

    def test_runs(self):
        for bits, want in (("0", 1), ("1" + "00", 2), ("1" + "01", 3), ("1" + "10" + "01", 5),
                           ("1" + "11" + "0111", 15), ("1" + "11" + "1000" + "00", 16),
                           ("1" + "11" + "1011" + "11", 31), ("1" + "11" + "1100" + "000", 32),
                           ("1" + "11" + "1111" + "111", 63)):
            self.assertEqual(untome.run_code(reader(bits)), want, bits)

    def test_distances(self):
        # up to 80 bytes of history: 4 bits give 1 to 8, '10' and 5 bits 9 to 40, and a tail from 41
        self.assertEqual(untome.distance_code(63, reader("0" + "000")), 1)
        self.assertEqual(untome.distance_code(63, reader("0" + "111")), 8)
        self.assertEqual(untome.distance_code(63, reader("10" + "00000")), 9)
        self.assertEqual(untome.distance_code(63, reader("11" + "10011")), 60)   # 41 + 19, 5 bits at 63
        self.assertEqual(untome.distance_code(41, reader("11" + "0")), 41)       # 1 bit reaches 42
        self.assertEqual(untome.distance_code(1, reader("0")), 1)                # the first set has no bits for 1

    def test_the_tail_code_set_with_the_odd_step(self):
        # 1000 to 2688 bytes of history: the tail (base 641) takes 10 bits to 1644 and 11 after,
        # where doubling would have put the change at 1664
        self.assertEqual(untome.distance_code(1644, reader("11" + "0000000001")), 642)
        self.assertEqual(untome.distance_code(1645, reader("11" + "00000000001")), 642)
        self.assertEqual(untome.distance_code(1663, reader("11" + "00000000001")), 642)
        self.assertEqual(untome.distance_code(2688, reader("11" + "11111111111")), 641 + 2047)

    def test_the_window_stops_growing(self):
        # past 43008 bytes the last code set goes on: 11 bits and then '10' and 13, tail from 10241
        self.assertEqual(untome.distance_code(500000, reader("0" + "00000000011")), 4)
        self.assertEqual(untome.distance_code(500000, reader("11" + "1" * 15)), 10241 + 32767)


class Forks(unittest.TestCase):
    data = bytes(range(1, 64))

    def literal_63(self):
        w = Writer().put(RUN_63)
        for b in self.data:
            w.byte(b)
        return w

    def test_a_literal_run_and_a_match(self):
        w = self.literal_63().put(MATCH_10).put(DIST_1_AT_63)
        out = untome.unpack_fork(w.packed(), 73)
        self.assertEqual(out, self.data + bytes([63]) * 10)

    def test_the_size_cuts_a_match_short(self):
        w = self.literal_63().put(MATCH_10).put(DIST_1_AT_63)
        self.assertEqual(untome.unpack_fork(w.packed(), 70), self.data + bytes([63]) * 7)

    def test_a_short_run_makes_the_next_match_one_longer(self):
        # a run of 2 (not 63), then count 1: a match of 1 + 3 = 4, not 3
        w = Writer().put("00" + "1" + "00").byte(7).byte(9).put("01").put("0")
        self.assertEqual(untome.unpack_fork(w.packed(), 6), bytes([7, 9, 9, 9, 9, 9]))

    def test_chunks_end_at_the_first_token_boundary_past_the_chunk_size(self):
        # chunks of 64: the first goes 63 literals and a match of 3 to 66, and is padded
        # to a byte; the second starts afresh (a literal run may start) and its match
        # reaches back into the first chunk, to byte 7
        first = self.literal_63().put("01").put(DIST_1_AT_63)
        second = Writer().put("00" + "0").byte(0xAA).put("00").put("11" + "10011")
        packed = first.packed() + second.packed()
        out = untome.unpack_fork(packed, 70, chunk=64)
        want = bytearray(self.data + bytes([63]) * 3 + bytes([0xAA]))
        for _ in range(3):
            want.append(want[-60])
        self.assertEqual(out, bytes(want))
        self.assertEqual(len(out), 70)

    def test_a_chunk_without_its_header_is_refused(self):
        first = self.literal_63().put("01").put(DIST_1_AT_63)
        with self.assertRaises(ValueError):
            untome.unpack_fork(first.packed() + b"\x01\x02\x03\x04\x05", 70, chunk=64)

    def test_a_distance_before_the_start_is_refused(self):
        w = Writer().put("00" + "1" + "00").byte(7).byte(9).put("01").put("11" + "0")   # a distance of 6 at 2 bytes
        with self.assertRaises(ValueError):
            untome.unpack_fork(w.packed(), 8)


if __name__ == "__main__":
    unittest.main()
