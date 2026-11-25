# Enclosure (enclosure/out/*.stl)

Five printed parts and a drilling jig. The **tray** is shared by both builds. The **LC frame**
carries the LC's clear acrylic top, which stands in for the dot-grid art panel
on boards built without it, and the **LC light** is a loose tube round its one
status LED; the **LC lid** is the all-printed alternative to the frame and top.
The **lattice** is the Pro build's light guide, between the board and the
panel. The **reset jig** guides a drill through the tray's east wall for the
reset hole.

    ./build.sh          # runs the tests, then writes out/*.{step,stl}

`out/airwriter-tray.stl`, `out/airwriter-lc-frame.stl`, `out/airwriter-lc-light.stl`,
`out/airwriter-lc-lid.stl`, `out/airwriter-lattice.stl` and `out/airwriter-reset-jig.stl`
are committed, so the case can be printed without a
build123d install; re-run `./build.sh` and commit
them again after any change here or to the board. The STEPs are not (6 MB, and
the same command rebuilds them).

Design: `docs/superpowers/specs/2026-09-08-airwriter-enclosure-design.md`.
Every board-derived number is read out of `hardware/airwriter/airwriter.kicad_pcb`
at build time by `board_geom.py` — nothing here is retyped, so a footprint move
re-cuts the case on the next run. `params.py` holds what this project chooses,
each constant commented with where it came from.

## Print

FDM, matte black. The tray prints floor-down (68.00 x 110.27 x 21.64 mm), the
lid plate-down (62.00 x 104.27 x 14.64 mm: the art panel's own outline, not
the pocket's; see "First print" below for the 0.30 mm/side it should clear
by). The other parts print on a 0.4 mm nozzle at 0.2 mm layers with no
supports.

### The tray

Black **PETG-CF**, 0.6 mm nozzle, 0.42 mm layers on a 0.30 mm first layer,
Arachne walls, three wall loops of 0.62 mm lines. PETG for its give, carbon
fibre for crisp detail and a smooth matte face. Below the panel seat every
wall is 2.40 mm (`THIN_WALL`); at 1.70 the long walls flexed in the hand
enough to crack along their layers. In translucent PETG make the whole floor
solid, or its infill shows through.

Support on, with Bambu's Support for PLA/PETG (or any PLA-type support) as
the interface:

| Setting | Value |
|---|---|
| Type | normal (auto), snug, threshold 30 degrees, not only on the bed |
| Top Z distance | 0 |
| Top interface layers | 3 (rectilinear, spacing 0) |
| Bottom interface layers | 2 |
| XY distance from the part | 0.6 |

The support carries the ceilings of the four foot pockets and the label
recess in the floor's underside (`RECESS_DEPTH`), and the lintels over the
ports. At an XY distance of 0.3 the support's outer line touched the pocket
walls, leaving no edge to pry it out by. A dry support spool and an open door
or top cover help it come away (Bambu's advice for a PLA-type interface under
PETG).

Give the slicer a layer-height range of 0.21 mm from Z 0.30 to 0.72: the
second layer prints as two, so the recesses come out 0.51 deep, two layers of
support, and the 1.5 mm feet stand 0.99 proud. On plain 0.42 layers they print
0.72 deep and the feet stand 0.78 proud. A recess one layer deep traps a
single sheet of support that won't come out.

Paint the seam onto the rounded corner beside port 1 (front west, in the case
frame), at the corner's 45-degree point from the bottom to the rim, with no
scarf. A seam on a flat face always shows, and a scarf printed lumpy on the
port face. Print from the project (`.3mf`), not its sliced `.gcode.3mf`:
Bambu Studio slices that again from the plate's settings alone and drops
every object and part setting.

### Every part

