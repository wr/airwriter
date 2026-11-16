# SPDX-License-Identifier: GPL-3.0-or-later
"""The bench tool's book of labels (W-1156).

    python3.14 -m unittest tools/bench/test_labels.py
"""
import datetime, pathlib, random, sys, tempfile, unittest

sys.path.insert(0, str(pathlib.Path(__file__).parent))
import labels  # noqa: E402

DAY = datetime.date(2026, 12, 23)                  # ISO 2026-W52


class Serial(unittest.TestCase):
    def test_format(self):
        self.assertEqual(labels.serial(2026, 52, "full", "2611", 2), "2652AWF2611-002")
        self.assertEqual(labels.serial(2026, 52, "lc", "2611", 2), "2652AWL2611-002")
        self.assertEqual(labels.serial(2027, 3, "full", "2611", 1000), "2703AWF2611-1000")

    def test_week_is_iso(self):
        # 31 Dec 2026 is a Thursday: ISO week 53 of 2026.
        self.assertEqual(labels.year_week(datetime.date(2026, 12, 31)), (2026, 53))
        # 1 Jan 2027 is in the same ISO week, so it keeps 2026's year.
        self.assertEqual(labels.year_week(datetime.date(2027, 1, 1)), (2026, 53))

    def test_count_reads_back(self):
        self.assertEqual(labels.count_of("2652AWF2611-002"), 2)
        self.assertEqual(labels.count_of("2703AWL2611-1000"), 1000)


class Password(unittest.TestCase):
    def test_alphabet_and_length(self):
        rng = random.Random(1)
        for _ in range(200):
            pw = labels.new_password(rng)
            self.assertEqual(len(pw), 8)
            self.assertTrue(set(pw) <= set(labels.ALPHABET), pw)
        self.assertEqual(labels.ALPHABET, "abcdefghjkmnpqrstuvwxyz23456789")


class Block(unittest.TestCase):
    def test_layout_matches_the_firmware(self):
        b = labels.pack_block("2652AWF2611-002", "k7m2x9qp")
        self.assertEqual(len(b), 32)
        self.assertEqual(b[0], 1)
        self.assertEqual(b[1:16], b"2652AWF2611-002")
        self.assertEqual(b[16], 0)
        self.assertEqual(b[17:25], b"k7m2x9qp")
        self.assertEqual(b[25:], bytes(7))

    def test_refuses_what_the_firmware_would(self):
        with self.assertRaises(ValueError):
            labels.pack_block("2652AWF2611-002", "k7m2x9q0")
        with self.assertRaises(ValueError):
            labels.pack_block("12345678901234567", "k7m2x9qp")


class Book(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.path = pathlib.Path(self.tmp.name) / "labels.csv"
        self.book = labels.Book(self.path, rng=random.Random(7))

    def tearDown(self):
        self.tmp.cleanup()

    def test_one_count_across_kinds(self):
        a = self.book.add_sheet("full", 3, DAY)
        b = self.book.add_sheet("lc", 2, DAY)
        self.assertEqual([labels.count_of(r.serial) for r in a + b], [1, 2, 3, 4, 5])
        self.assertEqual([r.sheet for r in a + b], [1, 1, 1, 2, 2])
        self.assertEqual([r.position for r in a], [1, 2, 3])
        self.assertTrue(b[0].serial.startswith("2652AWL2611-"))

    def test_a_void_number_is_never_used_again(self):
        self.book.add_sheet("full", 3, DAY)
        self.book.add_sheet("lc", 2, DAY)
        self.book.void("2652AWL2611-005")
        c = self.book.add_sheet("full", 1, DAY)
        self.assertEqual(c[0].serial, "2652AWF2611-006")

    def test_next_unburned_skips_burned_and_void(self):
        self.book.add_sheet("lc", 3, DAY)
        first = self.book.next_unburned("lc")
        self.assertEqual(first.position, 1)
        self.book.mark_burned(first, "dc:da:0c:01:02:03")
        self.book.void(self.book.next_unburned("lc").serial)
        self.assertEqual(self.book.next_unburned("lc").position, 3)
        self.assertIsNone(self.book.next_unburned("full"))

    def test_reloads_the_same(self):
        self.book.add_sheet("full", 2, DAY)
        self.book.mark_burned(self.book.next_unburned("full"), "dc:da:0c:01:02:03")
        again = labels.Book(self.path)
        self.assertEqual(again.rows, self.book.rows)
        self.assertEqual(again.next_unburned("full").position, 2)
        self.assertEqual(self.path.stat().st_mode & 0o777, 0o600)

    def test_find_by_mac(self):
        self.book.add_sheet("full", 2, DAY)
        r = self.book.next_unburned("full")
        self.book.mark_burned(r, "dc:da:0c:01:02:03")
        self.assertEqual(self.book.by_mac("dc:da:0c:01:02:03").serial, r.serial)
        self.assertIsNone(self.book.by_mac("00:00:00:00:00:00"))


if __name__ == "__main__":
    unittest.main()
