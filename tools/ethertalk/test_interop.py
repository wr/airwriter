#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The board's PAP client against the fake LaserWriter, on this computer
(W-1082). tools/rip/build/lwhost runs the firmware's own EtherTalk link and
PAP client; etpeer.py's LaserWriter answers it over a pipe. Each scenario is
the board's phase 1 in small: lookup, query (binary and hex), print, busy,
paper out, a PostScript error, cancel, a printer that goes silent.

    make -C tools/rip interop
"""
import asyncio, os, pathlib, subprocess, sys, tempfile

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import etpeer  # noqa: E402

RIP = HERE.parent / "rip" / "build"
NAME = "LaserWriter II NT"
failed = 0


def check(cond, what):
    global failed
    if cond:
        print(f"ok  interop {what}")
    else:
        failed += 1
        print(f"FAIL interop {what}")


async def scenario(args, fake_kw=None, during=None, timeout=150):
    """Runs lwhost with args against a fresh fake; returns (exit code, its
    stderr lines, the fake). `during(fake, lines)` runs beside them."""
    proc = await asyncio.create_subprocess_exec(
        RIP / "lwhost", *args, stdin=asyncio.subprocess.PIPE, stdout=asyncio.subprocess.PIPE,
        stderr=asyncio.subprocess.PIPE)
    pipe = await etpeer.Pipe(proc.stdout, proc.stdin).open()
    fake = etpeer.LaserWriter(pipe, NAME, **(fake_kw or {}))
    lines = []

    async def read_err():
        while True:
            line = await proc.stderr.readline()
            if not line:
                return
            lines.append(line.decode(errors="replace").rstrip("\n"))

    async def serve():
        await fake.acquire()
        await fake.serve()

    tasks = [asyncio.create_task(serve()), asyncio.create_task(read_err())]
    if during:
        tasks.append(asyncio.create_task(during(fake, lines)))
    try:
        code = await asyncio.wait_for(proc.wait(), timeout)
    except asyncio.TimeoutError:
        proc.kill()
        code = -1
    await asyncio.sleep(0.2)
    for t in tasks:
        t.cancel()
    return code, lines, fake


def has(lines, prefix):
    return next((l for l in lines if l.startswith(prefix)), None)


async def wait_for(lines, prefix, seconds=60):
    for _ in range(int(seconds * 20)):
        if has(lines, prefix):
            return True
        await asyncio.sleep(0.05)
    return False


async def main():
    with tempfile.TemporaryDirectory() as t:
        tmp = pathlib.Path(t)
        page = tmp / "page.ps"
        subprocess.run([sys.executable, HERE.parent / "rip" / "test" / "make_fixture.py",
                        tmp / "checker.urf"], check=True, capture_output=True)
        subprocess.run([RIP / "rip", "--in", tmp / "checker.urf", "--printer", "laser",
                        "--ps", page], check=True, capture_output=True)
        data = page.read_bytes()

        code, lines, _ = await scenario(["lookup"])
        check(code == 0 and has(lines, f"found {NAME} "), "lookup finds it")

        code, lines, _ = await scenario(["query", NAME])
        q = has(lines, "query ") or ""
        check(code == 0 and "binary=1" in q and "level=1" in q and "complete=1" in q
              and "product=LaserWriter II NT" in q, f"query: {q}")

        code, lines, _ = await scenario(["query", NAME], {"hex_only": True})
        q = has(lines, "query ") or ""
        check(code == 0 and "binary=0" in q and "complete=1" in q, f"query, hex only: {q}")

        out = tmp / "jobs"
        code, lines, fake = await scenario(["print", NAME, str(page)], {"out": out})
        job = out / "job-1.ps"
        check(code == 0 and has(lines, "ended ok") and job.exists() and job.read_bytes() == data,
              f"print: {len(data)} bytes arrive as sent")

        code, lines, _ = await scenario(["print", NAME, str(page)], {"busy": 5})
        check(code == 0 and has(lines, "status status: busy") and has(lines, "ended ok"),
              "busy for 5 s, then prints")

        async def paper(fake, lines):
            await wait_for(lines, "opened")
            await asyncio.sleep(3)
            fake.command("ok")
        big = tmp / "big.ps"
        big.write_bytes(data * 6)
        code, lines, _ = await scenario(["print", NAME, str(big)], {"paper": True}, during=paper)
        check(code == 0 and any("out of paper" in l for l in lines) and has(lines, "ended ok"),
              "paper out says so, and the job finishes once paper is back")

        code, lines, _ = await scenario(["print", NAME, str(page)], {"error": True})
        check(any(l.startswith("output %%[ Error: undefined") for l in lines)
              and has(lines, "ended ok"), "a PostScript error comes back as output")

        out = tmp / "cancel"
        code, lines, _ = await scenario(["print", NAME, str(big), "--cancel-after", "4096"],
                                        {"out": out})
        job = out / "job-1.ps"
        check(code == 0 and has(lines, "ended canceled") and job.exists()
              and len(job.read_bytes()) < len(big.read_bytes()), "cancel: EOF early, a short job")

        async def drop(fake, lines):
            await wait_for(lines, "opened")
            fake.command("drop")           # out of paper from the start: nothing pulled
        code, lines, _ = await scenario(["print", NAME, str(big), "--dead-ms", "5000"],
                                        {"paper": True}, during=drop, timeout=60)
        check(has(lines, "ended dead") is not None, "a printer gone silent ends the job")

    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    asyncio.run(main())
