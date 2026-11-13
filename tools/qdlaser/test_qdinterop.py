#!/usr/bin/env python3.14
# SPDX-License-Identifier: GPL-3.0-or-later
"""The firmware's Personal LaserWriter 300 code against a software one, on this computer (W-1102).

tools/rip/build/qdhost is the board's session, page stage and rip built for the host; it
prints over a pseudo-terminal that qdfake.py serves, in qdfake's framing (the clock the
real link runs on is the board's business). The whole conversation (the identity, each
page's setup, rows, the print sequence, the printer's faults, silence, cancel) runs over
a line against a printer that checks every packet.

Scenarios, each its own qdfake and qdhost, side by side:

  * a Letter page: printed, the dots `rip --printer plw300 --out-pbm` makes of it
  * three pages: three printed, loaded one at a time (no row between a page's 8A 01 and
    its page done in qdfake's log)
  * a noisy page over a small budget: it goes in Apple's screen, the dots `rip --dither
    apple` makes; under a budget it cannot meet even so, it fails, the next page prints,
    and qdhost exits 1
  * paper, the cassette, a jam and the cover during a page, then put right: qdhost shows
    the state and the page prints (after a jam, once more from the printer's memory)
  * the manual slot, with a sheet in it and without (qdhost asks; a sheet goes in)
  * the printer switched off mid-page, SIGINT mid-page, a Personal LaserWriter LS, a row
    whose check fails

    make -C tools/rip qdinterop          (builds build/qdhost and build/rip first)
"""
import asyncio
import os
import pathlib
import re
import signal
import subprocess
import sys
import tempfile
import time

import numpy as np

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parents[1]
RIP_DIR = ROOT / "tools" / "rip"
RIP = RIP_DIR / "build" / "rip"
QDHOST = RIP_DIR / "build" / "qdhost"
QDFAKE = HERE / "qdfake.py"
MAKER = RIP_DIR / "test" / "make_300dpi_fixture.py"
TOP = 62                                    # Letter's image area starts 62 rows down
LOGLINE = re.compile(r"^\s*\d+ (.) (.*)$")

failed = checks = 0


def default_sigint():
    signal.signal(signal.SIGINT, signal.SIG_DFL)


