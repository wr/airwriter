# StyleWriters on the printer port — design

`W-1103 StyleWriters on the printer port`. Agreed with Wells in chat on 2026-10-25. Builds on
`W-1088` (the printer list) and the colour work of `W-1064`; shares groundwork with
`W-1102 QuickDraw LaserWriters on the printer port (Personal LaserWriter LS/300, Select 300)`.
Reverses the StyleWriter cut in `2026-08-04-printer-support-scope-design.md`.

**Amended 2026-11-24** by `W-1193 Vintage Macs print to a StyleWriter through AirWriter, as
Apple's network adapter shared one (ADSP)`: phase 2 is being built, and its section below is
rewritten for what changed after 2026-10-25. Each shared StyleWriter gets a listening socket of
its own (0x89, 0x8A) in place of socket 129, which is the ImageWriter's and can serve one printer;
rev 2026.11 offers a StyleWriter on each port; the board calls the Mac back without waiting for
the Mac to close its first connection, as Apple's adapter did for lpstyl; and both fast modes
are refused on every board for now. There are no captures of Apple's drivers to build from, so
the exchange comes from lpstyl, TailTalk and InkTalk, and a Mac's print is the check.

**Amended 2026-11-20** by `W-1177 Color StyleWriter: colours print clean and bright, with
black only for grays` (SPEC §3.16 has the record): blue noise is a StyleWriter's default screen,
on Apple's `GAMA` 2, the curve its drivers pair with scattered dots, and a colour page has the
inkjet's own split: black for grays and, from 38% dark, dark colours, and a colour in its own
inks at full strength.
Atkinson lost an order slip's pale panel, outlines and dividers; the ribbon's split, made for
the ImageWriter, spotted every colour short of full strength with black. Wells chose both on
paper on board 1's Color StyleWriter 2500.

**Amended 2026-11-19** by `W-1168 The Color StyleWriter 2500 is confirmed on paper`: board 1's
2500 has printed in colour since 2026-11-07 with a new old stock colour cartridge, so the wiki's
Supported Printers page lists it as **Yes** and its settings page drops the not-confirmed line
(Docs, below). The other six stay **Yes\***.

**Amended 2026-11-16** by `W-1162 Remove the DIP switch code: rev A boards ignore S1, the page's switches decide`: DIP4 no longer prints a test page at power-on, so the
button and the page's Print Test Page are the two ways to one.

