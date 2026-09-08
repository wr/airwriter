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
(68.00 x 110.27 x 22.94 mm), lid plate-down (62.40 x 104.67 x 15.94 mm).
`test_printable.py` fails the build on any face steeper than the real
45-degree FDM overhang limit, checked in each part's own print orientation —
a geometric proxy for printability, not a guarantee it slices clean: it can't
see warping, first-layer adhesion, or your printer/filament's actual bridging
distance, only unsupported flats and minimum wall thickness. See that file's
module docstring, and its own comments on `downward_flats()`/`upward_flats()`,
for what it does and doesn't catch (a curved overhang, e.g. a round pinhole
through a wall, is fine and deliberately not flagged; a flat one is not). The
first-print checklist below is the backstop for what the gate can't see.

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
5. The full stack screwed together.

## Interference check

`out/*.step` opens in KiCad's 3D viewer alongside the real board. That check is
why this is build123d and not OpenSCAD.
