# Colour printing over AirPrint — design

Linear: `W-1064 Colour printing over AirPrint with the ImageWriter II colour ribbon`.
Written 2026-10-17. Amends SPEC §3.2 (TXT `Color`, `printer-uuid`), §3.3 (the URF string), §3.5 (printer
attributes) and §3.6–3.8 (decode, halftoning, output stage), and lifts the "Colour" exclusion in `plans/2026-08-04-rip-host-pipeline.md`.

## Intent

AirWriter tells AirPrint clients it is black-only, so an iPhone or Mac sends a grayscale page
and every colour comes out as black dots. Wells has Apple's four-colour ribbon in his
ImageWriter II. With that ribbon fitted, a colour page printed from an iPhone or Mac should
come out in colour; with a black ribbon, nothing should change.

## What the printer offers

From the ImageWriter II Technical Reference (Wells's copy, not in the repo):

- **Ribbon colour is selected by `ESC K n`**, n an ASCII digit: `0` black (the default), `1`
  yellow, `2` magenta, `3` cyan. `4`–`6` are orange, green and purple, which the printer makes
  itself by overprinting two bands (Table 8-6, p. 123; Table A-18, p. 146).
- **The colour option turns itself on only when a colour ribbon is fitted** (p. 123; Table A-1
  hard-reset defaults). With a black ribbon, colour commands print black.
- **The fastest way to print colour graphics** is a whole strip in one colour, a carriage return
  with no line feed, the same strip in the next colour, and so on (p. 123).
- **Print yellow first**, so the darker inks on the paper don't dirty the yellow band (p. 123).
- **Self-ID: `ESC ?` makes the printer send `IW10CF`**, with `C` only if a colour ribbon is in
  and `F` only if a SheetFeeder is fitted (Table 6-7, pp. 88–89). The reply comes back on the
  printer's TxD, which rev A already wires to the UART's receive pin (SPEC §2.4); nothing reads
  it today. It needs a selected printer (status-language spec) and is acted on only when the
  printer reaches it, so it is asked only while the printer is idle.

## Goals, non-goals, anti-goals

Goals
- With the colour ribbon fitted, iPhone and Mac print sheets offer colour (and a Black & White
  switch), and a colour page prints in colour, with nothing to set up.
- AirWriter checks the ribbon itself before each colour job, so a colour page never prints
  on a black ribbon as overprinted black.
- A page that is black text with some colour costs extra time only on the strips that have
  colour.

Non-goals
- LocalTalk jobs. They pass the Mac driver's own bytes through untouched, so colour there is
  the driver's business already.
- The test page (stays black), the LED panel, PWG raster.
- Colour matching or calibration beyond a simple, tunable split into four inks.
- The printer's own orange, green and purple.

Anti-goals (a failure even if colour works)
- Anything changes for someone with a black ribbon: what clients see, the UUID, or the bytes
  sent.
- A grayscale job's bytes change: `fixtures/checker.urf` must still rip to crc32 `8ee84844`,
  and the iOS fixture to its expected stream.
- A colour job on a black ribbon comes out as a mess of overprinted black dots.
- A phone holds jobs at "Waiting" because what AirWriter advertises changed under its UUID.
- The printer is left on a colour after a job, so the next job's text comes out yellow.

## Design

### Knowing the ribbon

AirWriter keeps a ribbon state: `colour`, `black` or `unknown`. It sends `ESC ?` and reads the
reply (timeout about 1 s; `C` in the reply means colour):

- each time the printer becomes ready while nothing is printing, and
- before each AirPrint job's first byte, under the job lock.

No reply leaves the state as it was. Never during a LocalTalk job or the cable test. The state
is saved in NVS when it changes, so a reboot starts from the last ribbon seen; `unknown` only
until the printer has answered once. `/api/status` reports `ribbon`, and the web page shows it.

A colour page prints in colour only when the ribbon state is `colour`; otherwise it prints as
gray (below).

### What AirPrint clients see: following the ribbon (first try)

Wells chose to try this first (2026-10-17), with the switch below as the fallback.

- Ribbon `colour`: `color-supported` true; `print-color-mode-supported` `monochrome, color`;
  `print-color-mode-default` `color`; `urf-supported` and the TXT `URF` gain `SRGB24`; TXT
  `Color=T`.
- Ribbon `black` or `unknown`: exactly what is advertised today.

All under today's UUID. When the state changes, the attributes change at once and the mDNS TXT
record is re-announced. Saving the state keeps a reboot from flipping it back and forth.

**The risk, and the test.** iOS keys a printer on its `printer-uuid` and caches its
capabilities under it (`docs/traps.md`): changing `operations-supported` under one UUID left
every iPhone job at "Waiting". Whether a change to the colour attributes does the same is
unknown. Board 1's phones have it cached as black-only, so the first colour advertisement is
the test: the iPhone's print sheet should offer colour once reopened (allowing for the cache
to age out), and jobs should print, not wait. If a black ribbon is to hand, the same test the
other way. If it fails, fall back.

