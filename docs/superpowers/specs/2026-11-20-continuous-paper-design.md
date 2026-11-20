# Continuous paper on ImageWriters

2026-11-20. `W-1173 Continuous paper on ImageWriters: a banner prints as one unbroken strip`.
Decided with Wells on 2026-11-20 (the longest banner), from a test in the iPhone simulator.

## What changes, in one paragraph

An ImageWriter's AirPrint printer offers continuous fanfold paper beside Letter: 8.5 in wide,
1 in to 10 ft long, no top or bottom margin. An app that prints banners asks iOS for a strip
of the banner's length and the board prints that page straight through, from the first fold
to the end, then feeds to the next fold so it tears off clean. Letter stays the default, and
everything that prints Letter today prints exactly as before.

## Why

A separate session is designing a banner app for ImageWriter owners, an homage to
Broderbund's BannerMania. It prints through the ordinary print screen, so it works with
whatever connects the printer; AirWriter is the one Wells uses. On a printer that offers
continuous paper a banner should print with no breaks. Printed as Letter pages today, a banner
gets about 1.5 in of white at every fold: the board drops each page's first inch (the Top
margin setting) and iOS keeps the page's 1 in top and 0.5 in bottom margins blank.

## Goals, non-goals, anti-goals

- **Goals.** A roll-aware iOS app prints a banner up to 10 ft as one strip on an ImageWriter II,
  LQ or original ImageWriter. A Mac app can do the same with a custom paper size.
- **Non-goals.** A paper choice people pick by hand on an iPhone (iOS has none for a roll).
  Continuous paper on StyleWriters or LaserWriters, which take cut sheets. A settings-page
  control. Tear-off positioning beyond the next fold. Wiki and shop text, which follow Wells's
  approval of a bench print.
- **Anti-goals**, which would make this a failure even if it builds:
  - Safari, Photos or any app that doesn't ask for a roll printing differently than today
  - a Mac's existing queue for the printer breaking
  - a runaway page feeding more than about 10 ft (the cap is a page's: copies or several
    pages each get their own 10 ft, as each is its own strip)

## What the test showed

A pretend ImageWriter on this Mac answered with the board's real attributes (the IPP golden
answer) plus a continuous roll, three ways: Letter only, the roll in `media-col-ready` too, and
the roll only in `media-col-database`. An iPhone 17 simulator on iOS 26.5 printed to it from
Safari, Photos and a throwaway app that prints a banner the way Apple's PrintBanner sample does.

- iOS treats the printer as roll-fed only when the roll is listed as loaded, in
  `media-col-ready`. Listed only in `media-col-database`, iOS ignores it.
- The banner app's `printInteractionController(_:cutLengthFor:)` was asked first, with a paper
  8.5 in by 10 ft (612 × 8640 pt, the range's maximum), printable 0.25 in in from each side and
  the full length. `choosePaper` then offered Letter and the roll at the cut length, and the
  page was drawn 612 pt wide by the cut length with no top margin.
- The print sheet's Paper Size row still read "US Letter" for the banner; its preview showed the
  strip. There is no roll in the paper list, so nothing in the print sheet can be named
  "Continuous".
- Safari and Photos listed only "US Letter, Plain Paper" and previewed Letter pages.
- The simulator renders a job but never sends it, so what iOS puts in a roll job's Print-Job is
  not seen. The board reads both the job's paper and the page's length (below).

CUPS's source (Apple's fork, `ppd-cache.c`) turns a range in `media-col-database` into custom
page sizes in the PPD a Mac makes for a driverless printer, with the largest of each margin
list as the custom sizes' margins.

## Approaches considered

| Approach | Verdict |
|---|---|
| One printer, two papers: Letter (default) and a loaded roll | **Chosen**: the test shows everyday apps can't reach the roll |
| A second printer per ImageWriter offering only the roll | Rejected: another printer in every list and another of the six Bonjour slots, for nothing the first doesn't do |
| A settings switch between sheets and continuous | Rejected: a step the test shows isn't needed, and advertised values that change under one UUID |
| Detect a banner from the page alone | Rejected as the only signal: a banner exactly 11 in long would print as Letter and lose its first inch |

