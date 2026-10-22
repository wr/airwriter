<!-- SPDX-License-Identifier: CC-BY-SA-4.0 -->
# etpeer

An EtherTalk Mac in software (W-1076): it takes an AppleTalk address with
AARP, finds the board with an NBP lookup as the Chooser does, and prints a file
over PAP as Apple's AppleTalk ImageWriter driver does. It reaches the board the
two ways an emulated Mac does: Basilisk II's UDP tunnel on this network, or an
Infinite Mac zone.

```sh
python3 tools/ethertalk/etpeer.py tunnel lookup
python3 tools/ethertalk/etpeer.py tunnel print fixtures/pap-iw701-page.bin
python3 tools/ethertalk/etpeer.py zone ZONE lookup     # needs: pip install websockets
python3 tools/ethertalk/etpeer.py zone ZONE print FILE
```

`print` makes the ImageWriter print: ask whoever is near it first. The file
goes to the printer as it is, so it must be an ImageWriter stream, such as
`fixtures/pap-iw701-page.bin` (a real Mac's page, crc32 `62416521`); compare
the board's "job printed" line in `/api/et`. The board's address defaults to
10.0.1.115 (`AIRWRITER=` to change it). The tunnel binds UDP 6066, so it
cannot run beside Basilisk II on the same computer.

## A fake LaserWriter (W-1082)

`etpeer.py ... laserwriter NAME` is a PostScript LaserWriter on AppleTalk for
the board to print to, since no real one is on the bench. It registers
`NAME:LaserWriter@*`, answers PAP as a LaserWriter does (it pulls the job,
quantum 8; its status; its output read back; EOF), answers the board's query
job as a LaserWriter II NT would, and saves each job as `job-N.ps` in `--out`,
drawn to `job-N.png` at 300 dpi when Ghostscript is installed. Nothing prints.

```sh
python3 tools/ethertalk/etpeer.py tunnel laserwriter "LaserWriter II NT" --out /tmp/lw
```

Lines typed on its stdin fault it: `busy 20` (OpenConn answered busy for 20 s),
`paper` and `jam` (a PrinterError status; it stops pulling), `ok`, `error` (the
next job gets a PostScript error), `drop` (it answers nothing more).
`--hex-only` answers the binary probe as a Level 1 machine that cannot take
binary would, so the board sends hex.

`make -C tools/rip interop` runs the board's own PAP client, built for this
computer (`tools/rip/build/lwhost`), against it over a pipe: lookup, the query
both ways, a print byte for byte, busy, paper out, a PostScript error, cancel,
and a printer gone silent. `etpeer.py pipe laserwriter NAME` is the fake on
stdin and stdout for that.