### Fallback: a switch and a second UUID

Only if the iPhone does not pick up the change. A **"Colour printing" switch in the web
settings**, off by default and saved in NVS. Off advertises today's values under today's UUID;
on advertises the colour values above under a **second UUID**, derived from the MAC like the
first but distinct from it. Each UUID's capabilities then never change. Flipping the switch
makes phones meet a different printer: the other identity's entry lingers for about 75 minutes
(stale mDNS twins), and a Mac's CUPS queue, which pins `uuid=`, has to be removed and added
again once per flip. It takes effect at a restart, which the web page does when nothing is
printing. The ribbon check still decides colour or gray at print time, and the web page
suggests the switch when the ribbon and the switch disagree.

### The colour raster path

A page arrives as 8-bit gray (`W8`, as today, also what Black & White sends) or 24-bit sRGB.
Gray pages go through the existing path untouched.

An sRGB page with the colour ribbon in:

1. **Split each pixel into four inks**, 8 bits each: cyan = 255 − red, magenta = 255 − green,
   yellow = 255 − blue, then black = the smallest of the three, taken out of each. So greys and
   black text print with the black band alone, in one pass, which is the cleanest black and
   saves the colour bands. *My call;* the split is one small pure function, so the amount of
   black can be tuned after a test print.
2. **Scale and dither each ink on its own**, with the same resampler and the same dither the
   board uses for gray. Four of each per page; memory is small (~30 KB).
3. **Print each 8-row strip in up to four passes**, in the order yellow, magenta, cyan, black:
   for each ink with at least one dot in the strip, `ESC K n`, `ESC G` with its columns, then
   CR with no LF. An ink with no dots in the strip is skipped. Then the LF to the next strip,
   held back the way the gray writer already does it.
4. **Each pass stops at its last inked column** (a shorter `ESC G` count). *My call:* at 9600
   baud a full-width pass is about 1.2 s, so this matters most for colour, where passes are
   often sparse. Colour pages only; the gray path keeps its bytes.
5. **The page ends on black.** If the last pass printed wasn't black, `ESC K0` before the form
   feed. A colour job that is cut short also gets `ESC K0` after W-1041's resync.

An sRGB page with a black ribbon (the switch is on but the colour ribbon is out) is turned
into gray (standard luma weights) and printed by the gray path.

## Cost

- **Time.** A Letter page is 99 strips. In black it is about 2 minutes at 9600 baud. A
  full-colour photo could take up to 4 times that, plus the ribbon's moves between bands
  (unmeasured). Text with a coloured heading costs a few extra passes.
- **Job size.** sRGB URF is up to three times the size of gray. A mostly-white document still
  fits the 1 MB PSRAM job buffer easily. A full-page colour photo at 144 dpi is about 5 MB
  before compression and will not fit. It then streams at the printer's pace, as oversize jobs
  already do, with the phone's upload open for the whole print. Whether iOS waits that long is
  unknown. Measure real captures before deciding whether to do anything (a bigger buffer
  helps only part of the way; board 1 has 2 MB of PSRAM).
- **Ribbon.** Colour ribbons are costly and wear by band; the black-for-greys split keeps
  wear on the colour bands down.

## Verification

- **Host tests:** the split (white → nothing; black and greys → black only; pure yellow,
  magenta and cyan → that ink only); the colour strip writer (pass order, empty passes skipped,
  CR without LF between passes, trimmed counts, ends on black); sRGB with a black ribbon
  printing the gray path's bytes.
- **Fixtures:** a real iOS colour capture (`tools/ippcap` advertising colour) with its expected
  stream; `make -C tools/rip regress` unchanged for the gray fixtures. The host `rip` preview
  shows the four inks in ribbon colours, so the split can be judged before paper.
- **Board 1:** self-ID read with the colour ribbon (and a black one, if there is one); an
  iPhone colour print; the same page with Black & White on; the checker print still crc32
  `8ee84844`.

## Left to the builder

Where the split, the colour strip writer and the self-ID
read live; whether to resample the
three RGB channels before the split or the four inks after; the exact self-ID timing and
parsing; and the web page's wording for the ribbon.
