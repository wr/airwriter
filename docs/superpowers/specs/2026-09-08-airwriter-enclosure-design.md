# AirWriter enclosure design

2026-09-08 · closes the "Plan 4, lattice + case" line in `docs/HANDOFF.md` for the case
half. Approved by Wells in the brainstorm of 2026-09-08. The light-guide lattice already
exists and is out of scope here.

## What this is, in one paragraph

A 3D-printed enclosure for both AirWriter builds, authored once as parametric build123d
source in `enclosure/` and exported to STEP (for interference checks against the real
board in KiCad) and STL (to print). Two printed parts: a **tray** — floor and four walls —
shared by both builds, and a **Lite lid** that stands in for the dot-grid art panel on
boards built without it. Pro closes with the panel; Lite closes with the printed lid.
Every board-derived number is read out of `hardware/airwriter/airwriter.kicad_pcb` at
build time rather than retyped, so the case cannot drift from the board.

## 1. Parts and what closes them

| Build | Stack |
|---|---|
| **Pro** | tray + main board + printed lattice + dot-grid art panel (the panel is the lid) |
| **Lite** | tray + main board + printed lid on four legs |

The Lite board is the same main board with the 37 SK6812MINI and their 100 nF caps
unpopulated. It has no panel, so the lid reproduces the panel's outline, its 1.6 mm
thickness and its position at the top of the tray's pocket: from the outside the two
builds are identical except the top face.

One tray, not two. The Lite's legs — not the tray — span the cavity, because the Pro's
lattice already occupies the four mounting-hole positions with its own indexing posts.
Putting the spanning pillars on the lid keeps that space free in the shared part.

## 2. The stack, bench up

| Element | mm | Set by |
|---|---|---|
| rubber feet | ~2.0 (off-part) | off-the-shelf |
| floor | 2.40 | 12 layers at 0.2 |
| standoff bosses at H1–H4 | 3.00 | through-hole tails of J2/J4/S1/SW3/U7 — see the amendment below |
| PCB | 1.60 | fab |
| cavity, board top face → lid underside | **14.34** | see below |
| panel or lid | 1.60 | fab / matched by the printed lid |
| **external height** | **22.94** | |

**14.34 is not a free variable.** The mini-DIN shells top out 12.74 mm above the board's
top face; the lattice is squashed to exactly that height; the panel's PEM SMTSO-M25-3ET
blind nuts stand 1.6 mm proud of the panel's underside and land on the lattice. 12.74 +
1.6 = 14.34. The Lite lid's legs reproduce that number in plastic.

**Amendment, 2026-09-08 (during implementation).** The standoff was specified
at 2.00 mm from an estimate of the through-hole tails. Measured off the Kycon
vendor STEP once the model could import it, J2/J4's pins protrude **2.150 mm**
below the board — the floor's top face would have sat 0.15 mm inside them and
the board could not have seated. Raised to **3.00 mm**, which leaves 0.85 mm
under the pin tips for the hand-soldered fillets the STEP does not model. The
external height follows: 21.94 → **22.94 mm**. Nothing else in the stack moves.

External footprint **68.00 × 110.27 mm** — the board's 62.00 × 104.27 outline plus 3.00 mm
of wall each side. The board pocket is cut 0.20 mm oversize inward on each side (62.40 ×
104.67), so the effective wall is 2.80 mm and the *outside* dimensions stay exact.

## 3. Wall thickness: uniform 3.00 mm

Two placement rulings in `docs/HANDOFF.md` each named a wall thickness, and they are not
the same number because they describe different walls:

