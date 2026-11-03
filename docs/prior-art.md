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
  uses its facts; `components/stylewriter` is written fresh.
- **lpstyl** (Monroe Williams, 1996-2000, BSD-2-Clause; github.com/Godzil/lpstyl): the public
  account of the StyleWriters' serial protocol, for seven models (`README.protocol`), and of
  Apple's StyleWriter network adapter (`README.atalk`). The identity query, the buffer
  accounting, the run coding with differencing and the CMYK planes are its. Its scanline coder
  is ported for the host tests only (`tools/rip/test/lpstyl_encode.c`, LICENSE.md), as a second
  coder that `swfake.py`'s decoder and ours are held to; the firmware has its own.
- **InkTalk** (FeralFirmware, in the TailTalk repository; firmware `GPL-3.0-or-later`): a board
  that stands in for Apple's StyleWriter network adapter, so a Mac's own driver prints through
  it. It is the reference for the adapter's ADSP exchange (phase 2 of W-1103, not built), for
  holding the printer's HSKi high so the printer answers (R9 on rev A), and for the printer's
  fast mode, whose clock is on HSKi. Nothing is ported.
- **Apple's StyleWriter drivers** (Color StyleWriter 1500, 2200 and 2500): the source of the
  tone curves, the 16 × 16 screen and the printer's icon, read out by
  `tools/stylewriter/mktables.py` (LICENSE.md). The drivers are never committed.
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
