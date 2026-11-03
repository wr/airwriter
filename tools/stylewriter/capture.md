<!-- SPDX-License-Identifier: CC-BY-SA-4.0 -->
# Capturing Apple's StyleWriter drivers

Part of `W-1103 StyleWriters on the printer port`. The idea: run Apple's own drivers in an
emulated Mac whose printer port is `swfake.py`'s pseudo-terminal, keep the conversation, and
hold it to what the firmware sends (`tools/rip/test/swcaptures.py`). It tells us where Apple's
drivers and TailTalk's reading (which `sw_session.c` follows) part, and gives the first Apple
page to compare AirWriter's `--dither apple` against.

**State, 2026-10-26: not run. The rig stops at a 68k Mac ROM.** Everything else it needs is on
this Mac, Apple's Color SW 1500, Color SW 2500 and StyleWriter 1200 drivers included, already
installed on the Mac OS 8.1 disk. No capture exists, so `make -C tools/rip swcaptures` prints a
skip line. Nothing from Apple is in the repository:
the drivers live in `~/.config/airwriter/sw-drivers/`, the captures go in
`~/.config/airwriter/sw-captures/`.

## What is on this Mac (checked 2026-10-26)

| Piece | Where | State |
|---|---|---|
| Basilisk II | `~/Projects/macemu/BasiliskII/src/Unix/BasiliskII` | Built 2026-10-20, arm64, SDL2 from Homebrew. Runs. With no ROM it prints `ERROR: Cannot open ROM file.` and exits. |
| Its prefs | `~/.basilisk_ii_prefs` | Names `/Users/wells/Downloads/ROM`, `/Users/wells/Downloads/mini.dsk` and `/Users/wells/Downloads/cd.iso`, with `extfs /Users/wells/Downloads`. None of the three files exists. |
| Mac OS 8.1 disk | `~/Projects/macintosh-mini/disk-image/out/macos-8.1.hda` | 1 GB raw classic HFS, bootable, volume "Macintosh HD". Mac OS 8 wants a 68040: `modelid 14`. macOS cannot mount it (`hdiutil`: "image not recognized"); `machfs` reads it (next row). Listed read-only on 2026-10-26 (818 files and folders). **Applications:** SimpleText (`Applications`), iCab and MR Browser 68K v0.39 (`Internet`), Disk Copy and StuffIt Expander (`Utilities`). No Internet Explorer. **Printing:** Chooser, Desktop PrintMonitor and QuickTime are installed. **Printer drivers, all in `System Folder:Extensions`:** `Color SW 1500` (`PRER chle`), `Color SW 2500` (`PRER auro`), `StyleWriter 1200` (`PRER dblo`), `Color SW Pro`, `AppleTalk ImageWriter`, `LaserWriter 8`, `LaserWriter 300/LS`. No `Color SW 2200`. The install CD is `.cache/MacOS8_1.iso` beside it; no `Color SW` or `StyleWriter` file shows on it by name (its installer pieces are compressed). |
| `machfs` 1.3, `macresources` | `~/Projects/macintosh-mini/disk-image/.venv` (run `.venv/bin/python`) | Installed already: it is what builds Wells's `transfer.dsk`. Reads `macos-8.1.hda` and writes classic HFS images with a type, a creator and both forks. Run on 2026-10-26: it listed the 8.1 disk, and wrote a 2 MB image with a file of type `PICT` and creator `ttxt`, which read back as that. |
| The driver downloads | `~/.config/airwriter/sw-drivers/unpacked/` | `raw/` has 800 KB HFS images of the installer disks: `sw1500-disk1..3.img` (Color SW 1500 2.2.1, which covers the original, II, 1200 and 1500) and `cs2500-disk1..4.img` (Color SW 2500 2.2.1, which covers the 2200, 2400 and 2500). `drivers/` has the driver files `untome.py` took out of the Tomes, two forks each: `sw1500`, `cs2500` and `cs2200` (Color SW 2200 2.1.2, `PRER sqid`, for the 2200 alone; a StuffIt archive unpacked to Tome files, with no disk image). The disk's Color SW 1500 and 2500 have data forks byte-identical to `sw1500.data` and `cs2500.data` and resource forks 13 bytes shorter than `sw1500.rsrc` and `cs2500.rsrc`. |
| A 68k ROM | nowhere | Searched the home folder for files of exactly 256 KB, 512 KB, 1, 2, 3 and 4 MB: none. |
| hfsutils, Snow | not installed | Neither is needed: `machfs` does hfsutils' job. |
| UTM, `qemu-system-m68k` | installed | A `q800` would also need a Quadra ROM. No help. |

Basilisk II did not have to be built: the binary above is current.

## What it needs from Wells

1. **A 68k ROM**, 512 KB or 1 MB, from a Mac IIci or a Quadra (`macintosh-mini` calls
   `064DC91D` a common one). Apple's ROMs are not downloaded for this and nothing here fetches
   one. Either Wells points at a file, or he says yes to copying `~/ROM` off the Raspberry Pi at
   10.0.1.110 (it has one if the Mac Mini build there is set up as its guide says; the Pi was
   not touched).
