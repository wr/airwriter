# Flex wall: why rev A would not go into the tray (2026-10-14)

Wells: the board won't fit the bottom case, "maybe slightly a width/height
issue, but probably moreso a maneuvering it into place issue". Both were true.

## Diagnosis

`flex-wall/sweep.py` moves the board (slab, J1/J2/J4 STEP hulls, S1's keys and
body) through the tray and reports overlaps.

- **The DIP keys made it impossible.** S1's keys stand 2.20 mm past the
  board's west edge, into a closed slot in the west wall, so the board has to
  finish with a 2.2 mm slide west. J2/J4 (2.75 mm) and J1 (1.5 mm) stand into
  closed windows in the front wall, which hold the board to ±0.40 mm side to
  side, and the pocket gave 0.20 mm. On the old tray the keys hit the wall
  above their slot at every tilt past 0°, whatever the pivot. No path in.
- **The ports alone are fine.** With the keys' path cleared, a rock-in works:
  board ~45° back-up, slide forward so the mini-DIN tops tuck under their
  lintels, rock down about a pivot near the lintels (A = y 0, z 12), then the
  last ~10° about a pivot just behind the USB-C (B = y 1, z 4.5) with a ~1 mm
  slide forward between. The lintels can stay.
- **The fit was tight anyway.** PCBWay routes the outline to ±0.2 mm, and an
  FDM pocket prints a little undersize. 0.20 mm per side covered neither.

## Fix

- `POCKET_CLEAR` 0.20 → 0.30 (the panel/lid gap grows by 0.10).
- West wall thinned to `WEST_FLEX_T` = 0.80 below the panel seat, with a
  45-degree ramp back to the full wall at `Z_LID_UNDER`. This was Wells's idea
  ("the side walls are more rigid than they need to be"), used so the wall
  bows ~2 mm outward while the keys pass the rim. The seat, the rim and the
  outside are unchanged. The thinned stretch tapers back to the full wall over
  `FLEX_TAPER` (8 mm) at each end, starts `FLEX_END` (0.5 mm) short of each
  corner arc, and has an `FLEX_COVE_R` (1 mm) cove at the floor. The DIP
  window's corners are rounded (`DIP_SLOT_R` = 1 mm).

Options turned down: an open-top notch over the keys (Wells had already
rejected the black bar a 1 mm gap there showed); a hidden channel inside the
west wall (it leaves a slot in the top rim beside the panel and a ~0.5 mm skin
over the keys); a notch plus a separate drop-in bezel.

## FEA

`flex-wall/fea.py`: linear elastic, P2 tets (gmsh + scikit-fem), floor
underside fixed. A uniform pull on the rim's inner face over y 19–42, scaled
until the whole key path at the rim (y 20–40.5) has moved out 2.0 mm, which
clears the keys' tips (x = −2.2) by 0.1 mm with the board centred. Strain does
not depend on E for this load; force scales with E (PETG 2 GPa shown; PLA
~3.5 GPa is ×1.75).

| Variant | Force (PETG) | Peak strain, where |
|---|---|---|
| t 1.20, square ends, square window | 32 N | 2.5% window corners; 2.3% J2 lintel |
| t 1.20, window r1 | 32 N | 2.3% J2 lintel; 2.1% thin-stretch front end |
| t 0.80, window r1 | 21 N | 2.2% J2 lintel; 2.1% thin-stretch front end |
| t 0.80, r1, taper 8, end 0.5 (chosen) | 22 N | 2.25% J2 lintel; 1.66% window |
| same, mesh 0.8 mm (converged) | 21 N | **1.65%** window corner; 1.61% J2 lintel; 1.43% taper |

The 1.2 mm mesh overstated the J2 lintel corner; at 0.8 mm everything is
≤1.65%. That is comfortable for PETG (yield ~4%) and fine for PLA (~2.5–3%)
over a handful of assemblies. About 21 N (PETG) or 37 N (PLA) is a firm pull
with a thumb.

The rigid-wall sweep leaves ≤0.7 mm³ of the front key overlapping the ramp
at y 19.3–20 (z 10.8–12.4), just in front of the pulled patch. The FEA wall is
already 1.5–1.8 mm out there, against the 0.2–1.5 mm the key needs. At the rim
itself, at y 18.5–19.5, it comes up to 0.17 mm short on steeper paths. Pushing
the board east into its 0.30 mm of pocket clearance covers that.

## Not verified

Nothing here is a physical print. The material is not pinned down (the README
now says PETG for the tray). The 2.0 mm requirement assumes the board is
centred; the FEA fixes the floor rigidly, which is slightly stiffer than a
hand holding the case.
