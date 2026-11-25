#!/usr/bin/env python3.14
# SPDX-License-Identifier: GPL-3.0-or-later
"""A Mac's StyleWriter driver printing through the board's adapter, on this computer (W-1193).

Three programs, each written apart from the others, meet here for a whole job:

  * etpeer.py's Mac asks NAME:TYPE to print over ADSP, takes the call back and drives the
    printer through it, as Apple's driver does (or in lpstyl's order). It is strict: an
    adapter that numbers a packet wrong or acknowledges the wrong count stops it.
  * tools/rip/build/swahost is the firmware's own C: an EtherTalk link, the adapter on its
    node, the ADSP engine, the pipe and its pump, built as the board runs them.
  * swfake.py is the StyleWriter on a pseudo-terminal: it answers the driver's questions,
    checks every byte and draws the page.

The page is one `rip --printer stylewriter --model M --out-sw` made, and what swfake drew
must be the dots the rip made, compared as swcheck and swinterop compare them. Each case
is a fresh swahost and swfake:

  * a colour page on a 2500 and a gray one on a StyleWriter II, in Apple's order and in
    lpstyl's: the dots, a clean end, no reset from the board, no byte the Mac did not send
  * two StyleWriters offered: each type answers with its own socket, a request for one
    while the other prints is busy, and each prints on its own port
  * busy: the board printing something else (lpstyl's 15 s sleep on the connection); a
    second Mac asking during the first's job, its queue answers, and its page after
  * the Mac gone after the accept, during the call back, with the pipe up and idle, with
    the printer's answer unacknowledged, and mid-page (the printer is reset once, a request
    inside the hold is busy, and the job after it prints)
  * the board stopping the job, as the test button does
  * a wire 20 ms a frame each way into a port at 57600 baud (the window shuts and opens),
    and a wire that loses every 17th ADSP frame each way
  * the fast modes refused and their switch bytes kept from the printer
  * a rename, a rename during another Mac's job, and the queue's answers with no job

The cases that wait on the adapter's timers run at their real length, side by side. About
two and a half minutes in all.

    make -C tools/rip swainterop          (builds build/swahost and build/rip first)
"""
import asyncio
import contextlib
import io
import pathlib
import re
import shutil
import sys
import tempfile
import time
import unittest

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parents[1]
RIP_DIR = ROOT / "tools" / "rip"
RIP = RIP_DIR / "build" / "rip"
SWAHOST = RIP_DIR / "build" / "swahost"

sys.path[:0] = [str(HERE), str(ROOT / "tools" / "stylewriter"), str(RIP_DIR / "test")]
import etpeer  # noqa: E402
import swcheck  # noqa: E402       its PBM reader and comparison: one reading of "the same dots"
import test_swinterop as swi  # noqa: E402    its Fake (swfake on a pty), pages and same_dots

COLOR, MONO = "ColorStyleWriter2400AT", "StyleWriter1200AT"
RESET = bytes.fromhex("00ffffff49")
SETTLE = 2.3              # the Mac's wait after its reset: swfake hears nothing for 2 s
END = re.compile(r"^end (clean|unclean) down=(\d+) up=(\d+) reached=(\d+) room_min=(\d+) "
                 r"stopped=(\d) failed=(\d)$")

PAGES = {}                # model -> (the page as the printer gets it, the rip's four planes)
TMP = None


def setUpModule():
    """One page a model, made once: a colour picture for the 2500's colour cartridge and a
    gray one for the StyleWriter II, each a few bands with one long block."""
    global TMP
    TMP = pathlib.Path(tempfile.mkdtemp(prefix="swainterop-"))
    gray, rgb = swi.picture()
    for model, cart, pix in (("2500", "colour", rgb), ("ii", "black", gray)):
        d = TMP / f"page-{model}"
        d.mkdir()
        swi.write_urf(d / "page.urf", pix)
        swi.sh([RIP, "--in", d / "page.urf", "--printer", "stylewriter", "--model", model,
                "--cart", cart, "--out-sw", d / "s.bin", "--out-pbm", d / "p.pbm"])
        PAGES[model] = ((d / "s.bin").read_bytes(), swcheck.planes(d / "p", "-"))


def tearDownModule():
    shutil.rmtree(TMP, ignore_errors=True)