2. Nothing else is known to be needed. The System disk has the drivers, SimpleText and two
   browsers; `machfs` puts pages and files on a disk image. Whether the Mac OS 8.1 programs
   print the checker at the right size is not known until the Mac runs (see "The checker and
   the photo"); nothing asks Wells for anything until all of them have failed.

## The rig, once there is a ROM

Untried past the ROM check. The steps follow the Basilisk II source; the unknown is whether
the Apple drivers talk through the Serial Driver (which Basilisk emulates) at all.

1. Copy the System disk, so the original stays as it is, make a disk of pages (below), and
   make a prefs file of its own (run Basilisk II with `--config FILE`):

   ```
   rom /Users/wells/Documents/sw-rig/ROM
   disk /Users/wells/Documents/sw-rig/macos-8.1-copy.hda
   disk /Users/wells/Documents/sw-rig/pages.hfs
   modelid 14
   cpu 4
   fpu true
   ramsize 67108864
   screen win/800/600
   nogui true
   nosound true
   seriala /dev/null
   serialb /Users/wells/Documents/sw-rig/printer-port
   ```

   Every path is in full. Basilisk II does not expand `~` or `$HOME` in a pref (nothing in its
   Unix sources does; `HOME` is used only to find the prefs file), so a `~` path names a file
   that does not exist and that disk does not mount. `/Users/wells/Documents/sw-rig/` stands
   for a folder of your own and `/Users/wells` for your home folder.

   `serialb` is the Mac's printer port. Basilisk opens it with a plain `open()` when Mac OS
   opens the port, not at start, so `printer-port` can be a symlink that is pointed at a new
   pseudo-terminal before each print. The `|command` form (a process on a pty) is not used:
   swfake does not speak stdio.
2. Start the fake as the model the driver is for, one log per print (swfake appends to its
   log, so a reused name mixes two prints):

   ```sh
   mkdir -p ~/.config/airwriter/sw-captures/2500
   python3.14 tools/stylewriter/swfake.py serve 2500 --pty --cart colour \
       --log ~/.config/airwriter/sw-captures/2500/checker-colour-q4.log
   ln -sf /dev/ttysNNN /Users/wells/Documents/sw-rig/printer-port   # the path swfake printed first
   ```

3. In the Mac: open the Chooser, pick the driver, then **Printer Port**. The disk has
   `Color SW 2500` (for the 2200, 2400 and 2500), `Color SW 1500` (the original, II, 1200 and
   1500) and `StyleWriter 1200`. No Installer is needed. Each driver runs only here, never on
   this computer.
   **Versions.** The drivers are 1997 (the 2.2.1 packages are dated January 15, 1997) and the
   plan assumed System 7.5; the disk is Mac OS 8.1. For Color SW 1500, Color SW 2500 and
   StyleWriter 1200 the disk answers that: they are installed in its Extensions folder. Their
   resource forks are 13 bytes shorter than the 2.2.1 downloads' (the data forks are identical),
   so what runs is not byte for byte what the tables were read from; why is not known.
   Not on the disk: Color SW 2200 (2.1.2, for the 2200 alone; the installed Color SW 2500 also
   drives a 2200) and the 2.2.1 files exactly as downloaded. If either is wanted, do not run
   the Installer: its disks also carry 1997 copies of Finder, Chooser, Desktop PrintMonitor,
   Thread Manager and Drag and Drop meant for older Systems. Put the driver file in by hand
   instead: `untome.py` already wrote it as `unpacked/drivers/cs2200.data` and `.rsrc` (or
   `sw1500`, `cs2500`), its type and creator being `PRER sqid` for the 2200 and `PRER auro`
   for the 2500. Make it a file of the pages disk with machfs (`f.type = b"PRER"`,
   `f.creator = b"sqid"`, `f.data`, `f.rsrc` from the two files), drag it into the System
   Folder's Extensions folder and restart. Whether the 2200's own driver runs under
   Mac OS 8.1 is not known.
4. Print the page set on each driver, restarting swfake (a new log name, a new pty) between
   prints: a blank page, the checker, a text page, and for the colour drivers a colour photo.
   Name the log for what was printed: `<page>[-colour|-black][-q3|-q4|-q5][-a4|-legal].log` in
   `<model>/`, where `<page>` is `blank`, `checker`, `text` or `photo`, the cartridge defaults
   to black and the quality to 4. The model is the one the fake was started as (`original`,
   `ii`, `1200`, `1500`, `2200`, `2400`, `2500`). How the pages get into the Mac is the next
   section.
5. `make -C tools/rip swcaptures`.

The Mac asks the printer at 57600 baud; swfake answers as the model it was started as, with
the behaviour in `tools/stylewriter/README.md` ("What it guesses"). If the driver finds no
printer or nothing reaches the pseudo-terminal, the design's fallback is Snow (a Mac Plus to
SE/30 emulator, which needs Wells's yes before it is installed) and, if neither works,
writing down what happened here. This task was never a merge gate.

## Getting files into the Mac

Everything a page or a driver needs is a file with a Mac type and creator, and sometimes a
resource fork, written from this computer. The first three routes need nothing installed or
downloaded.

| Route | Needs | State |
|---|---|---|
| **A disk image made with `machfs`.** Run `~/Projects/macintosh-mini/disk-image/.venv/bin/python`: `from machfs import Volume, File`; `v = Volume(); v.name = "SWPAGES"`; `f = File(); f.type, f.creator, f.data, f.rsrc = b"TEXT", b"ttxt", ..., b""`; `v["text page"] = f`; write `v.write(size=8 * 1024 * 1024, bootable=False)` to `pages.hfs`. Text files use CR line ends. Add it to the prefs as a `disk` line. | Nothing. | Written and read back on this Mac on 2026-10-26, type and creator intact (a classic HFS image: the signature at byte 1024 is `BD`). Not mounted in the emulator. The first thing to use. |
| **The shared folder.** Basilisk II's `extfs` pref names a folder on this Mac that the Mac shows as a volume. Its type and creator come from the file's extension or, overriding that, from a 16-byte `.finf/NAME` file beside it; a resource fork comes from `.rsrc/NAME`. A `.pict` gets `PICT` with creator `ogle`, which is not on the 8.1 disk: open it from SimpleText's **File > Open**, or write a `.finf` with `ttxt`. A `.txt` gets `TEXT`/`ttxt`. | Nothing; a folder of its own, not `Downloads`. | Read in the source (`extfs_unix.cpp`, `get_finfo`); not run. |
| **An HFS+ disk image made with `hdiutil`.** `hdiutil create -size 8m -fs HFS+ -layout NONE -volname SWPAGES FILE` writes a bare HFS+ volume; `hdiutil attach` it, copy files in, set type and creator with `xattr -wx com.apple.FinderInfo` (its first eight bytes), and a resource fork by copying to `FILE/..namedfork/rsrc`. Mac OS 8.1 reads HFS+. | Nothing. | Made, written to and read back here on 2026-10-26: type `PICT` and creator `ttxt` survived a detach and a re-attach. Not mounted in the emulator. Only needed if `machfs` fails. |
| **hfsutils** (`brew install hfsutils`). | Wells's yes to the install. | Not installed, and not needed. |
| **Snow**, a different emulator, if the Apple drivers do not reach Basilisk's serial port. | Wells's yes, a download and a ROM. | Not installed; nothing says it will be needed. |

## The checker and the photo

The text page is a `TEXT` file with creator `ttxt` and SimpleText prints it. The checker and
the photo are images, and printing them at the right size is the open part.

The checker is 2 x 1 in: 288 x 144 pixels at 144 dpi, in 16-pixel squares, black and white
only. QuickDraw draws an image with no resolution in it at 72 dpi, so the same picture as
144 x 72 pixels with 8-pixel squares (the checker halved with nearest-neighbour is exactly
that) should print at 2 x 1 in.

* **Writing the image.** Pillow, which is installed, writes GIF, PNG and TIFF; `machfs` can
  give any of them a Mac type. It cannot write PICT. Two hand-written PICTs of the checker
  (version 1, then version 2, both `PackBitsRect`) were tried on 2026-10-26: `sips` read them as
  the right size and wholly white, so there is no host-side proof yet that a PICT writer is
  right. It was a scratch attempt; none of it is in the repository. QuickTime is installed on
  the 8.1 disk, so SimpleText may open a GIF or a JPEG (`GIFf`, `JPEG`) through it; that is for
  the running Mac to say.
* **Printing it from the Mac.** Three programs on the disk can show an image: SimpleText, iCab
  and MR Browser 68K. Whether SimpleText prints a PICT or a GIF at its pixel size (rather than
  fitted to the page), and what iCab and MR Browser do, is not known. Try SimpleText first,
  then iCab, then MR Browser, and check the size of the ink on the page.
* **If none of them prints an image at a known size**, the checker cannot be captured, and the
  tolerance check has no Apple page to meet. It would then need a PICT that has been shown to be
  right, or another program, and that is the point at which to ask Wells. The text, blank and
  the other captures, and the three open questions, do not depend on the checker; the colour
  photo needs an image the same way.

## What `swcaptures` checks

For every capture: the log reads, swfake never ejected a page, and the bytes decode to a page
(decoded pages are kept in `<model>/<name>.pages/`). **FAIL** if not.

The command order, page data taken out, against what `sw_session` sends for the same model,
cartridge, quality and kind of page (swhost on a swfake): every difference is a **DIFF** line
and does not fail. Those lines, with the geometry ones (rect offset, band height, block length
against the cap), are what to read when deciding whether `sw_session.c` should change, citing
the capture; the check changes nothing.

The checker with `--dither apple`: AirWriter's page and Apple's, both cropped to their ink,
must have the same size to 2% and differ in at most 5% of the ink dots. **Those two figures
are guesses.** The first real capture replaces them in `swcaptures.py` (`TOLERANCE`) and in
this file, with the figures it measured.

Three open questions are answered from the captures, or say "not captured":

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
