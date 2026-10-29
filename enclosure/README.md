# Enclosure (enclosure/out/*.stl)

Four printed parts. The **tray** is shared by both builds. The **LC frame**
carries the LC's clear acrylic top, which stands in for the dot-grid art panel
on boards built without it; the **LC lid** is the all-printed alternative to
the two. The **lattice** is the Pro build's light guide, between the board and
the panel.

    ./build.sh          # runs the tests, then writes out/*.{step,stl}

`out/airwriter-tray.stl`, `out/airwriter-lc-frame.stl`,
`out/airwriter-lc-lid.stl` and `out/airwriter-lattice.stl` are committed, so the case can be printed without a
build123d install; re-run `./build.sh` and commit
them again after any change here or to the board. The STEPs are not (6 MB, and
the same command rebuilds them).

Design: `docs/superpowers/specs/2026-09-08-airwriter-enclosure-design.md`.
Every board-derived number is read out of `hardware/airwriter/airwriter.kicad_pcb`
at build time by `board_geom.py` — nothing here is retyped, so a footprint move
re-cuts the case on the next run. `params.py` holds what this project chooses,
each constant commented with where it came from.

## Print

FDM, 0.4 mm nozzle, 0.2 mm layers, matte black, **PETG** for the tray (see
"Fitting the board"). **No supports.** Tray floor-down
(68.00 x 110.27 x 21.64 mm), lid plate-down (62.00 x 104.27 x 14.64 mm — the
art panel's own outline, not the pocket's; see "First print" below for the
0.30 mm/side it should clear by).
Slice the tray with Arachne walls and 4 wall loops: below the rim every wall
is 1.70 mm, which Arachne fills with exactly four lines, where Classic adds a
hair-thin filler line down the middle. In translucent PETG also give it 12
bottom layers (the floor's full 2.4 mm), or the floor's infill grid shows
through.
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
one flat it allows is a lintel: the DIP slot and the three port openings keep
flat tops, each a bridge across its own opening and only a wall deep (20.7 mm
at the DIP slot, 14.0 at J2/J4, 6.5 at J1; the gate caps the span at 25). Print
them with part cooling on. The first-print checklist below is the backstop for
what the gate can't see.

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
sits at Z = -5.70, so an 8mm screw only reaches 2.30mm into the lid's 4.50mm
insert bore. 10mm reaches 4.30mm — most of the insert's own length.

Pro is **M2.5 × 18, not × 20**. The panel's standoffs are blind: 3.0 mm tall,
thread in the bottom 1.8 mm, and the screw tip must stop about 0.5 mm short of
the seat. From the same Z = -5.70 head seat, an 18 mm screw ends at Z 12.30,
0.74 short of the panel's underside at 13.04 and 2.26 mm into the standoff. A
20 mm screw would end at 14.30, past the panel's underside, and bottom out.

Press the four inserts into the **bottom** of the frame's or lid's legs before
assembly.
Rubber feet go on the floor's underside, over the screw counterbores.

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

1. Board into the tray, inserts into the frame's legs.
2. Frame onto the board, four M2.5 × 10 from below.
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
there are four, all from a lit cell into a gold cell or the front region,
which show nothing: S1 opens r7c1 west and front, R3 opens r7c6 west, and
C9/C5 open r7c6 front. A notch into another lit cell or the test cell fails
the build.

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

Below the panel seat every wall is `THIN_WALL` (1.70 mm) thick, ramping back
to the full wall at the seat, so the rim, the seat and the outside look the
same as before. One thickness all round: a wall that changes thickness behind
a flat face prints a line up the outside there (`docs/traps.md`). Bowing the
west wall 2 mm takes about 46 N in PETG (about 80 N in PLA) and puts about
1.2% vertical strain where the wall meets the floor (FEA, `params.THIN_WALL`).
That strain pulls the print's layers apart, so **print the tray in PETG**. A
0.80 mm west wall split there on the first print. If the keys still catch at the rim, push the board
0.3 mm east while it goes down.

## First print, check in this order

Cheapest-to-verify first — cheap checks first so an early failure is caught
before it costs you the ones further down the list.

1. The board in, per "Fitting the board". The west wall should bow and spring
   back without whitening at the DIP window's corners.
2. A real USB-C cable into the chamfered opening. The receptacle sits 1.5 mm
   behind the wall; this is the one clearance the board file could not settle
   — `USBC_CHAMFER` was tuned by eye, not measured.
3. Both mini-DIN plugs.
4. A pen tip on S1's keys (they sit 0.80 mm recessed by design).
5. A paperclip on the east-wall reset pinhole. Its Z (1.75mm, half of SW1's
   footprint-descr body height) is a **derivation, not a measurement** — SW1
   has no 3D model on the board, so nothing here booleaned it off real
   geometry, and no Panasonic mechanical drawing is in this repo. The
   apparent cross-check against `EVQP44B3M_SidePress.wrl` doesn't add
   independent confidence: that file was hand-authored with the actuator's Z
   set equal to the body's centre Z — the same assumption `RESET_ACTUATOR_Z`
   makes — so the two were always going to agree (see `params.py` and
   task-6-report.md for the full derivation).
6. (LC only) The lid, or the acrylic, into the tray's pocket, by hand, before any screws go
   in. It should drop in with a light push and lift back out — expect about
   0.30 mm of clearance per side (`POCKET_CLEAR`), not a press fit and not a
   rattle. Too tight almost always means the pocket printed a little
   undersize (common on the first layers of a part this size); too loose
   past a wobble means something upstream drifted.
7. The full stack screwed together.

## Interference check

`out/*.step` opens in KiCad's 3D viewer alongside the real board. That check is
why this is build123d and not OpenSCAD.

## Shop model

The shop's product viewer shows both builds inside this case
(`~/Projects/shop/public/models/airwriter-{pro,lc}-cased.glb`). The scripts
that put it there live in the shop repo, beside the viewer and the
glTF-transform install they depend on; after `./build.sh` here:

    cd ~/Projects/shop/scripts/board-model/tools
    E=~/Projects/airwriter/enclosure/out
    H="105.636,65.636;156.364,65.636;105.636,151.93;153.364,151.93"   # H1-H4, KiCad mm
    node case-shell.mjs ../../../public/models/airwriter-pro-cased.glb  ../../../public/models/airwriter-pro-cased.glb  --tray $E/airwriter-tray.stl
    node case-shell.mjs ../../../public/models/airwriter-lc-cased.glb ../../../public/models/airwriter-lc-cased.glb --tray $E/airwriter-tray.stl --lid $E/airwriter-lc-lid.stl
    node case-hardware.mjs ../../../public/models/airwriter-pro-cased.glb  ../../../public/models/airwriter-pro-cased.glb  --holes "$H" --floor -5.4 --feet black
    node case-hardware.mjs ../../../public/models/airwriter-lc-cased.glb ../../../public/models/airwriter-lc-cased.glb --holes "$H" --floor -5.4 --feet clear --screws 10 --clear

`case-shell.mjs` swaps the `case` (and `lid`) mesh for these STLs in the KiCad
GLB frame (a -90 degree turn about X, then +100 / +1.6 / +164.27 mm) and keeps
each node's material; `case-hardware.mjs` re-seats the rubber feet, the LC's
screws and the clear materials. `--floor` is glTF Y of the tray's underside:
`Z_FLOOR_BOTTOM` (-7.00) + the 1.60 frame offset. The full write-up is "The
cased AirWriter" in the shop's `scripts/board-model/README.md`; then
`npm run renders` there re-shoots the home-page stills.

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
  thinned to 1.70 below the rim so no flat face shows a line. `git log --
  enclosure` has each one with its reasoning.
