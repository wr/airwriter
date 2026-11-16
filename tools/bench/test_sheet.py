# SPDX-License-Identifier: GPL-3.0-or-later
"""The bench tool's sheet, its trip to the printer, and the burn (W-1156).

    python3.14 -m unittest tools/bench/test_sheet.py

Needs Pillow and numpy (Homebrew's python3.14 has both). The rip check also
needs tools/rip/build/rip (make -C tools/rip) and skips without it.
"""
import datetime, os, pathlib, random, shutil, socket, stat, subprocess, sys, tempfile, time, unittest

HERE = pathlib.Path(__file__).parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(HERE))
import bench, ipp, labels, render, urf  # noqa: E402

RIP = ROOT / "tools/rip/build/rip"
QDFAKE = ROOT / "tools/qdlaser/qdfake.py"
# A made-up sheet: letter, 3 x 8 labels of 2.5 x 1.25 in. Not any maker's.
SHEET = {"page_in": [8.5, 11], "label_in": [2.5, 1.25], "cols": 3, "rows": 8,
         "first_in": [0.375, 0.5], "pitch_in": [2.625, 1.25], "offset_mm": [0, 0]}
FIELDS = {"serial": {"x_mm": 3.0, "y_mm": 14.0, "pt": 7.4},
          "password": {"x_mm": 26.0, "y_mm": 14.0, "pt": 7.4}}


def stand_in_art():
    """White with a one-dot border, the label's size at 300 dpi."""
    from PIL import Image, ImageDraw
    im = Image.new("1", (750, 375), 1)
    ImageDraw.Draw(im).rectangle([0, 0, 749, 374], outline=0)
    return im


def font():
    for f in ("/System/Library/Fonts/Supplemental/Arial.ttf", "/Library/Fonts/Arial.ttf",
              "/System/Library/Fonts/Helvetica.ttc"):
        if os.path.exists(f):
            return f
    raise unittest.SkipTest("no font to draw the serial with")


def rows_of(im):
    """A mode "1" page as 8-bit gray rows (0 ink, 255 paper)."""
    g = im.convert("L")
    w, h = g.size
    data = g.tobytes()
    return [data[y * w:(y + 1) * w] for y in range(h)]


class Sheet(unittest.TestCase):
    def setUp(self):
        self.rows = [labels.Row(1, 1, "full", "2652AWF2611-001", "k7m2x9qp", "2026-12-23", "", ""),
                     labels.Row(1, 5, "full", "2652AWF2611-002", "a2b3c4d5", "2026-12-23", "", "")]

    def page(self):
        f = {k: dict(v, font=font()) for k, v in FIELDS.items()}
        return render.sheet(SHEET, self.rows, {"full": stand_in_art()}, f)

    def test_page_is_black_and_white_at_300_dpi(self):
        im = self.page()
        self.assertEqual(im.mode, "1")
        self.assertEqual(im.size, (2550, 3300))

    def test_labels_land_where_the_sheet_says(self):
        im = self.page()
        # Label 5 is column 2, row 2: its top-left corner, the art's border.
        x = round((0.375 + 2.625) * 300)
        y = round((0.5 + 1.25) * 300)
        self.assertFalse(im.getpixel((x, y)))                 # ink
        self.assertTrue(im.getpixel((x - 2, y - 2)))          # paper
        # An empty position stays paper.
        self.assertTrue(im.getpixel((round(0.375 * 300) + 5, round((0.5 + 1.25 * 3) * 300) + 5)))

    def test_text_is_drawn_without_gray(self):
        im = self.page().convert("L")
        self.assertEqual(set(im.tobytes()) - {0, 255}, set())

    def test_urf_round_trips_through_the_rip_to_the_printer(self):
        if not RIP.exists():
            self.skipTest("no tools/rip/build/rip: make -C tools/rip")
        import numpy as np
        im = self.page()
        with tempfile.TemporaryDirectory() as d:
            d = pathlib.Path(d)
            (d / "sheet.urf").write_bytes(urf.encode_page(rows_of(im), im.size[0], 300))
            subprocess.run([RIP, "--in", d / "sheet.urf", "--printer", "plw300", "--out-qd", d / "s.qd",
                            "--out-pbm", d / "p.pbm"], check=True, capture_output=True)
            subprocess.run([sys.executable, QDFAKE, "decode", d / "s.qd", "--out", d / "a", "--no-png"],
                           check=True, capture_output=True)
            sys.path.insert(0, str(ROOT / "tools/rip/test"))
            from qdcheck import read_pbm, SHEETS
            ink = ~np.array(im, dtype=bool)                  # mode "1": 1 is paper
            rip = read_pbm(d / "p.pbm")
            # The printer's 8 in are centred on the 8.5 in page: 75 dots off each side.
            x0 = (ink.shape[1] - rip.shape[1]) // 2
            self.assertEqual(x0, 75)
            self.assertEqual(int((rip != ink[:rip.shape[0], x0:x0 + rip.shape[1]]).sum()), 0,
                             "the rip's dots differ from the sheet's")
            a = read_pbm(d / "a" / "page-1.pbm")
            h, w = a.shape
            top = (SHEETS.get(h, h) - h) // 2
            want = ink[top:top + h, x0:x0 + w]
            self.assertEqual(int((a[:want.shape[0], :want.shape[1]] != want).sum()), 0,
                             "the printer's dots differ from the sheet's")
            self.assertTrue(a.any())