The tray's bottom outside edge carries a 0.80 mm 45-degree chamfer
(`BOTTOM_CHAMFER`): exactly the overhang limit, so it needs no support, and it
keeps first-layer squish inside the wall line. The top outside edge carries a
smaller 0.40 mm one (`TOP_CHAMFER`), held small so the lintels over J2/J4
(1.20 mm tall) keep 0.80 mm at the wall face; the pocket edge stays sharp.
`test_printable.py` fails the build on any face steeper than the real
45-degree FDM overhang limit, checked in each part's own print orientation —
a geometric proxy for printability, not a guarantee it slices clean: it can't
see warping, first-layer adhesion, or your printer/filament's actual bridging
distance, only unsupported flats and minimum wall thickness. See that file's
module docstring, and its own comments on `downward_flats()`/`upward_flats()`,
for what it does and doesn't catch (a curved overhang, e.g. a round pinhole
through a wall, is fine and deliberately not flagged; a flat one is not). The
flats it allows are the three port openings' lintels, each a bridge across its
own opening and only a wall deep (14.4 at J2/J4, 9.7 at J1; the gate caps the
span at 25; rev A's DIP slot, 20.7), and the recesses' ceilings in the floor's
underside, which print on support and must be deeper than a 0.42 layer. The
first-print checklist below is the backstop for what the gate can't see.

**The gate assumes the print orientation stated above and nothing checks that
you sliced it that way.** It walks each part's own downward faces in ITS
orientation — tray floor-down, lid plate-down — so reprinting the tray any
other way, or the lid any way but plate-down, invalidates every claim
`test_printable.py` makes: faces it never flagged as overhangs can turn into
real unsupported ceilings once the part is reoriented in the slicer.

## Hardware

| Build | Screws | Into |
|---|---|---|
| Pro | 4 × M2.5 × 18 | the art panel's Würth 9774030151R blind standoffs |
| LC | 4 × M2.5 × 10 | M2.5 brass heat-set inserts in the frame's (or lid's) legs |

LC is **M2.5 × 10, not × 8**: with `STANDOFF` at 3.00 the counterbore seat
sits at Z = -5.50, so an 8mm screw only reaches 2.50mm into the lid's 4.50mm
insert bore. 10mm reaches 4.50mm, the bore's full depth.

Pro is **M2.5 × 18, not × 20**. The panel's standoffs are blind: 3.0 mm tall,
thread in the bottom 1.8 mm, and the screw tip must stop about 0.5 mm short of
the seat. From the same Z = -5.50 head seat, an 18 mm screw ends at Z 12.50,
0.54 short of the panel's underside at 13.04 and 2.46 mm into the standoff. A
20 mm screw would end at 14.50, past the panel's underside, and bottom out.

The screws pass through 3.40 mm bores in the tray (`TRAY_SCREW_CLEAR_D`), loose
enough that the thread doesn't cut the plastic; they thread into the standoffs
or inserts above.

Press the four inserts into the **bottom** of the frame's or lid's legs before
assembly.
Rubber feet (10 × 1.5 mm) go in the four pockets in the floor's underside, each
over a screw counterbore. The 2.5 × 1.25 in label goes in the recess between
them, its long side front to back.

## Acrylic top and frame (LC)

Spec: `docs/superpowers/specs/2026-10-28-lc-acrylic-top-design.md`.

The top is 1.5 mm (1/16 in) clear **cast** acrylic (extruded barely frosts), cut and
engraved from `hardware/panel/airwriter-lc-top.svg`: red hairline is cut,
black fill is engraved. Engrave first, on the top face.

`out/airwriter-lc-frame.stl` is the LC lid's four legs joined by a rim
round the top edge (1.7 mm wide, 2.5 mm deep), with an arm from each leg out to
the side rim, and a light-tight tube round the power LED, closed but for a slit
right under the frosted slash, so the LED lights the slash and nothing else. It stops
a glue line under the acrylic, which it holds 0.10 mm under the tray's rim.
It is cut for 1.5 mm acrylic: for another thickness change `ACRYLIC` in
`params.py` and rebuild. Print it in matte black, **rim down** (its top on the bed,
legs up), no supports.

`out/airwriter-lc-light.stl` is a light-tight tube round D26, the LC's status LED
under the test button, with a 3 mm hole in its cap under the frosted dot: smaller than
the dot, to soften the LED. It reaches over the test button too, a sleeve 0.2 mm round
the switch's body and a collar round its plunger, so the switch holds it on its dots
and it steadies the switch. It stands loose on the board, and the acrylic traps it
0.2 mm over its top. Print it in matte black, **cap down**, no supports.

1. Board into the tray, inserts into the frame's legs.
2. Frame onto the board, four M2.5 × 10 from below. Bridge JP1 (marked LC, beside
   the LED under the test button) with solder if it isn't yet, and set the light tube
   over the test button and that LED, cap up, the plunger through its collar.
