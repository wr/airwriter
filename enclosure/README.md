# Enclosure (enclosure/out/*.stl)

Two printed parts. The **tray** is shared by both builds. The **Lite lid**
replaces the dot-grid art panel on boards built without it.

    ./build.sh          # runs the tests, then writes out/*.{step,stl}

Design: `docs/superpowers/specs/2026-09-08-airwriter-enclosure-design.md`.
Every board-derived number is read out of `hardware/airwriter/airwriter.kicad_pcb`
at build time by `board_geom.py` — nothing here is retyped, so a footprint move
re-cuts the case on the next run. `params.py` holds what this project chooses,
each constant commented with where it came from.

## Print

FDM, 0.4 mm nozzle, 0.2 mm layers, matte black. **No supports.** Tray floor-down
(68.00 x 110.27 x 22.94 mm), lid plate-down (62.00 x 104.27 x 15.94 mm — the
art panel's own outline, not the pocket's; see "First print" below for the
0.20 mm/side it should clear by).
`test_printable.py` fails the build on any face steeper than the real
45-degree FDM overhang limit, checked in each part's own print orientation —
a geometric proxy for printability, not a guarantee it slices clean: it can't
see warping, first-layer adhesion, or your printer/filament's actual bridging
distance, only unsupported flats and minimum wall thickness. See that file's
module docstring, and its own comments on `downward_flats()`/`upward_flats()`,
for what it does and doesn't catch (a curved overhang, e.g. a round pinhole
through a wall, is fine and deliberately not flagged; a flat one is not). The
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
| Pro | 4 × M2.5 × 20 | the art panel's PEM SMTSO-M25-3ET blind nuts |
| Lite | 4 × M2.5 × 10 | M2.5 brass heat-set inserts in the lid legs |

Lite is **M2.5 × 10, not × 8**: with `STANDOFF` at 3.00 the counterbore seat
sits at Z = -5.60, so an 8mm screw only reaches 2.40mm into the lid's 4.50mm
insert bore. 10mm reaches 4.40mm — most of the insert's own length. The Pro's
M2.5 × 20 is unchanged (it runs the full 14.34mm cavity plus panel).

Press the four inserts into the **bottom** of the lid's legs before assembly.
Rubber feet go on the floor's underside, over the screw counterbores.

## First print, check in this order

Cheapest-to-verify first — cheap checks first so an early failure is caught
before it costs you the ones further down the list.

1. A real USB-C cable into the chamfered opening. The receptacle sits 1.5 mm
   behind the wall; this is the one clearance the board file could not settle
   — `USBC_CHAMFER` was tuned by eye, not measured.
2. Both mini-DIN plugs.
3. A pen tip on S1's keys (they sit 0.80 mm recessed by design).
4. A paperclip on the east-wall reset pinhole. Its Z (1.75mm, half of SW1's
   footprint-descr body height) is a **derivation, not a measurement** — SW1
   has no 3D model on the board, so nothing here booleaned it off real
   geometry, and no Panasonic mechanical drawing is in this repo. The
   apparent cross-check against `EVQP44B3M_SidePress.wrl` doesn't add
   independent confidence: that file was hand-authored with the actuator's Z
   set equal to the body's centre Z — the same assumption `RESET_ACTUATOR_Z`
   makes — so the two were always going to agree (see `params.py` and
   task-6-report.md for the full derivation).
5. (Lite only) The lid into the tray's pocket, by hand, before any screws go
   in. It should drop in with a light push and lift back out — expect about
   0.20 mm of clearance per side (`POCKET_CLEAR`), not a press fit and not a
   rattle. Too tight almost always means the pocket printed a little
   undersize (common on the first layers of a part this size); too loose
   past a wobble means something upstream drifted.
6. The full stack screwed together.

## Interference check

`out/*.step` opens in KiCad's 3D viewer alongside the real board. That check is
why this is build123d and not OpenSCAD.

## Shop model

    cd ~/Projects/shop && node scripts/addon-models/stl2glb.mjs \
      ~/Projects/airwriter/enclosure/out/airwriter-tray.stl \
      public/models/airwriter-case.glb \
      --name case --color 0.025,0.025,0.025 --rough 0.8 --metal 0 --lay-flat

Converts the tray (not the lid) to a GLB for the shop's product viewer, at
`shop/public/models/airwriter-case.glb`. `--color`/`--rough`/`--metal` match
the shop's existing convention for a matte black FDM part
(`scripts/addon-models/build.sh`'s bezel: 0.025 linear grey reads as black
plastic once the viewer's room light hits it — a true 0/0/0 goes visually
flat). No `--flip`: the tray is modelled with the board's own +Z toward the
open top and prints floor-down at model -Z (see `test_printable.py`'s module
docstring), so `--lay-flat` alone (printer/model Z → glTF Y) already puts the
open top — the ports, the pocket — facing up, and the closed floor slab
resting on the ground plane. Checked two ways: (1) traced stl2glb.mjs's
`orient()` — with `--lay-flat` and no `--flip`, old +Z maps to new +Y, and
`export.py` writes the STL straight from `case.tray()` with no
reorientation, so model +Z (open top) really is glTF +Y (up); (2) probed
`case.tray()` directly — a thin Z-slice 0.3mm off the top is ~13% filled (an
open rim), one 0.3mm off the bottom is ~99% filled (the closed floor slab),
confirming which end is which before it ever reaches the converter.
Following `scripts/addon-models/build.sh`'s own optimize step
(`join-prims.mjs` then gltf-transform `dedup`/`prune`/`weld`/`meshopt`)
after conversion took it from 1.28MB to 188KB.

Nothing on the site references it yet — making the case a purchasable add-on is
a product decision, not part of this design.
