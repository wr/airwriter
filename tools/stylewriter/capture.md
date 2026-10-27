<!-- SPDX-License-Identifier: CC-BY-SA-4.0 -->
# Capturing Apple's StyleWriter drivers

Part of `W-1103 StyleWriters on the printer port`. The idea: run Apple's own drivers in an
emulated Mac whose printer port is `swfake.py`'s pseudo-terminal, keep the conversation, and
hold it to what the firmware sends (`tools/rip/test/swcaptures.py`). It tells us where Apple's
drivers and TailTalk's reading (which `sw_session.c` follows) part, and gives the first Apple
page to compare AirWriter's `--dither apple` against.

**State, 2026-10-26: not run. The rig stops at a 68k Mac ROM, and then at getting the checker
image into the Mac at the right size (both below).** No capture exists, so
`make -C tools/rip swcaptures` prints a skip line. Nothing from Apple is in the repository:
the drivers live in `~/.config/airwriter/sw-drivers/`, the captures go in
`~/.config/airwriter/sw-captures/`.

## What is on this Mac (checked 2026-10-26)

| Piece | Where | State |
|---|---|---|
| Basilisk II | `~/Projects/macemu/BasiliskII/src/Unix/BasiliskII` | Built 2026-10-20, arm64, SDL2 from Homebrew. Runs. With no ROM it prints `ERROR: Cannot open ROM file.` and exits. |
| Its prefs | `~/.basilisk_ii_prefs` | Names `/Users/wells/Downloads/ROM`, `/Users/wells/Downloads/mini.dsk` and `/Users/wells/Downloads/cd.iso`, with `extfs /Users/wells/Downloads`. None of the three files exists. |
| Mac OS 8.1 | `~/Projects/macintosh-mini/disk-image/out/macos-8.1.hda` | 1 GB raw HFS, bootable, volume "Macintosh HD". The install CD is `.cache/MacOS8_1.iso` beside it. Mac OS 8 wants a 68040: `modelid 14`. macOS cannot mount it (classic HFS: `hdiutil` says "image not recognized"), so what is on it cannot be listed from here. |
| The drivers | `~/.config/airwriter/sw-drivers/unpacked/raw/` | 800 KB HFS images of the installer disks: `sw1500-disk1..3.img` (Color SW 1500 2.2.1, which also covers the original, II and 1200) and `cs2500-disk1..4.img` (Color SW 2500 2.2.1, which covers the 2200, 2400 and 2500). Color SW 2200 2.1.2 is a StuffIt archive, unpacked to Tome files under `unpacked/cs2200/`, with no disk image. |
| A 68k ROM | nowhere | Searched the home folder for files of exactly 256 KB, 512 KB, 1, 2, 3 and 4 MB: none. |
| hfsutils, Snow | not installed | Not needed if the routes under "Getting files into the Mac" work. |
| UTM, `qemu-system-m68k` | installed | A `q800` would also need a Quadra ROM. No help. |

Basilisk II did not have to be built: the binary above is current.

## What it needs from Wells

1. **A 68k ROM**, 512 KB or 1 MB, from a Mac IIci or a Quadra (`macintosh-mini` calls
   `064DC91D` a common one). Apple's ROMs are not downloaded for this and nothing here fetches
   one. Either Wells points at a file, or he says yes to copying `~/ROM` off the Raspberry Pi at
   10.0.1.110 (it has one if the Mac Mini build there is set up as its guide says; the Pi was
   not touched).
2. **Possibly an application that prints an image at a known size**, depending on what is on
   the Mac OS 8.1 disk (see "The checker and the photo"). The first two routes under "Getting
   files into the Mac" need no install or download; even so, only the text page is certain to
   get through them.
3. Nothing else is needed from Wells for the System disk or the drivers: both are on this Mac.
   A System 7.5 disk, which the brief assumed, is not, and none will be fetched.

## The rig, once there is a ROM

Untried past the ROM check. The steps follow the Basilisk II source; the unknown is whether
the Apple drivers talk through the Serial Driver (which Basilisk emulates) at all.