3. Scuff the four leg tops with sandpaper and put a small dab of UV-cure glue
   (395–405 nm) on each. Squeeze-out shows through the top at the front legs.
   Not superglue: its fumes haze clear acrylic.
4. Peel the acrylic's film off both faces, drop it into the pocket frosted face
   up, and press it down. The pocket lines it up.
5. Cure through the acrylic, longer than the glue's label says (many clear
   acrylics block some UV), and longer again over the back legs, which sit
   under frosted dots. Try it on an offcut of the same sheet first.

To open it, take the four screws out from below: frame and acrylic come out
together.

## Light-guide lattice (Pro)

`out/airwriter-lattice.stl` sits on the board, under the art panel, and keeps
each lit dot's light in its own cell. It is 13.04 mm tall (the mini-DIN
shells' measured height; the panel rests on both). Spec:
`docs/superpowers/specs/2026-10-14-light-guide-lattice-design.md`.

- A 7 x 8 grid of open cells on the dot pitch, 0.80 mm walls.
- Posts at H1–H4. The Pro screws pass up through them into the panel's
  standoffs, which sit in pockets in the post tops; the panel bears on the
  lattice, not the standoffs.
- Side rails forward to the front posts: the west one over S1, the east one
  over U1's shield (clear of its antenna).
- A walled tube from the power LED D3 up to the panel's FR4 window. (A clear
  PETG light pipe in it came out slightly dimmer than the open tube on the
  2026-10-15 print, so it was dropped; rev B may move to a through-hole dome
  LED.)
- A collar round the test button's plunger in the top 2 mm (Ø3.6 bore on its
  measured 3.1–3.2), tied to the cell walls by ribs, to steady it without
  drag. It sits on where the real plunger stands, 0.2 east and 0.2 forward of
  the KiCad model's (`TEST_PLUNGER_OFFSET`, found over two prints).
- A 1.5 mm lip round the whole top edge, less the two mini-DINs, 0.1 mm short
  of the tray wall, so the lattice sits snug and the gap round the panel
  shows black plastic instead of the inside of the case. A 1.5 mm skin under
  the panel over the front region carries it along the front.

Every part on the board is cleared using KiCad's own STEP export
(`components.py`), so `./build.sh` needs kicad-cli (path in AGENTS.md; set
`KICAD_CLI` to override). The build prints every wall a clearance notch cuts
straight through (a notch that only thins a wall lets no light out). Today
four lit cells have them, two walls each, all into a dark cell or the front
region, which show nothing: r4c3 east and front, r5c4 west and back, r7c4
east and front, r7c6 west and front. A notch into another lit cell or the
test cell fails the build.

**Print** in matte black PLA or PETG, **panel face down** (the flattest face
seals against the panel), no supports.

**First print, check:** the panel seats flush on lattice and mini-DINs
together; the posts drop over the screws; with the LEDs on, no dot lights its
neighbours and the power window glows only from D3.

## Fitting the board

The tray is cut to the board in `hardware/airwriter/`. Rev 2026.11 has no DIP switches, so
its tray has no DIP window and steps 1–2 alone fit it: ports in under their lintels, then
rock the back down. The rest of this section is rev A's (board 1), whose tray is the one at
the `hw-rev-a` tag.

The board does not drop in. The mini-DINs and USB-C stand into closed windows
in the front wall, and S1's keys stand 2.2 mm into the west wall, at right
angles to them. So the ports go in first, and the west wall bows out of the
way of the keys:

1. Hold the board steep, back edge up (~45°), ports toward the front wall.
2. Slide it forward so the mini-DIN tops tuck under their lintels.
3. Rock the back down. As S1's keys reach the west rim, pull the rim outward
   about 2 mm with a thumb over the key band, and let it spring back once the
   keys are down in their window. Near the end the board slides forward
   another millimetre or so as the USB-C finds its opening.

On rev A's tray every wall below the panel seat is 1.70 mm thick, ramping back
to the full wall at the seat, so the west wall bows. Bowing it 2 mm takes
about 46 N in PETG (about 80 N in PLA) and puts about 1.2% vertical strain
where the wall meets the floor (FEA, in git history at `params.THIN_WALL`).
That strain pulls the print's layers apart, so print rev A's tray in PETG. A
0.80 mm west wall split there on the first print. If the keys still catch at
the rim, push the board 0.3 mm east while it goes down. Rev 2026.11's walls
are 2.40, too stiff to bow; one thickness all round either way, since a wall
that changes thickness behind a flat face prints a line up the outside there
(`docs/traps.md`).