## The design

### What the printer offers

For the three ImageWriters only (`printer_caps`), beside today's Letter:

- `media-supported`: `na_letter_8.5x11in`, `roll_min_8.5x1in`, `roll_max_8.5x120in` (PWG 5101.1's
  roll class).
- `media-ready`: `na_letter_8.5x11in`, `roll_current_8.5x0in`.
- `media-col-ready` and `media-col-database`: Letter as today, then the roll: `media-size`
  `x-dimension` 21590, `y-dimension` 2540–304800 (rangeOfInteger), margins top 0, bottom 0, left
  and right 635, source `main`, type `stationery`.
- `media-size-supported`: Letter, then {21590, 2540–304800}.
- `media-top-margin-supported` 2540, 0; `media-bottom-margin-supported` 1270, 0.
- `media-default` and `media-col-default` stay Letter.

The test's pretend printer also sent `media-col-supported`, which the board doesn't. iOS doesn't
need it: the firmware's own encoder (`gpa_server`) behind the same test gave the same cut-length
call (612 × 8640 pt) and a banner drawn 612 × 2880 pt with no top margin, so the board doesn't
add it.

No `CONFIG_AIRWRITER_UUID_EPOCH` bump: iOS asked for `media-col-ready` at every print sheet in
the test, and a new UUID would orphan every Mac's queue. A Mac queue added before this keeps
working on Letter; removing the printer and adding it again gives it the custom sizes, as with
the icons (SPEC §3.5).

### Which pages print continuous

- **The job asks for it.** `ipp_req` reads the Job group's `media-col` `media-size`
  `y-dimension`, and the `media` and `PageSize` keywords. A length more than 1/8 in from Letter's
  27940, or a keyword naming a roll or a custom size (`roll_`, `custom_`, `Custom.`), makes every
  page of the job continuous.
- **The page is longer than Letter.** Otherwise a page more than 1/8 in longer than 11 in prints
  continuous; anything else prints as Letter does today. A shorter page (the 2 × 1 in checker
  fixture) stays on the Letter path, so `checker.expected.bin` and the on-board checker test are
  unchanged.

### How a continuous page prints

- Every job already starts at top of form, the head about 1 in below a fold (§3.8).
- The Top margin setting doesn't apply: no rows dropped, no paper added. The page's first row
  prints at the head.
- Rows past 10 ft (8640 rows at 72 dpi) are read and dropped, with one log line.
- The page ends with the form feed every page ends with, which on fanfold feeds to the next fold
  past the strip's end, so the strip tears off at folds. A job's next page starts a new strip.
- Colour, the screens and the page count work as for any page.

### Mac

A queue made after this has custom sizes 8.5 in wide and 1 to 120 in long, with margins
0.25 in at the sides, 1 in at the top and 0.5 in at the bottom (CUPS takes each margin list's
largest). The board prints those margins as blank paper at the strip's two ends, not at its
folds.

## Testing

- Host: `ipp_req` reads the roll length and keywords; the ImageWriter's attributes and IPP
  golden answers include the roll and still fit `RESP_MAX`; `ripjob` prints a continuous page
  with no rows dropped and stops at 10 ft; a Letter page is unchanged. `make -C tools/rip test`,
  `make -C tools/rip regress` (byte for byte, no fixture regenerated) and
  `make -C tools/ipp test`.
- `tools/ipp/build/gpa_server` builds again (its Makefile lost the test page's include path)
  and, advertised briefly, shows the simulator's banner app the roll from the real encoder.
- Bench, board 1, with Wells's OK (the ImageWriter is loud): an iPhone Letter page from Safari
  prints as before; a banner sent with `ipptool` as a roll job, and one sent as a long page
  with no paper named, each print as one strip and stop at the next fold.

## Left to the builder

The shape of the roll in `printer_caps_t`, how the job's flag reaches `ripjob` through
`job_pipe`, and the log line's wording.
