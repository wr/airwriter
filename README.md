# AirWriter

AirWriter lets your iPhone, iPad and Mac print to classic Apple printers: an ImageWriter on its
printer port, and PostScript LaserWriters on AppleTalk. It's a small board that plugs into the
printer's serial port and joins your Wi-Fi; your devices see ordinary AirPrint printers, with no
drivers and no computer in the middle.

It finds what is plugged in: it asks the printer on its port what it is, and listens on AppleTalk
(a LocalTalk cable, or EtherTalk over Wi-Fi) for LaserWriters, and shares each one it can
identify, up to four. It also speaks AppleTalk the other way, so a vintage Mac, Basilisk II or
Infinite Mac can print to the same printers, a LaserWriter on another link included.

**Status:** rev A boards are built and printing from iPhones, Macs and vintage Macs on an
ImageWriter II, in colour with Apple's colour ribbon. LaserWriters are built and tested against a
software LaserWriter, not yet a real one. Rev B and a v1 release are next.

## How it works

- An ESP32-S3 runs an IPP Everywhere printer: Bonjour advertising, the IPP job protocol, and
  Apple's URF raster format.
- Each page is decoded, dithered to 1-bit, and turned into ImageWriter II graphics commands,
  then sent at 9600 baud with the printer's own hardware handshake, so the printer sets the pace.
- A LaserWriter's page is ripped at 300 dpi into Level 1 PostScript and sent over AppleTalk PAP.
- A 37-dot light panel shows what it's doing: Wi-Fi, ready, printing, out of paper, offline.
- A PIC12F1840 running [TashTalk](https://github.com/lampmerchant/tashtalk) joins the board to a
  LocalTalk network as a printer node.

## Using it

The [wiki](../../wiki) covers setup, your printers, the status lights, the DIP switches, settings,
updates and troubleshooting. For an ImageWriter you need an 8-pin mini-DIN crossover printer
cable, wired like the one a Mac used (Apple's 590-0552 is one); straight-wired cables don't work.
The ImageWriter II is the one tested on paper. The original ImageWriter and the ImageWriter LQ
take the same commands and should work too, untested; the original has a 25-pin port, so it needs
the Mac cable made for that. A LaserWriter needs nothing but AppleTalk: a LocalTalk cable to
AirWriter's AppleTalk port, or the network.

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
