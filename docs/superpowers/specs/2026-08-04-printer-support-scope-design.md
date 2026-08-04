# Printer support scope

Written 2026-08-04. Decides which printers AirWriter's firmware targets, in what order, and
what that does to the firmware's shape. It does not describe how to build any of it — that
belongs in the implementation plan.

## The decision

**v1 supports the ImageWriter II over serial, and nothing else.**

The StyleWriter family and the ImageWriter LQ are cut. LocalTalk is built second, after the
serial path prints. No printer-driver abstraction is written until a second printer is
physically on the bench.

## Why the ImageWriter II alone

Three families were candidates. Each was assessed on whether the protocol is documented,
whether the hardware can drive it, and whether an owner can still buy supplies for it. The
third question turned out to be the decisive one.

**ImageWriter II — in.** Apple published a technical reference and it has been scanned in
full. Rev A's printer port was designed for it. Black ribbons cost about ten dollars and are
plentiful, partly because the cartridge shell survived into the receipt-printer supply
chain. Tractor paper is a live commodity, and the printer also takes friction feed. One
sitting on the desk, which means everything can be verified.

**ImageWriter LQ — out.** Same connector, same electrical interface, and the command set is
a superset of the II's. But its ribbon cartridges are effectively unobtainable, and the
graphics commands it adds — `ESC C`, `ESC U`, `ESC a 3` — appear in no Apple document
anywhere. Ghostscript's `gdevadmp.c` is the only source for them. Supporting the LQ would
mean building on one secondary source, for a printer nobody can feed.

**StyleWriter family — out, and this one is a genuine loss.** Monroe Williams reverse
engineered the wire format in 1996 and `lpstyl` documents it: the identity query, the buffer
accounting, the RLE-with-differencing compression, the CMYK planes. Rev A drives it with no
board change — 57600 baud, RS-422, no handshaking, and the receive path already exists.
Technically it is the most interesting target in the project.

It fails on supplies. The print head *is* the cartridge, and nobody has manufactured one
since roughly 2005. Everything sold as remanufactured is a thirty-year-old head refilled.
The best-documented recent restoration — a Color StyleWriter 2200, 2024 — ended with the
owner buying a second printer as a donor for a corroded carriage cable, after ultrasonic
cleaning failed to revive the head. Ink is not the problem and refill kits are still sold.
The head is the problem, and firmware cannot fix a dead head.

**LaserWriter — later, and better placed than expected.** Toner is the healthiest of the
three, because Apple used Canon engines shared with HP LaserJets. The LaserWriter II family
takes the same cartridge as a LaserJet II or III, currently about $70 for a new-build
two-pack. Two corrections to the obvious assumption: the LaserWriter Select 300/310/360 is a
Fuji Xerox engine with no HP equivalent, and the 92298A that the Pro 600 and 16/600 need has
been dropped by most remanufacturers. It is cartridge by cartridge, not family wide.

What gates the LaserWriter is not toner. It is fuser gears, capacitor plague, and forty-five
pounds of shipping weight. Owning one is a restoration hobby.

## Why LocalTalk is second, not cut

The board already carries the LocalTalk port, so this is a firmware question only. The
hardware cost is sunk in Rev A.

Two things made deferral the easy call. First, the payload is identical. The ImageWriter II
LocalTalk Option Card takes the plain escape-code stream wrapped in PAP — not PostScript —
so `stream.bin` goes over the network byte for byte. Confidence is roughly 95%, resting on
four independent lines: the option card's clone has 8KB of SRAM and an 8-bit CPU, which
cannot hold a PostScript interpreter; netatalk's `pap` performs no transformation on its
input and gained ImageWriter support in 2.3.2; a packet capture posted to 68kMLA; and
Apple's own Mac-side driver is the QuickDraw ImageWriter driver with a different transport
bolted on.

Second, the LocalTalk path sends its job *out through the serial port* to the printer. The
serial output stage is therefore a prerequisite. Building LocalTalk first would mean
building on something untested.

**The compelling direction is the server side, not the client side.** Not AirWriter printing
to a LaserWriter, but a vintage Mac printing to a serial ImageWriter through AirWriter:

```
SE/30 --LocalTalk--> AirWriter --serial--> ImageWriter II
iPhone --WiFi-------^
```

AirWriter registers on LocalTalk as an NBP `ImageWriter`, accepts a PAP connection from the
Chooser, and forwards the stream out the serial port. That is what the AppleTalk Option Card
does, except external, and the cards are scarce enough that polpo cloned one. It is also
testable with hardware already on hand — an ImageWriter II, an SE/30, and two connector
boxes — which the client direction is not.