def read_pbm(path):
    data = pathlib.Path(path).read_bytes()
    m = re.match(rb"P4\s+(\d+)\s+(\d+)\s", data)
    w, h = int(m.group(1)), int(m.group(2))
    raw = np.frombuffer(data[m.end():], np.uint8).reshape(h, (w + 7) // 8)
    return np.unpackbits(raw, axis=1)[:, :w].astype(bool)


def same_area(mine, theirs):
    """The rip's page against qdfake's image area: rows TOP on, the columns the rip kept.
    What qdfake has past the page must be white. Returns a problem, or None."""
    h, w = theirs.shape
    want = np.zeros((h, w), bool)
    part = mine[TOP:TOP + h, :w]
    want[:part.shape[0], :part.shape[1]] = part
    if want.shape != theirs.shape:
        return f"shape {theirs.shape}, expected {want.shape}"
    bad = int((want != theirs).sum())
    return f"{bad} dots differ" if bad else None


class Fake:
    def __init__(self, proc, path, out, log):
        self.proc, self.path, self.out, self.log = proc, path, out, log
        self.lines = []
        self.task = asyncio.create_task(self._read())

    @classmethod
    async def start(cls, root, name, *extra):
        d = pathlib.Path(root) / name
        d.mkdir(parents=True)
        out, log = d / "pages", d / "fake.log"
        proc = await asyncio.create_subprocess_exec(
            sys.executable, QDFAKE, "serve", "--pty", "--out", out, "--no-png", "--log", log,
            *extra, stdin=asyncio.subprocess.PIPE, stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.STDOUT, preexec_fn=default_sigint,
            env={**os.environ, "PYTHONUNBUFFERED": "1"})
        path = (await asyncio.wait_for(proc.stdout.readline(), 15)).decode().strip()
        return cls(proc, path, out, log)

    async def _read(self):
        while line := await self.proc.stdout.readline():
            self.lines.append(line.decode(errors="replace").rstrip("\n"))

    async def say(self, command):
        self.proc.stdin.write(command.encode() + b"\n")
        await self.proc.stdin.drain()

    async def stop(self):
        if self.proc.returncode is None:
            self.proc.send_signal(signal.SIGINT)
            try:
                await asyncio.wait_for(self.proc.wait(), 5)
            except asyncio.TimeoutError:
                self.proc.kill()
                await self.proc.wait()
        await self.task

    def entries(self):
        try:
            text = self.log.read_text()
        except FileNotFoundError:
            return []
        return [(m.group(1), m.group(2)) for m in map(LOGLINE.match, text.splitlines()) if m]

    def heard(self):
        return [t for a, t in self.entries() if a == ">"]

    def rows_heard(self):
        return sum(1 for t in self.heard() if t[:1] in "01234567")

    def events(self):
        return [t for a, t in self.entries() if a == "!"]

    def pages(self):
        return sorted(self.out.glob("page-*.pbm"), key=lambda p: int(p.stem[5:])) if self.out.exists() else []


class Run:
    def __init__(self, proc):
        self.proc, self.lines = proc, []
        self.t0 = time.monotonic()
        self.elapsed = None
        self.task = asyncio.create_task(self._read())

    @classmethod
    async def start(cls, device, *args):
        proc = await asyncio.create_subprocess_exec(
            QDHOST, "--dev", device, *map(str, args),
            stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.STDOUT,
            preexec_fn=default_sigint)
        return cls(proc)

    async def _read(self):
        while line := await self.proc.stdout.readline():
            self.lines.append(line.decode(errors="replace").rstrip("\n"))

    async def wait(self, timeout=120):
        try:
            code = await asyncio.wait_for(self.proc.wait(), timeout)
        except asyncio.TimeoutError:
            self.proc.kill()
            await self.proc.wait()
            code = "timeout"
        self.elapsed = time.monotonic() - self.t0
        await self.task
        return code

    @property
    def text(self):
        return "\n".join(self.lines)


async def until(pred, timeout, step=0.02):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        if pred():
            return True
        await asyncio.sleep(step)
    return pred()


def sh(cmd):
    r = subprocess.run([str(c) for c in cmd], capture_output=True, text=True)
    if r.returncode:
        raise RuntimeError(f"{' '.join(str(c) for c in cmd)}\n{r.stdout}{r.stderr}")
    return r


def expected(urf, tmp, tag, screen="atkinson"):
    p = pathlib.Path(tmp) / f"exp-{tag}.pbm"
    sh([RIP, "--in", urf, "--printer", "plw300", "--dither", screen, "--out-pbm", p])
    return read_pbm(p)


def tail(run):
    return run.text.strip().replace("\n", " | ")[-160:]


# ---- scenarios: each returns [(ok, what), ...]

async def scenario_letter(root, tmp, page):
    fake = await Fake.start(root, "letter")
    run = await Run.start(fake.path, page)
    code = await run.wait()
    await fake.stop()
    res = [(code == 0, f"a Letter page prints (exit {code}) {tail(run)}"),
           (re.search(r"page 1: atkinson, \d+ bytes", run.text) is not None, "qdhost says the page went in Atkinson")]
    got = fake.pages()
    res.append((len(got) == 1, f"one page printed ({len(got)})"))
    if got:
        diff = same_area(await asyncio.to_thread(expected, page, tmp, "letter"), read_pbm(got[0]))
        res.append((diff is None, f"the printed dots are the rip's ({diff})"))
    return res


async def scenario_three(root, small):
    fake = await Fake.start(root, "three")
    run = await Run.start(fake.path, small, small, small)
    code = await run.wait()
    await fake.stop()
    # No row heard between a page's print and its done.
    printing, overlap = False, 0
    for arrow, text in fake.entries():
        if arrow == "!" and text == "print":
            printing = True
        elif arrow == "!" and text.startswith("done"):
            printing = False
        elif arrow == ">" and printing and text[:1] in "01234567":
            overlap += 1
    return [(code == 0, f"three pages print (exit {code}) {tail(run)}"),
            (len(fake.pages()) == 3, f"three pages printed ({len(fake.pages())})"),
            (overlap == 0, f"one page in the printer at a time ({overlap} rows sent while one printed)")]


async def scenario_apple(root, tmp, noisy):
    fake = await Fake.start(root, "apple")
    # The noisy page codes to about 165 KB in Atkinson and 71 KB in Apple's screen.
    run = await Run.start(fake.path, "--budget", 100000, noisy)
    code = await run.wait()
    await fake.stop()
    res = [(code == 0, f"a noisy page over the budget prints (exit {code}) {tail(run)}"),
           (re.search(r"page 1: apple, \d+ bytes", run.text) is not None, "qdhost says it went in Apple's screen")]
    got = fake.pages()
    res.append((len(got) == 1, f"one page printed ({len(got)})"))
    if got:
        diff = same_area(await asyncio.to_thread(expected, noisy, tmp, "apple", "apple"), read_pbm(got[0]))
        res.append((diff is None, f"its dots are Apple's screen's ({diff})"))
    return res


async def scenario_too_big(root, noisy, blank):
    fake = await Fake.start(root, "too-big")
    # A blank Letter page is 3,175 bytes of codes (a byte a row): 5,000 lets it through.
    run = await Run.start(fake.path, "--budget", 5000, noisy, blank)
    code = await run.wait()
    await fake.stop()
    return [(code == 1, f"a page too big in both screens fails the job (exit {code}) {tail(run)}"),
            ("page 1: didn't fit in the printer's memory" in run.text, "qdhost says why"),
            (len(fake.pages()) == 1, f"the next page still prints ({len(fake.pages())})")]


# The cassette pulled out reads as no paper: board 1's printer answers it as it does an empty one.
STATES = {"paper": "out of paper", "cassette": "out of paper", "jam": "paper jam",
          "cover": "the cover is open"}


async def scenario_fault(root, page, fault):
    fake = await Fake.start(root, f"fault-{fault}")
    run = await Run.start(fake.path, page)
    res = []
    loading = await until(lambda: fake.rows_heard() > 0, 20)
    await fake.say(fault)
    shown = await until(lambda: STATES[fault] in run.text, 20)
    await asyncio.sleep(0.5)
    await fake.say("ok")
    code = await run.wait()
    await fake.stop()
    res += [(loading and shown, f"{fault}: qdhost shows \"{STATES[fault]}\" {tail(run)}"),
            (code == 0, f"{fault}: put right, the page prints (exit {code})"),
            (len(fake.pages()) == 1, f"{fault}: one page printed ({len(fake.pages())})")]
    if fault in ("jam", "cover"):
        ev = fake.events()
        rows_after = 0
        seen_print = False
        for arrow, text in fake.entries():
            seen_print |= arrow == "!" and (text == "print" or text.startswith("8A 01"))
            rows_after += seen_print and arrow == ">" and text[:1] in "01234567"
        res.append(("print" in ev and rows_after == 0,
                    f"{fault}: printed from the printer's memory, no row sent again ({rows_after})"))
    return res


async def scenario_manual(root, page, sheet_first):
    tag = "manual-fed" if sheet_first else "manual-asked"
    fake = await Fake.start(root, tag)
    if sheet_first:
        await fake.say("manual")
    run = await Run.start(fake.path, "--source", "manual", page)
    res = []
    if not sheet_first:
        asked = await until(lambda: "load the manual feed slot" in run.text, 30)
        res.append((asked, f"manual slot empty: qdhost asks for a sheet {tail(run)}"))
        await fake.say("manual")
    code = await run.wait()
    await fake.stop()
    heard = fake.heard()
    # 83 4F before the page; 83 4C only after it, setting the cassette back at the job's end.
    printed = max((i for i, t in enumerate(heard) if t == "8a018b"), default=len(heard))
    res += [(code == 0, f"{tag}: the page prints (exit {code})"),
            ("834fcc" in heard[:printed] and "834ccf" not in heard[:printed],
             f"{tag}: 83 4F chose the slot"),
            ("834ccf" in heard[printed:], f"{tag}: the job's end sets the cassette back"),
            (len(fake.pages()) == 1, f"{tag}: one page printed ({len(fake.pages())})")]
    return res


async def scenario_silent(root, page):
    fake = await Fake.start(root, "silent")
    run = await Run.start(fake.path, page)
    await until(lambda: fake.rows_heard() > 0, 20)
    await fake.say("silent")
    t0 = time.monotonic()
    code = await run.wait(30)
    took = time.monotonic() - t0
    await fake.stop()
    return [(code == 1 and took < 10, f"switched off mid-page: exit 1 within 10 s (exit {code}, {took:.1f} s) {tail(run)}"),
            ("not answering" in run.text or "stopped answering" in run.text, "qdhost says the printer stopped answering")]


async def scenario_cancel(root, page):
    fake = await Fake.start(root, "cancel")
    run = await Run.start(fake.path, page)
    await until(lambda: fake.rows_heard() > 0, 20)
    run.proc.send_signal(signal.SIGINT)
    code = await run.wait(30)
    await fake.stop()
    heard = fake.heard()
    try:
        i = heard.index("8a008a")
        tail_ok = "810081" in heard[i:]
    except ValueError:
        tail_ok = False
    return [(code == 1, f"SIGINT mid-page: exit 1 (exit {code}) {tail(run)}"),
            (tail_ok, "SIGINT: 8A 00 then 81 00 sent"),
            (len(fake.pages()) == 0, f"SIGINT: nothing printed ({len(fake.pages())})")]


async def scenario_ls(root, page):
    fake = await Fake.start(root, "ls", "--engine", "0d")
    run = await Run.start(fake.path, page)
    code = await run.wait(30)
    await fake.stop()
    heard = fake.heard()
    return [(code == 1, f"a Personal LaserWriter LS: exit 1 (exit {code}) {tail(run)}"),
            ("Personal LaserWriter LS" in run.text, "qdhost names it"),
            (heard == ["810081", "8340c3", "833ebd", "820082"], f"nothing after the identity ({heard[:8]})")]


async def scenario_badrow(root, page):
    fake = await Fake.start(root, "badrow")
    await fake.say("badrow 100")
    run = await Run.start(fake.path, page)
    code = await run.wait(30)
    await fake.stop()
    return [(code == 1, f"a row whose check failed: exit 1 (exit {code}) {tail(run)}"),
            (len(fake.pages()) == 0, f"the page did not print ({len(fake.pages())})")]


async def main():
    global failed, checks
    for tool in (RIP, QDHOST):
        if not tool.exists():
            sys.exit(f"qdinterop: build {tool.name} first: make -C tools/rip qdinterop")
    t0 = time.monotonic()
    with tempfile.TemporaryDirectory() as t:
        root = pathlib.Path(t)
        tmp = root / "work"
        tmp.mkdir()
        pages = {}
        for kind, name, extra in (("text", "letter", []), ("text", "small", ["--rows", "700"]),
                                  ("photo", "noisy", ["--rows", "700"]), ("text", "blank", ["--rows", "250"])):
            pages[name] = tmp / f"{name}.urf"
            sh([sys.executable, MAKER, kind, pages[name], *extra])
        scenarios = [
            scenario_letter(root, tmp, pages["letter"]),
            scenario_three(root, pages["small"]),
            scenario_apple(root, tmp, pages["noisy"]),
            scenario_too_big(root, pages["noisy"], pages["blank"]),
            *[scenario_fault(root, pages["letter"], f) for f in STATES],
            scenario_manual(root, pages["small"], True),
            scenario_manual(root, pages["small"], False),
            scenario_silent(root, pages["letter"]),
            scenario_cancel(root, pages["letter"]),
            scenario_ls(root, pages["small"]),
            scenario_badrow(root, pages["small"]),
        ]
        for results in await asyncio.gather(*scenarios):
            for ok, what in results:
                checks += 1
                if ok:
                    print(f"ok  qdinterop {what}")
                else:
                    failed += 1
                    print(f"FAIL qdinterop {what}")
    print(f"{'FAIL' if failed else 'ok '} qdinterop: {checks - failed}/{checks} checks "
          f"({time.monotonic() - t0:.0f} s)")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
