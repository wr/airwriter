#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""A StyleWriter's plain question reaches an ImageWriter's UART as nothing (W-1103).

    python3.14 -m unittest tools/stylewriter/test_plain_question.py

The design (docs/superpowers/specs/2026-10-25-stylewriters-design.md, "The port")
sends one `?` at 57600 baud to a port that might hold an ImageWriter, whose UART
runs at 9600. At 9600 that byte is a 17 us low pulse, a long high, then a 35 us low
that ends in the stop bit. A receiver that samples 16 times a bit and checks the
start bit again at its middle (the 8251 family and the ESP32 both do) must call
both pulses false starts and hear nothing.

This is a model of that receiver, not a measurement of an ImageWriter: it fed the
57600 waveform of `?` at 64 phase offsets across one 9600 bit, in four variants of
how a receiver might check (a vote of three samples or one; after a false start,
waiting for a new falling edge or taking any low as a start). It asserts that `?`
yields no byte in any of them, and that the same model receives ordinary 9600
frames, so it is not deaf. For `00 FF FF FF 49` (the full sequence, which this
design sends only to a printer the person named) it prints what arrives and
asserts nothing: that is the cost of a wrong pick.
"""
import collections
import unittest

BIT96 = 1 / 9600
TICK = BIT96 / 16              # the receiver's sample clock: 16 samples a 9600 bit
OFFSETS = 64                   # phases of the sample clock across one 9600 bit


def frame_bits(data):
    """8N1, least significant bit first, frames back to back."""
    bits = []
    for b in data:
        bits += [0] + [(b >> i) & 1 for i in range(8)] + [1]
    return bits


class Line:
    """The wire: idle high, then `data` framed at `baud`, then idle high."""

    def __init__(self, data, baud, lead=4 * BIT96):
        self.bits, self.baud, self.lead = frame_bits(data), baud, lead
        self.length = len(self.bits) / baud

    def level(self, t):
        u = t - self.lead
        if u < 0 or u >= self.length:
            return 1
        return self.bits[int(u * self.baud)]

    def samples(self, offset, tail=4 * BIT96):
        n = int((self.lead + self.length + tail) / TICK)
        return [self.level(offset + k * TICK) for k in range(n)]


def receive(s, vote=True, edge=True):
    """A 9600 8N1 receiver on 16x samples `s`. A falling edge starts a candidate;
    the start bit is checked again at its middle (a vote of the samples either
    side of tick 8, or the one at tick 8); a good start is followed by the data
    bits and the stop bit, each taken at its middle. After a false start the
    receiver waits for a new falling edge (`edge`) or takes any low sample as a
    new start (not `edge`). Returns [(byte, stop bit was high)]."""
    got, i = [], 1
    while i < len(s) - 160:
        if s[i] != 0 or (edge and s[i - 1] != 1):
            i += 1
            continue
        mid = (s[i + 7] + s[i + 8] + s[i + 9] <= 1) if vote else s[i + 8] == 0
        if not mid:
            i += 9 if not edge else 1
            continue
        byte = 0
        for k in range(8):
            c = i + 8 + 16 * (k + 1)
            high = (s[c - 1] + s[c] + s[c + 1] >= 2) if vote else s[c] == 1
            byte |= high << k
        c = i + 8 + 16 * 9
        stop = (s[c - 1] + s[c] + s[c + 1] >= 2) if vote else s[c] == 1
        got.append((byte, bool(stop)))
        i = c + 1
    return got


def offsets():
    # a nanosecond past each grid point, so a sample on an edge sees the new level
    return [k / OFFSETS * BIT96 + 1e-9 for k in range(OFFSETS)]


VARIANTS = [(vote, edge) for vote in (True, False) for edge in (True, False)]


class PlainQuestion(unittest.TestCase):
    def test_question_mark_is_heard_by_nobody(self):
        line = Line(b"?", 57600)
        for vote, edge in VARIANTS:
            heard = {o: receive(line.samples(o), vote, edge) for o in offsets()}
            for o, got in heard.items():
                self.assertEqual(got, [], f"vote={vote} edge={edge} offset={o * 1e6:.1f} us")

    def test_the_model_is_not_deaf(self):
        data = bytes([0x3F, 0x55, 0x00, 0xFF, 0xA5, 0x0D])
        line = Line(data, 9600)
        for vote, edge in VARIANTS:
            for o in offsets():
                got = receive(line.samples(o), vote, edge)
                self.assertEqual(got, [(b, True) for b in data],
                                 f"vote={vote} edge={edge} offset={o * 1e6:.1f} us")

    def test_the_pulses_are_what_the_design_says(self):
        # 17 us low, then high past a 9600 start bit's middle, then 35 us low into the stop bit
        line = Line(b"?", 57600)
        t = [line.lead + k / 57600 for k in range(11)]
        self.assertEqual([line.level(x + 1e-9) for x in t[:10]], frame_bits(b"?"))
        low1 = 1 / 57600
        low2 = 2 / 57600
        self.assertAlmostEqual(low1 * 1e6, 17.4, 1)
        self.assertAlmostEqual(low2 * 1e6, 34.7, 1)
        self.assertLess(low1, BIT96 / 2)
        self.assertLess(low2, BIT96 / 2)

    def test_what_each_sequence_costs_is_reported(self):
        question = Line(b"?", 57600)
        full = Line(bytes([0x00, 0xFF, 0xFF, 0xFF, 0x49]), 57600)
        print("\nplain question, a 9600 8N1 receiver with 16x samples and the start bit checked at its middle,")
        print(f"fed the 57600 waveform at {OFFSETS} phase offsets across one 9600 bit:")
        for vote, edge in VARIANTS:
            name = f"{'vote of 3' if vote else 'one sample'}, {'edge' if edge else 'level'}"
            heard = sum(bool(receive(question.samples(o), vote, edge)) for o in offsets())
            print(f"  [{name}] `?`: a byte at {heard} of {OFFSETS} offsets")
            outcomes = collections.defaultdict(list)
            for k, o in enumerate(offsets()):
                got = receive(full.samples(o), vote, edge)
                key = " ".join(f"{b:02x}{'' if ok else '!'}" for b, ok in got) or "nothing"
                outcomes[key].append(k)
            for key, ks in outcomes.items():
                print(f"  [{name}] `00 FF FF FF 49`: {len(ks)} of {OFFSETS} offsets receive: {key}"
                      f"   (! marks a bad stop bit)")
            self.assertEqual(sum(len(v) for v in outcomes.values()), OFFSETS)


if __name__ == "__main__":
    unittest.main()
