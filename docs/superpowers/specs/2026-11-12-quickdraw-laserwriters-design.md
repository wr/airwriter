# QuickDraw LaserWriters on the printer port — design

`W-1102 QuickDraw LaserWriters on the printer port (Personal LaserWriter LS/300, Select 300)`.
Agreed with Wells in chat on 2026-11-12, after a throwaway spike on board 1 printed pages on
his Personal LaserWriter 300. Builds on `W-1116` (port printers), `W-1103` (StyleWriters: the
run coder, the port's questions), `W-1082` (LaserWriters: the 300 dpi rip) and `W-1138` (each
printer's own Apple icon). Reverses the QuickDraw non-goal in
`2026-10-21-laser-printers-design.md`.

## Why

Apple's QuickDraw LaserWriters have no PostScript. The Mac drew every page and sent the dots
down the printer port, so without a vintage Mac and Apple's driver they print nothing. They are
lasers: quiet, sharp, and fed with toner that is still sold (the Personal LaserWriter 300 takes
HP's 92274A). With the ImageWriters and the StyleWriters, they are most of the Apple printers
that plug into a Mac's printer port. AirWriter makes them AirPrint printers.

## Goals

1. A Personal LaserWriter 300 on J2 is found by itself within a few seconds of being switched
   on, named, shared over AirPrint, and prints any page an iPhone or a Mac sends, at 300 dpi.
2. Its state (out of paper, cassette out, waiting for a sheet in the manual slot, jam, cover
   open, engine fault, switched off) reaches the phone, the Mac, the settings page and the panel
   within seconds, and the printer's own three lights mean what they meant on a Mac.
3. Photos print with Atkinson, the most detailed screen. A page too big for the printer's
   memory in Atkinson prints in Apple's own 300 screen instead. No page fails for its size unless
   it fails in Apple's screen too, which takes a page of dense small type.
4. Rev A as built. No hardware change, no added pad.
5. The LS and the Select 300 later become a model-table row and a bench test, not a new driver
   (Wells: generalize across the QuickDraw printers where it makes sense).

## Non-goals

- **The Personal LaserWriter LS and the LaserWriter Select 300.** They use the same Apple driver
  but keep the page differently (page memory with pointers), have their own tray and density
  commands, and the Select 300 has a Fuji Xerox engine where the 300 has a Canon one. Nobody here
  can test them. Docs list them as not yet. A board that finds one says so (below).
- **Vintage Macs printing to the 300 through AirWriter.** Apple's driver wants the printer on
  the Mac's own port. Like the StyleWriters' phase 2, a design of its own.
- **PhotoGrade** (never on the 300), **print density** (Apple's default, 32, always), the Select
  300's lower cassette, and printing while a page still loads (`8A 02`, page-memory models only).
- **Copies.** `copies-supported` is advertised 1–99 for every printer today and no printer
  repeats a page; what clients send for copies is unchecked. The 300 could reprint a page from
  its own memory cheaply (`81 00`, `8A 01`), which a later change can use.
- The SCSI QuickDraw LaserWriters (no SCSI port), the Portable StyleWriter and the Color
  StyleWriter Pro (not looked at).

## Anti-goals

- A page that half prints, prints garbage, or is silently dropped. A page is only told to print
  once it is in the printer whole.
- A third copy of the port machinery. The port's family stops being a StyleWriter boolean and
  becomes one switch the three families share.
- Anything that disturbs an ImageWriter or a StyleWriter on the same port: the raster regression,
  the StyleWriter host tests and both printers on board 1 come through unchanged.

## What the printer and the board allow

Everything here was measured on board 1 (rev A, the original SP3232EEN as U3, R9 open) with a
Personal LaserWriter 300 on J2, or read from Apple's LaserWriter 300 v1.1 driver (Apple
Developer CD, October 1993; kept outside the repo) and checked on the printer. TailTalk's
`lw300/README.md` (FeralFirmware, from the 1.2 driver) documents most commands and agrees.
The bench notes and the driver stay outside the repo; what a builder needs is here.

**The link.** The printer drives a clock on J2 pin 2 (HSKi in the Mac's naming; the firmware's
`cts_pin`, `s_hsko`, IO8 on rev A) the whole time it is on: 533 kHz, duty 50.7% as rev A's U3
delivers it to IO8. The link is synchronous on that clock: one bit per clock, 8N1, LSB first,
both ways. Every packet to the printer starts with a BREAK (Apple: about 95 bits low), then at
least 113 bits idle, then the bytes; Apple left at least 310 µs between packets. Data sent on
either edge of IO8 was taken, so rev A has margin both ways. Replies come back on RXD as plain
8N1 bytes at the clock rate, which UART1 reads when set to the measured rate. The printer sends
a BREAK of its own to say "read my status".

**Sending without a core.** The ESP32-S3's UART takes no outside clock. GP-SPI2 as a slave,
clocked from IO8 through the GPIO matrix, with its MISO on IO17 and DMA from internal RAM, sent
whole pages with no core watching the clock: 3,175 rows in 4.7 to 13.5 s by size, every chunk
read back off the pad (MOSI fed from IO17) and compared, 0 bad bits in all runs. The spike drove
chip select through the matrix (constant 0 when a chunk loads, constant 1 from an `esp_timer`
in the chunk's tail of marks) and handed IO17 to a plain GPIO at mark between chunks. The end
timer was up to 873 µs late against a 960 µs margin: the real driver needs more margin or a
better end. The spike took IO36 (J6's SDA) as the pad ESP-IDF's driver wants for chip select;
the real driver must not.

**Commands** (`BREAK cmd arg cmd^arg`). Only `82`, `83` (value packets too) and `8E` reply,
one byte each, and only `8E` may answer `00` and then a second byte that is the status. `81`,
`84` to `89`, `8A`, `8B` and row packets never reply. Bit 0 of an `83` reply is odd parity; bit
7 set means refused, and with another bit set, send it again (Apple's driver tries an `83` five
times and waits up to 5 s for any reply; the 300 answers an `83` in about 30 ms, an `82` before
its third byte is in).

| Packet | Meaning on the 300 |
|---|---|
| `81 00` | Reset the link and the page memory's pointers. No reply. |
| `82 00` | Controller status. `A4` idle. Bits 7–6 controller type (`10`: no page RAM pointers). Bit 0 memory full; bit 1 memory past about half (set from about 300 KB of page); bit 3 engine not ready (set with no paper). |
| `83 40` | Tells the engine the controller clocks its status. Sent after `81 00`. |
| `83 3E` | Engine identity: `29` is a Personal LaserWriter 300 (`0D` LS, `23` a Japanese model, else Select 300, per Apple's driver). |
| `83 01` | Engine status: `20` paper moving, `10` page to be sent again (after a jam), `02` a fault stored. |
| `83 02` | Paper path: `08` jam, `04` cover open or no toner cartridge, `20` cassette not in properly. |
| `83 04`, `83 07` | Service faults: fuser, laser, scanner motor. |
| `83 2A` | Paper present: `20` cassette, `40` manual slot. |
| `83 4C` / `83 4F` | Take paper from the cassette / the manual slot. |
| `83 5D` | Clear the "send the page again" request. |
| `83 9D`, value | Sent with `21` before each job. A value goes as a second `83` packet, `(v << 1)` with odd parity. |
| `83 A7`, value | Density, 0–63, Apple's default 32. |
| `84 v` | The front lights and engine lines: bit 0 Ready, bit 1 Paper, bit 2 Jam, bit 3 engine reset. Write only. |
| `85`/`86`, `87`, `88`/`89` | Top offset in rows, left margin in bytes, row width in bytes, each sent negative (`88 D4 89 FE` is −300). |
| `8B 8B` ×2, then `8A 01` | End the page, then print it. `8A 00` stops. |
| `8E 00` | Status: bit 2 page done (cleared by the read), bit 1 error, bit 0 memory full. |

**A page.** One packet per row: `BREAK 4F|4E len codes... xor`, the length a 12-bit negative
count (`4F FF` one byte of codes, `4E FF` 257), the last byte the XOR of every byte before it.
The codes are the StyleWriter's run codes with no XOR to the row before: literal, repeat, white
and black runs, counts to 62, a lone `80` for a blank row. Apple's coder uses all four classes;
`sw_code_row` with no previous row writes exactly this. Letter is 3,175 rows of 300 bytes (2,400
dots, 8 in), centred: Apple sends `87 DD` (left margin 35 bytes) and `85 C2 86 FF` (top 62 rows).
Apple's driver offers Letter, Legal, Executive, #10 and Monarch envelopes and A4, from the
cassette or the manual slot, and works out each size's offsets from paper and image size:
`85`/`86` is minus half of (paper length − image length) in rows, and for `87`, with
m = (paper width − image width + 0x1A0) / 2 in dots, Apple sends −(m >> 3), adding any of m
past 0x1FF to the row width instead.

Apple's job: `81 00`, `83 40`, `83 3E`, `82 00`, `87`, `85`, `86`, `83 9D`/`21`, `83 A7`/32,
`84 00`, `88`, `89`, `83 4C`, `84 01`, the rows (blinking Ready with `84` bit 0 about once a
second), then `83 2A`, `84` with the Paper light off, `8B 8B` twice and `8A 01`. On the bench
the page came out 17 s after `8A 01`. The driver learns a page is done from the printer's BREAK
and `8E 00` bit 2, and gives up after 85 s. Apple's driver sends the next page's rows while a
page prints, and on memory full waits for `82 00` bit 0 to clear; whether `81 00` during a print
cuts the page being printed is not known. Another copy is `81 00` and
`8A 01` again, with no data. A jam: `8A 00`, then `81 00`, `84 08` for about 280 ms, `84 01`,
`83 40`, `83 3E`, `82 00`, `83 5D`, the paper source again, and `81 00`, `8A 01` reprints the
page still in memory. Out of paper: `8A 00`, wait for `83 2A`, then `8A 01`.

**Unknown:** whether a row whose check fails is refused, and whether anything says so. The read
back off the pad proves what left IO17, not what the printer kept.

**Memory.** About half a megabyte, and Apple's driver never measured it: a page that ran out
mid-load was an alert and a lost page. Loads without printing put 513,287 bytes of codes in a
whole page and no more than 533,655. A full-bleed 300 dpi photo codes to 75 KB in Apple's 300
screen, 238 KB in Bayer, 366 KB in AirWriter's StyleWriter "apple" screen, and 934 to 955 KB
in Atkinson, Floyd and blue noise. A page of text codes to a few hundred KB (Apple's own text
page was 395 KB).

**Apple's screen.** For gray on the 300, Apple's driver maps each pixel through a 256-entry
curve (`GAMA` 3, 0–255 to 0–33) and inks it where an 8 × 8 clustered-dot threshold table
(`HTSC` 2, thresholds 1–32) is greater. On paper it is the 53-line laser look; Wells could not
tell it from AirWriter's "apple" screen. Atkinson was by far the most detailed of the three.

## Approaches

1. **A family of its own beside the ImageWriters and StyleWriters, sharing the port, the
   records, the rip, the coder and the status plumbing (chosen).** Its link and session are
   unlike either: a clock it must follow, packets with checksums, a page that must fit before it
   prints.
2. **A variant of the StyleWriter session.** The run codes are the same, but nothing else is:
   the StyleWriter is asynchronous at 57,600 baud with flow by buffer reports, bands and rects.
   Tangling the two would make both harder to read and test.
3. **A variant of the AppleTalk LaserWriter (`laser`).** That is PostScript over PAP; only the
   300 dpi rip is shared, and it is shared either way.

For the link, the spike tried bit-banging from IRAM with interrupts off (fine for a 3-byte
packet, not for 13 s of page) and GP-SPI2 as a clocked slave with DMA (whole pages, no core).
The SPI slave is the link.

## Phase 1: AirPrint to a Personal LaserWriter 300

### What an owner sees

They plug the 300 into the printer port and switch it on. Within a few seconds its Ready light
comes on steadily, the settings page lists "Personal LaserWriter 300" with its own icon, and
iPhones and Macs see it as a printer. A Mac's Paper Feed menu offers the cassette or the manual
slot. A photo prints in Atkinson; a full-page photo in Apple's screen. While it prints, the
Ready light blinks as it did on a Mac. Out of paper, the Paper light comes on and the phone says
so; a jam lights Jam. Switched off, it shows Offline within a few seconds. A page takes about
25 s from the phone to the tray, a little more for a photo.

### The port finds it by its clock

- **A clock on HSKi is a QuickDraw LaserWriter switched on.** No other printer AirWriter knows
  toggles that line hundreds of thousands of times a second. `serial_out`'s line task keeps
  reading the level every 100 ms, but a level read from a clock is noise, so the port also counts
  edges on the same GPIO (PCNT, or another counter that leaves the pin to UART1's CTS) and calls
  the line "clocked" above a threshold well below 533 kHz (100 kHz, say). While clocked, the
  level's edges are ignored: no line-up, line-down or 2 s settle reaches the ImageWriter logic.
- **Clocked and nothing named yet:** the board sends `81 00`, `83 40`, `83 3E`, `82 00`. `29`
  makes an auto-found Personal LaserWriter 300 record (`port_new`, shared if there is room), as a
  StyleWriter's answer does. `0D`, `23` or anything else: the port has an Apple QuickDraw
  LaserWriter AirWriter doesn't support yet, shown on the settings page as a grayed row at the
  end of the Printers list (`Printer port · Not supported yet`, nothing to open), naming the
  model where the identity names it; it goes when the printer is unplugged.
- **The plain question, `ESC ?` and the StyleWriter's status never go to a clocked port.** The
  300 sees a long space as a BREAK and a packet with a bad check as noise; nothing was harmed in
  the spike, but nothing is gained either.
- **Clock gone:** the record goes Offline within a second or two, as a StyleWriter that stops
  answering does. Clock back: the identity is asked again; a 300 is the same record.
- **Each find logs the clock's rate and duty,** so a board whose U3 delivers a marginal clock
  shows in its log. Rev A's margin was measured on board 1 only, against U3's typical figures.
- **The family switch.** Today the port's family is `s_port_sw`, a boolean, and
  `printers_test_kind` returns 1, 2 or 3 checked by number in `main.c`. Both become one enum of
  port families (ImageWriter, StyleWriter, QuickDraw LaserWriter) that `port_policy`,
  `printers_port_mode()`, the job sink in `doc_begin`, the link switch and the test page branch
  on. `port_policy` gains the clocked input and an act for the identity probe; its order of
  precedence (port_policy.h) puts "clocked" first.
- **Records.** A new `pr_kind_t` (appended) and model (`PR_MODEL_PLW300`, appended), the stored
  list's version goes to 4 with a reader for 3 (`docs/traps.md`, stored forms), and `port_uuid`'s
  family byte 3. Every switch on kind or model in `printers_core.c` (names, icons, caps, JSON)
  learns the family.

### The link

A link module for the family, the only part that touches ESP32 peripherals:

- **Out:** GP-SPI2 as a slave on the HSKi GPIO, mode 0, LSB first, DMA chunks from internal
  RAM, MISO to J2's TXD GPIO through the matrix, IO17 handed back to UART1 when the family isn't
  the port's. Packets are built as bit images: BREAK (100 bits), idle (120), bytes 8N1, then at
  least 180 marks before the next BREAK.
- **No pad for chip select.** Drive the slave's CS input from the matrix constants alone. If
  ESP-IDF's slave driver can't do that (it restores CS from `spics_io_num` on every load), set the
  slave up with the HAL/LL directly. J6 stays free, and rev 2026.11's spare pins too.
- **End of a chunk with real margin:** the tail of marks covers the timer's worst case several
  times over (the spike saw 873 µs), or the chunk ends on the DMA's own end of data. Every chunk is
  still read back from the pad and compared; a mismatch fails the job, as a broken cable would.
- **In:** UART1 at the measured clock, flow control off, BREAK from the printer seen as the
  UART's break event. Back to the ImageWriter's or StyleWriter's link when the family changes.
- **Clock lost mid-job** (switched off, cable pulled): a chunk that never starts or never ends
  within a second ends the job as Offline.
- **Pins from the board config,** so rev 2026.11 (`W-1109`) only changes numbers. Its faster U3
  carries this clock with more margin than rev A's (the rev 2026.11 design's U3 row).

### The AirPrint printer

- `RS300`, gray only (`monochrome`), Apple's six sizes (`na_letter`, `na_legal`,
  `na_executive`, `na_number-10`, `na_monarch`, `iso_a4`), margins from Apple's image area for
  each size (Letter: 2,400 × 3,175 dots centred on the sheet). The builder reads the other sizes'
  image areas out of the driver and checks Letter's against the bench.
- **Paper source per printer:** `main` (the cassette, the default) and `manual`. Today
  `media-source` is one static `main` in `ipp_attrs`; it becomes part of the printer's caps. A
  job asking for `manual` sends `83 4F` instead of `83 4C`. AirWriter never moves to the slot by
  itself.
- **Icon:** Apple's for the 300, from its driver (`ICON`/`icl8`), as `W-1138` did for the others.
- Its own endpoint and Bonjour record as every port record has (`printers.c:others_apply`).
- **Response size:** `ipp_server`'s `RESP_MAX` is 5,120 bytes and a StyleWriter's three sheets
  already take about 3.8 KB of Get-Printer-Attributes. Six sizes and two sources must fit, by a
  larger buffer (it is internal RAM; `docs/traps.md`) or a leaner answer, checked with the
  `tools/ipp` goldens.

### The page

- **Rip:** the shared 300 dpi path (`ripjob`, as the AppleTalk LaserWriters use), cropped to the
  size's image area, gray, then the record's screen and gain. The sheet is found by width and
  height: `printer_media_t` chooses by page width alone today, and Letter and Legal are both 2,550
  dots wide at 300 dpi.
- **Two screens at once.** Each row is screened in the record's screen (Atkinson by default) and
  in Apple's 300 screen, and coded both ways with `sw_code_row(row, NULL, ...)`. `ripjob` hands a
  stage one row in one screen today (`rip_stage_t.row`, one `dither` in the target), so this
  needs a gray-row stage or a second screen in the rip, made with the raster regression
  unchanged. The record's
  rows go to the printer as they are made, so ripping and sending overlap. Apple-screen rows are
  kept on the board for the whole page. The board counts what it has sent against the page budget
  (500,000 bytes of codes, under the 513,287 measured, so the printer's own count of a full page
  is never reached). A page that would pass it stops there: `81 00` drops what the printer holds,
  and the kept Apple-screen page goes instead. If that passes the budget too, the page fails with
  its reason (too detailed for the printer's memory), and the job goes on to the next page, as
  Apple's Continue did. The job then ends as failed on the phone and the Mac, and the settings
  page says The last job failed, naming the page and why. A record set to Apple's screen keeps
  nothing; its rows are the page.
- **Memory:** a 300 job splits the 1 MB job pool into a URF ring and the kept page (about
  520 KB), where a StyleWriter's has two 512 KB rings. The builder checks the rev A board's free
  PSRAM through a full-page photo job before anything goes to Wells.
- **Then check and print:** `8E 00` and `82 00` after the last row (the builder finds on the
  bench whether a row with a bad check sets `8E` bit 1 or anything else; if something does, the
  page fails rather than print a row short), then `83 2A` (paper for the chosen source), `84` with
  the Paper light off, `8B 8B` twice, `8A 01`. Page done is the printer's BREAK, then `8E 00` bit 2; with no BREAK seen,
  polling `8E 00` every half second finds it too (the spike did). 85 s with no page done fails the
  page (Apple's limit).
- **One page at a time.** The next page of a job loads only after the last one is done. Apple's
  driver overlapped them, but whether `81 00` (the fallback's drop, the jam recovery) cuts a page
  that is printing is not known, and with one page in the printer the budget is exact and a jam
  can always reprint. A page takes about its load (5 to 13 s) plus 17 s. Overlap is a later
  change, once the bench has answered that.
- **Cancel:** `8A 00`, `81 00`, Ready light on.

### During a job and at rest

- **At rest:** the board asks `83 01`, `83 02`, `83 2A` and `82 00` every few seconds, and at
  once after the printer's BREAK, so a cassette pulled out or a cover opened shows within
  seconds with nothing printing. The port task asks only while it holds the job lock, so during
  another printer's job (an AppleTalk LaserWriter's, say) the 300's changes wait for its end, as a
  StyleWriter's do today.
- **Out of paper** (or the cassette out) during a job: the page waits in the printer, the phone
  says out of paper, the Paper light is on; paper back, `8A 01` prints it. The job waits as a
  paper-out ImageWriter job does, up to the stall limit.
- **Manual slot empty** when the job asked for it: the phone says to load the manual slot
  (`media-needed`), Paper light on, and the page prints when a sheet goes in.
- **Jam** or **cover open**: Jam light, the phone says so, and Apple's recovery when it is
  cleared, reprinting the page from the printer's memory, which with one page at a time is always
  the page that jammed.
- **Engine fault** (fuser, laser, scanner): Needs attention on the page, `other` on the phone,
  with the fault named in the log.
- **Lights:** Ready on at rest and ready; blinking about once a second while a page loads and
  prints; Paper with no paper, no cassette or an empty manual slot; Jam with a jam or the cover
  open. Apple's driver lit them the same way. The board sets them; the printer never does.

### Status everywhere

The family's status table maps the printer's bits to the existing states and reasons, as
`sw_status.c` does for StyleWriters:

| Printer | Settings page | Phone and Mac (`printer-state-reasons`) | Panel |
|---|---|---|---|
| Clock, ready | Ready | `none` | Ready |
| Loading or printing | Printing | `none` | Printing |
| No paper, cassette out | Needs attention: paper | `media-empty-error` (`input-tray-missing-error` for the cassette) | Attention |
| Manual slot empty when asked for | Needs attention: paper | `media-needed-report` | Attention |
| Jam | Needs attention: jam | `media-jam-error` | Attention |
| Cover open | Needs attention: cover | `door-open-error` | Attention |
| Engine fault | Needs attention | `other-error` | Attention |
| No clock | Offline | `offline-report` | Offline |

Wording on the page follows `docs/voice-and-tone.md` and the StyleWriter's fault names; the
builder checks each state in `tools/webui/preview.py` at phone and desktop width.

### Settings

The record's screen list is the usual five plus "Apple", which for this family means Apple's
300 screen (`HTSC` 2 with `GAMA` 3), as "Apple" means the 16 × 16 screen for a StyleWriter: the
screen Apple's own driver used for that printer. Default Atkinson. Gain as for any printer (no
control on the page). `pr_post`'s "apple for StyleWriters only" check becomes "apple for families
that have one".

### Test page

A raster test page at 300 dpi, 1-bit, from the StyleWriter's black-cartridge test page
(`sw_testpage.c`): the same layout engine, face, ruler and screen swatches, made to take the
family's dpi, image area and screens, with the 300's icon as its letterhead. It goes through the
same page path, so it also proves the page budget. Wells sees it printed on the 300 before
anything else about it ships.

### Log

The family's events go in the log as the StyleWriter's do: found, identity, offline, each job's
pages, bytes and seconds, which screen each page went in, faults by name, and clock lost. No
document or person names (`make -C tools/rip test` checks).

### Generalizing across the QuickDraw printers

- **A model table** for the family, as `sw_model.c` is for the StyleWriters: engine identity,
  controller type, page memory or not, the paper-source commands, the density command, paper
  sizes and image areas, the printer's memory budget, Apple's screen and curve, icon. The 300 is
  its only row. The LS and Select 300 need what TailTalk's notes list (`83 51`/`83 52` sources,
  `83 58`/`83 5B` density, `83 67`/`83 68` job start and end, the page-memory probe, `8A 02`),
  and a printer to test on.
- **Shared with the StyleWriters:** the run coder, the test page's renderer, the per-family
  "Apple" screen, the port family switch and the fault vocabulary on the settings page.
- **Shared with every port printer:** records, sharing, status pushes, the panel and the job
  lock.

### Testing with no printer

- **Pure cores**, added to `tools/rip`'s `SRCS` with the no-writable-statics checks: the model
  table, packet and row framing, the bit image a packet becomes (BREAK, idle, 8N1), the two-screen
  page with its budget and fallback, the session (probe, job, print, page done, every fault and
  its recovery, cancel, clock lost) behind an ops struct as `sw_session` is, and the status table.
- **A software 300** (`qdfake.py`, beside `swfake.py`): decodes packets and row codes, keeps a
  memory count with the measured limit, answers the commands above, and can be told to run out of
  paper, jam, open its cover or stop its clock. It decodes the bit image too, so the packing is
  held to an independent reader.
- **`qdhost`** prints through the firmware's session to `qdfake.py` over a pty (packets framed for
  the pty; the clock is the board's business), as `swhost` and `swinterop` do, and `rip --printer
  plw300 --out-qd F` writes a page as the printer would get it.
- **Caps** in `tools/ipp/test` with goldens, and `port_policy`/`printers_core` tests for the
  clocked port, the new kind, the stored list's version 4.
- The raster regression and every StyleWriter target unchanged.

### Checked on board 1 before Wells looks

With the 300 on J2: found within seconds of power-on, Offline within seconds of power-off, found
again; the probe never sends the plain question or `ESC ?`; a letter page from an iPhone and from
a Mac; a full-page photo (Atkinson overflows, Apple's screen prints, the log says so); a
three-page job; Legal or A4 if paper is at hand; the manual slot, chosen from a Mac's Paper Feed
(check macOS shows that choice for two `media-source` values); the cassette pulled out, the
cover opened and paper run out, at rest and mid-job; cancel mid-page; the settings page, the panel
and the printer's lights in each state; the test page; free PSRAM and internal RAM through the
photo job; `stackcheck`. Then the ImageWriter and the Color StyleWriter 2500 back on J2, each
found and printing as before.

## Docs

- `docs/SPEC.md`: a section for the family beside §3.16 (link, commands, page, budget, status,
  test page); §2.4 (the clock on J2 pin 2, rev A's U3 carries it); §3.1, §3.5, §3.9, §3.15 where
  they name families; §4 for anything still unverified.
- Amendment blocks: `2026-10-30-port-printers-design.md` (the clocked port, the family enum),
  `2026-10-25-stylewriters-design.md` (shared coder, test page and "Apple" screen),
  `2026-10-21-laser-printers-design.md` (the non-goal reversed),
  `2026-10-23-printers-design.md` (the new kind).
- `docs/traps.md`: IO17 is UART1's IO_MUX pin (a GPIO write reaches it only after the pad goes
  back to the GPIO function); ESP-IDF's SPI slave restores CS from a pad on every load; the
  stored list's version 4.
- `docs/hardware-rev-a.md` and the rev 2026.11 board design: rev A's U3 carries the 300's
  533 kHz clock; the 1 MHz check on the faster U3 still stands for the StyleWriters' fast mode.
- AGENTS.md: What's built, the component table, Verification (the new make targets), Layout.
- `LICENSE.md` (Apple's screen, curve and icon), `docs/prior-art.md` (TailTalk's LaserWriter
  300 notes, the NeXT emulator Previous's Canon engine model), the README, the wiki's Supported
  Printers (the 300 Yes, the LS and Select 300 not yet), its Printers page and the shop's
  description, after Wells approves.

## Licences and prior art

- **Apple's driver** is never committed. Its `HTSC` 2, `GAMA` 3 and the 300's icon are read out
  by a tool, as `tools/stylewriter/mktables.py` reads the StyleWriters' tables, into
  `apple_tables.c` and the icon files, each with a `LICENSE.md` row (© Apple, not licensed here).
- **TailTalk** is GPL-3.0 with no "or later": read as a reference, facts used, nothing ported.
- The command meanings beyond TailTalk's notes come from reading Apple's driver; the facts are
  in this design and SPEC, the disassembly stays private.

## Decided by Wells (2026-11-12)

- The Personal LaserWriter 300 only for now; the design generalizes so the LS and Select 300 can
  follow.
- A Mac can choose the manual feed slot; the cassette is the default; AirWriter never moves to
  the slot by itself.
- Atkinson is the 300's default screen ("by far the most detailed"); Apple's 300 screen and
  AirWriter's StyleWriter "apple" screen looked the same to him on paper.
- The white lines across the first photo sheet are the printer's drum.

## Calls made in this design (check while reading)

- A page too big in Atkinson prints in Apple's 300 screen, decided per page, with no notice
  beyond the log; a page too big in both fails, the job goes on, and it ends as failed with the
  page named on the settings page.
- One page in the printer at a time: about 25 s a page, not Apple's overlap.
- The budget is 500,000 bytes of codes, below the measured 513,287.
- An LS or Select 300 on the port is named and said to be unsupported, not shared: a grayed
  row in the Printers list, not a notice (2026-11-13).
- Copies stay out, as for every printer today.
- Print density stays at Apple's default with no control.
- The test page grows out of the StyleWriter's black-cartridge page.
- The port counts edges on HSKi to find the clock; the family switch becomes an enum.

## Left to the builder

- Chip select without a pad, and the chunk's end margin.
- The other sizes' image areas and offsets, from the driver.
- Edge-count threshold and timing, and the at-rest status interval.
- The split of the 1 MB pool, and whether the kept page can be smaller.
- The test page's layout at 300 dpi, shown to Wells on paper.

## Sources

- Bench: board 1 and Wells's Personal LaserWriter 300, 2026-11-12 (Linear `W-1102` comments).
- Apple, LaserWriter 300 v1.1 driver (CODE −8191, −8195; `HTSC`, `GAMA`, `PREC` 3, `STR#`).
- TailTalk, `lw300/README.md` (github.com/FeralFirmware/TailTalk).
- Arctessa's capture of Apple's driver sending a page, decoded 2026-11-02.
- Previous (NeXT emulator), `src/printer.c`, for the Canon engine's print request and VSYNC.
