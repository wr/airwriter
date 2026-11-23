# Which way AirWriter faces

2026-11-15. Decided by Wells in the brainstorm of 2026-11-15, from a mock of the turned panel
and LC top.

## What changes, in one paragraph

AirWriter sits with its sockets toward the wall, so the side away from the sockets is its
front. The art panel and the LC's acrylic top keep every dot, window and hole where it is, and
turn their words to read from the front. Every board draws its lights as seen from the front,
rev A included. Nothing on the main board, the panel's materials or the case moves.

## Why

Wells places AirWriter the way most boxes sit: cables out the back to the printer and the
wall. From his chair the name and the socket labels read upside down, and the lights that
have a direction run upside down too: the print head works up the panel, a finished page
drops off the bottom, an update fills from the top.

## Goals, non-goals, anti-goals

- **Goals.** From the front, the name, the line under it and the socket labels read the right
  way, each label over its own socket. The lights read from the front on every board.
- **Non-goals.** Moving a socket, an LED, the test button or the power light. Re-routing either
  main board. A setting for which way it faces. Renaming anything in code or the case model
  (the enclosure code calls the socket wall "front"; it keeps that name).
- **Anti-goals**, which would make this a failure even if it builds:
  - a dot window, the test hole or the power slash moving, so the panel no longer matches the
    lights under it
  - a label that ends up over the wrong socket
  - a light that should come out of a socket coming from the far edge instead

## Approaches considered

| | Approach | Verdict |
|---|---|---|
| Art | Turn only the words; dots, windows and holes stay put | **Chosen** |
| | Turn the whole art 180° | Rejected: the labels would sit on the edge away from their sockets, and the dot grid would sit over the sockets, where the board has no LEDs |
| | Move the sockets to the other edge | Rejected: a new layout for a routed board |
| Dot pattern | Seen turned from the front (the Frame 7 pattern upside down) | **Chosen** by Wells: the test dot stays in the middle column, one row nearer the front |
| | Redrawn to look as drawn from the front | Rejected: the LEDs and the test button would move on the routed rev 2026.11 board |
| Labels | A: at the left of each socket's section, as drawn now | **Chosen** by Wells |
| | B: each label turned where it sits, so at the right of its section | Rejected |
| Lights | Every board, rev A included: one rule, the sockets are the back | **Chosen** by Wells |
| | Only the boards that wear the turned art (rev 2026.11, LC) | Rejected |
| | A setting | Rejected: everyone will place it the same way, and it's one more thing to explain |

## The art

As seen from the front (the sockets along the top edge), on both the Pro panel and the LC top:

- **Labels** along the top edge, each in its socket's third: power on the left, appletalk in the
  middle, serial on the right (the LC has no appletalk label, as now). Since W-1176 they read
  power, port 2 and port 1 (the dot-panel cover design's 2026-11-23 amendment). Each sits at the left of
  its section with today's inset, except power, which starts 26 px (1.5 mm) right of the power
  slash, as in the mock.
  The label band is today's mirrored, as in the mock: the tops of the letters sit 1.53 mm from
  the edge, as far as today's descenders sit from it, all on one baseline.
- **The rules** under the labels, where turning the panel puts them (they are symmetric across
  the panel, so they land in the same three sections).
- **The power slash** stays over the power light, so it is now in the top-left corner.
- **The name block** ("AirWriter" or "AirWriter LC", the line under it, "by Wells Workshop ✱ rev
  2026.11") at the same left margin as now, the grid's left edge. Its gaps are mirrored: rule
  to name as today's name to rule, name to grid as today's grid to name.
- **The dots** exactly where they are, so from the front the Frame 7 pattern reads upside down.
- **The test dot's tab** turned back, so its squared corner and small dot are at its lower right
  as seen, as drawn today. The plated hole stays where it is.

The mock is in Wells's notes (`screens/2026-11-15-panel-turned-mock.png`, column A).

**Source.** Figma stays the source of truth. The turned art is drawn in new frames, "Frame 16 ·
from the front" (Pro) and "Frame 17 · from the front" (LC), under Frames 16 and 17, as seen
from the front, sockets at the top. Frames 16 and 17 stay untouched. Frame 16's dots in Figma
have since changed (new colours, six blank dots) and no longer match the board; the turned Pro
frame takes its dots from the art checked in under `hardware/panel/svg/`, which the LEDs are
built for, as Wells chose. The exports checked in under `hardware/panel/svg/` are drawn as seen
from the front; the panel and LC-top generators turn them into the board's frame before
anything reads them.

**What must come out the same.** Turning the art back into the board's frame must reproduce
today's dot set exactly: `panel_layout.json`, `dotmap.txt` and the LED map are unchanged, the
panel's dot windows, test hole and power-slash window are unchanged, `lc_slash.polys.json` is
unchanged, and so the case STLs are unchanged. The rules come back where they are today too.
Only the words and the test tab move. `floors.py` passes as it does today (the type is the same
size).

**The order files follow in the same change.** Rev 2026.11 and the LC aren't made yet, so the
regenerated `panel.kicad_pcb`, the combined order panel and both fab packages
(`hardware/combined/make_fab.sh`), `airwriter-lc-top.svg` and `panel-front.png` all carry the
turned art, and `docs/ORDER-NOTES.md` changes where it describes the art. The fab gates run as
today: panel DRC, `make_fab.sh`'s row counts and drill floor.

## The lights

Every effect is drawn as seen from the front: row 0 is the row farthest from you (today's row
7), column 0 is on your left (today's column 6). The test button is at row 4, column 3.

**Effects that follow the reader** keep their descriptions; they now read that way from the
front:

- the boot wave from the top-left corner
- the print head working down the rows, alternating direction
- the page rising off the top when a job ends
- an update filling from the bottom, the green rising through it when it's verified
- LocalTalk's stripes, green at the top
- the white bar sweeping left to right while TashTalk is written
- the reset countdown's hand, clockwise from 12 o'clock
- ripples from the test button (symmetric, but centred on its new row)

**Effects that come out of a socket stay at the sockets.** The sockets are now along the top
edge, just above the grid: the printer socket over column 5.1, LocalTalk's over column 2.8.
These light the same dots, moving the same way over the board, as today:

- the printer back and printer gone waves
- the cable test's strip, now the top three rows, columns 2–6, and its sweep between the sockets
- the red blink by the LocalTalk socket when TashTalk doesn't answer
- LocalTalk on, A Mac heard and TashTalk written: the stripes and the green ripple come out of
  the LocalTalk socket, so they now travel down from the top

**Where it applies.** Every build: rev A, rev 2026.11 and the LC. The LC has one light, so it
shows no change. Board 1's lights turn while its printed words still read the old way; it
keeps its rev A panel.

**Everything that draws the lights follows:** the settings page's dot mark beside the name (it
shows the panel as seen from the front), the page preview's `/api/panel` playback, and
`tools/lights`, which draws the wiki's GIFs.

**The cable test's picture follows the art.** The settings page's Test a Cable dialog draws the
cover's label strip with the two plugs hanging below it, as the box reads today. It is redrawn
as the box reads from the front: power, appletalk and serial left to right, the rules under
the labels, the plugs and the cable loop beyond them in the appletalk and serial sections. A
mock of it goes to Wells with the rest before anything is committed.

## Trying it before anything is committed

1. **Lights on board 1.** An OTA of the turned lights on top of board 1's bench build, after
   Wells holds prints and `/api/status` shows `printing=false`. Wells watches from his chair: a
   power-up, a print (to the LaserWriter, or the ImageWriter if he says so), the page leaving,
   and a cable test.
2. **The art.** A true-scale print of the turned Pro panel (62.00 × 104.27 mm) to lay on board
   1's case.

Commits, the PR and the docs follow his OK.

## Docs that change with it

In the commit that changes the behaviour: `docs/SPEC.md` where it places the sockets or the
panel's rows; the status-language design's amendment block (the front, the sockets now at the
top, the cable strip's new rows and columns); the dot-panel cover design's amendment block (the
labels at the top edge, art drawn as seen from the front, turned by the generators); the LC
acrylic top design (its new frame); `docs/traps.md` if anything there assumes the old way.
The enclosure design calls the socket wall the front; a line in its amendment says the box
sits with that wall at the back.

After Wells's OK, outside this repo: the wiki's light GIFs and any photo or text that places
the sockets in front, and the shop's AirWriter page, its 3D model's panel art and lights, and
its render (which the settings page also uses, `tools/webui/airwriter.webp`).

## Left to the builder

- How the generators turn the exports into the board's frame.
- Whether the LED map carries front-view positions or the light code turns them.
- The tests that hold the above: the generated files that must not change, which way each
  effect runs from the front, and that each socket effect lights the dots nearest its socket.

## As built, 2026-11-15

- **The turn.** `hardware/panel/front.py`'s `to_board()` turns an export 180° as text: path
  data, circles (or the matrix Figma places a flipped circle with), rects. It refuses anything
  else it would turn wrongly (another transform, a relative command, a line, an image).
  `panel_layout.py`, `svg2poly.py` (`front` argument) and `lc_top.py` read the art through it.
  A circle under 20 px is art (the test tab's dot), Figma's `<rect>`s are art (the rules), and
  `lc_top.py` welds the test tab in whichever quadrant it sits.
- **The lights.** `LG_MAP` stays in the board's rows and columns; `lg_row()` and `lg_col()`
  give each LED's place from the front, and every effect is drawn in those. The socket anchors
  are `PORT_ROW` −1.6, `PRINTER_COL` 5.1, `LT_COL` 2.8; LocalTalk on runs down from the top.
  Per-dot hashes key on the front positions. The LC's one light is unchanged.
- **What came out the same.** `panel_layout.json`, `dotmap.txt` and the LED map byte for byte.
  `fr4.polys.json` and `lc_slash.polys.json` within 0.0004 mm, so `airwriter-lc-frame.stl`
  changed only in rounding (same 10,919 triangles and bounds, volume within 0.001 mm³). The
  LC's fab package came out the same but for its dates and is unchanged.
- **Tried.** Wells watched board 1's lights from his chair (power-up, a test page on the
  Personal LaserWriter 300, the page leaving) and laid a true-scale print on its case. The
  cable test's lights were not run on the board (it needs one cable in both ports).