1. Copy the System disk, so the original stays as it is, and make a prefs file of its own
   (run Basilisk II with `--config FILE`):

   ```
   rom /Users/wells/Documents/sw-rig/ROM
   disk /Users/wells/Documents/sw-rig/macos-8.1-copy.hda
   disk /Users/wells/.config/airwriter/sw-drivers/unpacked/raw/sw1500-disk1.img
   disk /Users/wells/.config/airwriter/sw-drivers/unpacked/raw/sw1500-disk2.img
   disk /Users/wells/.config/airwriter/sw-drivers/unpacked/raw/sw1500-disk3.img
   disk /Users/wells/.config/airwriter/sw-drivers/unpacked/raw/cs2500-disk1.img
   disk /Users/wells/.config/airwriter/sw-drivers/unpacked/raw/cs2500-disk2.img
   disk /Users/wells/.config/airwriter/sw-drivers/unpacked/raw/cs2500-disk3.img
   disk /Users/wells/.config/airwriter/sw-drivers/unpacked/raw/cs2500-disk4.img
   extfs /Users/wells/Documents/sw-rig/shared
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

3. In the Mac: run the Installer from the driver's first disk, restart, choose the driver in
   the Chooser and **Printer Port**. Each driver runs only here, never on this computer.
   **Versions.** The drivers are 1996-97 (Color SW 1500 and 2500 2.2.1 are dated January 15,
   1997; Color SW 2200 2.1.2 is 1996), and the System disk on hand is Mac OS 8.1 (January 1998);
   the brief assumed System 7.5. Mac OS 8.0 came out six months after these drivers, and their
   readme speaks of System 7.5 and earlier. They are expected to work, because the driver
   itself is one Chooser file (type `PRER`, creator `auro`) and Chooser drivers written for
   System 7 are what Mac OS 8's Chooser lists. That is a reading, not a fact for these three,
   and nothing on this Mac can confirm it until there is a ROM. If the Installer gives trouble:
   * It refuses, or says the System is too new or not one it knows: try a Custom Install of the
     printer software only. The disks also carry 1997 copies of Finder, Chooser, Desktop
     PrintMonitor, Thread Manager and Drag and Drop meant for older Systems (the 2500's disks 2
     and 3, as `untome.py` lists them); the Installer normally compares versions before it
     replaces anything, but say no to any step that offers to replace the System's own parts.
   * It still refuses: skip it and put the driver file in by hand. `untome.py` already wrote it
     as `unpacked/drivers/cs2500.data` and `.rsrc` (its two forks; likewise `cs2200`, `sw1500`).
     In the shared folder (below) make `Color SW 2500` from the data fork, `.rsrc/Color SW 2500`
     from the resource fork, and `.finf/Color SW 2500` of 16 bytes: `PRER`, `auro`, two zero
     bytes of flags, `ff ff ff ff` for the location, two zero bytes of folder (from reading
     `get_finfo` in Basilisk's source; not run). Drag it from that volume into the System
     Folder's Extensions folder and restart. `untome.py` prints the 1500's and 2200's type and
     creator when it unpacks their disks and Tome.
   * Last resort: an older System (7.5 or 7.6) on a disk of its own. There is none on this Mac,
     and none will be downloaded; it would be Wells's to supply.
4. Print the page set on each driver, restarting swfake (a new log name, a new pty) between
   prints: a blank page, the checker, a text page, and for the colour drivers a colour photo.
   Name the log for what was printed: `<page>[-colour|-black][-q3|-q4|-q5][-a4|-legal].log` in
   `<model>/`, where `<page>` is `blank`, `checker`, `text` or `photo`, the cartridge defaults
   to black and the quality to 4. The model is the one the fake was started as (`original`,
   `ii`, `1200`, `1500`, `2200`, `2400`, `2500`); the 2200 can be had from either 2200 2.1.2
   or the 2500's 2.2.1. How the pages get into the Mac is the next section.
5. `make -C tools/rip swcaptures`.

The Mac asks the printer at 57600 baud; swfake answers as the model it was started as, with
the behaviour in `tools/stylewriter/README.md` ("What it guesses"). If the driver finds no
printer or nothing reaches the pseudo-terminal, the design's fallback is Snow (a Mac Plus to
SE/30 emulator, which needs Wells's yes before it is installed) and, if neither works,
writing down what happened here. This task was never a merge gate.

## Getting files into the Mac

The gap that decides whether the checker can be captured at all. Everything a page or a driver
needs is a file with a Mac type and creator, and sometimes a resource fork, written from this
computer. The first two routes need nothing installed or downloaded; the last two do.

| Route | Needs | State |
|---|---|---|
| **The shared folder.** Basilisk II's `extfs` pref names a folder on this Mac that the Mac shows as a volume. Its type and creator come from the file's extension (its table has `.pict` as `PICT`, `.gif`, `.jpg`, `.tif`, `.png` and `.bmp` as images, `.txt` as `TEXT`/`ttxt`, `.sit`, `.hqx`) or, overriding that, from a 16-byte `.finf/NAME` file beside it; a resource fork comes from `.rsrc/NAME`. | Nothing: a Python script writes the files. Use a folder of its own, not `Downloads`. | Read in the source (`extfs_unix.cpp`, `get_finfo`); not run. The first thing to try. |
| **An HFS+ disk image made here.** `hdiutil create -size 8m -fs HFS+ -layout NONE -volname SWPAGES FILE` writes a bare HFS+ volume. Mount it with `hdiutil attach`, copy files in, set type and creator with `xattr -wx com.apple.FinderInfo` (the first eight bytes are the type and the creator) and a resource fork by copying to `FILE/..namedfork/rsrc`, then detach. Add it to the prefs as a `disk` line. | Nothing. Mac OS 8.1 reads HFS+. | Made, written to and read back on this Mac on 2026-10-26: the type `PICT` and creator `ttxt` survived a detach and a re-attach. Not mounted in the emulator. The fallback if the shared folder does not behave. |
| **hfsutils** (`brew install hfsutils`) to write classic HFS images such as the `transfer.dsk` Wells keeps. | Wells's yes to the install. | Not installed. Not needed if either route above works. |
| **Snow**, a different emulator, if the Apple drivers do not reach Basilisk's serial port. | Wells's yes, a download and a ROM. | Not installed; nothing says it will be needed. |

## The checker and the photo

The text page is a `.txt` file and gets through the shared folder as it is: SimpleText prints it.
The checker and the photo are images, and that is the open part.

The checker is 2 x 1 in: 288 x 144 pixels at 144 dpi, in 16-pixel squares, black and white
only. QuickDraw draws an image with no resolution in it at 72 dpi, so the same picture as
144 x 72 pixels with 8-pixel squares (the checker halved with nearest-neighbour is exactly
that) should print at 2 x 1 in.

* **Writing the image.** Pillow, which is installed, writes GIF, PNG and TIFF, which the shared
  folder types. It cannot write PICT. Two hand-written PICTs of the checker (version 1, then
  version 2, both `PackBitsRect`) were tried on 2026-10-26: `sips` read them as the right size
  and wholly white, so there is no host-side proof yet that a PICT writer is right. It was a
  scratch attempt; none of it is in the repository.
* **Printing it from the Mac.** Which program opens and prints an image at its pixel size is
  not known. SimpleText opens PICT (and with QuickTime may open other image types); a web
  browser prints an image at its pixel size. Whether any of them is on the Mac OS 8.1 disk cannot
  be seen from here (macOS will not mount it). The `macintosh-mini` disk-image build unpacks
  Internet Explorer 4.01, iCab and Mr. Browser for 68k, so they are probably on it, not
  confirmed. The first thing to do in the running Mac is look in its Applications folder.
* **If nothing there prints an image at a known size**, the checker cannot be captured, and the
  tolerance check has no Apple page to meet. Then it needs an application from Wells (a
  drawing program, a browser) copied in by the shared folder, or a PICT that has been shown to
  be right. The text, blank and colour-photo captures, and the three open questions, do not
  depend on the checker, except the photo, which needs an image the same way.

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
