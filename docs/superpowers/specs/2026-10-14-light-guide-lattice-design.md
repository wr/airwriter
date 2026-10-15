# AirWriter light-guide lattice design

2026-10-14 · `W-1038 Light-guide lattice for the Pro build (incl. power LED)`. Approved
by Wells in the brainstorm of 2026-10-14. Builds on the tray changes in `W-1036 Tray:
rev A board can't be fitted (DIP keys vs west wall)` (branch `wells/w-1036-tray-board-fit`).

## What this is

A 3D-printed matte-black part that sits between the main board and the dot-grid art panel
in the Pro build. It has four jobs:

1. Keep each lit dot's light in its own cell, so dots don't bleed into each other.
2. Carry the panel at all four screw points and register it to the LEDs.
3. Bring the power LED, D3, up to the panel's FR4 window in its own tube.
4. Clear every part on the board.

It's authored as parametric build123d source next to the tray (`enclosure/`). Every
position is read from the board file, `panel_layout.json` or KiCad's STEP export at build
time, so a board or art change re-cuts the part on the next build.

The only earlier lattice was Wells's hand-drawn `~/Downloads/AirWriter.stl`, with no
source. It covered the dot grid only, with 0.6 mm vertical walls, rounded-corner square
cells and a 14 mm height, which Wells found a little too tall. This design keeps its
egg-crate idea.

## 1. Height and the stack

- **13.04 mm**, board top face to panel underside. Wells measured the mini-DIN shells at
  13.04 above the board; the Kycon STEP says 12.75. The panel rests on the shells, so the
  lattice must reach the same height, or a gap opens under the panel and light runs from
  cell to cell across it.
- **The tray follows** (landed with W-1036's second print fix). `params.CAVITY` goes from 12.74 to 13.04. That moves `Z_LID_UNDER`
  to 13.04 and `Z_WALL_TOP` to 14.64, and `EXT_H` grows to 21.64. The Lite lid's legs
  follow automatically. A tray printed at 12.74 leaves the panel 0.3 mm proud of the rim.
- **Pro screws: M2.5 × 18, not × 20.** The panel's standoffs are Würth 9774030151R:
  Ø5.1 × 3.0, steel, a blind M2.5 thread in the bottom 1.8 mm, and the screw tip must stop
  about 0.5 short of the seat (`hardware/panel/emit_board.py`). With the head seat at
  Z −5.60:
  - a × 20 tip lands at 14.40, past the panel's underside at 13.04, so it would bottom out;
  - a × 18 tip lands at 12.40, which is 0.64 short of the seat and 2.36 into the spacer,
    covering the whole thread.

## 2. Shape

One part, case frame (`board_geom`: X east from the board's west edge, Y north from its
front edge, Z up from its top face). Dot centres: case x = panel x, case y =
104.27 − panel y.

- **Grid.** 7 × 8 cells on the dot pitch from `panel_layout.json` (8.4545). Walls are
  `LATTICE_WALL` = 0.80 thick (two perimeters), centred on the lines midway between dot
  centres. The grid's front wall is row 7's front line (y ≈ 35.22). The outer cells run out
  to the perimeter, which is the board outline inset by `LATTICE_INSET` = 0.25 (the pocket
  is 0.30 bigger than the board, so 0.55 clears the tray walls). Inner corners are rounded
  at `CELL_R` = 1.0. Every cell is open top to bottom, lit or not. Gold dots are opaque
  copper on the panel, so an unlit cell needs no cap.
- **Posts.** H1 and H2 sit under the two top-corner gold dots (r0c0, r0c6). H3 and H4 are
  free-standing at the front. Each post is Ø8.0 (`POST_D`) and has:
  - a Ø2.80 screw bore all the way through;
  - at the top, a Ø6.40 × 3.30 pocket (`NUT_POCKET_D`/`_DEPTH`) for the standoff and its
    solder fillet, which reaches out to the 6.0 land;
  - a 45° cone from the pocket floor down into the bore, so the pocket's floor isn't a flat
    ceiling when the part prints upside down.

  The panel bears on the lattice's top face, and the screw pulls the panel down onto it;
  the standoffs never bear on the lattice.
- **West rail.** Full height, from the grid's front-west corner forward to the H3 post,
  x from the perimeter to 7.0. Its underside is cut by S1's envelope, which leaves a beam
  about 3 mm tall over the switch body.
- **East rail.** Full height, from the grid's front edge forward to the H4 post,
  x 50.0–55.75. Its underside is cut by U1's envelope, which leaves about 9.4 mm over the
  module's shield. It stays west of the antenna keepout (below).
- **D3 tube.** A rectangular tube from the board to the panel, around the panel's single FR4
  window (read from `hardware/panel/fr4.polys.json`; case x 58.42–60.28, y 1.29–6.48):
  - inside: the window plus 0.25 per side;
  - wall: 0.80;
  - clipped to the inset outline, which thins the wall at the board's R2 corner; at least
    0.6 mm must remain;
  - D3 and its resistor R17 sit inside it.

  A full-height arm joins the tube to the H4 post, cut under by R1/R2.

## 3. Clearance

- **Component envelopes.** `kicad-cli pcb export step --no-board-body --subst-models
  -D KICAD10_3DMODEL_DIR=…/SharedSupport/3dmodels` gives each part's 3D body. Its
  axis-aligned box, grown by `PART_CLEAR` = 0.40 sideways and +0.50 on top, is cut from the
  lattice, from below the board up to that top. These boxes are what notch the cell walls,
  the rail undersides and the post feet. The export is cached in `out/` and rebuilt
  whenever the board file is newer. It needs kicad-cli (path in AGENTS.md); without it the
  build fails with a message rather than guessing.
- **Parts with no model** get a box from their courtyard (`F.CrtYd`) and a height from one
  table:

  | Part | Height | Source |
  |---|---|---|
  | SK6812MINI-C ×37 | 1.60 | conservative over the 3.5 × 3.5 × 1.45 package |
  | S1 | 9.41 | `test_case` / dp08rp.mjs body top |
  | SW1 | 3.50 | footprint descr, 6.2 × 2.55 × 3.5 |
  | U7 with the PIC seated | 9.00 | the "socketed PIC ~9 mm proud" ceiling |
  | Test points, H1–H4 | none | flat |

- **SW1's reset path.** A box from SW1's plate face (x 59.35) out through the perimeter at
  y = SW1, z 0.75–2.75, so the paperclip from the east-wall pinhole reaches the switch.
- **Antenna keepout.** U1's keepout zone, read from its footprint (case x 56–62,
  y 9.7–36.7) and grown by 0.25, is cut from the whole part in front of the grid. The H4
  post loses a sliver; the D3 tube sits south of the zone. *Amended during build:* the zone
  overlaps the grid by 1.85 mm, the module's antenna ends 1.7 mm short of the grid, and
  cutting there opened lit cell r7c6, so the cut stops at the grid's front wall.