Both directions need the same stack underneath: DDP, NBP, ATP, PAP over TashTalk. Roughly
2–3k lines of C. ATP's exactly-once state machine is the only genuinely fiddly part.

## What this does to the firmware

**No printer-driver abstraction. One `printer_caps` struct instead.**

The first instinct was a backend interface with function pointers, on the argument that a
second printer would otherwise force a refactor. That argument died with the StyleWriter.
It was the only candidate that differed structurally — 360 dpi CMYK planes against 144 dpi
bilevel. Every remaining target takes the same 1-bit page as input, including the LocalTalk
ImageWriter (identical bytes) and a LaserWriter (a PostScript `imagemask` wrapper around the
same raster).

What is real, and independent of any abstraction, is that printer capability is read from
four places: `ipp_attrs` for resolution, colour and margins; the URF string in the mDNS TXT
record; the halftoner's output geometry; and `imagewriter`'s dot layout. Spec §3.2, §3.3 and
§3.5 currently write those as constants at each site. They belong in one struct, read by all
four. That costs nothing and does not presuppose a second printer.

Revisit the abstraction question when a second printer is on the bench. Not before.

## Facts the firmware now depends on

Two of `SPEC.md` §4's unverified assumptions were resolved from Apple's technical reference
and cross-checked byte for byte against `gdevadmp.c`. Both are recorded in place there. In
brief:

- **Bit 0 is the top pin**, bit 7 the bottom. The transpose reverses bit order.
- **`ESC p` selects 144 dpi** at 1152 dots. `ESC P` is 160 dpi. Case-sensitive, adjacent in
  the manual's table, and the most commonly mis-transcribed pair in circulating docs.
- **`ESC G nnnn` takes four ASCII decimal digits.** Sources describing a 16-bit binary count
  are quoting Epson's `ESC K`, a different printer family.
- **`ESC T nn` is nn/144 inch**, exactly two digits. `ESC T16` abuts successive 8-dot bands.
- **DTR goes false with fewer than 30 characters of buffer left**, and the host has 27 more
  characters to stop. At 9600 baud that is a 28 ms window — comfortable for the hardware CTS
  path, which is why the peripheral stalls the FIFO rather than software watermarking.

Assumption 4 (HSKo levels) and 5 (URF header offsets) remain open.

## Hardware facts worth recording

**AirWriter's LocalTalk port is wired as a node, not as a network.** J4 presents TxD± on
pins 3 and 6 and RxD± on pins 5 and 8, exactly like a Macintosh printer port. U6 drives,
U8 receives, both SP3485 halves. Verified against the exported netlist.

The consequence is a product-documentation matter: AirWriter needs its own LocalTalk
connector box, the same as any Mac. It cannot be cabled directly to another device's DIN-8.
The wiring was inherited from TashTalkUSB rather than chosen — copied node for node, per the
handoff — and the alternative (a transformer and two phone jacks on the board, making
AirWriter its own connector box) was never evaluated. Fine either way, but it should be a
deliberate choice at Rev B rather than an inherited one.

`docs/topologies.html` diagrams the supported wiring arrangements.

## Reference material

Archived under `docs/reference/` because both are load-bearing and both were hard to find:

- `ImageWriter_II_Technical_Reference.pdf` — 227 pages, image-only, no OCR layer. PDF page =
  book page + 20. Appendix A (133–146) is the command summary, Chapter 8 (103–125) the
  graphics detail, Appendix E (187–194) the electrical interface.
- `Inside_AppleTalk_2nd_ed.pdf` — chapter 9 is ATP, chapter 10 is PAP.

Worth pulling before the AppleTalk work starts: the Wireshark capture of a real AppleTalk
ImageWriter job attached to the 68kMLA thread "Anyone have an AppleTalk ImageWriter and a
TashTalk or AirTalk?". It is the highest-value artifact for that milestone and the forum
sits behind an anti-bot wall, so fetch it manually.

## Open questions

- Whether the ImageWriter II's built-in serial port stays usable while the LocalTalk option
  card is installed. Only matters if someone wants both at once.
- The option card's PAP flow quantum and its 2-byte status format. Netatalk notes that
  ImageWriters return a raw 2-byte value where LaserWriters return a Pascal string, but the
  encoding is undocumented.
- Whether ROM 341-0034-B transforms the byte stream at all beyond acting on `ESC b` (rename)
  and `ESC ?` (self ID).
- Whether AirTalk's WiFi wire format is stable enough to target. It is one project's own
  invention, not a standard, so interoperating means tracking someone else's changes.

## What this unblocks

The implementation plan covers M2 first — the host-side `rip` tool, which needs no ESP-IDF
and no hardware — then M1, M3, M4, M5 in order, all against the ImageWriter II over serial.
AppleTalk becomes its own plan once M5 prints.
