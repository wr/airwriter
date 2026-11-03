#!/usr/bin/env python3.14
# SPDX-License-Identifier: GPL-3.0-or-later
"""The firmware's StyleWriter code against a software StyleWriter, on this computer (W-1103).

tools/rip/build/swhost is the board's session, page stage and rip built for the host; it
prints to a serial device. Here the device is the pseudo-terminal swfake.py serves, so the
whole conversation (recognition, the job's setup, pacing on the printer's free buffer, a
fault held and let go, silence, cancel) runs for real, over a line, against a printer that
checks every byte it is sent.

What is checked, per scenario (each its own swfake and swhost, run side by side):

  * all seven models: swhost finds the model (`--ask`, the plain question; the original
    only by being named, as the plain question cannot find it), a gray and a colour page
    print, and the pages swfake drew are the dots `rip --printer stylewriter` made of the
    same pages, on the cartridge the printer has
  * a colour page on a black cartridge prints gray, through the screen
  * `--quality`, the setup letters in swhost's log
  * a fault mid-page (paper out, a jam, ink out, the door open, waste ink, no cartridge), then
    put right (`ok`): swhost shows it, waits, and the page completes with the same dots
  * the printer goes silent mid-page (`silent`): swhost exits 1 after about 30 s with
    "printer stopped answering"
  * cancel (SIGINT) mid-band: swhost finishes the block in flight, sends the reset (and `E`
    on a black-only model), exits 1; swfake never faults, ejects the page, and the next run
    prints
  * a PNG page (gray, RGB, RGBA, 16-bit gray, palette, with and without its resolution) prints
    the dots the same pixels as a URF would
  * a printer that is not the model named, a printer that does not answer the plain question,
    an interlaced or a damaged PNG: refused before anything is printed

    make -C tools/rip swinterop          (builds build/swhost and build/rip first)

The silence rule is real time (30 s), so the run takes about 35 seconds.
"""
import asyncio
import os
import pathlib
import re
import signal
import struct
import subprocess
import sys
import tempfile
import time
import zlib

import numpy as np

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parents[1]
RIP_DIR = ROOT / "tools" / "rip"
RIP = RIP_DIR / "build" / "rip"
SWHOST = RIP_DIR / "build" / "swhost"
SWFAKE = HERE / "swfake.py"
MAKER = RIP_DIR / "test" / "make_300dpi_fixture.py"

sys.path.insert(0, str(RIP_DIR / "test"))
import swcheck  # noqa: E402  its PBM reader and plane comparison: one reading of "the same dots"

MODELS = ("original", "ii", "1200", "1500", "2200", "2400", "2500")
COLOUR = ("1500", "2200", "2400", "2500")
NAMES = {"original": "StyleWriter", "ii": "StyleWriter II", "1200": "StyleWriter 1200",
         "1500": "Color StyleWriter 1500", "2200": "Color StyleWriter 2200",
         "2400": "Color StyleWriter 2400", "2500": "Color StyleWriter 2500"}
FAST = 4_000_000          # swfake's drain, bytes a second: the buffer is empty by the next look
SLOW = 40_000             # slow enough for a photo page to take several seconds
LOGLINE = re.compile(r"^\s*\d+ ([<>]) ([0-9a-f]+)(\.\.\.)?")   # a byte line of swhost's or swfake's log
HEAD = re.compile(r"^47[0-9a-f]{4}$")          # the `G` and length that begin a block

failed = checks = 0


def default_sigint():
    """A runner started with SIGINT ignored hands that to its children across exec; swfake
    (Python) then never sees Ctrl-C. Undo it, so the cancel and stop below mean what they say."""
    signal.signal(signal.SIGINT, signal.SIG_DFL)


def wire(path, arrow):
    """The byte lines of one direction in a log, as hex (a cut write ends where it was cut)."""
    try:
        text = pathlib.Path(path).read_text()
    except FileNotFoundError:
        return []
    return [m.group(2) for m in map(LOGLINE.match, text.splitlines()) if m and m.group(1) == arrow]


