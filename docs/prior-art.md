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
