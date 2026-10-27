# AirWriter

Add AirPrint to your Apple ImageWriter, StyleWriter or LaserWriter. AirWriter plugs into the
printer, joins your Wi-Fi, and your printers show up on your phone and computer like any other
printer. Nothing to install.

Vintage Macs can print to the same printers over AppleTalk, from a LocalTalk cable, your network,
Basilisk II or Infinite Mac. That part runs on Tashtari's
[TashTalk](https://github.com/lampmerchant/tashtalk).

Rev A boards print from iPhones, Macs and vintage Macs to an ImageWriter II, in color with Apple's
color ribbon, and to a Personal LaserWriter NTR. Assembled boards are at
[Wells Workshop](https://shop.wells.ee/products/airwriter).

## Using it

The [wiki](../../wiki) has setup, printers, the status lights, settings and troubleshooting.

You need:

- **A printer.** An ImageWriter II, ImageWriter LQ or ImageWriter, a StyleWriter, or a PostScript
  LaserWriter. The wiki's Supported Printers page has each model.
- **For an ImageWriter or StyleWriter, a crossover cable.** An 8-pin mini-DIN cable wired like
  Apple's 590-0552. Straight-through cables won't work. The original ImageWriter needs the 25-pin
  Mac cable.
- **For a LaserWriter, a connection.** LocalTalk cabling to AirWriter's AppleTalk port, or your
  network.
- **USB-C power and 2.4 GHz Wi-Fi.**

Firmware updates are on the [Releases page](../../releases), and AirWriter can install them itself.

## How it works

- An ESP32-S3 runs an IPP Everywhere printer for each of your printers: Bonjour, IPP and Apple's
  URF raster format.
- ImageWriter pages are dithered and sent as the printer's own graphics commands at 9600 baud,
  paced by the printer's handshake. Color pages print in four passes on the color ribbon.
- StyleWriter pages are screened at 360 dpi, run-length coded and sent at 57600 baud, in color
  with a color cartridge.
- LaserWriter pages go out as 300 dpi PostScript over AppleTalk.
- A PIC12F1840 running TashTalk handles LocalTalk, and EtherTalk runs over Wi-Fi.
- A 37-dot light panel shows the state of things.

## Repository

| Path | Contents |
|---|---|
| `firmware/` | ESP-IDF application (ESP32-S3) |
| `tools/` | Host builds of the raster and IPP code, tests, fixtures and the settings page |
| `hardware/` | KiCad board and front panel |
| `enclosure/` | Case and light-guide lattice in build123d, with printable STLs in `out/` |

Firmware builds with ESP-IDF 5.5 (`idf.py -C firmware build`). Host tests run anywhere with a C
compiler: `make -C tools/rip test regress` and `make -C tools/ipp test`. `AGENTS.md` has the full
toolchain notes.

## Credits

The StyleWriter support rests on other people's work. Monroe Williams worked out the printers'
serial protocol in the 1990s and wrote [lpstyl](https://github.com/Godzil/lpstyl). FeralFirmware
took Apple's drivers apart for [TailTalk](https://github.com/FeralFirmware/TailTalk) and InkTalk.
Thanks to Monroe Williams, FeralFirmware and Bolacore.

## License

Firmware and tools are GPL-3.0-or-later, hardware and enclosure CERN-OHL-S-2.0, docs CC-BY-SA-4.0.
See [`LICENSE.md`](LICENSE.md), which also covers third-party material.
