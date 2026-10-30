# StyleWriters on the printer port — design

`W-1103 StyleWriters on the printer port`. Agreed with Wells in chat on 2026-10-25. Builds on
`W-1088` (the printer list) and the colour work of `W-1064`; shares groundwork with
`W-1102 QuickDraw LaserWriters on the printer port (Personal LaserWriter LS/300, Select 300)`.
Reverses the StyleWriter cut in `2026-08-04-printer-support-scope-design.md`.

**Amended 2026-10-30** by `W-1116 Printer port: several printers, the one plugged in shows
online` (`2026-10-30-port-printers-design.md`): the swapped-port refusal and the "changed"
notice of the 2026-10-27 amendment are replaced; the port keeps a record per model, the one
answering online and the rest offline, and the family check alone stays (`pr_port_other_family`)
so one family's bytes never reach the other. The automatic look is every 5 s, not 60, while
the port's printer is not answering, whatever the list holds; the order of the questions is
the list's, not the record's: the plain question first wherever a StyleWriter record is
listed, the port last answered as a StyleWriter, or an original ImageWriter record is listed;
the full sequence wherever an original StyleWriter record is listed. A StyleWriter record
that answers is asked its status on a line-up edge as well, so an ImageWriter put in its
place is found by the failed check and the stop's look in about 1.5 s. An ImageWriter record
whose port a StyleWriter answered is asked as any port that is not answering (the quiet rule
below went with the notice).

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

- **ADSP** in `components/localtalk` (new, pure C, host-tested against `etpeer.py` taught to
  open an ADSP connection the way a Mac does): connection open and close, the attention
  messages, sequencing, retransmission and the receive window. TailTalk's `adsp.rs` and
  InkTalk's server are the references; Inside AppleTalk's ADSP chapter is the source of truth.
- **The adapter.** On every AppleTalk connection that is on, the port's StyleWriter registers
  as `<name>:StyleWriter1200AT@*` or `<name>:ColorStyleWriter2400AT@*` by model, on socket 129
  (0x81; the port ImageWriter's socket today, and the port holds one printer). The open
  handshake, attentions and data path as InkTalk does them; each fast mode refused unless the
  port can follow it, since a Mac told the adapter takes one switches the printer over and an
  adapter that cannot follow drops off the network (FeralFirmware, 2026-10-28). The II and
  1200's 230,400 baud is likely within rev A's reach; the attentions that ask for either mode
  are in TailTalk's `tailtalk-core/src/stylewriter.rs`. A request
  while any job holds the box's lock (AirPrint, the test page, another Mac) answers busy
  (`0001`); the Mac's driver waits and asks again. The Mac's bytes go to J2 as they are under
  the job lock, the printer's come back; a Mac that vanishes mid-job leaves the printer reset.
  A rename from a Mac renames the record. The last job's bytes are kept (`GET /api/sw/job`,
  as `/api/lt/job`), which also makes every Mac print a capture.
- **Bench.** Basilisk II on the Mac with Apple's StyleWriter driver, over the tunnel to board 1,
  with `swfake` on J2 through a USB serial adapter or, without one, a firmware build that
  routes the port to the software StyleWriter compiled in (`CONFIG_AIRWRITER_SW_FAKE`, builder's
  name): Chooser finds it, a page prints end to end and decodes, busy both ways with AirPrint.
  Then the SE/30 on the cable, and an owner's Mac and printer.

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
- Phase 2's busy answer, rename-renames-the-record, and socket 129.

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
