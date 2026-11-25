# Projects this builds on

- **TashTalk** (lampmerchant): a LocalTalk data-link engine that fits in a PIC12F1840,
  bit-banging SDLC and FM0 framing behind a 1 Mbps UART with CTS flow control. U7 runs it
  (2.1.3, `firmware/components/pic_prog/tashtalk-2.1.3.hex`, flashed in place by the ESP32).
- **TailTalk / TashTalkUSB** (FeralFirmware): the LocalTalk section's wiring is copied from
  TashTalkUSB's KiCad netlist node for node. TailTalk, an AppleTalk toolkit, emulates the
  ImageWriter LocalTalk Option Card and was checked against a real one
  (`tailtalk-core/src/imagewriter.rs`, `pap.rs`): it registers as `<name>:ImageWriter@*`,
  answers SendStatus with the card's status word (low byte first; a ready II with a colour
  ribbon reads 0x0080, deselected 0x0088, out of paper 0x00AA), holds one PAP connection, and
  passes the job through as raw ImageWriter command bytes. `components/localtalk` follows it.
  TailTalk 0.13.0 (2026-10-24) also prints to every StyleWriter from a computer, from a reading
  of Apple's Color StyleWriter 1500 and 2500 drivers (`tailtalk/src/stylewriter/*`, at
  `bf38658`): the command letters and their order per model, the margins, band heights and
  block caps. It is GPL-3.0 with no "or later" grant, so AirWriter reads it as a reference and
  uses its facts; `components/stylewriter` is written fresh. Its `lw300/README.md`, from
  Apple's LaserWriter 300 driver 1.1, documents most of the Personal LaserWriter 300's commands
  and agrees with what board 1 measured; `components/qdlaser` uses those facts and none of its
  code (W-1102). For the StyleWriter adapter (W-1193) its ADSP engine and its adapter role
  (`tailtalk-core/src/stylewriter.rs`, written from a decompile of Apple's 1500 and 2500
  drivers) gave the attention codes and their answers, and `tailtalk/tests/adsp_test.rs`
  replays a real Mac renaming a real adapter, packet by packet: the only capture of a real
  adapter there is. `test_sw_adapter.c` replays it byte for byte. Facts only; `adsp.c` and
  `sw_adapter.c` are written fresh.
- **lpstyl** (Monroe Williams, 1996-2000, BSD-2-Clause; github.com/Godzil/lpstyl): the public
  account of the StyleWriters' serial protocol, for seven models (`README.protocol`), and of
  Apple's StyleWriter network adapter (`README.atalk`). The identity query, the buffer
  accounting, the run coding with differencing and the CMYK planes are its. Its scanline coder
  is ported for the host tests only (`tools/rip/test/lpstyl_encode.c`, LICENSE.md), as a second
  coder that `swfake.py`'s decoder and ours are held to; the firmware has its own. Its
  `adsp.c` and `at_printer_*`, written from packet dumps of a real adapter printing, and
  `README.atalk` are the account of a real adapter that the board's is built to (W-1193): the
  order of a print, the 15 s sleep when busy, the Forward Reset at a job's end. The board
  takes a print in lpstyl's order as well as Apple's, and `etpeer.py --lpstyl` sends one.
  Nothing of it is ported there.
- **InkTalk** (FeralFirmware, in the TailTalk repository; firmware `GPL-3.0-or-later`): a board
  that stands in for Apple's StyleWriter network adapter, so a Mac's own driver prints through
  it, and has done so with real Macs. AirWriter's adapter (W-1193, SPEC §3.13) does what it
  does where the sources agree: a name registers on whatever socket it has, every attention
  is answered, `000E`'s answer is laid out its way, a pipe lost without `0012` gets
  `00 FF FF FF 49`, and a lone `x` or `h` is not passed on. It is also the reference for
  holding the printer's HSKi high so the printer answers (R9 on rev A), and for the printer's
  fast mode, whose clock is on HSKi and which AirWriter refuses. Nothing is ported.
- **Apple's StyleWriter drivers** (Color StyleWriter 1500, 2200 and 2500): the source of the
  tone curves, the 16 × 16 screen and the printer's icon, read out by
  `tools/stylewriter/mktables.py` (LICENSE.md). The drivers are never committed.
- **Apple's LaserWriter 300 driver 1.1** (Apple Developer CD, October 1993): the source of the
  300's 8 × 8 screen (`HTSC` 2), its curve (`GAMA` 3) and the image area of each
  paper size, read out by `tools/qdlaser/mkqdtables.py` (LICENSE.md), and of the commands
  TailTalk's notes leave out. The driver is never committed.
- **Previous** (the NeXT emulator, `src/printer.c`): its model of a Canon laser engine's print
  request and vertical sync, read as background on the 300's Canon engine. Nothing is ported.
- **tashrouter** (lampmerchant): the reference TashTalk host. Its start-up (1024 NULs, an empty
  node bitmap, features off) and node acquisition are what `localtalk.c` copies.
- **AirTalk**: ESP32 LocalTalk-over-Wi-Fi dongles. Relevant if mode 2 or a Wi-Fi tunnel is ever
  built; we would need to match its wire protocol to interoperate.
- **ImageScribbler** (polpo): a 1:1 clone of Apple's ImageWriter II/LQ LocalTalk Option Card
  (Apple's ROM on a 65C02 and Z8530; CC0; JCM-1 sells it assembled). It does what mode 3 does,
  with no protocol risk, but needs vintage silicon and lives inside the printer. Its ROM image
  (341-0034-B) is publicly archived and is the authoritative answer to what PAP delivers. TailTalk
  says raw ImageWriter commands; `GET /api/lt/job` captures what a real Mac's driver sends, which
  settles it for the printing work.
- **Ghostscript `gdevadmp.c`** and **OpenPrinting `cups/raster-stream.c`**: independent checks for
  the graphics bit order, the density escapes and the URF header (SPEC §4).
