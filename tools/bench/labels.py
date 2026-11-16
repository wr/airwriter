# SPDX-License-Identifier: GPL-3.0-or-later
"""Serials, passwords and the book of labels (W-1156; SPEC §3.20).

Each label on a printed sheet gets a serial and a password; the book (a CSV
kept private, never in a repo) records them, and which board each one was
burned into. Serials are one running count across full and LC boards, given
out as a sheet prints; a void label's number is never used again.
"""
import csv, dataclasses, datetime, os, pathlib, re, secrets, tempfile

ALPHABET = "abcdefghjkmnpqrstuvwxyz23456789"       # no 0, o, 1, l, i: as auth_core.h
KINDS = {"full": "F", "lc": "L"}
REV = "2611"
FIELDS = ["sheet", "position", "kind", "serial", "password", "printed", "mac", "void"]


def year_week(day):
    """The ISO year and week, so a serial's four digits always sort by date."""
    y, w, _ = day.isocalendar()
    return y, w


def serial(year, week, kind, rev, count):
    return "%02d%02dAW%s%s-%03d" % (year % 100, week, KINDS[kind], rev, count)


def count_of(s):
    return int(s.rsplit("-", 1)[1])


def new_password(rng=None):
    pick = rng.choice if rng else secrets.choice
    return "".join(pick(ALPHABET) for _ in range(8))


def pack_block(ser, pw):
    """The 32 bytes burned into EFUSE_BLK_USER_DATA, as auth_label_pack lays them out."""
    if not ser or len(ser) > 16 or not re.fullmatch(r"[\x21-\x7e]+", ser):
        raise ValueError("serial %r won't fit the block" % ser)
    if len(pw) != 8 or set(pw) - set(ALPHABET):
        raise ValueError("password %r isn't 8 from the alphabet" % pw)
    b = bytearray(32)
    b[0] = 1
    b[1:1 + len(ser)] = ser.encode()
    b[17:25] = pw.encode()
    return bytes(b)


def unpack_block(b):
    """(serial, password) from a burned block, None for a blank one; ValueError otherwise."""
    if not any(b):
        return None
    if len(b) != 32 or b[0] != 1 or any(b[25:]):
        raise ValueError("the chip's user block isn't AirWriter's")
    ser = b[1:17].rstrip(b"\0").decode("ascii", "replace")
    pw = b[17:25].decode("ascii", "replace")
    pack_block(ser, pw)                              # the same checks the firmware makes
    return ser, pw


@dataclasses.dataclass
class Row:
    sheet: int
    position: int
    kind: str
    serial: str
    password: str
    printed: str
    mac: str
    void: str


class Book:
    def __init__(self, path, rng=None):
        self.path = pathlib.Path(path)
        self.rng = rng
        self.rows = []
        if self.path.exists():
            with self.path.open(newline="") as f:
                for r in csv.DictReader(f):
                    self.rows.append(Row(int(r["sheet"]), int(r["position"]), r["kind"], r["serial"],
                                         r["password"], r["printed"], r["mac"], r["void"]))

    def save(self):
        self.path.parent.mkdir(parents=True, exist_ok=True)
        fd, tmp = tempfile.mkstemp(dir=self.path.parent, prefix=".labels-")
        with os.fdopen(fd, "w", newline="") as f:
            w = csv.DictWriter(f, FIELDS)
            w.writeheader()
            for r in self.rows:
                w.writerow(dataclasses.asdict(r))
        os.chmod(tmp, 0o600)
        os.replace(tmp, self.path)

    def add_sheet(self, kind, n, day):
        if kind not in KINDS:
            raise ValueError("kind is full or lc")
        sheet = max((r.sheet for r in self.rows), default=0) + 1
        count = max((count_of(r.serial) for r in self.rows), default=0)
        y, w = year_week(day)
        new = [Row(sheet, i + 1, kind, serial(y, w, kind, REV, count + i + 1),
                   new_password(self.rng), day.isoformat(), "", "") for i in range(n)]
        self.rows += new
        self.save()
        return new

    def next_unburned(self, kind):
        free = [r for r in self.rows if r.kind == kind and not r.mac and not r.void]
        return min(free, key=lambda r: (r.sheet, r.position), default=None)

    def by_serial(self, s):
        return next((r for r in self.rows if r.serial == s), None)

    def by_mac(self, mac):
        return next((r for r in self.rows if r.mac == mac), None)

    def mark_burned(self, row, mac):
        row.mac = mac
        self.save()

    def void(self, s):
        r = self.by_serial(s)
        if not r:
            raise KeyError(s)
        r.void = datetime.date.today().isoformat()
        self.save()
