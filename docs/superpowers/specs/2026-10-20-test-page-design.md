# Test page

A full US Letter page that shows at a glance that the printer, the cable, the ribbon and the
board's raster path work, what the ImageWriter II can do, and which AirWriter printed it.
It replaces the 64-row strip `test_page_task` printed before, from the same three triggers: a
tap of `test`, DIP4 at power-on, and the settings page's Print Test Page (`POST /api/test`).

Wells asked for "a full letter page that tests the various functions, text sizes, fonts,
color, images... you know." He approved the preview (round 1, 2026-10-20) as it stands: "Build
the test page as shown: keep the letterhead and the footer."

## The page

Inside the 0.5 in top and bottom margins AirPrint pages use, 8 in (1152 dots) wide:

1. **Letterhead.** Apple's colour ImageWriter icon (`tools/icon/imagewriter.txt`, each pixel 4
   dots by 2 rows, ordered screen, in the four inks on a colour ribbon) beside "AirWriter" in
   Apple Garamond Light, the settings page's title face, and "Test page" right-aligned in the
   same face, both as bit images. A one-dot rule.
2. **The board**, correspondence at 12 cpi, two columns (labels 11 characters; left values at
   most 34; the right column at character 47, its values at most 38). Left: Printer (model and
   ribbon), Name, Address (host name and IP), Settings (its URL). Right: Firmware (version),
   Built (build date), Halftone (the screen in use), AppleTalk (the ways in that are on, as the
   settings page's cards name them, or Off). A value too long for its column is cut; anything
   but printable ASCII prints as one '?' a character. Before the network gives an address the
   IP reads "no IP yet"; in setup mode the Address row reads "Wi-Fi  join AirWriter-xxxx" and
   Settings "http://192.168.4.1".
3. **Text**, all in the printer's own fonts: the pangram in draft, correspondence and NLQ at
   10 cpi and in NLQ proportional; the pangram at 9, 10, 12, 13.4, 15 and 17 cpi; every
   printable character at 17 cpi; bold, underline, double width, half height, superscript and
   subscript; MouseText. Labels in correspondence at 10 cpi, samples from 2 in.
4. **Colour** (colour ribbon). The seven colour names, each in its own ink (`ESC K` 1, 2, 3, 0,
   4, 5, 6; orange, green and purple are the printer's own overprints), over a solid swatch of
   each, 1 in wide on 1 1/6 in centres. Then the still life (below), 3 in by 1 1/4 in, in four
   inks through the screen in use, beside 0-100 % ramps of cyan, magenta, yellow and black.
   **Photo** instead on a black ribbon, or one the printer did not name: a one-line note, the
   still life in gray, and an eleven-step gray scale labelled 0-100 %. No `ESC K` is sent.
5. **Halftone.** The still life (1.5 in by 0.9 in) and a ramp through all five screens side by
   side, in the order Atkinson, Floyd-Steinberg, Blue noise, Ordered, Threshold; the one in use
   in bold with MouseText's check mark. One line under them: "The ticked screen prints your
   pages. Change it on the Printer page of the settings." Always black.
6. **Alignment.** A ruler 8 in long, ticks every 1/16 in, numbered each inch. Four one-inch
   squares at 0, 2, 4 and 6 in, each with two caption lines: a quarter-inch grid ("One inch
   each way" / "Measure it with a ruler"), solid black ("Solid black" / "A white line: a pin is
   out"), vertical lines every 4 dots ("Straight lines" / "The head is aligned"), the finest
   checker ("Even gray" / "The paper feeds evenly").
7. **Footer.** A rule and "AirWriter by Wells Workshop    wells.ee    github.com/wr/airwriter".

**The still life** is computed, not stored: four glossy balls (red, green, blue, yellow) on a
checkered floor under a blue sky, ray traced at the printer's own 144 x 72 dpi grid with 2 x 2
supersampling and soft shadows, so it costs no flash and has smooth tone, highlights and shadow
for the screens to show their differences.

## Constraints

- Text stays the printer's own fonts. Images are bit-image rows (`ESC G`) through the
  firmware's own halftone and ink split, so the page exercises the same raster path as a job.
- Graphics never share a line with text, so NLQ's two-pass printing never meets bit images.
  Text in several colours on one line prints one pass per colour (CR, no LF), yellow first, as
  colour graphics already do.
- Horizontal placement uses `ESC F` in 144 dpi pitch (`ESC p`), ascending along a line; a
  graphics pass starts at its first dot and ends at its last.
- No line runs past 8 in: the printer does not wrap (Table A-2, LF when line full off).
- About 57 KB of stream on a colour ribbon, 41 KB on black.
- Cancellable as before: the builder checks a stop flag before every line and strip; a cut
  page is resynced before the next job, as any cut job is.
- RAM: per strip and per row, never a page. At its heaviest (the colour band: four halftoners
  432 dots wide, one for the ramps, the strip buffers) the page holds 25.5 KB of heap, 17 KB on
  a black ribbon, less than a colour AirPrint page's own 31 KB, and the two never run at once.
  Flash: 12 KB.

## Decided by the builder

- The ink order and mask phases a colour job uses move from `ripjob.c` into `ink.c`
  (`ink_screens_*`), so the test page and jobs screen colour the same way; the colour fixture
  must not change.
- The letterhead bitmaps and the icon are generated into a committed header by a script that
  reads the Garamond embedded in `tools/webui/style.css` and the icon grid.
- The test page asks the ribbon first, as a job does, and that wait ends on a second tap.
- `tools/iwview/iwview.py` draws any ImageWriter stream as ink on paper. Its text needs the
  printer's font dot patterns, taken from the Technical Reference and kept outside the repo
  (`~/.config/airwriter/iw-fonts/`); without them text is drawn as boxes.

## Not in it

International character sets, custom (downloaded) characters, a printed date (the board keeps
no clock), PWG raster, a page for the original ImageWriter or the LQ (the page is the
ImageWriter II's).
