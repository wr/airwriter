# AirWriter

AirWriter is a small dongle that turns an Apple ImageWriter II into an AirPrint printer. Plug it
into the printer's serial port, join it to your Wi-Fi, and your iPhone, iPad or Mac prints to the
ImageWriter like any modern printer, with no drivers and no computer in the middle.

It also speaks AppleTalk, so a vintage Mac can print to the same printer through Apple's
AppleTalk ImageWriter driver, over a LocalTalk cable or over Wi-Fi with EtherTalk. Basilisk II
and Infinite Mac can print to it too.

**Status:** rev A boards are built and printing from iPhones, Macs and vintage Macs, in colour
with Apple's colour ribbon. Rev B and a v1 release are next.

## How it works

- An ESP32-S3 runs an IPP Everywhere printer: Bonjour advertising, the IPP job protocol, and
  Apple's URF raster format.
- Each page is decoded, dithered to 1-bit, and turned into ImageWriter II graphics commands,
  then sent at 9600 baud with the printer's own hardware handshake, so the printer sets the pace.
- A 37-dot light panel shows what it's doing: Wi-Fi, ready, printing, out of paper, offline.
- A PIC12F1840 running [TashTalk](https://github.com/lampmerchant/tashtalk) joins the board to a
  LocalTalk network as a printer node.

## Using it

The [wiki](../../wiki) covers setup, the status lights, the DIP switches, settings, updates and
troubleshooting. You need an ImageWriter II and an 8-pin mini-DIN crossover printer cable, wired
like the one a Mac used (Apple's 590-0552 is one); straight-wired cables don't work. The
original ImageWriter and the ImageWriter LQ take the same commands and should work too, but
neither has been tested yet. The original has a 25-pin port, so it needs the Mac cable made for
that.

AirWriter can also share a PostScript LaserWriter it hears on AppleTalk (a LocalTalk cable or
the network) as a second AirPrint printer, beside the ImageWriter. That is built and tested
against a software LaserWriter, but not yet on a real one.

## Repository

| Path | What's there |
|---|---|
| `firmware/` | ESP-IDF application (ESP32-S3) |
| `tools/` | Host builds of the raster pipeline and IPP code, with tests and fixtures |
| `hardware/` | KiCad board and front panel |
| `enclosure/` | Case and light-guide lattice in build123d, with printable STLs in `out/` |
| `docs/` | Spec, design notes and traps |

Firmware builds with ESP-IDF 5.5 (`idf.py -C firmware build`). Host tests run anywhere with a C
compiler: `make -C tools/rip test regress`. `AGENTS.md` has the full toolchain notes.

## Licence

Firmware and tools are GPL-3.0-or-later, hardware and enclosure CERN-OHL-S-2.0, docs
CC-BY-SA-4.0. See [`LICENSE.md`](LICENSE.md), which also covers third-party material. The
AirWriter name and logo are not licensed: if you build and sell your own, please call it
something else.
