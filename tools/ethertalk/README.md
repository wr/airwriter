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

## More than one, and old Macs (W-1088)

`--also NAME` adds a second fake LaserWriter (its own AppleTalk address) on the
same tunnel: one computer can bind the tunnel's port only once. Its jobs go in
`--out`/NAME. A tunnel fake starts itself again when this computer's address
changes, so the board never holds a printer that stopped answering.

Two things an old Mac does, for the printers the board offers it:

```sh
python3 tools/ethertalk/etpeer.py zone ZONE chooser LaserWriter
python3 tools/ethertalk/etpeer.py zone ZONE print-ps "LaserWriter II NT" page.ps
```

`chooser TYPE` lists every answer to `=:TYPE@*`, as the Chooser does. `print-ps`
prints PostScript to a LaserWriter by name and shows what the printer says
back: through the board's relay when the printer is on another link.

## A Mac's StyleWriter driver (W-1193)

`print-sw NAME TYPE FILE` prints to a StyleWriter through Apple's StyleWriter
network adapter, or through AirWriter standing in for one, as a Mac's
StyleWriter driver does. Over ADSP it asks `NAME:TYPE@*` to print, takes the
adapter's call back, and talks to the printer through that connection: a
reset, `?`, the setup letters, then the page, each command one message, paced
by the printer's own `1`, `2` and `B`. FILE is a page as the rip writes it:

```sh
tools/rip/build/rip --sw-testpage colour --model 2500 --out-sw page.sw
python3 tools/ethertalk/etpeer.py tunnel print-sw "StyleWriter" ColorStyleWriter2400AT page.sw
python3 tools/ethertalk/etpeer.py tunnel sw-queue "StyleWriter" ColorStyleWriter2400AT
python3 tools/ethertalk/etpeer.py tunnel sw-rename "StyleWriter" ColorStyleWriter2400AT "Upstairs"
```

This prints: ask whoever is near the printer first. `--lpstyl` does the job in
lpstyl's order (attentions without Ack Request, the control connection left
open, a Forward Reset at the end). `sw-queue` asks the adapter what it is
printing and `sw-rename` renames it, as the Chooser's setup does. The Mac's
end is strict: an adapter that numbers a packet wrong, acknowledges the wrong
count, sends past the window it was given or ends an attention's answer with
EOM stops the job with an error that says which.

`python3 -m unittest tools/ethertalk/test_adsp_peer.py` runs two ADSP ends
against each other, and the driver's side against a small adapter and a
canned printer, with no board.

`make -C tools/rip swainterop` (`test_sw_adapter_interop.py`, python3.14 with
numpy) prints from this Mac through the board's own adapter, built for this
computer (`tools/rip/build/swahost`), to `tools/stylewriter/swfake.py` as the
printer, and holds the page swfake drew to the dots the rip made: a 2500 and
a StyleWriter II in both orders, two StyleWriters, busy, the Mac gone at each
point of a job, the board stopping it, a rename, a slow wire and one that
loses frames. It waits on the adapter's real timers, so it takes about two
and a half minutes.