**Amended 2026-11-14** by `W-1140 One test page for every printer: the ImageWriter and PostScript
LaserWriters print the shared page`: the test page is the page every printer prints
(`testpage`; the test-page design's amendment block), no longer this family's own. A StyleWriter keeps what was its own: the 360 dpi grid, its model's tone curves and ink placement, its Apple screen, its captions (`sw_tp_geom`), and each model now shows its own icon. The page gained the type tester, lost the big still life, and puts the ink ramps two by two.

**Amended 2026-11-13** by `W-1102 QuickDraw LaserWriters on the printer port (Personal LaserWriter LS/300, Select 300)` (`2026-11-12-quickdraw-laserwriters-design.md`): the run coder (`sw_code_row` with no previous row),
the test page's renderer (`sw_testpage`, drawn at 300 dpi for a Personal LaserWriter 300) and
the record's "Apple" screen are shared with the 300, where "Apple" means Apple's 300 screen. The
port's family is one enum of three (`pr_family_t`) in place of the StyleWriter flag, and a clock
on HSKi is a QuickDraw LaserWriter, never asked the plain question or a StyleWriter's status
(SPEC §3.18).

**Amended 2026-11-09** (`W-1138 Every supported printer shows its own Apple icon`): a
StyleWriter's icon over AirPrint and on its Printers row is Apple's for its model, where the
Settings page section below has one drawing for the family. The 2400 and 2500 share one. The
test page's letterhead is still the family's (SPEC §3.5).

**Amended 2026-11-08** (`W-1134 StyleWriter test page: show the three colour mixes, as the
ImageWriter's does`, Wells's word on a mock): with a colour cartridge the test page's colour
row is seven swatches across the page, the four inks and then red (magenta over yellow), green
(cyan over yellow) and blue (cyan over magenta), in the space the four took, so the page grows
no taller. The mixes show whether the inks lie cleanly over each other. A black made of the
three colours was left out: it tunes AirWriter's colour, it does not tell an owner anything
(SPEC §3.16, Test page).

**Amended 2026-11-07** (`W-1133 StyleWriter test page stops partway while the board draws its
pictures`): the test page's still life and the scene its six screens show are traced once on
the host and stored in the firmware, not traced on the board as the page prints. The page is
drawn and sent from one task, so the board's seconds of tracing were seconds the printer had
nothing, and the 2500 stopped twice partway down the page, the second time making the noises it
makes at power-up before going on. The page is the same dots (SPEC §3.16, Test page).

**Amended 2026-11-07** (`W-1132 Show Cleaning while a StyleWriter cleans its print head`, Wells's
word): while a cleaning runs, the settings page shows the StyleWriter as Cleaning, not Printing,
in its row, its Status and the sidebar, with Printing's green lamp. The printer never says it is
cleaning; the board started it and knows when it ends. The panel keeps the printing light and
the phone is unchanged (SPEC §3.16, Web).

**Amended 2026-11-07** after the first cleaning on board 1's Color StyleWriter 2500: it reads
`1` busy from its reset on, cleaning or not, so a cleaning that waited for busy to go off
(TailTalk's rule) showed Printing for the full 600 s and then failed, though the head was done
in 26 s. A cleaning now ends when busy goes off after reading on, or when `1` 08 (all it was
sent is done) comes on after reading off, whichever is first; the 2500's 08 is off from the
reset until the cleaning ends (SPEC §3.16, Clean Print Head).

**Amended 2026-11-02** after the first StyleWriter on the bench, a Color StyleWriter 2500 on
board 1 (rev A; it answered every question with R9 open, first printed with R9 fitted, and on
2026-11-03 printed the test page with R9 taken off again, so the 2500 does not need R9): a rect is as wide as its rows' whole bytes, not the printable width.
The 2500 counts a row's bytes from the rect's width rounded down, so Letter's 2916-dot rect over
365-byte rows lost it the first row, and it stopped with `2` 80 and its buffer full (SPEC §3.16,
Geometry). lpstyl's rects are always whole bytes; TailTalk's are the printable width, which on
Letter is not. With 2920-dot rects the test page ran to its end at 57600 baud, with blank rows
skipped, several bands in the buffer, `m2bA` and `B`, and pauses of 13 to 20 s between a rect
and its blocks while the board drew them. The cartridge was dry, so the sheet came out blank (a colour cartridge printed from 2026-11-07).
The same bench changed two rules. The 2400 and 2500's `2` 80, read as spurious, also marks a
page the printer has stopped on (`1` not busy, nothing draining), which now fails after 10 s
rather than the 600 s stall limit. And the full sequence at start-up and on the person's Look
for Printers goes to any StyleWriter record that a plain question got nothing from, not only
the original's: the 2500 goes dark on its own about 5 minutes after a page, and is left silent
by a page cut short, and only a reset wakes it. Wells's call the same day: a 2200, 2400 or
2500 asleep after a job reads as Ready (as the original's quiet idle does) and the next job's
reset wakes it, as Apple's driver leaves it, rather than dropping `l` to keep it awake; one
switched off at its button reads Ready too until a job finds it off. And a waking printer gets
10 s before a second reset, which has left it dark.

**Amended 2026-10-30** by `W-1116 Printer port: several printers, the one plugged in shows
online` (`2026-10-30-port-printers-design.md`): the swapped-port refusal and the "changed"
notice of the 2026-10-27 amendment are replaced; the port keeps a record per model, the one
answering online and the rest offline, and a family check alone stays so one family's bytes
never reach the other: on what is on the port now and what the work's own questions got, never
the family that answered last (SPEC §3.16: `pr_port_absent` at a job's claim, the ImageWriter
job's own gate, and `printers_port_on_is_imagewriter` for a StyleWriter's job, page or
cleaning). The automatic look is every 5 s, not 60, while the port's printer is not answering,
whatever the list holds; the order of the questions is the list's, not the record's: the plain
question first wherever a StyleWriter record is listed, the port last answered as a
StyleWriter, or an original ImageWriter record is listed; the full sequence wherever an
original StyleWriter record is listed. A StyleWriter record that answers is asked its status on
a line-up edge as well, so an ImageWriter put in its place is found by the failed check and the
stop's look in about 1.5 s. An ImageWriter record whose port a StyleWriter answered is asked as
any port that is not answering (the quiet rule below went with the notice).

**Amended 2026-10-27, before the bench** (SPEC §3.16 has each in full; the 2026-10-30
amendment above changes the first two). A port whose record is, or that last answered as, a
StyleWriter is asked the plain question first at a line-up or a look, and `ESC ?` only when
that gets nothing: a StyleWriter may stop listening once it has heard `ESC ?`. While the
family that last answered on the port is not the record's, the record's jobs, test page and
cleaning are refused until its own family answers again or the person accepts the change; an
ImageWriter record whose port a StyleWriter answered asks nothing more on its own, so its
notice can be dismissed. The original StyleWriter's silence while idle reads as ready, not
offline, since it ignores the plain question until a reset. A block leaves one step of `B`
below the empty buffer.

## Why

In August the StyleWriter was cut for one reason: the print head is the cartridge on the early
models, and nobody has made one since about 2005. Nothing technical stood in the way. Since then
it has become clear that vintage Mac owners restore StyleWriters and print with them anyway,
the II and 1200 most of all, then the Color 2400 and 2500, then the 2200. The later colour
models are in better shape than August assumed: the 2400 and 2500 take a separate head with
cheap ink tanks.

The work is also mostly done in public. Monroe Williams's lpstyl (1996–2000, BSD-2) documents
the serial protocol for seven models. FeralFirmware's TailTalk 0.13.0 (2026-10-24, PR #92) took
Apple's 1500 and 2500 drivers apart and prints to every StyleWriter from a computer; its InkTalk
board stands in for Apple's StyleWriter network adapter so a Mac's own driver prints through it.
Wells's words: "feralfirmware / bolacore's work is a good starting point (and credit)", and a
collaboration on a QuickDraw driver (`W-1102`) is likely soon.

Wells has no StyleWriter and no strong wish to buy one. Plan A: build against a software
StyleWriter and have an owner confirm on paper. Plan B: buy one.

## Goals

- A StyleWriter plugged into the printer port shows up in the printer list under its own name
  and is shared over AirPrint at once, as an ImageWriter is today. It prints in colour when a
  colour cartridge is in, gray otherwise.
- Seven models: the StyleWriter, StyleWriter II, StyleWriter 1200, and the Color StyleWriter
  1500, 2200, 2400 and 2500.
- Vintage Macs print to it through AirWriter with their own StyleWriter driver, AirWriter
  standing in for Apple's StyleWriter EtherTalk/LocalTalk adapter, on every AppleTalk
  connection that is switched on (phase 2).
- Built and checked with no StyleWriter here: a software StyleWriter for the host tests and the
  bench, Apple's own drivers captured against it, and an owner's prints before it is called
  working.
- lpstyl and TailTalk/InkTalk (FeralFirmware) credited in the docs and `LICENSE.md`.

## Non-goals

- The Color StyleWriter 4100, 4500 and 6500. They are HP DeskJet engines and nobody has
  documented what they take. The Portable StyleWriter (parallel port) too.
- StyleWriters already on the network behind Apple's adapter or an InkTalk, found over
  AppleTalk and shared on AirPrint (my call; the same reasoning that cut `W-1090`). Owners can
  plug the printer into AirWriter's port instead.
- The printers' fast modes, which InkTalk follows since TailTalk PR 93 (2026-10-30;
  `InkTalk/firmware/src/fast_clock.rs` and `printer.rs`). A StyleWriter II or 1200 is sent `h`
  and then runs at 230,400 baud on the port's own clock; J2's data lines go through U2, an
  RS-422 part, so rev A should carry it (untested). A Colour StyleWriter (1500, 2200, 2400,
  2500) is sent `x` and then drives a bit clock on HSKi, one clock per bit with 8N1 framing on
  it, about 885 kHz on a 2200: send each bit just after a falling edge, sample on the rising
  one. Either way the port switches about 33 ms after the command is out, as a Mac allows, and
  a printer that stops clocking has been reset or switched off and is back at 57600. Rev A's U3
  is too slow to pass the HSKi clock, rev B's SP3232EU should (`W-1091`), and the ESP32's UART
  takes no outside clock (InkTalk uses the RP2040's PIO), so how the board follows it is
  `W-1102`'s to work out for the QuickDraw LaserWriters. How a job brings the printer back out
  of either mode cleanly is not yet known. Here every page goes at 57600 baud, as lpstyl sends
  it.
- 720 dpi on the 2500, paper types other than plain, transparencies, envelopes.

## Anti-goals

- **StyleWriter bytes printed as junk on an ImageWriter, or ImageWriter bytes on a
  StyleWriter.** A printer that answered as one never hears the other's language.
- **Wasted ink or paper on irreplaceable heads.** No cleaning AirWriter runs on its own, no
  test page nobody asked for, no reset that feeds a sheet while idle.
- A change for anyone whose ImageWriter or LaserWriter prints today.
- Claiming a model works before an owner has printed on it. The wiki's Supported Printers
  table stars it (`Yes*`, `W-1101`) until then.
- Any public copy that names one forum as where StyleWriter owners are.
- **Wells hand-holding the build.** This document settles the experience; the build is checked
  end to end (below) before he is asked to look.

## What the printer and the board allow

Facts the design rests on, from lpstyl (`README.protocol`, `lpstyl.c`), TailTalk at `bf38658`
(`tailtalk/src/stylewriter/*`, `tailtalk-core/src/stylewriter.rs`, `InkTalk/firmware/src/*`)
and Apple's IIGS StyleWriter ERS (Rev 1.3, 1991). Only the Color StyleWriter 2200 has printed
from TailTalk on real hardware; the rest is the drivers' reading and lpstyl's testers.

- **The link.** 57600 baud, 8N1 (inferred), RS-422, on the same mini-DIN-8 as the ImageWriter.
  No flow control: the printer's handshake line is not a pacing signal ("some like the II" do
  not hold it ready, InkTalk), and Apple's driver ignores it. Pacing is by asking the printer's
  free buffer.
- **The board's handshake out.** InkTalk holds the printer's HSKi high through a pull-up,
  because "a StyleWriter won't send anything back otherwise". On rev A, J2 pin 1 (HSKo out)
  reaches U3's T1OUT only through R9, a 0 Ω link that is not fitted; with it fitted, the
  `HSKI_DRV` clamp app_main already applies (IO35 low) holds the line asserted at RS-232 levels,
  as a Mac's port does. An ImageWriter ignores that pin (it lands on its DSR, SPEC §2.4).
  Whether a StyleWriter answers with the pin floating is unknown.
- **The cable** is expected to be the ImageWriter II's (Apple's System/Peripheral-8 crossover),
  as a StyleWriter plugs into a Mac's printer port the same way. To confirm on the first owner's
  printer.
- **Recognising it.** Send the reset, `00 FF FF FF 49` (`I`: eject and reset), wait 2 s (8 s
  for the original, which ignores `?` until reset), drain, send `?` and read to CR (250 ms a
  byte). `IJ10` is the original, `SW` the II, `SW3` the 1200, `CS` a colour model; then, only
  after `CS`, `FF FF FF 70` (`p`) gives one byte: 01 the 2400, 02 the 2200, 04 the 1500, 05 the
  2500, anything else treated as a 2200. `p` sent to any other model ejects and resets it.
- **Commands.** `FF FF FF` + a letter asks something and gets one byte back: `1` status (busy
  01, error 02, hardware 04, page done 08, door 40, waste ink 80), `2` errors (cartridge jammed
  01, ink out 02, no paper 04, paper jam 08, error 10, cartridge missing 20, paper too small 40,
  80 spurious on the 2400/2500 per lpstyl), `B` free buffer, `H` after `D` the cartridge (CS:
  0x81 colour, 0x01 black; 1500: bit 0x80 colour), `R` a quality hint, `S` resume after a fault.
  Letters without the prefix set up the job and the page (below).
- **A page** is bands. Each band is a rect, `R` (black) or `c` (four inks), then left, top,
  right, bottom as little-endian u16 at 360 dpi, inclusive; then `G` blocks, each `G`, a
  little-endian u16 length, the data, and `00`. Rows are run-coded (low 6 bits a count 1–62;
  top bits 00 literal, 01 repeat the next byte, 10 white run, 11 black run; an empty plane is
  `80`), each plane XORed with the same plane of the row before within a band, the band's first
  row raw, and no delta at all on the original. Colour rows go C, M, Y, K. `0C` ends a page.
- **Per model** (TailTalk `family.rs`, from the drivers' `ENGC` resources):

  | | Dots across | Margins L/R, top, bottom (dots) | Band | Rect offset | Block cap | `B` empty |
  |---|---|---|---|---|---|---|
  | Original | 2880 | 72/72, 72, 114 | 800 rows | 0 | 8 KB | A0 |
  | II | 2880 | 72/72, 72, 114 | 800 | +16 | 0x76ED − planes × row | F8 |
  | 1200 | 2880 | 72/72, 72, 114 | 800 | +16 | as the II | F8 |
  | Color 1500 | 2880 | 72/72, 72, 114 | 800 | +16 | as the II | 87 colour, 9F black |
  | Color 2200 | 2919 | 72/72, 45, 99 | 400 | 0 | 0x7E77 black, 0x79FA colour | A2/A3 (`B & A0`) |
  | Color 2400 | 2919 | 72/72, 45, 99 | 400 | 0 | as the 2200 | F8 |
  | Color 2500 | 2919 | 72/72, 45, 99 | 400 | 0 | as the 2200 | 80 |

  Free bytes are `B` × 256 − 2048 (the original × 64). A block of 0x8000 stalls a CS printer.

  Reading the table (TailTalk `family.rs` is the authority, the captures below the check):
  margins are from the sheet's edges; "dots across" is the widest row the engine prints, so a
  page's printable width is the lesser of that and the paper's width less both side margins
  (Letter: 3060 − 144 = 2916, so 2880 on the 1200 family and 2916 on the CS family; A4: 2832 on
  both). Rect coordinates are relative to the printable area's top-left corner, and on the II,
  1200 and 1500 both x values are 16 more for the same page. IPP's margins are the left margin,
  the top and bottom margins, and on the right whatever the paper leaves past the printable
  width.
- **Paper size** has no command: the printer prints the rows it is sent and ejects at `0C`, so
  Letter, A4 and Legal differ only in width (above) and rows. `F` marks paper under 5.5 in,
  which AirWriter does not offer.
- **Job and page commands** differ per model (TailTalk `job.rs`, `session.rs`). Per job: the
  reset, 2 s, status clear, `?` matching, `D` (CS) or `N` (1200), `B`, `H`, then the model's
  setup: CS `m2` (plain), quality (draft `d`, normal `n Z` with a colour cartridge or `b` with
  black, best `s`), `A`, then per page `H` (colour page) or `B` when it changes, and `L`; 1500
  `m2`, `d`/`Z`/`s`, `A`, `L` per page; II `u`, `d`/`n`/`b`, `D`, `A`; 1200 `N` then the II's,
  `L` per page; original `n u A`. After the last page: wait for status `1` 0x08, then `E` (the
  1200 adds `Y`) and `l`. Cleaning is `P` once (or twice for a deep clean) in a job's setup.
- **Speed.** 57600 baud is about 5.6 KB/s. A text page is tens of KB; a full-page gray photo
  about 1.4 MB raw at 360 dpi and barely compresses once screened; a full-page colour photo
  several MB, so ten minutes or more on the wire. Apple's drivers had the same limit without
  the fast mode.
- **Apple's network adapter** (InkTalk `stylewriter.rs`, lpstyl `README.atalk`): ADSP, not PAP.
  NBP type `ColorStyleWriter2400AT` for the 2200, 2400 and 2500, `StyleWriter1200AT` for the
  original, II, 1200 and 1500; a real adapter used socket 129. The Mac opens a connection and
  sends attention `0x000B` (its data socket and user name); the adapter answers `0000`
  (accepted), `0001` (busy) or `FFFF`; the Mac sends `0x0012` and closes, and the adapter
  connects back to the Mac's data socket. On that connection the Mac's bytes go to the printer
  as they are and the printer's come back; the Mac paces itself with `B` through the pipe.
  InkTalk's table answers the other attentions (status text, drained, busy, rename, the fast
  clock refused with `00EF`/`FFEF`, and the `x`/`h` messages dropped). A Mac that vanishes
  mid-job leaves the adapter to reset the printer.

## Phases

1. **AirPrint to a StyleWriter on the port.** All seven models, gray and colour, recognised,
   shared, with status, the settings page and a test page. Merges once it passes against the
   software StyleWriter and the independent encoders (and the Apple-driver captures, if an
   emulator runs the drivers) and board 1's ImageWriter prints unchanged, before any
   StyleWriter has printed (as `W-1082` merged before a LaserWriter printed); the
   docs star it.
2. **Vintage Macs through AirWriter.** ADSP and the adapter. After phase 1 has printed on an
   owner's printer, so a fault is the adapter's and not the page's.

## Phase 1: AirPrint to a StyleWriter

### What an owner sees

They plug the StyleWriter into the printer port with the cable they have. Within seconds of
switching it on (or at setup's first step) it appears in Printers as "StyleWriter II" (its
model), with a StyleWriter drawn on its row, and on phones and computers under that name. With
a colour cartridge in a colour model, phones offer colour; with black, gray. A page comes out
at the quality picked on the phone. Out of paper, the phone says the printer needs attention;
loading paper carries on. The test button prints a StyleWriter test page.

### The port

The port holds one printer: an ImageWriter or a StyleWriter, never both. Today it asks
`ESC ?` at 9600 when the handshake line comes up after a second or more down (`printers.c`,
SPEC §3.15). A StyleWriter may never raise that line, and the two printers speak at different
speeds, so what reaches the wrong printer has to be harmless.

**Two StyleWriter questions.** The *plain question* is the single byte `?` at 57600 with no
flow control. At 9600 its frame is a 17 µs low pulse, then high past the receiver's mid-bit
check, then a 35 µs low that ends in the stop bit before the next check: an ImageWriter's UART
rejects both as false start bits and hears nothing. The II, 1200 and colour models answer it.
The *full sequence* (the reset, the settle, `?`, and `p` after `CS`) also finds the original,
which ignores `?` until reset, but its `00` and `49` bytes can read as a stray character at
9600. So:

- **Automatic looks send only the plain question.** At boot after the quiet power-on, when
  setup or the Printers section looks, at line-up when `ESC ?` went unanswered, and every 5 s
  while the port's printer is not answering (60 s at first, InkTalk's retry; 5 s since
  W-1116, so a StyleWriter plugged in is found in seconds), never while the port prints. One
  byte, then back to 9600.
- **The full sequence goes only to a StyleWriter the person or the record names:** before
  every job to a StyleWriter record, and once when the person picks a StyleWriter in Add a
  Printer… or in the unknown printer's notice, to confirm it and read its model. Amended
  2026-10-27 (Wells): an original StyleWriter record also gets it once at start-up and on Look
  for Printers when nothing else answered, so an empty port reads offline rather than ready.
- **A port whose record is an ImageWriter** is asked `ESC ?` at line-up as today; while its
  printer does not answer (off or gone), the automatic looks add the plain question, so a
  StyleWriter swapped in is noticed.
- **A port whose record is a StyleWriter** is asked `1`/`2` while it answers (idle state,
  below). Once it stops answering, that stops and the port is back to the automatic looks and
  `ESC ?` at line-up, so an ImageWriter swapped in is noticed.

So the original StyleWriter is the one model found only once the person says what it is: an
"Unknown printer" notice when its line is up, Add a Printer… when not. The bench checks the
plain question on board 1's ImageWriter II (nothing printed, several hundred times), and the
full sequence on it too, to know what a wrong pick costs.

A recognised StyleWriter gets a port record (`kind` `stylewriter`, `model` one of the seven),
shared at once (`W-1088`'s rule), named by its model. A different printer turning up on the
port is `W-1088`'s "changed" notice ("The printer port now has a StyleWriter II. Use it instead
of ImageWriter II?"), not a swap. The record keeps the cartridge last read (black or colour)
for the page and for IPP. Until phase 2, vintage Macs are offered nothing for a port
StyleWriter.

ImageWriter-only traffic (the CTRL-X kick, the resync, `ESC ?` before a job, the ImageWriter
test page) never goes to a StyleWriter.

**Idle state.** A StyleWriter's line says nothing, so while it is idle the board asks `1` and
`2` every 10 s (my call): an answer is ready (or out of paper, or a fault), none is offline.
That feeds the page, the panel and IPP as the ImageWriter's line does today. A StyleWriter
that stops answering stays in the list, offline (`W-1088`: gone is not removed); one that
starts answering again is asked `D`/`H` for its cartridge.

**The handshake pin.** Rev A boards that will run a StyleWriter get R9 fitted (a 0 Ω part or a
solder bridge) until an owner shows the printer answers without it; the StyleWriter wiki page
says how. Rev B fits R9 (`W-1091`; my call, Wells to confirm), harmless to an ImageWriter.

### The AirPrint printer

`printer_caps` per family:

- 360 × 360 dpi, URF `RS360`. Media Letter, A4 and Legal (my call; the ImageWriter offers
  Letter only, a StyleWriter feeds cut sheets): `printer_caps` grows from one media keyword to a
  short list, the phone picks, and the margins are the engine's from the table above.
- Colour advertised while the record's cartridge is colour, under the same UUID, as the colour
  ribbon is today (`W-1064`); black-only models never.
- `print-quality` 3/4/5 (supported today) map to draft, normal and best.
- `printer-make-and-model` and Bonjour `ty` name the model ("Apple StyleWriter II", "Apple
  Color StyleWriter 2500").
- The UUID is the port record's, made once (`W-1088`); a board whose port changed from
  ImageWriter to StyleWriter has a new port record and so a new printer on phones.

### The page

The rip already ends in a stage (`rip_stage_t`); add a StyleWriter stage, pure C and
host-built (`components/sw_page`, name the builder's), fed 360 dpi rows:

- **Apple's tables** (Wells, 2026-10-26: use them). The drivers carry Apple's own calibration
  of StyleWriter ink on paper: tone curves (`GAMA` 1 and 2; the Color SW 2200 driver's for the
  2200, 2400 and 2500, the Color SW 1500 driver's for the other four) and a 16 × 16 threshold
  screen (`HTSC` 1, the same in both). A script of ours (`tools/stylewriter/mktables.py`,
  builder's name) reads them out of Apple's driver files and writes a header that is committed,
  with where each table came from; nothing is copied from TailTalk's source. The tone curve is
  the StyleWriter's default tone correction, in place of the dot-gain curve's 0, so the first
  print is already close to a Mac's with no printer here to calibrate on; the gain setting
  still adjusts on top. Apple's screen is a sixth screen beside AirWriter's five, offered on
  StyleWriters ("Apple" in the Screen list).
- **Screening.** The record's screen at 360 dpi, after the tone curve. Gray pages and black
  cartridges screen one plane (K). Colour pages go through `ink_split`/`ink_screens` into C,
  M, Y, K (full gray-component replacement, black first, the colours kept off its dots), with
  Apple's screen placed per ink as TailTalk does (an ink prints where the threshold is under
  its amount, black on the screen turned over). `rip_stage_t` takes 1bpp rows today and turns RGB to
  gray; it grows a four-plane row for this stage (and `colour` stops being the ImageWriter's
  alone).
- **Bands and blocks.** Rows collect into the model's band height; each band's rows are
  run-coded with the per-plane delta and cut into `G` blocks under the model's cap, the delta
  carrying across blocks within a band as TailTalk sends it. Blank bands are skipped (the next
  rect's top moves on). A band's encoded blocks are held whole (at most 32 KB, PSRAM) because
  each carries its length up front.
- **Pacing.** TailTalk's: track free buffer from `B`, send a block while it fits, re-read `B`
  when short, and wait for status clear before each band. lpstyl's slower rule (wait for empty
  before every block) is the fallback if an owner's printer overflows.
- **Where it runs.** The rip writes framed bands into the printer-bytes half of the job buffer
  (`W-1066`'s 512 KB) as it writes ImageWriter strips today; the `prn` task runs the session
  (queries between blocks) rather than streaming bytes. When the 512 KB fills, the upload waits
  on the printer, as for a colour ImageWriter page now.
- **The job.** Before the first page: the per-job sequence above (reset, settle, status, `?`
  must match the record's model, cartridge, `B`), then the model's setup for the quality asked,
  per page the page commands, bands, `0C`; after the last, wait for 0x08, `E`(`Y`) `l`.
- **Faults.** No paper: the existing pause (`stopped`, `media-empty-error`), its message "check
  paper" where the ImageWriter's says "check paper and SELECT" (a StyleWriter has no SELECT),
  `S` every 5 s until it clears. Paper jam, cartridge missing or jammed, ink out, door open,
  waste ink full: `stopped` with the matching IPP reason (`media-jam`, `marker-supply-missing`,
  `marker-supply-empty`, `door-open`, `marker-waste-full`), `S` when the fault bit clears.
  Either way the job aborts after `CONFIG_AIRWRITER_STALL_ABORT` (600 s) as today. A fault the
  table does not name aborts the job.
- **Cancel** stops sending, sends `E` (black-only models) and the reset. **The next job after
  an abort** starts with the reset anyway.
- **Memory.** At 2919 dots a colour row's working set is about 2.5× the ImageWriter's
  (~150 KB a page, PSRAM). Internal RAM follows `W-1094`'s rule: nothing new there; check
  `/api/mem` across a colour page.

### Status, panel, test page, settings

- **Panel.** As `W-1088`: the box's state, and the port's own while it prints. The StyleWriter's
  ready, offline, out of paper and printing come from its answers in place of the line.
- **Test page.** The button, DIP4 and the page's Print Test Page reach a port StyleWriter as
  they reach a port ImageWriter. A StyleWriter has no fonts, so its test page is a raster drawn
  on the board (the stage above): the letterhead art (`tp_art.h`), the printer's name and the
  board's details in a small built-in bitmap face, the inks as swatches (colour cartridge) or a
  gray ramp, the five screens side by side, a ruler and four one-inch squares. One Letter page.
- **Settings page.** A Printers row draws a StyleWriter (one drawing for the family, in the
  page's icon style, from Apple's product art the way the ImageWriter's is). Its detail: name,
  Share, Model (found, or picked), Screen, Cartridge (Black or Color, read from the printer),
  and a Clean Print Head key that runs one cleaning (`P`) with the line "Cleaning uses ink."
  (none on the original, which has no cleaning). Words per `docs/voice-and-tone.md`.
- **API.** `/api/printers` carries the record's model and cartridge; `POST /api/printers
  {"id":N,"clean":true}` runs a cleaning when no job holds the lock. `/api/status` `model`
  names the port printer.

### Testing with no StyleWriter

- **The software StyleWriter** (`tools/stylewriter/swfake.py`, beside `etpeer.py`'s fake
  LaserWriter): answers as any of the seven (`?`, `p`, `1`, `2`, `B` draining at a printer's
  pace, `D`/`H`, `R`), on a pty, a serial device or TCP; decodes rects and blocks into pages and
  draws them as ink on paper (PNG, per ink and together); logs both directions; and on command
  runs out of paper mid-page, jams, loses its cartridge, stops answering, or overflows when sent
  too much.
- **Apple's drivers, captured.** Each driver (StyleWriter II, 1200, Color StyleWriter 1500,
  2200, 2400/2500, from Apple's archived downloads) prints a small page set (blank, checker,
  text, a colour photo) on a Mac emulator whose printer port goes to `swfake`. Basilisk II
  first (on hand in `~/Projects/macemu`; it redirects the Mac's serial driver to a host device,
  so a pty to `swfake`), Snow if a driver bypasses the serial driver. `swfake` must decode every
  capture; each model's command order is checked against them, and AirWriter's page and
  Apple's for the same image are compared as decoded dots: with Apple's screen and curve the
  pattern pages should agree closely, which also checks the extracted tables.
  Captures hold Apple's driver output, so they stay local like the printer fonts
  (`~/.config/airwriter/sw-captures/`, never committed) and the tests that use them skip
  without them. This is also `W-1102`'s calibration step, so the two share the rig. The rig is
  the strongest check but not a merge gate: if no emulator runs a driver, phase 1 merges on
  the rest, and the captures come from an owner's Mac through phase 2's adapter
  (`/api/sw/job`).
- **Independent encoders.** lpstyl's encoder (BSD-2, built into the host tests) and, when
  `cargo` is present, TailTalk's, run on the same pages; their streams and AirWriter's must
  decode through `swfake` to pages that agree with the same screen, so `swfake` is not checked
  only against code written from the same reading.
- **Host tests** (`make -C tools/rip test`): the run coder round-trips; the stage's stream
  decodes, through `swfake`'s decoder, to exactly the rip's own bits for the checker and a
  colour page, every model; block caps and band heights hold; the job's command order matches
  the captures per model.
- **Interop** (`make -C tools/rip swinterop`): the firmware's session code built for the host
  (`build/swhost`) prints to `swfake` over a pty, all seven models, with each fault and a
  cancel.
- **`swprint` for owners**: the same host build as a small tool that prints a page (a URF
  fixture or a PNG) to a real StyleWriter through any USB serial adapter, logging both
  directions. An owner with an adapter confirms the protocol on paper without an AirWriter, and
  sends the log back.
- **On an owner's printer** with an AirWriter (R9 fitted): recognised within seconds of power-on
  and at setup, a gray and a colour page from a phone at each quality, the test page, paper out
  mid-page and back, cancel mid-page, a cleaning, the cable, and whether the printer answers
  without R9.

### Checked end to end before Wells looks

- Host tests, `swinterop`, the raster regression (`checker.expected.bin` unchanged) and
  `idf.py build` clean.
- Board 1 (bench rules: no OTA mid-print, ask before printing on the ImageWriter): its
  ImageWriter recognised and printing as before (the checker's crc at top margin 0); the plain
  question sent at 57600 to its ImageWriter, several hundred times, prints nothing, and what
  the full sequence prints is written down; with no printer answering, the automatic look
  (60 s then, 5 s since W-1116) costs nothing visible.
- If a USB serial adapter is to hand: `swfake` on the Mac on board 1's J2, recognised as each
  model, an AirPrint page from a phone decoded by `swfake` as the dots the rip made, out of
  paper and cancel.
- The settings page in every new state at desktop and phone width, light and dark
  (`tools/webui/preview.py`).

## Phase 2: vintage Macs through AirWriter

`W-1193`. Rewritten 2026-11-24; the 2026-10-25 sketch is in git history.

### What an owner sees

A vintage Mac opens the Chooser, picks Apple's Color SW 2500 driver (for a 2200, 2400 or 2500)
or Color SW 1500 driver (for the original, II, 1200 or 1500), and the StyleWriter is listed
under the name AirWriter gives it, on LocalTalk and on every EtherTalk connection that is on.
It prints with Apple's driver, at the speed AirPrint does. While the board prints anything
else the Mac's driver waits and asks again. Renaming the printer from the Mac renames it
everywhere. The settings page's "In the Chooser on vintage Macs" line lists it with the others.

### Approaches

1. **Stand in for Apple's adapter** (chosen). The Mac's own driver makes the page and paces
   the printer; the board passes bytes. Apple's quality, every model the drivers cover, and
   nothing to rip.
2. Offer the StyleWriter as an AppleTalk ImageWriter and turn the Mac's ImageWriter stream
   into StyleWriter dots: 144 dpi on a 360 dpi printer, and no colour on most.
3. Offer it as a LaserWriter: needs a PostScript interpreter on the board.

### What the sources are worth

No capture of a real Mac printing through a real adapter exists, here or in the references.
What there is: lpstyl's `adsp.c` and `at_printer_*`, written from packet dumps of a real
adapter printing; TailTalk's replay of a real Mac renaming a real adapter
(`tailtalk/tests/adsp_test.rs`); and TailTalk's adapter role (`tailtalk-core/src/stylewriter.rs`,
what InkTalk runs), written from a decompile of Apple's 1500 and 2500 drivers and used with
real Macs. Where they disagree the board accepts both. The working brief with every byte is
kept with the plan, outside this repository.

### ADSP (`components/localtalk/adsp.c`, pure C)

A connection engine with no knowledge of the node or the adapter: packets and a clock in,
packets and events out. DDP type 7, the 13-byte header, the 8-byte open parameters.

- **Accepts and opens.** A listener answers an Open Request with Request+Ack, the same one
  again for a repeated request, and works whether or not the third leg arrives. An opener
  resends its request each second, five times, takes a second Request+Ack from a new
  connection ID as the one to use, and stops on a denial; an answer or a denial counts only
  from the network and node the request went to, any socket there. Connections are told
  apart by the far address and the far connection ID together.
- **Stream.** In order only; a packet ahead is dropped, an overlap gives its new bytes. A
  received end of message takes one sequence number; the engine never sends one. An ack for
  every packet that asks. The receive window is the room the taker has, and an ack goes out
  unasked when it opens from zero, or when the far end was last told under one packet's room
  (572 bytes) and there is now a packet more: a Mac that will not send part of a packet into
  a small window would otherwise wait 5 s for a probe. Sent bytes ask for an ack, respect
  the far window, and are resent after 3 s, five times, or at once on Retransmit Advice.
- **Attention messages**, received only: their own sequence, an ack (13 bytes) for every one
  whether or not it asks, a repeat acked and not delivered. They never touch the stream's
  acks or window.
- **Probes.** Answers every probe with one ack. Sends its own after 5 s of silence and closes
  after the fourth unanswered, so a Mac that vanishes frees the printer in 20 s, and lpstyl's
  15 s sleep survives.
- **Forward Reset** is answered (lpstyl sends one at a job's end). Close Advice is sent once
  and taken as final.
- Not built: sending attention messages, Forward Reset or Retransmit Advice; buffering out
  of order; Open Denial (an unknown socket stays silent).

### The adapter (`components/localtalk/sw_adapter.c`, pure C)

One per AppleTalk link, riding the link's node as the PAP client does.

- **Names.** Each shared StyleWriter whose port is doing the printer job and whose printer is
  the one on the port now is registered as `<name>:ColorStyleWriter2400AT@*` (2200, 2400,
  2500) or `<name>:StyleWriter1200AT@*` (original, II, 1200, 1500), on every link that is on.
  Up to two: one a port. Each has a listening socket of its own, 0x89 and 0x8A, which is also
  where its call-back comes from, and keeps it while it is offered: when the first is
  unplugged the second stays on 0x8A, under the Macs that chose it and the job in its pipe.
  A second with the same name and type gets " 2".
- **A print.** The Mac opens a connection to the name's socket and sends attention `000B`
  (70 bytes: its data socket in byte 3, its user name). Every attention is acked and then
  answered on that connection's stream, 2 bytes unless noted; an unknown one gets `00 00` so
  the Mac never hangs. `000B` gets `00 00` and the job lock, `00 01` while anything else holds
  the lock, `FF FF` for a body under 5 bytes. The board then opens a connection back to the
  Mac's data socket at the first of: the Mac closing its connection, attention `0012` on it,
  or 1 s. If the pipe is not up in 15 s the job is dropped and the lock given back.
- **The pipe.** The Mac's stream goes to the port as it is, at 57600 with no handshake, and
  the printer's bytes go back as they come, without end-of-message marks. The window is the
  room in a 4 KB buffer in front of the port. The Mac paces the printer itself with the
  printer's own status questions, so answers go back within a tick or two.
- **Attentions on the pipe.** `0006` (has everything reached the printer?) `FF FF` once the
  buffer is empty and the port has sent it, else `00 00`. `000A` (status text) kept for the
  questions below. `0005` gets `00 EF` and `0007` gets `FF EF`: no fast mode. `0003`, `0004`,
  `0008` get `00 00` and do nothing. A message that is the one byte `x` or `h` is not passed
  on. `0012` gets `FF FF` and marks the job as ending cleanly. An answer always fits: 343 of
  the pipe's 640 bytes of send buffer are kept from the printer's bytes, room for both long
  answers and six short ones unacknowledged at once.
- **Questions from any Mac, any time**: `0011` job count and `000D` queue position (`00 01`
  while the board is busy, else `00 00`), `000E` (72 bytes: the request of the job in the
  pipe, zeros otherwise) and `0010` (259 bytes: its status text, zeros otherwise), `000C`
  withdraws a request that has not reached the pipe. Three connections a link beside the pipe;
  a fourth Mac is not answered and its driver tries again.
- **The end.** A close after `0012`: nothing is sent to the printer. A pipe lost any other way
  (a bare close, 20 s of silence, resends spent, 300 s with nothing through it either way
  though the Mac answers probes) or the test button, with bytes at the printer: the board
  sends the printer `00 FF FF FF 49`. Two resets close together have left a 2500 dark, and
  the Mac's driver resets the printer itself as its first bytes, so each port keeps the time
  of its printer's last reset, the board's or the driver's: the board's reset waits until
  10 s after it, a Mac's request inside those 10 s is busy, and the lock is kept until 10 s
  after the last. Either way the port goes back to its record's settings and the printer's
  next status question is 10 s out.
- **Rename.** `0009` on a connection, then `0012` on the same one, renames the record: its
  AirPrint name and its name on every link. An empty name, or one with `:` or `@`, is ignored.

### On the board

- **Offers.** `pr_offers_on` carries the port's StyleWriters beside its ImageWriter;
  `lt_offers_t` and `lt_node_offer` take up to two adapter names. A cable link is up for a
  StyleWriter as it is for an ImageWriter. On rev 2026.11 with a printer on each port there is
  no LocalTalk, so the adapter is on EtherTalk alone.
- **The lock and the pump.** The link's task takes the job lock without waiting, so busy is
  answered at once, and starts a task for the job (if it cannot start, busy). That task owns
  the port: selects it, sets 57600, moves bytes between two PSRAM rings and the UART, and at
  the end does the reset if one is due, puts the port back and gives the lock. Holding the lock
  is what pauses the port's questions, AirPrint, other Macs, the test page, cleaning and
  updates. The link's task never waits on the port.
- **What the board shows.** The job reads as a Mac's job does today: `ps` printing, the
  panel's LocalTalk stripes, "LocalTalk job" (or the link's) as the job's name. The record's
  asleep and answering marks are set at the start and end as a session sets them, so a 2500
  that goes quiet after a Mac's job reads ready.
- **Log.** A line when a job starts, one when the pipe is up, and one at the end with the
  bytes that reached the printer, the bytes back and how it ended. A busy answer is logged
  once in 30 s a link. Never the Mac's user name, the printer's name or the status text.
- **`GET /api/sw/job`**: the last job's bytes to the printer, the first 256 KB, as
  `/api/lt/job`, and in the same buffer: whichever Mac's job came last, an ImageWriter's or a
  StyleWriter's, has the capture and the other route answers 404. A second 256 KB would be a
  Personal LaserWriter 300's page on rev A. Every Mac's print is a capture.

### Not in it

The fast modes (a II or 1200 at 230,400 baud; a Colour StyleWriter's clock on HSKi). A queue:
one job, the rest wait. StyleWriters behind someone else's adapter.

### Checks

- `test_adsp.c`: the codec's vectors, opens both ways with their repeats and who may answer
  one, the end-of-message arithmetic, windows shut, small and opened, resends, probes,
  Forward Reset, close, and all of it across the sequence numbers' wrap.
- `test_sw_adapter.c`: the rename replayed from the real capture; the whole print in Apple's
  order and lpstyl's; busy and the questions; withdraw; an abandoned request; a Mac that
  vanishes; a pipe gone quiet; long answers beside the printer's bytes; names keeping their
  sockets; fast modes refused and the lone byte dropped.
- `test_sw_session.c`: every reset the session writes is said (`reset_sent`), and the wait
  between two.
- `test_localtalk.c`, `test_ethertalk.c`, `test_printers.c`: the names answered by type, type 7
  to the right socket, the offers.
- `etpeer.py` learns ADSP and both orders of print; `swahost` (a link, the adapter and the
  pump on the host, the port a pty to `swfake.py`) prints a page from it that decodes dot for
  dot, with each fault (`make -C tools/rip swainterop`).
- Board 1: Basilisk II with Apple's Color SW 2500 driver over the tunnel to the 2500, then
  the SE/30 on the cable; busy both ways with AirPrint; the plug pulled mid-page. An owner's
  Mac for the 1500 driver's models. Rev 2026.11's two StyleWriters are host-tested only.

### Calls made here (check while reading)

- Sockets 0x89 and 0x8A, not 129: lpstyl and TailTalk take the socket from the name's answer
  and InkTalk, used with real Macs, registers whatever it is given. If Apple's driver turns
  out to want 129, the ImageWriter's listener moves.
- The call-back at the first of close, `0012` or 1 s.
- The 13-byte attention ack; answers without end-of-message marks, as the capture shows.
- A request is busy (`00 01`), not refused, whenever the record's StyleWriter is not surely the
  printer on its port at that moment: the list no longer offers it, another port record's
  printer or an ImageWriter is there, a first sighting of another printer awaits its repeat,
  a clock is on the handshake, or a line came up in the last 10 s and has not been asked what
  it is. The Mac's driver asks again, and the name goes from the Chooser once the list knows.
- A link's adapter (3.7 KB of PSRAM) is made when the link is first offered a StyleWriter, so
  a board with none pays nothing; the pipe (5 KB) at the first job.
- The printer's first byte back in a job marks its record answering and ready. The board
  reads nothing else of what the two say: a fault during a Mac's job shows in the Mac's own
  dialog, and on the board 10 s after the job, when the port asks again.
- The reset at an unclean end is the five bytes alone, with no `E` first for the black-only
  models (a session's abort sends one): the board does not know where in a page the Mac
  stopped, so the reset can land inside a block (`docs/traps.md`).
- Resets are kept 10 s apart across senders, not only after the board's own: the driver's
  reset is taken to be the job's first bytes to the port, since the board reads nothing of
  the stream, and a session's are stamped where it writes them. An AirPrint job starting
  inside 10 s of another session's abort is not held off; that is the session's to pace.
- `000E`'s answer as InkTalk lays it out (the request first); lpstyl reads the name two
  bytes later, and only ever to show it.
- 10 s of lock after the printer's last reset, the board's or the driver's, so a job a Mac
  ends cleanly seconds after it began keeps the lock for the rest of them.
- A pipe with no stream byte and no attention either way for 300 s is ended as a Mac gone,
  though the Mac answers probes (the PAP server's figure for a quiet client). A driver that
  waits on paper still asks the printer its status through the pipe, so a real wait has
  traffic; a Mac whose driver hung with the pipe open would otherwise hold every printer
  on the board for good.
- A name keeps its socket while its record is offered; the place is the adapter's own, a
  link at a time, not the list's order.
- The two job routes share one capture (`/api/lt/job`, `/api/sw/job`): the last Mac's job of
  either kind has it.
- An open of ours is answered only by the node it went to. Our connection IDs count up from
  a random start and are 16 bits: any node could have named one.
- 343 bytes of the pipe's send buffer kept for answers, 297 for the printer's bytes: a
  StyleWriter's answers are a byte or a short line.
- Three question connections a link; the fourth place is kept for the pipe, job or no job.
- A connection that is not the pipe says the window Apple's adapter says, 1716, and its
  stream is dropped. A Mac that leaves 640 bytes of answers unread is closed.
- `0006` off the pipe gets `FF FF`: nothing waits there.
- A lone `x` or `h` whose end of message comes in a packet of its own is still dropped: the
  byte waits for it.
- The first 256 KB of a job kept, not all of it.

## Docs

- The wiki's Supported Printers rows for the seven: `Yes*` until confirmed on paper per model
  (`W-1101`'s rule), then `Yes`; the 4100/4500/6500 stay `No`. A StyleWriter page: plugging it
  in, the cable, cartridges and the cleaning key, and for rev A boards the R9 note.
- README, SPEC (a §3.16 for the StyleWriter stage, the port's two languages, the adapter),
  `docs/prior-art.md` (InkTalk, lpstyl), `LICENSE.md`'s table for anything ported, and
  AGENTS.md's "StyleWriters stay out" line, changed in the commit that makes it false.
- The August scope spec gets a superseded note on its StyleWriter paragraph.
- Copy follows `docs/voice-and-tone.md`; no forum is named as the audience.

## Licences and prior art

- **lpstyl** (Monroe Williams, BSD-2-Clause in `lpstyl.c`, `adsp.c` and `LICENSE`): may be
  ported with its notice in `LICENSE.md`'s table.
- **TailTalk** (FeralFirmware, GPL-3.0, no "or later" grant in the files): the reference; write
  our own C, as for the PAP client. Facts (command bytes, orders, margins) are used freely. If
  code is ever ported, ask for an "or later" grant first (AirWriter's firmware is
  GPL-3.0-or-later).
- **InkTalk firmware** (`GPL-3.0-or-later` in its `Cargo.toml`): may be ported with credit.
- **Apple's driver data** (the tone curves and the threshold screen, about 1.3 KB): used, read
  out of Apple's driver files by our own script, its source named in the generated header and
  in `LICENSE.md`'s table. The drivers themselves are never committed.
- Credit for the StyleWriter work goes to Monroe Williams and FeralFirmware, in the
  README's credits, the StyleWriter wiki page and `docs/prior-art.md`.

## Decided by Wells (2026-10-25)

1. The StyleWriter comes back into scope because owners restore and use them.
2. TailTalk/InkTalk, FeralFirmware's work, is the starting point, credited under the
   repository's name, FeralFirmware (Wells, 2026-10-27).
3. No StyleWriter on the bench: build against a software one and an owner confirms (plan A);
   buying one is plan B.
4. The seven lpstyl models; recognised and shared under its own name; colour with a colour
   cartridge; vintage Macs through AirWriter as Apple's adapter. (Proposed by Claude as a list,
   confirmed "sounds right".)
5. Public copy never names one forum as the audience.
6. Use Apple's halftone tables from the drivers (2026-10-26).

## Calls made in this design (check while reading)

- Two phases, AirPrint first; phase 1 merges before any StyleWriter has printed, docs starred.
  Its gate is the host tests, interop and the independent encoders; the Apple-driver captures
  when an emulator runs the drivers, kept local.
- Automatic looks send only the one-byte plain question, harmless to an ImageWriter by its bit
  timing (bench-checked); the full sequence only to a named StyleWriter. So the original
  StyleWriter is found once the person says what it is.
- Idle status by asking `1`/`2` every 10 s; a look every 5 s while the port is empty (60 s
  until W-1116).
- R9 fitted on rev A boards that run a StyleWriter until shown unnecessary; rev B fits it.
- Letter, A4 and Legal; plain paper only; draft, normal, best from the phone's quality.
- Apple's tone curve as the StyleWriter default and Apple's screen as a sixth choice (Wells
  asked to use the tables; how they fit is mine): the default screen stays the board's usual
  one, and the tables come from our own script, not TailTalk's file.
- One Clean Print Head key, one cleaning a press, no deep clean and nothing automatic.
- A StyleWriter test page drawn as a raster on the board.
- StyleWriters on the network behind an adapter are out.
- Phase 2's busy answer, rename-renames-the-record, and its sockets (0x89 and 0x8A, not 129).

## Left to the builder

Component and file names, the stage's API change for four-plane rows, the NVS fields for
model and cartridge, how the `prn` task frames bands in the job buffer, band heights and block
splits within the caps, where Apple's screen sits per ink, the bitmap face
and layout of the StyleWriter test page, the StyleWriter drawing, the exact IPP reason
mapping, `swfake`'s command interface, and ADSP's timers and window.

## Sources

- lpstyl: github.com/Godzil/lpstyl (`README.protocol`, `README.atalk`, `lpstyl.c`, `adsp.c`).
- TailTalk at `bf38658`: github.com/FeralFirmware/TailTalk (`tailtalk/src/stylewriter/`
  `family.rs`, `encode.rs`, `job.rs`, `session.rs`, `status.rs`, `raster.rs`;
  `tailtalk-core/src/stylewriter.rs`; `tailtalk-core/tests/stylewriter_cross.rs`;
  `InkTalk/firmware/src/identify.rs`, `printer.rs`, `stack_task.rs`; PRs #76, #87, #92; release
  0.13.0).
- Apple IIGS StyleWriter driver ERS delta, Rev 1.3 (1991):
  applefritter.com/files/StyleWriter.ERS_.Rev_.1.3.pdf.
- Apple spec sheets (support.apple.com/en-lb/docs/accessories/pp207); supplies per
  retrotechnology.com/herbs_stuff/cart2400.txt.
- `W-1102`'s research notes (Snow's serial bridging, the capture plan).