## First print, check in this order

Cheapest-to-verify first — cheap checks first so an early failure is caught
before it costs you the ones further down the list.

1. The board in, per "Fitting the board". (Rev A) the west wall should bow
   and spring back without whitening at the DIP window's corners.
2. A real USB-C cable into its opening, the shell's outline with 0.40 round it.
   The receptacle sits 1.5 mm behind the wall, so the plug's overmould has to
   clear the wall's face: this is the one fit the board file could not settle.
3. Both mini-DIN plugs.
4. (Rev A) a pen tip on S1's keys (they sit 0.80 mm recessed by design).
5. The reset hole, drilled with the jig ("Reset hole" below), then a
   paperclip through it onto SW1's button. Its height, 1.90 mm over the board
   (`RESET_ACTUATOR_Z`), comes from LCSC's model of the switch, not from a
   real one.
6. (LC only) The lid, or the acrylic, into the tray's pocket, by hand, before any screws go
   in. It should drop in with a light push and lift back out — expect about
   0.30 mm of clearance per side (`POCKET_CLEAR`), not a press fit and not a
   rattle. Too tight almost always means the pocket printed a little
   undersize (common on the first layers of a part this size); too loose
   past a wobble means something upstream drifted.
7. The full stack screwed together.

## Reset hole

The tray's east wall is solid at SW1: a hole that small, printed sideways
through a wall, comes out as a slit and marks the layers round it. Drill it
instead, with a 1.2 mm (or 3/64 in) bit (`RESET_DRILL_D`), through
`out/airwriter-reset-jig.stl`.

1. Print the jig as its STL lies (on its outer face, the guide bore upright),
   no supports. Run the bit through its bore once by hand: printed holes come
   out a little small.
2. Stand the empty tray on a table, before its feet go on.
3. Set the jig on the same table against the outside of the east wall, its
   fence round the corner on the wall opposite the ports. The table sets the
   height, the fence the place along the wall.
4. Drill through the jig's bore into the wall, holding the jig flat on the
   table and against both walls.

## Interference check

`out/*.step` opens in KiCad's 3D viewer alongside the real board. That check is
why this is build123d and not OpenSCAD.

## Shop model

The shop's product viewer shows both builds inside this case
(`~/Projects/shop/public/models/airwriter-{pro,lc}-cased.glb`), built whole from
this checkout: the board, `out/`'s tray, lattice and LC frame, the LC's acrylic
top and art, and the screws' and feet's places, read from `case.py` and
`params.py`. The scripts live in the shop repo, beside the viewer and the
glTF-transform install they depend on; after `./build.sh` here, run the
commands in "The cased AirWriter" in the shop's `scripts/board-model/README.md`
(`AIRWRITER_DIR` picks a worktree), then `npm run renders` there re-shoots the
stills.

## History

- Design: `docs/superpowers/specs/2026-09-08-airwriter-enclosure-design.md`.
- `docs/enclosure-build-log/` is the build's own record: a brief and a report
  per task, the two fix-wave reports, and `progress.md` (the running ledger,
  every review finding and ruling). The `task-N-report.md` files the
  docstrings and tests here cite are those.
- After that branch: three measured fit fixes (wall/panel flush, DIP slot,
  boss relief), the DIP slot resized to the switch's key recess, the LC
  lid's legs pulled clear of the board, flat port lintels in place of the
  45-degree ridge roofs (shop W-806), the bottom chamfer, the top
  chamfer, and (2026-10-14, after rev A would not go in) 0.30 pocket
  clearance with a thinned, flexing west wall, then (2026-10-17) every wall
  thinned to 1.70 below the rim so no flat face shows a line. Then
  (2026-11-19 to 11-24, for PETG-CF on a 0.6 mm nozzle) the mini-DIN windows
  as rectangles round the sockets, walls 2.40, the reset hole drilled from a
  jig, the recesses printed on support, and 3.40 screw bores. `git log --
  enclosure` has each one with its reasoning.