class Fake:
    """One swfake on a pty: `path` is the device to give swhost."""

    def __init__(self, proc, path, out, log):
        self.proc, self.path, self.out, self.log = proc, path, out, log
        self.lines = []
        self.task = asyncio.create_task(self._read())

    @classmethod
    async def start(cls, root, name, model, cart="black", drain=FAST):
        d = pathlib.Path(root) / name
        d.mkdir(parents=True)
        out, log = d / "pages", d / "fake.log"
        proc = await asyncio.create_subprocess_exec(
            sys.executable, SWFAKE, "serve", model, "--pty", "--cart", cart, "--out", out,
            "--no-png", "--log", log, "--drain", str(drain),
            stdin=asyncio.subprocess.PIPE, stdout=asyncio.subprocess.PIPE,
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
        """Ctrl-C, as the fake's own README says it ends; a kill if it does not."""
        if self.proc.returncode is None:
            self.proc.send_signal(signal.SIGINT)
            try:
                await asyncio.wait_for(self.proc.wait(), 5)
            except asyncio.TimeoutError:
                self.proc.kill()
                await self.proc.wait()
        await self.task

    def faults(self):
        return [l for l in self.lines if ": bad:" in l]

    def wire(self, arrow=">"):
        """The log's lines of one direction, as hex."""
        return wire(self.log, arrow)

    def pages(self):
        return sorted(self.out.glob("page-*-k.pbm")) if self.out.exists() else []


class Run:
    """One swhost."""

    def __init__(self, proc, log):
        self.proc, self.log, self.lines = proc, log, []
        self.t0 = time.monotonic()
        self.elapsed = None
        self.task = asyncio.create_task(self._read())

    @classmethod
    async def start(cls, root, name, device, *args):
        log = pathlib.Path(root) / f"{name}.swhost.log"
        proc = await asyncio.create_subprocess_exec(
            SWHOST, "--dev", device, "--log", log, *map(str, args),
            stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.STDOUT,
            preexec_fn=default_sigint)
        return cls(proc, log)

    async def _read(self):
        while line := await self.proc.stdout.readline():
            self.lines.append(line.decode(errors="replace").rstrip("\n"))

    async def wait(self, timeout=150):
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

    def wire(self, arrow=">"):
        return wire(self.log, arrow)

    def blocks(self):
        return sum(1 for h in self.wire() if HEAD.match(h))


async def until(pred, timeout, step=0.05):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        if pred():
            return True
        await asyncio.sleep(step)
    return pred()


def sh(cmd, **kw):
    r = subprocess.run([str(c) for c in cmd], capture_output=True, text=True, **kw)
    if r.returncode:
        raise RuntimeError(f"{' '.join(str(c) for c in cmd)}\n{r.stdout}{r.stderr}")
    return r


def expected(urf, model, cart, tmp, tag):
    """The dots the rip makes of a page for this model and cartridge: four planes."""
    d = pathlib.Path(tmp) / f"exp-{tag}"
    d.mkdir(parents=True, exist_ok=True)
    sh([RIP, "--in", urf, "--printer", "stylewriter", "--model", model, "--cart", cart,
        "--out-pbm", d / "p.pbm"])
    return swcheck.planes(d / "p", "-")


async def expected_a(*a):
    """expected() on a thread: the loop keeps watching the other scenarios meanwhile."""
    return await asyncio.to_thread(expected, *a)


def same_dots(fake, mine_pages, what):
    """The pages swfake drew against the rip's, plane by plane. Returns problems."""
    problems = []
    drawn = fake.pages()
    if len(drawn) != len(mine_pages):
        return [f"{what}: swfake drew {len(drawn)} page(s), expected {len(mine_pages)}"]
    for n, mine in enumerate(mine_pages, 1):
        theirs = swcheck.planes(fake.out / f"page-{n}", "-")
        for p in swcheck.PLANES:
            diff = swcheck.compare(mine[p], theirs[p])
            if diff:
                problems.append(f"{what}, page {n}, {p} plane: {diff}")
    return problems


# ---- the pages

def urf_header(w, h, dpi, bits, space):
    hdr = bytearray(32)
    hdr[0], hdr[1] = bits, space
    struct.pack_into(">III", hdr, 12, w, h, dpi)
    return b"UNIRAST\0" + struct.pack(">I", 1) + bytes(hdr)


def write_urf(path, pix, dpi=360):
    """A page of 8-bit gray (h, w) or sRGB (h, w, 3) as URF, in literal runs: the plainest coder."""
    h, w = pix.shape[:2]
    ch = 1 if pix.ndim == 2 else 3
    out = bytearray(urf_header(w, h, dpi, 8 * ch, 0 if ch == 1 else 1))
    for y in range(h):
        out.append(0)                                   # this line once
        row = pix[y].reshape(w, ch)
        x = 0
        while x < w:
            n = min(128, w - x)
            out.append(0 if n == 1 else 257 - n)        # one pixel is a repeat of one
            out += row[x:x + n].tobytes()
            x += n
    pathlib.Path(path).write_bytes(bytes(out))


def picture(w=640, h=480):
    """Smooth gray and colour planes with edges: something a screen has to work at."""
    y, x = np.mgrid[0:h, 0:w]
    gray = ((x * 255 // (w - 1)) // 2 + (y * 255 // (h - 1)) // 2).astype(np.uint8)
    gray[100:180, 60:300] = 0
    gray[200:260, 300:580] = 255
    rgb = np.stack([(x * 255 // (w - 1)), (y * 255 // (h - 1)), 255 - (x * 255 // (w - 1))], axis=2).astype(np.uint8)
    rgb[40:120, 40:200] = (230, 20, 20)
    rgb[300:400, 100:500] = (20, 40, 220)
    return gray, rgb


def png_pages(root):
    """Pages as PNGs, and the 8-bit pixels each must become. Returns [(png, urf, label)]."""
    from PIL import Image
    d = pathlib.Path(root)
    gray, rgb = picture()
    h, w = gray.shape
    out = []

    def add(name, image, pix, label, **save):
        png, urf = d / f"{name}.png", d / f"{name}.urf"
        image.save(png, **save)
        write_urf(urf, pix)
        out.append((png, urf, label))

    add("gray8", Image.fromarray(gray), gray, "8-bit gray", dpi=(360, 360))
    add("rgb8", Image.fromarray(rgb), rgb, "RGB", dpi=(360, 360))
    # No resolution in the file: one pixel a dot, as the tool says.
    add("rgb8-nodpi", Image.fromarray(rgb), rgb, "RGB with no resolution")
    alpha = np.full((h, w), 255, np.uint8)
    alpha[h // 2:] = 128
    alpha[:, : w // 4] = 0
    a = alpha.astype(np.int32)[..., None]
    over = (rgb.astype(np.int32) * a + 255 * (255 - a) + 127) // 255
    add("rgba8", Image.fromarray(np.dstack([rgb, alpha])), over.astype(np.uint8), "RGBA over white",
        dpi=(360, 360))
    g16 = (gray.astype(np.uint16)) * 257
    add("gray16", Image.fromarray(g16), gray, "16-bit gray", dpi=(360, 360))
    # A palette of 16 colours, not gray: the page is RGB.
    pal = [(i * 16, 255 - i * 16, (i * 53) % 256) for i in range(16)]
    idx = ((np.arange(w)[None, :] // 40 + np.arange(h)[:, None] // 30) % 16).astype(np.uint8)
    im = Image.fromarray(idx)
    im.putpalette([c for rgbv in pal for c in rgbv])
    add("palette", im, np.array(pal, np.uint8)[idx], "palette", dpi=(360, 360))
    # The same palette with one entry transparent: that entry is white paper.
    expect = np.array(pal, np.uint8)[idx]
    expect[idx == 3] = 255
    add("palette-trns", im, expect, "palette with a transparent colour", dpi=(360, 360), transparency=3)
    # A palette of grays is a gray page.
    gim = Image.fromarray(idx)
    grays = [i * 17 for i in range(16)]
    gim.putpalette([v for g in grays for v in (g, g, g)])
    add("palette-gray", gim, np.array(grays, np.uint8)[idx], "palette of grays", dpi=(360, 360))
    # 1 bit a pixel.
    bits = (((np.arange(w)[None, :] // 7) + (np.arange(h)[:, None] // 5)) % 2).astype(np.uint8)
    add("bilevel", Image.fromarray(bits * 255).convert("1"), bits * 255, "1-bit gray", dpi=(360, 360))
    return out


def with_ihdr_byte(png_bytes, offset, value):
    """A PNG with one IHDR byte changed and the chunk's CRC made good."""
    b = bytearray(png_bytes)
    b[16 + offset] = value
    struct.pack_into(">I", b, 29, zlib.crc32(bytes(b[12:29])))
    return bytes(b)


# ---- scenarios; each returns [(ok, what)]

async def scenario_model(root, tmp, model, pages, quality=None):
    """pages: URF paths, a gray page and a colour one."""
    cart = "colour" if model in COLOUR else "black"
    fake = await Fake.start(root, f"model-{model}", model, cart)
    res = []
    try:
        named = model == "original"
        args = ["--model", "original"] if named else ["--ask"]
        if quality:
            args += ["--quality", quality]
        run = await Run.start(root, f"model-{model}", fake.path, *args, *pages)
        code = await run.wait(120)
        res.append((code == 0, f"{model}: swhost {'is told the model' if named else '--ask finds it'} and "
                    f"prints {len(pages)} pages (exit {code})"))
        if code != 0:
            res.append((False, f"{model}: swhost said: {run.text}"))
        res.append((NAMES[model] in run.text, f"{model}: swhost names it ({NAMES[model]})"))
        sent = run.wire(">")
        if not named:
            res.append((sent[:1] == ["3f"], f"{model}: the plain question is the one byte ? and nothing else in its write"))
        res.append((not fake.faults(), f"{model}: swfake took every byte without a fault {fake.faults()[:1]}"))
        mine = [await expected_a(urf, model, cart, tmp, f"{model}-{i}") for i, urf in enumerate(pages)]
        problems = same_dots(fake, mine, model)
        inked = mine[0]["k"].any() and (cart == "black" or all(mine[1][p].any() for p in "cmy"))
        res.append((inked, f"{model}: the pages have ink to compare (black on the gray one"
                    f"{', cyan, magenta and yellow on the colour one' if cart == 'colour' else ''})"))
        res.append((not problems, f"{model}: the pages swfake drew are the rip's dots "
                    f"({cart} cartridge){': ' + problems[0] if problems else ''}"))
        if quality:
            want = {"2200": "6d327341", "ii": "75644441"}[model]   # m2sA (best), udDA (draft)
            res.append((want in sent, f"{model}: --quality {quality} sends its setup ({want}) {sent[:6]}"))
    finally:
        await fake.stop()
    return res


async def scenario_black_cart(root, tmp, pages):
    """A colour page to a black cartridge: gray, through the screen, never only the K separation."""
    fake = await Fake.start(root, "blackcart", "2400", "black")
    res = []
    try:
        run = await Run.start(root, "blackcart", fake.path, "--ask", pages["colour"])
        code = await run.wait(120)
        res.append((code == 0, f"2400 with a black cartridge: a colour page prints (exit {code}) {run.text if code else ''}"))
        mine = await expected_a(pages["colour"], "2400", "black", tmp, "blackcart")
        problems = same_dots(fake, [mine], "2400 black")
        res.append((not problems, f"2400 with a black cartridge: the colour page is the rip's gray dots"
                    f"{': ' + problems[0] if problems else ''}"))
        theirs = swcheck.planes(fake.out / "page-1", "-")
        res.append((not any(theirs[p].any() for p in "cmy") and theirs["k"].sum() > 0,
                    "2400 with a black cartridge: black ink only, and plenty of it"))
    finally:
        await fake.stop()
    return res


FAULTS = (            # the model, the bench command, what swhost shows while it holds
    ("2200", "paper", "out of paper"),
    ("ii", "paper", "out of paper"),
    ("1200", "jam", "paper jam"),
    ("2400", "ink", "out of ink"),
    ("2500", "door", "door open"),
    ("1500", "waste", "waste ink full"),
    ("2200", "cart", "no cartridge"),
)


async def scenario_fault(root, tmp, photo, model, fault, text):
    """A fault mid-page, then put right: the job waits, then the page completes."""
    cart = "colour" if model in COLOUR else "black"
    tag = f"{fault} on the {model}"
    fake = await Fake.start(root, f"fault-{model}-{fault}", model, cart, drain=SLOW)
    res = []
    try:
        run = await Run.start(root, f"fault-{model}-{fault}", fake.path, "--ask", photo)
        got = await until(lambda: run.blocks() >= 2, 40)
        res.append((got, f"{tag}: the page is going (two blocks sent)"))
        await fake.say(fault)
        seen = await until(lambda: f"printer: {text}" in run.text, 20)
        res.append((seen, f"{tag}: swhost shows the printer is {text} {run.lines[-3:]}"))
        await asyncio.sleep(2.5)
        res.append((run.proc.returncode is None, f"{tag}: the job waits while it lasts"))
        await fake.say("ok")
        code = await run.wait(120)
        res.append((code == 0, f"{tag}: the page completes once it is put right (exit {code})"))
        res.append((not fake.faults(), f"{tag}: swfake took every byte without a fault {fake.faults()[:1]}"))
        problems = same_dots(fake, [await expected_a(photo, model, cart, tmp, f"fault-{model}-{fault}")], tag)
        res.append((not problems, f"{tag}: the page is the rip's dots, complete{': ' + problems[0] if problems else ''}"))
    finally:
        await fake.stop()
    return res


async def scenario_silent(root, photo):
    """The printer stops answering mid-page: 30 s, then exit 1."""
    fake = await Fake.start(root, "silent", "ii", "black", drain=SLOW)
    res = []
    try:
        run = await Run.start(root, "silent", fake.path, "--ask", photo)
        got = await until(lambda: run.blocks() >= 2, 40)
        res.append((got, "silent: the page is going (two blocks sent)"))
        t0 = time.monotonic()
        await fake.say("silent")
        code = await run.wait(90)
        gone = time.monotonic() - t0
        res.append((code == 1, f"silent: swhost exits 1 (exit {code})"))
        res.append((25 <= gone <= 45, f"silent: after about 30 s of silence ({gone:.0f} s)"))
        res.append(("printer stopped answering" in run.text, f"silent: and says so {run.lines[-2:]}"))
    finally:
        await fake.stop()
    return res


async def scenario_cancel(root, tmp, model, photo, small):
    """SIGINT mid-band: the block in flight finishes, the printer is reset, the next run prints."""
    cart = "colour" if model in COLOUR else "black"
    fake = await Fake.start(root, f"cancel-{model}", model, cart, drain=SLOW)
    res = []
    tag = f"cancel {model}"
    try:
        run = await Run.start(root, f"cancel-{model}-1", fake.path, "--ask", photo)
        got = await until(lambda: run.blocks() >= 2, 40)
        res.append((got, f"{tag}: the page is going (two blocks sent)"))
        run.proc.send_signal(signal.SIGINT)
        code = await run.wait(30)
        res.append((code == 1, f"{tag}: swhost exits 1 (exit {code}) in {run.elapsed:.0f} s"))
        res.append(("cancelled" in run.text, f"{tag}: and says it was cancelled {run.lines[-2:]}"))
        await asyncio.sleep(0.5)
        heard = fake.wire(">")
        tail = "".join(heard[-2:])
        end = "00ffffff49"
        res.append((tail.endswith(("45" if model not in COLOUR else "") + end),
                    f"{tag}: the last thing swfake heard is {'E and ' if model not in COLOUR else ''}the reset ({tail[-14:]})"))
        res.append((not fake.faults(), f"{tag}: swfake never faulted (a block cut short would have) {fake.faults()[:1]}"))
        res.append((not fake.pages(), f"{tag}: the cut page was not printed"))
        await asyncio.sleep(2.5)              # a reset printer is deaf for 2 s: --ask asks once
        run2 = await Run.start(root, f"cancel-{model}-2", fake.path, "--ask", small)
        code2 = await run2.wait(60)
        res.append((code2 == 0, f"{tag}: the next run prints (exit {code2}) {run2.text if code2 else ''}"))
        problems = same_dots(fake, [await expected_a(small, model, cart, tmp, tag.replace(' ', '-'))], tag)
        res.append((not problems, f"{tag}: and it is the rip's dots{': ' + problems[0] if problems else ''}"))
    finally:
        await fake.stop()
    return res


async def scenario_wrong_model(root, small):
    """Named a II, but it is a 1200: `?` says so, and nothing is printed."""
    fake = await Fake.start(root, "wrong", "1200", "black")
    res = []
    try:
        run = await Run.start(root, "wrong", fake.path, "--model", "ii", small)
        code = await run.wait(60)
        res.append((code == 1, f"wrong model: swhost exits 1 (exit {code})"))
        res.append(("another model" in run.text, f"wrong model: and says it is another model {run.lines[-2:]}"))
        res.append((not fake.pages() and not fake.faults(), "wrong model: nothing printed, no fault"))
    finally:
        await fake.stop()
    return res


async def scenario_no_answer(root, small):
    """The original does not answer the plain question until it is reset: `--ask` says so, and
    sends that one byte and no more."""
    fake = await Fake.start(root, "noanswer", "original", "black")
    res = []
    try:
        run = await Run.start(root, "noanswer", fake.path, "--ask", small)
        code = await run.wait(30)
        res.append((code == 1, f"original --ask: exit 1 (exit {code})"))
        res.append(("no answer" in run.text, f"original --ask: says there was no answer {run.lines[-2:]}"))
        sent = run.wire(">")
        res.append((sent == ["3f"], f"original --ask: sent the one byte ? and nothing else {sent}"))
    finally:
        await fake.stop()
    return res


async def scenario_png(root, tmp, pngs):
    """PNG pages come out as the dots their pixels make as URF."""
    fake = await Fake.start(root, "png", "2500", "colour")
    res = []
    try:
        run = await Run.start(root, "png", fake.path, "--ask", *[p for p, _, _ in pngs])
        code = await run.wait(120)
        res.append((code == 0, f"png: {len(pngs)} pages print (exit {code}) {run.text if code else ''}"))
        mine = [await expected_a(u, "2500", "colour", tmp, f"png{i}") for i, (_, u, _) in enumerate(pngs)]
        for n, (_, _, label) in enumerate(pngs, 1):
            if fake.pages() and len(fake.pages()) >= n:
                theirs = swcheck.planes(fake.out / f"page-{n}", "-")
                bad = [swcheck.compare(mine[n - 1][p], theirs[p]) for p in swcheck.PLANES]
                bad = [b for b in bad if b]
                inked = any(theirs[p].any() for p in swcheck.PLANES)
                res.append((not bad and inked, f"png: {label} is the rip's dots of the same pixels"
                            f"{': ' + bad[0] if bad else '' if inked else ': and it has no ink'}"))
            else:
                res.append((False, f"png: {label} was not printed"))
        res.append((not fake.faults(), f"png: swfake took every byte without a fault {fake.faults()[:1]}"))
    finally:
        await fake.stop()
    return res


async def scenario_bad_pngs(root, pngs):
    """Refused before the printer is opened."""
    good = pathlib.Path(pngs[0][0]).read_bytes()
    cases = {
        "interlaced": (with_ihdr_byte(good, 12, 1), "interlaced"),
        "damaged": (good[: len(good) // 2], "PNG"),
        "not an image": (b"just some text\n" * 20, "neither"),
    }
    res = []
    for name, (data, word) in cases.items():
        p = pathlib.Path(root) / f"bad-{name.replace(' ', '-')}.png"
        p.write_bytes(data)
        r = await asyncio.create_subprocess_exec(
            SWHOST, "--dev", "/dev/null", "--model", "ii", p,
            stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.STDOUT, preexec_fn=default_sigint)
        out = (await r.communicate())[0].decode(errors="replace")
        res.append((r.returncode == 2 and word in out,
                    f"{name} page: refused with exit 2 and a word about it (exit {r.returncode}) {out.strip()[-80:]}"))
    return res


async def main():
    global failed, checks
    for tool in (RIP, SWHOST):
        if not tool.exists():
            sys.exit(f"swinterop: build {tool.name} first: make -C tools/rip swinterop")
    t0 = time.monotonic()
    with tempfile.TemporaryDirectory() as t:
        root = pathlib.Path(t)
        tmp = root / "work"
        tmp.mkdir()
        pages = {}
        for kind, name, extra in (("text", "gray", ["--rows", "900"]), ("colour", "colour", ["--rows", "900"]),
                                  ("photo", "photo", ["--rows", "700"]), ("text", "small", ["--rows", "700"])):
            pages[name] = tmp / f"{name}.urf"
            sh([sys.executable, MAKER, kind, pages[name], "--dpi", "360", *extra])
        pngs = png_pages(tmp)

        scenarios = [scenario_model(root, tmp, m, [pages["gray"], pages["colour"]],
                                    quality="5" if m == "2200" else "3" if m == "ii" else None)
                     for m in MODELS]
        scenarios += [
            scenario_black_cart(root, tmp, pages),
            *[scenario_fault(root, tmp, pages["photo"], *f) for f in FAULTS],
            scenario_silent(root, pages["photo"]),
            scenario_cancel(root, tmp, "ii", pages["photo"], pages["small"]),
            scenario_cancel(root, tmp, "2500", pages["photo"], pages["small"]),
            scenario_wrong_model(root, pages["small"]),
            scenario_no_answer(root, pages["small"]),
            scenario_png(root, tmp, pngs),
            scenario_bad_pngs(root, pngs),
        ]
        for results in await asyncio.gather(*scenarios):
            for ok, what in results:
                checks += 1
                if ok:
                    print(f"ok  swinterop {what}")
                else:
                    failed += 1
                    print(f"FAIL swinterop {what}")
    print(f"{'FAIL' if failed else 'ok '} swinterop: {checks - failed}/{checks} checks "
          f"({time.monotonic() - t0:.0f} s)")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
