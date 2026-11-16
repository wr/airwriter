#!/usr/bin/env python3.14
# SPDX-License-Identifier: GPL-3.0-or-later
"""The bench: label sheets and each board's password (W-1156; SPEC §3.20).

    bench.py align --printer ipp://<airwriter>.local:631/<rp>
    bench.py sheet --kind full|lc --printer ipp://<airwriter>.local:631/<rp>
    bench.py burn --kind full|lc --port /dev/cu.usbmodemNNNN
    bench.py void SERIAL

A sheet prints whole, through an AirWriter sharing the Personal LaserWriter
300 (its rp is in /api/printers), with a serial and a password on every
label. Then each board, plugged in over USB, gets the next label's serial and
password burned into its chip, which can't be undone, and the tool names the
label to stick on. Everything private lives in --dir, never in a repo:
labels.csv (the book: back it up off this Mac after each sheet), sheet.json
(the sheet's layout, from its maker's spec, and the printer's offset),
fields.json (where the serial and password go on a label, and their font),
and art-full.png and art-lc.png (each label's art at 300 dpi, black and white,
the serial and password cells blank).

Runs on a Python with Pillow (Homebrew's python3.14); `espefuse.py` comes from
ESP-IDF's export.sh, or --espefuse names it.
"""
import argparse, datetime, json, pathlib, re, subprocess, sys, tempfile

HERE = pathlib.Path(__file__).parent
sys.path.insert(0, str(HERE))
import labels  # noqa: E402

DIR = pathlib.Path.home() / ".config/airwriter/labels"


class Refused(Exception):
    pass


def _espefuse(cmd, port, *args):
    r = subprocess.run([cmd, "--chip", "esp32s3", "--port", port, *args], capture_output=True, text=True)
    if r.returncode:
        raise Refused("espefuse failed:\n" + r.stdout + r.stderr)
    return r.stdout


def read_block(cmd, port):
    """BLOCK_USR_DATA's 32 bytes, in the order burn_block_data writes them."""
    with tempfile.TemporaryDirectory() as d:
        _espefuse(cmd, port, "dump", "--format", "split", "--file_name", str(pathlib.Path(d) / "blk.bin"))
        return (pathlib.Path(d) / "blk3.bin").read_bytes()[:32]


def read_mac(cmd, port):
    m = re.search(r"MAC.*?=\s*([0-9a-f]{2}(?::[0-9a-f]{2}){5})", _espefuse(cmd, port, "summary"))
    return m.group(1) if m else "unknown"


def ask():
    return input("Burn it? It can't be undone. [y/N] ").strip().lower() == "y"


def burn(folder, kind, port, cmd="espefuse.py", confirm=ask):
    """Burns the next unused label of `kind` into the board on `port`; returns what to say."""
    book = labels.Book(pathlib.Path(folder) / "labels.csv")
    try:
        got = labels.unpack_block(read_block(cmd, port))
    except ValueError as e:
        raise Refused("%s. Nothing burned." % e)
    if got:
        row = book.by_serial(got[0])
        if row and row.mac in ("", "burning"):
            book.mark_burned(row, read_mac(cmd, port))     # a burn cut short last time
        where = "Label %d: " % row.position if row else ""
        return "%s%s (already burned%s)" % (where, got[0], "" if row else ", not in this book")

    row = book.next_unburned(kind)
    if not row:
        raise Refused("No unused %s labels. Print a sheet first." % kind)
    print("Label %d of sheet %d: %s, password %s" % (row.position, row.sheet, row.serial, row.password))
    if not confirm():
        raise Refused("Nothing burned.")
    block = labels.pack_block(row.serial, row.password)
    # Taken before the burn: if anything after it fails, this label never goes to another board.
    book.mark_burned(row, "burning")
    with tempfile.TemporaryDirectory() as d:
        f = pathlib.Path(d) / "usr.bin"
        f.write_bytes(block)
        _espefuse(cmd, port, "--do-not-confirm", "burn_block_data", "BLOCK_USR_DATA", str(f))
    if read_block(cmd, port) != block:
        raise Refused("The chip reads back something else. Don't stick a label on: check it first.")
    book.mark_burned(row, read_mac(cmd, port))
    return "Label %d: %s" % (row.position, row.serial)


def _load(folder, name):
    p = pathlib.Path(folder) / name
    if not p.exists():
        raise Refused("No %s in %s (see bench.py's help)." % (name, folder))
    return json.loads(p.read_text())


def _send(printer, im):
    import ipp, urf
    g = im.convert("L")
    w, h = g.size
    data = g.tobytes()
    status = ipp.print_job(printer, urf.encode_page([data[y * w:(y + 1) * w] for y in range(h)], w, 300))
    if status > 0xFF:
        raise Refused("The printer refused the sheet (IPP status 0x%04x)." % status)


def sheet(folder, kind, printer):
    from PIL import Image
    import render
    geo, fields = _load(folder, "sheet.json"), _load(folder, "fields.json")
    art = pathlib.Path(folder) / ("art-%s.png" % kind)
    if not art.exists():
        raise Refused("No %s." % art)
    book = labels.Book(pathlib.Path(folder) / "labels.csv")
    rows = book.add_sheet(kind, geo["cols"] * geo["rows"], datetime.date.today())
    try:
        im = render.sheet(geo, rows, {kind: Image.open(art).convert("1")}, fields)
        _send(printer, im)
    except Exception:
        for r in rows:
            book.void(r.serial)                      # the labels never existed: their numbers go
        raise
    return "Sheet %d: %d %s labels, %s to %s. Back up labels.csv now." % (
        rows[0].sheet, len(rows), kind, rows[0].serial, rows[-1].serial)


def align(folder, printer):
    import render
    _send(printer, render.alignment(_load(folder, "sheet.json")))
    return "Hold the page over a label sheet against a window; put any offset in sheet.json's offset_mm."


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--dir", default=str(DIR), help="the private folder (default %(default)s)")
    sub = ap.add_subparsers(dest="cmd", required=True)
    a = sub.add_parser("align")
    a.add_argument("--printer", required=True)
    s = sub.add_parser("sheet")
    s.add_argument("--kind", choices=labels.KINDS, required=True)
    s.add_argument("--printer", required=True)
    b = sub.add_parser("burn")
    b.add_argument("--kind", choices=labels.KINDS, required=True)
    b.add_argument("--port", required=True)
    b.add_argument("--espefuse", default="espefuse.py")
    v = sub.add_parser("void")
    v.add_argument("serial")
    a = ap.parse_args()
    try:
        if a.cmd == "align":
            print(align(a.dir, a.printer))
        elif a.cmd == "sheet":
            print(sheet(a.dir, a.kind, a.printer))
        elif a.cmd == "burn":
            print(burn(a.dir, a.kind, a.port, a.espefuse))
        else:
            labels.Book(pathlib.Path(a.dir) / "labels.csv").void(a.serial)
            print("%s is void; its number won't be used again." % a.serial)
    except (Refused, KeyError, OSError) as e:
        print(e, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