def adsp_desc(frame):
    """An Ethernet frame's ADSP descriptor byte, or None for any other frame."""
    if frame[14:22] != etpeer.SNAP_DDP or len(frame) < 48 or frame[34] != etpeer.DDP_ADSP:
        return None
    return frame[47]


class Lane:
    """One direction of one Mac's wire: frames in order, each `delay` late, and every
    `lose`th ADSP frame lost. The third open leg (0x82) is never lost: neither this Mac nor
    lpstyl sends its 0x83 again, so no adapter could recover from that."""

    def __init__(self, deliver, delay, lose):
        self.deliver, self.delay, self.lose = deliver, delay, lose
        self.q, self.count, self.lost = asyncio.Queue(), 0, 0
        self.task = asyncio.create_task(self._run()) if delay else None

    def put(self, frame):
        d = adsp_desc(frame)
        if self.lose and d is not None and d != 0x82:
            self.count += 1
            if self.count % self.lose == 0:
                self.lost += 1
                return
        if self.task:
            self.q.put_nowait((time.monotonic() + self.delay, frame))
        else:
            self.deliver(frame)

    async def _run(self):
        while True:
            due, frame = await self.q.get()
            await asyncio.sleep(max(0.0, due - time.monotonic()))
            self.deliver(frame)


class Tap:
    """A Mac's place on the wire, with an Ethernet address of its own."""

    def __init__(self, bus, mac, delay=0.0, lose=0):
        self.bus, self.mac, self.q = bus, mac, asyncio.Queue()
        self.out = Lane(lambda f: bus.from_tap(self, f), delay, lose)
        self.inn = Lane(self.q.put_nowait, delay, lose)

    async def open(self):
        return self

    async def send(self, frame):
        self.out.put(frame)

    async def recv(self, timeout):
        try:
            return await asyncio.wait_for(self.q.get(), timeout)
        except asyncio.TimeoutError:
            return None

    async def close(self):
        pass


class Bus:
    """The Ethernet: swahost at the end of a pipe, and every Mac hears every frame."""

    def __init__(self, pipe):
        self.pipe, self.taps = pipe, []
        self.task = asyncio.create_task(self._pump())

    def tap(self, **kw):
        t = Tap(self, bytes([0x42, 0x32, 0, 0, 0, 0x10 + len(self.taps)]), **kw)
        self.taps.append(t)
        return t

    def from_tap(self, tap, frame):
        asyncio.get_running_loop().create_task(self.pipe.send(frame))
        for t in self.taps:
            if t is not tap:
                t.inn.put(frame)

    async def _pump(self):
        while True:
            f = await self.pipe.recv(0.5)
            if f:
                for t in self.taps:
                    t.inn.put(f)


class Rig:
    """One swahost and the swfakes behind it. printers: (object, type, id, model, cartridge)."""

    @classmethod
    async def start(cls, name, printers, *args, drain=swi.FAST):
        r = cls()
        r.dir = TMP / name
        r.fakes = [await swi.Fake.start(r.dir, f"fake{i}", p[3], p[4], drain)
                   for i, p in enumerate(printers)]
        names = []
        for p, f in zip(printers, r.fakes):
            names += ["--name", p[0], p[1], str(p[2]), f.path]
        r.proc = await asyncio.create_subprocess_exec(
            SWAHOST, *names, *map(str, args), stdin=asyncio.subprocess.PIPE,
            stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.PIPE)
        r.bus = Bus(await etpeer.Pipe(r.proc.stdout, r.proc.stdin).open())
        r.lines, r.times, r.macs = [], [], 0
        r.task = asyncio.create_task(r._read())
        if not await r.line("online ", 15):
            raise RuntimeError("swahost never came online\n" + r.text)
        return r

    async def _read(self):
        while line := await self.proc.stderr.readline():
            self.lines.append(line.decode(errors="replace").rstrip("\n"))
            self.times.append(time.monotonic())

    @property
    def text(self):
        return "\n".join(self.lines)

    def has(self, prefix, after=0):
        return next((l for l in self.lines[after:] if l.startswith(prefix)), None)

    def count(self, prefix):
        return sum(1 for l in self.lines if l.startswith(prefix))

    def when(self, prefix, nth=0):
        """When the nth line that starts so was read."""
        return [t for l, t in zip(self.lines, self.times) if l.startswith(prefix)][nth]

    async def line(self, prefix, timeout, after=0):
        await swi.until(lambda: self.has(prefix, after), timeout, 0.02)
        return self.has(prefix, after)

    def ends(self):
        """Each job's end line: (clean, down, up, reached, room_min, stopped, failed)."""
        out = []
        for l in self.lines:
            if m := END.match(l):
                out.append((m.group(1) == "clean", *map(int, m.groups()[1:])))
        return out

    def mac(self, **wire):
        """A Mac on the wire with its address already its own (swahost's probe for the
        same one is answered, so it picks another)."""
        m = etpeer.Mac(self.bus.tap(**wire))
        m.net, m.node, m.online = 0xFF01, 10 + self.macs, True
        self.macs += 1
        return m

    async def stop(self):
        if self.proc.returncode is None:
            self.proc.kill()
            await self.proc.wait()
        await self.task
        self.bus.task.cancel()
        for f in self.fakes:
            await f.stop()


