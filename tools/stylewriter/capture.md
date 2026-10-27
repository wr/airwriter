<!-- SPDX-License-Identifier: CC-BY-SA-4.0 -->
# Capturing Apple's StyleWriter drivers

Part of `W-1103 StyleWriters on the printer port`. The idea: run Apple's own drivers in an
emulated Mac whose printer port is `swfake.py`'s pseudo-terminal, keep the conversation, and
hold it to what the firmware sends (`tools/rip/test/swcaptures.py`). It tells us where Apple's
drivers and TailTalk's reading (which `sw_session.c` follows) part, and gives the first Apple
page to compare AirWriter's `--dither apple` against.

**State, 2026-10-26: not run. The rig stops for one thing: a 68k Mac ROM.** No capture exists,
so `make -C tools/rip swcaptures` prints a skip line. Nothing from Apple is in the repository:
the drivers live in `~/.config/airwriter/sw-drivers/`, the captures go in
`~/.config/airwriter/sw-captures/`.

## What is on this Mac (checked 2026-10-26)

| Piece | Where | State |
|---|---|---|
| Basilisk II | `~/Projects/macemu/BasiliskII/src/Unix/BasiliskII` | Built 2026-10-20, arm64, SDL2 from Homebrew. Runs. With no ROM it prints `ERROR: Cannot open ROM file.` and exits. |
| Its prefs | `~/.basilisk_ii_prefs` | Names `~/Downloads/ROM`, `mini.dsk` and `cd.iso`. None of the three exists. |
| Mac OS 8.1 | `~/Projects/macintosh-mini/disk-image/out/macos-8.1.hda` | 1 GB raw HFS, bootable, volume "Macintosh HD". The install CD is `.cache/MacOS8_1.iso` beside it. Mac OS 8 wants a 68040: `modelid 14`. |
| The drivers | `~/.config/airwriter/sw-drivers/unpacked/raw/` | 800 KB HFS images of the installer disks: `sw1500-disk1..3.img` (Color SW 1500 2.2.1, which also covers the original, II and 1200) and `cs2500-disk1..4.img` (Color SW 2500 2.2.1, which covers the 2200, 2400 and 2500). Color SW 2200 2.1.2 is a StuffIt archive, unpacked to Tome files under `unpacked/cs2200/`, with no disk image. |
| A 68k ROM | nowhere | Searched the home folder for files of exactly 256 KB, 512 KB, 1, 2, 3 and 4 MB: none. |
| hfsutils, Snow | not installed | Not needed for the plan below. |
| UTM, `qemu-system-m68k` | installed | A `q800` would also need a Quadra ROM. No help. |

Basilisk II did not have to be built: the binary above is current.

## What it needs from Wells

1. **A 68k ROM**, 512 KB or 1 MB, from a Mac IIci or a Quadra (`macintosh-mini` calls
   `064DC91D` a common one). Apple's ROMs are not downloaded for this and nothing here fetches
   one. Either Wells points at a file, or he says yes to copying `~/ROM` off the Raspberry Pi at
   10.0.1.110 (it has one if the Mac Mini build there is set up as its guide says; the Pi was
   not touched).
2. Nothing else. The System disk and the drivers are on this Mac.

## The rig, once there is a ROM

Untried past the ROM check. The steps follow the Basilisk II source; the unknown is whether
the Apple drivers talk through the Serial Driver (which Basilisk emulates) at all.

1. Copy the System disk, so the original stays as it is, and make a prefs file of its own
   (run Basilisk II with `--config FILE`):

   ```
   rom /path/to/ROM
   disk /path/to/copy-of-macos-8.1.hda
   disk ~/.config/airwriter/sw-drivers/unpacked/raw/sw1500-disk1.img   (and 2, 3, then cs2500-disk1..4)
   modelid 14
   cpu 4
   fpu true
   ramsize 67108864
   screen win/800/600
   nogui true
   nosound true
   seriala /dev/null
   serialb /path/to/printer-port
   ```

   `serialb` is the Mac's printer port. Basilisk opens it with a plain `open()` when Mac OS opens
   the port, not at start, so `printer-port` can be a symlink that is pointed at a new
   pseudo-terminal before each print. The `|command` form (a process on a pty) is not used:
   swfake does not speak stdio.
