# Test page

A full US Letter page that shows at a glance that the printer, the cable, the ribbon and the
board's raster path work, what the ImageWriter II can do, and which AirWriter printed it.
It replaces the 64-row strip `test_page_task` printed before, from the same three triggers: a
tap of `test`, DIP4 at power-on, and the settings page's Print Test Page (`POST /api/test`).

Wells asked for "a full letter page that tests the various functions, text sizes, fonts,
color, images... you know." He approved the preview (round 1, 2026-10-20) as it stands: "Build
the test page as shown: keep the letterhead and the footer."

**Amended 2026-11-14** (`W-1140 One test page for every printer: the ImageWriter and PostScript
LaserWriters print the shared page`, Wells's word on a mock): every printer prints one page, the
one StyleWriters and the Personal LaserWriter 300 already printed (`W-1103`, `W-1102`), drawn on
the board as rows of dots and given to the stage a phone's page goes to. What changes from
printer to printer is its icon, its name, its dots and whether it prints in color. Wells kept
nothing ImageWriter-only: on a simulated ImageWriter II the printer's draft and correspondence
type looked like the board's face (only near letter quality was smoother), and AirPrint pages
can never use the printer's type. So the Text section, the page's own escape codes and its
traced still life are gone, and the rest of this design below is the ImageWriter's page as it
was. The page now (`testpage.h`), top to bottom:

1. **Letterhead.** The printer's own AirPrint icon (every model's, `tp_icons.h`) beside
   "AirWriter" and "Test page" in Apple Garamond Light (`tp_words.h`, at each family's dots), and
   a rule.
2. **The board**, in the 5 × 7 face: Printer (its model, and its ribbon or cartridge), Name,
   Settings (its address; on the setup network, the address and the network to join); AirWriter
   (version and IP), Halftone, Page (the printable dots and dpi), Margins.
3. **Type** (new). A sans and a serif, TeX Gyre Heros and Termes (free copies of Helvetica and
   Times), at 6, 8, 10, 12 and 18 pt, each size labelled. Stored as 1-bit rows at 720 dpi
   (`tp_type.h`, 43 KB, `testpage/tools/mktype.py`), averaged down to the printer's dots as a
   phone's page is and screened with the screen in use, so it shows how a person's documents
   print. On an ImageWriter, 6 pt is a smudge and 8 pt reads.
4. **Color** (a colour ribbon or cartridge): seven swatches (the four inks and the three they
   make printed over each other), then a ramp of each ink, two by two. **Gray** (otherwise):
   eleven steps and a black ramp side by side, and on a printer that prints in color a line
   saying why this one is gray. Every label sits under what it names. The big still life is
   gone: the screen panels show it.
5. **Halftone.** The still life and a ramp through each screen, five of them, or six with the
   family's own Apple screen (StyleWriters, the 300); the one in use ticked.
6. **Alignment.** The ruler and four one-inch squares, the captions the family's own.
7. **Footer.**

The layout is in 360 dpi units, scaled across and down apart (144 × 72 on an ImageWriter), and
fits 9.5 in down, the ImageWriter's printable sheet (from 1 in to 10.5 in). On an ImageWriter the
rows go through `iw_stage`, the strips a phone's page goes through (byte for byte what ripjob
writes for the same rows), with the top margin setting as a job has it but dropping no more than
the sheet's blank first inch: about 89 KB on a colour ribbon and 81 KB on black, against 64 KB and
41 KB before. A PostScript LaserWriter's rows go through its PostScript stage: about 375 KB
binary and 760 KB hex, against 83 KB of PostScript before, about 30 s over LocalTalk.

## The page

Laid out as an AirPrint page is, from the sheet's top edge: its content from 1 in down (where
printing starts on paper loaded as the Owner's Guide says) to at most 10.5 in, 8 in (1152 dots)
wide. The Top margin setting moves it as it moves an AirPrint page, but drops no more than that
blank inch:

1. **Letterhead.** Apple's colour ImageWriter icon (`tools/icon/imagewriter.txt`, each pixel 4
   dots by 2 rows, ordered screen, in the four inks on a colour ribbon) beside "AirWriter" in
   Apple Garamond Light, the settings page's title face, and "Test page" right-aligned in the
   same face, both as bit images. A one-dot rule.
2. **The board**, correspondence at 12 cpi, two columns (labels 11 characters; left values at
   most 34; the right column at character 47, its values at most 38). Left: Printer (model and
   ribbon), Name, Address (host name and IP), Settings (its URL). Right: Firmware (version),
   Built (build date), Halftone (the screen in use), AppleTalk (the ways in that are on, as the
   settings page's cards name them, or Off, going on to a fifth row after a comma when they do
   not fit). A value too long for its column is cut; anything
   but printable ASCII prints as one '?' a character. Before the network gives an address the
   IP reads "no IP yet"; in setup mode the Address row reads "Wi-Fi  join AirWriter-xxxx" and
   Settings "http://192.168.4.1".
3. **Text**, all in the printer's own fonts: the pangram in draft, correspondence and NLQ at
   10 cpi and in NLQ proportional; the pangram at 9, 10, 12, 13.4, 15 and 17 cpi; every
   printable character at 17 cpi; bold, underline, double width, half height, superscript and
   subscript; MouseText. Labels in correspondence at 10 cpi, samples from 2 in.
4. **Colour** (colour ribbon). The seven colour names in correspondence at 12 cpi, each in its
   own ink (`ESC K` 1, 2, 3, 0, 4, 5, 6; orange, green and purple are the printer's own
   overprints), over a solid swatch of each, 1 in wide on 1 1/6 in (14 characters) centres. Then the still life (below), 3 in by 1 1/4 in, in four
   inks through the screen in use, beside 0-100 % ramps of cyan, magenta, yellow and black.
   **Photo** instead on a black ribbon, or one the printer did not name: a one-line note, the
   still life in gray, and an eleven-step gray scale labelled 0-100 %. No `ESC K` is sent.
5. **Halftone.** The still life (1.5 in by 0.9 in) and a ramp through all five screens side by
   side, in the order Atkinson, Floyd-Steinberg, Blue noise, Ordered, Threshold, labelled at
   12 cpi within 6 dots of each panel; the one in use in bold with MouseText's check mark. One line under them: "The ticked screen prints your
   pages. Change it on the Printer page of the settings." Always black.
6. **Alignment.** A ruler 8 in long, its edge two rows thick, ticks every 1/16 in, numbered
   each inch at 10 cpi (the 8 in the line's last column). Four one-inch
   squares at 0, 2, 4 and 6 in, each with two caption lines: a quarter-inch grid ("One inch
   each way" / "Measure it with a ruler"), solid black ("Solid black" / "A white line: a pin is
   out"), vertical lines every 4 dots ("Straight lines" / "The head is aligned"), the finest
   checker ("Even gray" / "The paper feeds evenly"), the captions at 15 cpi.
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
- No `ESC F`, anywhere. Board 1's first print (round 1, 2026-10-20) put whole strips and
  colour passes inches to the right of where they belonged, and a ruler number placed with it
  at 7.95 in wrapped to the left margin. AirPrint's streams, which never place the head, print
  right on the same printer. Text is placed with spaces in a fixed pitch; a graphics pass runs
  from the left margin to its last dot, as a colour AirPrint page's do.
- No line runs past 8 in: a line that overflows wraps, its overflow printing from the left
  margin on the same row (Table A-2, LF when line full off), as the ruler's 8 did.
- NLQ never prints in a line of several CR-only passes: the colour names are correspondence.
- About 64 KB of stream on a colour ribbon, 41 KB on black.
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