- **J2 at the H3 post.** J2's body (the same grown hull the tray and Lite lid use) trims the
  post's front-east side, as it trims the Lite lid's leg there.
- **SW3 and the test cell.** SW3 sits in cell r3c3 under the panel's Ø3.8 hole. The cell
  stays open. *Amended during build:* envelopes are stepped (`components.py`: one box per
  1 mm slice for parts over 2.5 mm), because one box from the board to the plunger tip cut
  the test cell's side walls away along their full height. The plunger is guided by
  the switch and the panel hole, not the lattice.

Light leaks through a notch are acceptable between two unlit cells, or into the front
region (it has no windows apart from D3's, and D3's is sealed by its tube). The build
prints any notch that joins a lit cell to a neighbour so it gets a look; it doesn't fail
the build.

## 4. Print

FDM, matte black PLA or PETG, 0.4 mm nozzle, 0.2 mm layers, **panel face down** on the bed,
which makes the contact face the flattest one. No supports: printed that way, every pocket,
notch and rail underside opens upward. The pocket cones keep the post pockets from becoming
flat ceilings. The overhang check is the Lite lid's: print-down is model +Z, so
`upward_flats` with the bed at Z = 13.04.

## 5. Source and build

```
enclosure/
  board_geom.py   + panel_layout() / fr4_windows() readers; antenna keepout from U1
  components.py   new: kicad-cli STEP export (cached) -> per-part boxes; no-model table
  lattice.py      new: lattice() and its pieces (grid, posts, rails, D3 tube, reliefs)
  params.py       + LATTICE_* constants; CAVITY 12.74 -> 13.04
  export.py       + airwriter-lattice
  test_lattice.py new
```

`out/airwriter-lattice.stl` is committed like the tray and lid STLs; the STEP is not.

## 6. Verification

Scripted, failing the build:
- one valid watertight solid, 13.04 tall;
- every lit cell's centre within 0.05 of its LED (board) and its dot (panel), and nothing
  over the LED footprint;
- the posts coaxial with H1–H4 to 0.01, the bores open end to end, and the pockets at least
  3.30 deep around Ø5.1;
- no overlap with any component envelope, the antenna keepout, the SW1 path, or the tray
  pocket (lattice ∩ tray = ∅);
- the D3 tube encloses D3, R17 and the FR4 window, and its thinnest wall is at least 0.6;
- no upward flat past 45° in the print orientation, outside the bed face;
- walls at least 0.8 away from notches.

By eye, on the first print: light bleed between lit cells and at notches; the panel seating
flush on lattice and shells together; the posts dropping onto the screws.

## 7. Out of scope

- The art panel and the board: neither moves for this part.
- The shop's Pro GLB: it currently embeds the old squashed STL. Refreshing it is a
  follow-up, like the tray's.
- Diffusers or light-shaping inside the cells: the FR4 and mask dots already diffuse.

## Amendments after the first prints (2026-10-15)

- **Test-button collar.** A ring in the test cell's top 2 mm (Ø3.6 bore on the plunger's
  measured 3.1–3.2), tied to the four walls by ribs. It's centred where the real plunger
  stands: 0.2 east and 0.2 forward of the KiCad model's axis (`TEST_PLUNGER_OFFSET`, found over two prints). A
  collar on the model's axis pushed the plunger off centre.
- **Edge lip, whole perimeter.** A 1.5 mm lip round the top edge, 0.1 short of the tray's
  pocket, cut only by J2/J4. A 1.5 mm skin under the panel over the front region carries it
  along the front and east edges. The antenna keepout now stops under that top band.
- **Light pipe dropped.** A clear-PETG pipe filling the D3 tube came out slightly dimmer than
  the open tube. A through-hole dome LED is the rev B candidate.