class Printing(unittest.TestCase):
    def test_print_job_delivers_the_urf_unchanged(self):
        with socket.socket() as s:
            if s.connect_ex(("127.0.0.1", 6631)) == 0:
                self.skipTest("something already listens on ippcap's port 6631")
        doc = urf.encode_page([bytes([255]) * 16, bytes([0]) * 16], 16, 300)
        with tempfile.TemporaryDirectory() as d:
            p = subprocess.Popen([sys.executable, ROOT / "tools/ippcap/ippcap.py", d],
                                 stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            try:
                for _ in range(50):
                    with socket.socket() as s:
                        if s.connect_ex(("127.0.0.1", 6631)) == 0:
                            break
                    time.sleep(0.1)
                status = ipp.print_job("ipp://127.0.0.1:6631/ipp/print", doc)
                self.assertEqual(status, 0)
                got = list(pathlib.Path(d).glob("*.urf"))
                self.assertEqual(len(got), 1)
                self.assertEqual(got[0].read_bytes(), doc)
            finally:
                p.terminate()
                p.wait()


STUB = """#!/bin/sh
# A stand-in espefuse: logs its arguments, dumps the block from $STUB_BLOCK.
echo "$@" >> "$STUB_LOG"
case "$*" in
  *"dump --format split --file_name "*)
    f=$(echo "$*" | sed 's/.*--file_name //')
    cp "$STUB_BLOCK" "$(dirname "$f")/$(basename "$f" .bin)3.bin" ;;
  *burn_block_data*)
    f=$(echo "$*" | sed 's/.*BLOCK_USR_DATA //')
    cp "$f" "$STUB_BLOCK" ;;
  *summary*)
    [ -n "$STUB_SUMMARY_FAILS" ] && exit 2
    echo "MAC (BLOCK1) Factory MAC Address = ${STUB_MAC:-dc:da:0c:01:02:03} (OK) R/W" ;;
esac
"""


class Burn(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        d = pathlib.Path(self.tmp.name)
        self.stub = d / "espefuse"
        self.stub.write_text(STUB)
        self.stub.chmod(self.stub.stat().st_mode | stat.S_IEXEC)
        self.block = d / "block.bin"
        self.log = d / "log.txt"
        os.environ.update(STUB_BLOCK=str(self.block), STUB_LOG=str(self.log))
        self.dir = d / "labels"
        self.dir.mkdir()
        self.book = labels.Book(self.dir / "labels.csv", rng=random.Random(3))
        self.book.add_sheet("full", 2, datetime.date(2026, 12, 23))

    def tearDown(self):
        self.tmp.cleanup()

    def burn(self):
        return bench.burn(self.dir, "full", "/dev/null", str(self.stub), confirm=lambda: True)

    def test_a_blank_board_gets_the_next_label(self):
        self.block.write_bytes(bytes(32))
        msg = self.burn()
        self.assertEqual(msg, "Label 1: 2652AWF2611-001")
        self.assertEqual(self.block.read_bytes(), labels.pack_block("2652AWF2611-001",
                                                                    self.book.rows[0].password))
        self.assertEqual(labels.Book(self.dir / "labels.csv").rows[0].mac, "dc:da:0c:01:02:03")
        self.assertEqual(self.log.read_text().count("burn_block_data"), 1)

    def test_a_burned_board_is_left_alone(self):
        self.block.write_bytes(bytes(32))
        self.burn()
        msg = self.burn()
        self.assertEqual(msg, "Label 1: 2652AWF2611-001 (already burned)")
        self.assertEqual(self.log.read_text().count("burn_block_data"), 1)

    def test_a_block_that_isnt_ours_is_left_alone(self):
        self.block.write_bytes(bytes([0x5a]) + bytes(31))
        with self.assertRaises(bench.Refused):
            self.burn()
        self.assertNotIn("burn_block_data", self.log.read_text())

    def test_a_burn_cut_short_never_hands_its_label_to_the_next_board(self):
        # Final review: the block is burned, then the MAC read fails (USB dropped, Ctrl-C).
        self.block.write_bytes(bytes(32))
        os.environ["STUB_SUMMARY_FAILS"] = "1"
        try:
            with self.assertRaises(bench.Refused):
                self.burn()
        finally:
            del os.environ["STUB_SUMMARY_FAILS"]
        # The same board again: it has label 1, and now the book knows its MAC.
        self.assertEqual(self.burn(), "Label 1: 2652AWF2611-001 (already burned)")
        self.assertEqual(labels.Book(self.dir / "labels.csv").rows[0].mac, "dc:da:0c:01:02:03")
        # The next, blank board gets label 2, never label 1's password.
        self.block.write_bytes(bytes(32))
        os.environ["STUB_MAC"] = "dc:da:0c:0a:0b:0c"
        try:
            self.assertEqual(self.burn(), "Label 2: 2652AWF2611-002")
        finally:
            del os.environ["STUB_MAC"]

    def test_no_burn_without_a_yes(self):
        self.block.write_bytes(bytes(32))
        with self.assertRaises(bench.Refused):
            bench.burn(self.dir, "full", "/dev/null", str(self.stub), confirm=lambda: False)
        self.assertNotIn("burn_block_data", self.log.read_text())


if __name__ == "__main__":
    unittest.main()