def heard(fake):
    """Every byte the fake printer was sent, and when each write reached it (ms)."""
    data, marks = bytearray(), []
    try:
        text = pathlib.Path(fake.log).read_text()
    except FileNotFoundError:
        return b"", []
    for l in text.splitlines():
        m = re.match(r"^\s*(\d+) > ([0-9a-f]+)$", l)
        if m:
            marks.append((int(m.group(1)), len(data)))
            data += bytes.fromhex(m.group(2))
    return bytes(data), marks


async def quiet(coro, timeout):
    """A Mac's call, bounded; returns (result, None). What the Macs print is kept by the
    test's own redirect, since several talk at once."""
    return await asyncio.wait_for(coro, timeout), None


P2500 = ("Colour 2500", COLOR, 3, "2500", "colour")
PII = ("Writer II", MONO, 5, "ii", "black")


class Interop(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.rigs, self.made = [], 0
        self.said = io.StringIO()                      # what the Macs print
        self.quiet = contextlib.redirect_stdout(self.said)
        self.quiet.__enter__()

    async def asyncTearDown(self):
        self.quiet.__exit__(None, None, None)
        for r in self.rigs:
            await r.stop()

    async def rig(self, printers, *args, **kw):
        self.made += 1
        r = await Rig.start(f"{self.id().rsplit('.', 1)[-1]}-{self.made}", printers, *args, **kw)
        self.rigs.append(r)
        return r

    async def job(self, mac, printer, timeout=90, **kw):
        """One print_sw of that printer's page: what it returned."""
        out, _ = await quiet(mac.print_sw(printer[0], printer[1], PAGES[printer[3]][0],
                                          settle=SETTLE, **kw), timeout)
        return out

    async def pipe_up(self, mac, printer, user="Ada"):
        """The exchange up to the pipe, by hand, in Apple's order: the data connection."""
        data = etpeer.Adsp(mac, etpeer.SW_DATA_SOCK).listen()
        _, ctl = await mac._sw_control(printer[0], printer[1], etpeer.SW_CTL_SOCK)
        req = etpeer.sw_request(etpeer.SW_DATA_SOCK, user)
        self.assertEqual(await ctl.attention(etpeer.SWA_PRINT, req), b"\0\0")
        self.assertEqual(await ctl.attention(etpeer.SWA_END, b"\0"), b"\0\0")
        await ctl.close()
        await data.accept(10)
        return data

    def printed(self, rig, fake, models, what):
        """swfake drew these models' pages, in order, with the rip's dots and no fault."""
        self.assertEqual(fake.faults(), [], f"{what}\n{rig.text}")
        self.assertEqual(swi.same_dots(fake, [PAGES[m][1] for m in models], what), [], rig.text)

    async def clean_job(self, printer, order, *args, job=None, **wire):
        """A whole job that ends as the Mac's driver means it to."""
        rig = await self.rig([printer], *args)
        out = await self.job(rig.mac(**wire), printer, order=order, **(job or {}))
        self.assertTrue(await rig.line("free", 30), rig.text)
        fake, model = rig.fakes[0], printer[3]
        self.assertEqual((out["result"], out["model"], out["pages"], out["end"], out["busy"]),
                         (0, model, 1, b"\xff\xff", []), rig.text)
        self.assertEqual(out["callback"][2], 0x89)           # from the socket it listens on
        self.assertEqual(out["status_answer"], b"\0\0")
        self.printed(rig, fake, [model], f"{model}, {order}'s order")
        # The job ended clean: the board sent the printer nothing of its own, so every byte
        # the printer heard is one the Mac sent (lpstyl's order ends with its own 00 and reset).
        own = len(RESET) if order == "lpstyl" else 0
        got, _ = heard(fake)
        self.assertEqual(len(got), out["sent"] + own, rig.text)
        self.assertTrue(got.startswith(RESET), got[:8])
        self.assertTrue(got.endswith(b"El" + RESET if own else b"El"), got[-8:])
        self.assertEqual(rig.count("reset"), 0, rig.text)
        self.assertEqual(rig.count("log adapter: job ended cleanly"), 1, rig.text)
        ends = rig.ends()
        self.assertEqual(len(ends), 1, rig.text)
        clean, down, up, reached, room_min, stopped, failed = ends[0]
        self.assertEqual((clean, down, reached, stopped, failed), (True, len(got), len(got), 0, 0), rig.text)
        self.assertGreater(up, 5)
        return rig, out, ends[0]

    # ---- a page, both orders

    async def test_2500_colour_in_apples_order(self):
        await self.clean_job(P2500, "apple")

    async def test_stylewriter_ii_in_apples_order(self):
        await self.clean_job(PII, "apple")

    async def test_2500_colour_in_lpstyls_order(self):
        rig, _, _ = await self.clean_job(P2500, "lpstyl")
        # No 0012 and no close on the connection it asked on: the call back came all the same.
        self.assertTrue(rig.has("log adapter: pipe up"), rig.text)

    async def test_stylewriter_ii_in_lpstyls_order(self):
        """And with the open of the connection it asks on left at two legs: the 0x82 never
        comes, and the adapter answers all the same."""
        await self.clean_job(PII, "lpstyl", job=dict(third_leg=False))

    # ---- two StyleWriters

    async def test_two_names_each_with_its_type_and_socket(self):
        rig = await self.rig([P2500, PII])
        mac = rig.mac()
        self.assertEqual((await mac.nbp_find(P2500[0], COLOR))[2], 0x89)
        self.assertEqual((await mac.nbp_find(PII[0], MONO))[2], 0x8A)
        self.assertIsNone(await mac.nbp_find(P2500[0], MONO, tries=2, wait=0.5))
        self.assertIsNone(await mac.nbp_find(PII[0], COLOR, tries=2, wait=0.5))
        self.assertEqual((await mac.nbp_find("=", MONO))[2], 0x8A)       # as the Chooser asks
        self.assertEqual((await mac.nbp_find("=", COLOR))[2], 0x89)
        # One job on the whole board: while the II prints, a request for the 2500 is busy,
        # its queue names the II's job, and it prints once the II is done.
        other = rig.mac()
        first = asyncio.create_task(self.job(mac, PII, user="Ada"))
        self.assertTrue(await rig.line("log adapter: pipe up", 20), rig.text)
        second = await self.job(other, P2500, user="Bea", busy_wait=1.0)
        a = await first
        self.assertTrue(await rig.line("free", 30, len(rig.lines) - 1), rig.text)
        self.assertEqual((a["model"], a["callback"][2], a["busy"]), ("ii", 0x8A, []), rig.text)
        self.assertEqual((second["model"], second["callback"][2]), ("2500", 0x89), rig.text)
        self.assertGreaterEqual(len(second["busy"]), 1, rig.text)
        self.assertEqual(second["busy"][0]["job"][:70], etpeer.sw_request(etpeer.SW_DATA_SOCK, "Ada"))
        self.printed(rig, rig.fakes[1], ["ii"], PII[0])
        self.printed(rig, rig.fakes[0], ["2500"], P2500[0])
        self.assertEqual([l for l in rig.lines if l.startswith("begin")], ["begin id=5", "begin id=3"])
        self.assertEqual([e[0] for e in rig.ends()], [True, True], rig.text)
        self.assertEqual(rig.count("reset"), 0)
        self.assertEqual(len(heard(rig.fakes[1])[0]), a["sent"])           # each its own bytes
        self.assertEqual(len(heard(rig.fakes[0])[0]), second["sent"])

    # ---- busy

    async def test_busy_while_the_board_prints_something_else(self):
        """In lpstyl's order: told busy, it asks the queue, sleeps 15 s deaf to the adapter's
        probes, ends that session and asks again. The connection it slept on is still there."""
        rig = await self.rig([P2500], "--busy-ms", 6000)
        out = await self.job(rig.mac(), P2500, order="lpstyl")
        self.assertTrue(await rig.line("free", 30), rig.text)
        self.assertEqual(len(out["busy"]), 1, rig.text)
        for q in out["busy"]:                              # busy, and nobody's job here to name
            self.assertEqual((q["result"], q["jobs"], q["job"], q["status"]),
                             (1, 1, bytes(72), bytes(259)), rig.text)
        self.assertEqual(rig.count("busy id=3"), len(out["busy"]))
        self.assertEqual(rig.count("log adapter: busy to"), len(out["busy"]))
        self.assertEqual(out["result"], 0)
        self.printed(rig, rig.fakes[0], ["2500"], "after the board was busy")

    async def test_a_second_mac_asks_during_the_first_macs_job(self):
        rig = await self.rig([P2500], drain=swi.SLOW)
        one, two = rig.mac(), rig.mac()
        first = asyncio.create_task(self.job(one, P2500, user="Ada"))
        self.assertTrue(await rig.line("log adapter: pipe up", 20), rig.text)
        await asyncio.sleep(0.3)
        q, _ = await quiet(two.sw_queue(P2500[0], COLOR), 20)
        self.assertEqual((q["jobs"], q["position"], q["status_id"], q["status"], q["user"],
                          q["data_sock"], q["end"]),
                         (1, 1, 0x0647, "Printing", "Ada", etpeer.SW_DATA_SOCK, 0), rig.text)
        self.assertEqual(q["job"], etpeer.sw_request(etpeer.SW_DATA_SOCK, "Ada") + b"\0\0")
        self.assertEqual(q["status_raw"], etpeer.sw_status("Printing") + b"\0\0")
        second = await self.job(two, P2500, user="Bea", busy_wait=1.0)
        a = await first
        self.assertTrue(await rig.line("free", 30, len(rig.lines) - 1), rig.text)
        self.assertEqual((a["result"], a["busy"], a["pages"], a["end"]), (0, [], 1, b"\xff\xff"))
        self.assertGreaterEqual(len(second["busy"]), 1, rig.text)
        b = second["busy"][0]
        self.assertEqual((b["result"], b["jobs"], b["job"][:70]),
                         (1, 1, etpeer.sw_request(etpeer.SW_DATA_SOCK, "Ada")), rig.text)
        self.assertEqual(b["status"][:11], bytes.fromhex("0647085072696e74696e67"))
        self.assertEqual((second["result"], second["pages"]), (0, 1))
        # The first job was not disturbed: both pages are the rip's, and both ended clean.
        self.printed(rig, rig.fakes[0], ["2500", "2500"], "two Macs, one after the other")
        self.assertEqual([e[0] for e in rig.ends()], [True, True], rig.text)
        self.assertEqual(rig.count("reset"), 0)
        got, _ = heard(rig.fakes[0])
        self.assertEqual(len(got), a["sent"] + second["sent"])

    # ---- the Mac goes away

    async def gone_before_the_pipe(self, where, limit):
        """The request was accepted and the Mac is gone: nothing reaches the printer, and
        the adapter takes the next request within `limit` seconds of the accept."""
        rig = await self.rig([P2500])
        mac = rig.mac()
        out = await self.job(mac, P2500, vanish=where)
        self.assertEqual(out["result"], 0)
        self.assertTrue(await rig.line("free", limit + 2), f"{where}\n{rig.text}")
        took = rig.when("free") - rig.when("begin")
        self.assertLess(took, limit + 0.5, f"{where}\n{rig.text}")
        self.assertTrue(rig.has("log adapter: job ended no pipe"), rig.text)
        self.assertEqual(rig.ends(), [(False, 0, 0, 0, 4096, 0, 0)], rig.text)
        self.assertEqual(rig.count("reset"), 0)
        self.assertEqual(heard(rig.fakes[0])[0], b"", where)
        q, _ = await quiet(rig.mac().sw_queue(P2500[0], COLOR), 20)
        self.assertEqual((q["jobs"], q["job"]), (0, bytes(72)), rig.text)
        return took

    async def gone_with_the_pipe_idle(self):
        """Gone with the pipe up and nothing sent: 20 s of probes, and no reset, since no
        byte reached the printer."""
        rig = await self.rig([P2500])
        out = await self.job(rig.mac(), P2500, vanish="pipe")
        self.assertEqual(out["result"], 0)
        self.assertTrue(await rig.line("free", 24), rig.text)
        took = rig.when("free") - rig.when("log adapter: pipe up")
        self.assertTrue(19.5 < took < 23.0, f"{took}\n{rig.text}")
        self.assertTrue(rig.has("log adapter: job ended Mac gone"), rig.text)
        self.assertEqual(rig.ends(), [(False, 0, 0, 0, 4096, 0, 0)], rig.text)
        self.assertEqual(rig.count("reset"), 0)
        self.assertEqual(heard(rig.fakes[0])[0], b"")

    async def gone_with_an_answer_unacknowledged(self):
        """Gone the moment it asked the printer something: the printer's answer is sent
        again each 3 s, five times, then the job is given up and the printer reset."""
        rig = await self.rig([P2500], "--hold-ms", 500)
        data = await self.pipe_up(rig.mac(), P2500)
        await data.write(b"\xff\xff\xff1")              # acknowledged, so it is in the adapter
        data.vanish()
        t0 = time.monotonic()
        self.assertTrue(await rig.line("reset", 22), rig.text)
        took = rig.when("reset") - t0                  # 18 s: before the probes would end it at 20
        self.assertTrue(17.5 < took < 19.7, f"{took}\n{rig.text}")
        self.assertTrue(rig.has("log adapter: job ended Mac gone"), rig.text)
        self.assertEqual([e[:4] + e[5:] for e in rig.ends()], [(False, 4, 1, 4, 0, 0)], rig.text)
        await swi.until(lambda: heard(rig.fakes[0])[0].endswith(RESET), 2)
        self.assertEqual(heard(rig.fakes[0])[0], b"\xff\xff\xff1" + RESET)
        self.assertTrue(await rig.line("free", 3), rig.text)
        self.assertEqual(rig.count("reset"), 1)

    async def gone_mid_page(self, hold):
        """Gone after the page's first block. The printer gets the board's reset, once; a
        request inside the hold is busy; the job after it prints, and its own reset comes
        no sooner than the hold after the board's."""
        rig = await self.rig([P2500], "--hold-ms", int(hold * 1000))
        fake = rig.fakes[0]
        out = await self.job(rig.mac(), P2500, vanish="page")
        self.assertEqual((out["result"], out["pages"]), (0, 0))
        self.assertTrue(await rig.line("reset", 24), rig.text)
        took = rig.when("reset") - rig.when("log adapter: pipe up")
        self.assertLess(took, 23.0, rig.text)
        await swi.until(lambda: heard(fake)[0].endswith(RESET), 2)
        got, _ = heard(fake)
        self.assertEqual(len(got), out["sent"] + len(RESET), rig.text)      # that, and no more
        self.assertTrue(got.endswith(RESET), got[-8:])
        self.assertTrue(rig.has("log adapter: job ended Mac gone"), rig.text)
        clean, down, _, reached, _, stopped, failed = rig.ends()[0]
        self.assertEqual((clean, down, reached, stopped, failed), (False, out["sent"], out["sent"], 0, 0))
        # Inside the hold: busy, and its queue says so.
        self.assertIsNone(rig.has("free"), rig.text)
        second = await self.job(rig.mac(), P2500, busy_wait=1.0)
        self.assertGreaterEqual(len(second["busy"]), 1, rig.text)
        self.assertEqual((second["busy"][0]["result"], second["busy"][0]["jobs"]), (1, 1))
        self.assertTrue(await rig.line("free", 30, len(rig.lines) - 1), rig.text)
        self.assertEqual(rig.count("reset"), 1, rig.text)                   # once
        self.assertEqual((second["result"], second["pages"], second["end"]), (0, 1, b"\xff\xff"))
        self.printed(rig, fake, ["2500"], "the job after a Mac vanished mid-page")
        self.assertEqual([e[0] for e in rig.ends()], [False, True], rig.text)
        got, marks = heard(fake)
        cut = out["sent"] + len(RESET)
        self.assertEqual(len(got), cut + second["sent"])
        at = {off: ms for ms, off in marks}
        boards = max(ms for ms, off in marks if off < cut)              # the board's reset
        self.assertGreaterEqual(at[cut] - boards, hold * 1000 - 50, rig.text)

    async def test_the_mac_vanishes(self):
        """Each point a Mac can go away at, side by side: these wait on the adapter's real
        timers (1 s to the call back, 6 s of Open Requests, 15 s for a pipe, 20 s of probes,
        18 s of an answer sent again)."""
        t0 = time.monotonic()
        res = await asyncio.gather(
            self.gone_before_the_pipe("accepted", 15), self.gone_before_the_pipe("callback", 15),
            self.gone_with_the_pipe_idle(), self.gone_mid_page(4.0),
            self.gone_with_an_answer_unacknowledged(), return_exceptions=True)
        for r in res:
            if isinstance(r, BaseException):
                raise r
        # Gone at the accept, the call back starts a second later; gone after the close, at
        # once. Then six Open Requests a second apart.
        self.assertTrue(6.5 < res[0] < 9.5 and 5.5 < res[1] < 8.5, res[:2])
        self.assertLess(time.monotonic() - t0, 75)

    async def test_the_board_stops_the_job(self):
        """The test button, here once the job's first byte has gone to the port: the Mac is
        told, the printer is reset once, the hold after the job's first bytes, and the
        printer is free the hold after that."""
        rig = await self.rig([PII], "--stop-after", 1, "--hold-ms", 3000)
        fake = rig.fakes[0]
        with self.assertRaises(etpeer.AdspError):
            await self.job(rig.mac(), PII)
        self.assertTrue(await rig.line("reset", 5), rig.text)
        self.assertTrue(rig.has("log adapter: job ended stopped"), rig.text)
        clean, _, _, reached, _, stopped, failed = rig.ends()[0]
        self.assertEqual((clean, stopped, failed), (False, 1, 0), rig.text)
        self.assertGreaterEqual(reached, 1)
        await swi.until(lambda: heard(fake)[0].endswith(RESET), 2)
        got, marks = heard(fake)
        self.assertEqual(got[reached:], RESET, got)
        # The job's first bytes are the driver's own reset of the printer: the board's comes
        # no sooner than the hold after them, though the stop came at once.
        at = {off: ms for ms, off in marks}
        self.assertGreaterEqual(at[reached] - at[0], 3000 - 50, rig.text)
        self.assertTrue(await rig.line("free", 6), rig.text)
        self.assertTrue(2.8 < rig.when("free") - rig.when("reset") < 5.0, rig.text)

    # ---- the wire

    async def test_a_slow_wire_into_a_slow_port(self):
        """20 ms a frame each way (the zone relay) and a port at 57600 baud: the 4 KB in
        front of the printer fills, the window shuts and is opened again unasked, and the
        page is the rip's dot for dot."""
        rig, out, end = await self.clean_job(P2500, "apple", "--baud", 57600, delay=0.02)
        self.assertLess(end[4], 64, rig.text)              # room_min: the window all but shut

    async def test_a_wire_that_loses_frames(self):
        """Every 17th ADSP frame each way is lost: each side sends again, and the page is
        the rip's dot for dot. The Mac is not strict here: on such a wire an acknowledgement
        rightly counts a byte the Mac never got."""
        rig, out, end = await self.clean_job(PII, "apple", job=dict(strict=False), lose=17)
        tap = rig.bus.taps[0]
        self.assertGreaterEqual(tap.out.lost, 2)
        self.assertGreaterEqual(tap.inn.lost, 2)

    # ---- the rest

    async def test_fast_modes_are_refused(self):
        """The port follows neither fast mode: the driver is told so, and a message that is
        the one byte that would switch the printer to one never reaches it. The job ends
        well inside the hold after its first bytes, which stand for the driver's reset of
        the printer: the printer is kept until the hold is over, for the next job's reset."""
        rig = await self.rig([P2500], "--hold-ms", 3000)
        data = await self.pipe_up(rig.mac(), P2500)
        for code, body, answer in ((etpeer.SWA_FAST_CLOCK, b"\0", b"\x00\xef"),
                                   (etpeer.SWA_FAST, b"\0\0", b"\xff\xef"),
                                   (etpeer.SWA_SERIAL, b"\x4c\0", b"\0\0"),
                                   (etpeer.SWA_OPTIONS, b"\x40", b"\0\0"),
                                   (etpeer.SWA_FAST_LOWER, b"\x01\x01", b"\0\0"),
                                   (0x0042, b"\0", b"\0\0")):
            self.assertEqual(await data.attention(code, body), answer, f"{code:04X}")
        await data.write(b"x")                             # the colour models' switch
        await data.write(b"h")                             # the II's and the 1200's
        await data.write(b"xx")                            # two bytes: data
        await data.write(b"ab", eom=False)
        await data.write(b"h")                             # the end of a longer message: data
        for _ in range(50):
            if await data.attention(etpeer.SWA_DRAINED, b"\0") == b"\xff\xff":
                break
            await asyncio.sleep(0.05)
        self.assertEqual(await data.attention(etpeer.SWA_END, b"\0"), b"\xff\xff")
        await data.close()
        self.assertTrue(await rig.line("free", 6), rig.text)
        self.assertGreaterEqual(rig.when("free") - rig.when("log adapter: pipe up"), 2.9, rig.text)
        self.assertEqual(rig.count("reset"), 0, rig.text)
        self.assertEqual([e[:4] + e[5:] for e in rig.ends()], [(True, 5, 0, 5, 0, 0)], rig.text)
        self.assertEqual(heard(rig.fakes[0])[0], b"xxabh")
        self.assertIsNone(data.error)

    async def test_rename(self):
        rig = await self.rig([P2500])
        mac = rig.mac()
        r, _ = await quiet(mac.sw_rename(P2500[0], COLOR, "Upstairs"), 20)
        self.assertEqual(r, dict(jobs=0, renamed=0, end=0))
        self.assertTrue(await rig.line("renamed id=3", 3), rig.text)
        self.assertEqual(rig.count("log adapter: rename asked"), 1)
        self.assertEqual((await mac.nbp_find("Upstairs", COLOR))[2], 0x89)
        self.assertIsNone(await mac.nbp_find(P2500[0], COLOR, tries=2, wait=0.5))
        self.assertNotIn("Upstairs", rig.text)             # a person's name is never logged
        out, _ = await quiet(mac.print_sw("Upstairs", COLOR, PAGES["2500"][0], settle=SETTLE), 60)
        self.assertEqual((out["result"], out["pages"]), (0, 1))
        self.printed(rig, rig.fakes[0], ["2500"], "under its new name")

    async def test_rename_during_another_macs_job(self):
        """The job goes on under the old name's socket; the new name answers at once."""
        rig = await self.rig([P2500], drain=swi.SLOW)
        one, two = rig.mac(), rig.mac()
        first = asyncio.create_task(self.job(one, P2500))
        self.assertTrue(await rig.line("log adapter: pipe up", 20), rig.text)
        r, _ = await quiet(two.sw_rename(P2500[0], COLOR, "Den"), 20)
        self.assertEqual(r, dict(jobs=1, renamed=0, end=0))
        self.assertTrue(await rig.line("renamed id=3", 3), rig.text)
        self.assertEqual((await two.nbp_find("Den", COLOR))[2], 0x89)
        a = await first
        self.assertTrue(await rig.line("free", 30), rig.text)
        self.assertEqual((a["result"], a["pages"], a["end"]), (0, 1, b"\xff\xff"))
        self.printed(rig, rig.fakes[0], ["2500"], "renamed mid-job")
        self.assertEqual([e[0] for e in rig.ends()], [True], rig.text)

    async def test_queue_with_no_job(self):
        rig = await self.rig([P2500])
        q, _ = await quiet(rig.mac().sw_queue(P2500[0], COLOR), 20)
        self.assertEqual((q["jobs"], q["position"], q["job"], q["status_raw"], q["end"]),
                         (0, 0, bytes(72), bytes(259), 0), rig.text)
        self.assertEqual(rig.count("begin"), 0)


if __name__ == "__main__":
    unittest.main(verbosity=2)