2. Start the fake as the model the driver is for, one log per print (swfake appends to its
   log, so a reused name mixes two prints):

   ```sh
   mkdir -p ~/.config/airwriter/sw-captures/2500
   python3.14 tools/stylewriter/swfake.py serve 2500 --pty --cart colour \
       --log ~/.config/airwriter/sw-captures/2500/checker-colour-q4.log
   ln -sf /dev/ttysNNN /path/to/printer-port        # the path it printed on its first line
   ```

3. In the Mac: run the Installer from the driver's first disk, restart, choose the driver in
   the Chooser and **Printer Port**. Each driver runs only here, never on this computer.
4. Print the page set on each driver, restarting swfake (a new log name, a new pty) between
   prints: a blank page, the checker, a text page, and for the colour drivers a colour photo.
   Name the log for what was printed: `<page>[-colour|-black][-q3|-q4|-q5][-a4|-legal].log` in
   `<model>/`, where `<page>` is `blank`, `checker`, `text` or `photo`, the cartridge defaults
   to black and the quality to 4. The model is the one the fake was started as (`original`,
   `ii`, `1200`, `1500`, `2200`, `2400`, `2500`); the 2200 can be had from either 2200 2.1.2
   or the 2500's 2.2.1.
   Getting pages in: a text page is a plain text file in a folder shared with `extfs`, which
   Basilisk shows as a volume; the checker and a photo need a PICT with a Mac file type, which
   the shared folder does not give (its extension table has no `.pict`). That step is open.
5. `make -C tools/rip swcaptures`.

The Mac asks the printer at 57600 baud; swfake answers as the model it was started as, with
the behaviour in `tools/stylewriter/README.md` ("What it guesses"). If the driver finds no
printer or nothing reaches the pseudo-terminal, the design's fallback is Snow (a Mac Plus to
SE/30 emulator, which needs Wells's yes before it is installed) and, if neither works, writing down what
happened here. This task was never a merge gate.

## What `swcaptures` checks

For every capture: the log reads, swfake never ejected a page, and the bytes decode to a page
(decoded pages are kept in `<model>/<name>.pages/`). **FAIL** if not.

The command order, page data taken out, against what `sw_session` sends for the same model,
cartridge, quality and kind of page (swhost on a swfake): every difference is a **DIFF** line
and does not fail. Those lines, with the geometry ones (rect offset, band height, block length
against the cap), are what the controller reads to decide whether `sw_session.c` should
change; the check changes nothing.

The checker with `--dither apple`: AirWriter's page and Apple's, both cropped to their ink,
must have the same size to 2% and differ in at most 5% of the ink dots. **Those two figures
are guesses.** The first real capture replaces them in `swcaptures.py` (`TOLERANCE`) and in
this file, with the figures it measured.

Three questions from the reviews are answered from the captures, or say "not captured":

1. What the 2400 and 2500 drivers do when `2` answers with bit 80 (swfake always sets it on
   those two, as the design says the printers do): the job goes on, or it does not.
2. The page-mode letter (H or B) the 2200, 2400 and 2500 drivers send for a gray page on a
   colour cartridge. `sw_session` sends B.
3. Whether the row delta carries across `G` blocks within a band. Both readings of each
   capture are decoded and the one whose block joins look most like the rest of the page
   wins. It is a heuristic: it needs a rect that holds more than one block, and says "no
   evidence" when the two readings look the same.

## Checking the checks

`make -C tools/rip swcaptures-selftest` makes captures from the firmware's own conversation
(seven of them, about 45 seconds), runs the same checks over them, then changes a setup string
and a block and requires the first to be listed and the second refused. It says nothing about
Apple's drivers; it is there so the first real capture does not meet code that has never run.
Run on 2026-10-26: passes.