- **Front (y = 164.27), 3.00 mm.** J2/J4's footprint origin *is* the connector's mating
  face (`MiniDIN-8_Kycon_KMDGX-8S-BS_RightAngle.kicad_mod`: "Origin at connector
  centreline (X) and mating face (Y)"). They sit at y = 167.02, so the faces stand
  **2.75 mm** proud of the board edge and need a 3 mm wall to land 0.25 mm recessed.
- **West (x = 100), 2.20 mm.** S1's piano keys reach x = 97.80, which is 2.20 mm past the
  edge — the wall thickness that puts the key tips flush with the outside.

Wells ruled for **one uniform 3.00 mm wall** over honouring both literally. The
consequence, accepted: S1's key tips sit **0.80 mm recessed** in their slot rather than
flush. That is a config bank set once at assembly, and recessing it makes it harder to
knock. The 2.25 mm west-wall ruling in HANDOFF is superseded by this document.

Anything reading HANDOFF's "all three front connectors stand 2mm proud" as a measured
fact should stop: it is a statement of intent. Measured, the mini-DIN mating faces are
2.75 mm proud and the USB-C shell face about 1.50 mm proud (`F.Fab` on
`USB_C_Receptacle_HRO_TYPE-C-31-M-12`, body ±3.65 about an origin at y = 162.12). They are
not coplanar, and HANDOFF says as much two sentences later.

## 4. Features, wall by wall

Coordinates are KiCad board coordinates unless marked board-local (origin at the outline's
top-left, x − 100, y − 60).

**Front wall, y = 164.27.** Two mini-DIN openings centred at x = 113.0 and x = 132.5 —
19.5 mm centres, bodies spanning x 104.2–121.8 and 123.7–141.3 from the footprint's
17.6 mm courtyard. One USB-C slot at x = 152.0. Mini-DIN faces land 0.25 mm recessed.

The USB-C sits **1.5 mm recessed** behind the same wall, so its opening carries a 45°
chamfer on the outside face, wide enough that a plug's overmould reaches the shell. This
is the one clearance on the part that a first print must be checked against with a real
cable before anything else is trusted.

**West wall, x = 100.** One slot for S1's eight piano keys, spanning the key band and
z 8.11–9.01 above the board's top face, sized so a fingernail or pen tip reaches a key
recessed 0.80 mm.

**East wall, x = 162.** A 1.2 mm reset pinhole on SW1's plate face at x ≈ 159.4. HANDOFF
carries a superseded note putting this on the west wall at x = 100; it is **east**.

**Top.** Not a separate rebate: the 62.40 × 104.67 pocket is one constant cross-section
prism from the floor top up to the wall tops, and the panel or lid — 1.6 mm thick, at the
panel's own 62.00 × 104.27 outline — simply occupies the top 1.6 mm of it, landing flush
with the wall tops and held in place by its own screws rather than resting on a shelf. One
pocket serves both builds.

**Floor.** Four M2.5 clearance holes with counterbores for the screw heads, at board-local
H1 (5.636, 5.636), H2 (56.364, 5.636), H3 (5.636, 91.930), H4 (53.364, 91.930). The
pattern is deliberately asymmetric at H4 — the tray keys off that asymmetry, so the board
can only go in one way. Four 3.00 mm bosses at the same positions carry the board.

**Internal carve-outs**, from the ledger: S1's top corner, U4 (119.3, 116.1), R3
(151.8, 128.8), U9 (146.7, 124.818). SW3's plunger passes up through the lattice and the
panel's 2.25 mm bore at (131, 91); the Lite lid carries the same bore at the same place.

## 5. Fastening

- **Pro:** M2.5 × 20 from below — rubber foot, floor, standoff, board, lattice, into the
  panel's SMTSO blind nuts. Unchanged from the panel design; hole/seat coaxiality between
  the two boards is load-bearing and is not this document's to move.
- **Lite:** M2.5 × 10 from below into an M2.5 heat-set insert pressed into the **bottom** of
  each lid leg, level with the board's top face. Short engagement into brass; the 14.34 mm
  of leg above the insert is a plain spacer that also guides the screw. Threading M2.5
  straight into printed plastic on a lid meant to be opened is what this avoids.

## 6. Source layout and build

```
enclosure/
  board_geom.py     reads hardware/airwriter/airwriter.kicad_pcb -> outline, H1-H4,
                    connector footprint extents, carve-out positions
  case.py           build123d model: tray() and lite_lid()
  params.py         the numbers this document fixes, one place, commented with provenance
  build.sh          -> out/airwriter-tray.{step,stl}, out/airwriter-lite-lid.{step,stl}
  README.md         how to print it, which screws, which inserts
```

`board_geom.py` is the reason this design is safe to build before the boards exist: every
position it needs is already in the board file, and re-running the build after any board
move re-cuts the case rather than silently mismatching it.

Toolchain: build123d (pip, OCCT). Chosen over OpenSCAD because it exports STEP, which is
what makes the interference check in §7 possible at all, and because Python matches the
rest of the hardware pipeline (`place_leds.py`, `route.py`, `emit_board.py`).

Print process the geometry assumes: FDM, 0.4 mm nozzle, 0.2 mm layers, matte black,
tray printed floor-down. No part of either model needs support.

## 7. Verification

- **Interference:** load `out/*.step` into KiCad's 3D viewer with the real board and step
  through every carve-out. This is the check the whole toolchain choice exists to enable.
- **Scripted assertions in `build.sh`**, all failing the build:
  - wall thickness ≥ 2.80 mm everywhere (pocket clearance eats 0.20 of the nominal 3.00)
  - each port cutout contains its footprint's courtyard with ≥ 0.30 mm clearance
  - H1–H4 hole centres match `airwriter.kicad_pcb` to 4 dp
  - no overhang past 45° with the tray floor-down
  - tray and lid are each a single watertight solid
- **First print, in this order:** a real USB-C cable into the chamfered opening; both
  mini-DIN plugs; a pen tip on S1; a paperclip on the reset pinhole; then the full stack
  screwed together.

## 8. Shop model

The tray and Lite lid STLs convert to GLB through the shop's existing
`scripts/addon-models/stl2glb.mjs`. Whether the case then becomes a purchasable add-on on
the AirWriter product page is a separate product decision and is not part of this design.

## 9. Explicitly out of scope

- The light-guide lattice — it exists and is unchanged.
- The art panel — built, ordered, and its SMTSO seat pattern is not moved by anything here.
- Any board change. If the case turns out to need one, that is a new decision, not a fix.

## Open items

1. Heat-set insert MPN for the Lite lid legs (M2.5 × 4 mm brass, short-flange; picked at
   implementation from what is actually stocked).
2. Rubber foot MPN and whether the floor gets a recess for it — decided against a real
   foot in hand, not from a datasheet.
3. Exact USB-C chamfer angle and depth, tuned on the first print against a real cable.
