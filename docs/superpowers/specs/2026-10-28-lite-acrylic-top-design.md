# Lite acrylic top and frame design

2026-10-28. Wells chose a clean top over screws through the acrylic, then UV-cure glue
over tape, the rev 2026.11 board (this branch has it merged) over rev A, and on 2026-10-29
1 mm acrylic, the sheet his library had (it has 1 and 3 mm). The shop sells AirWriter Lite as "the same black bottom case under a clear
acrylic top with the AirWriter mark lasered into it".

## The stack

| Part | Where | Set by |
|---|---|---|
| Tray, board, M2.5 × 10 from below | unchanged | the Lite lid's hardware |
| Lite frame (printed) | board face to `ACRYLIC_UNDERSIDE - GLUE_LINE` | this design |
| UV-cure glue, about 0.10 | on the four leg tops | `GLUE_LINE` |
| Acrylic top, 1.00 mm clear cast | `ACRYLIC_UNDERSIDE` to 14.54 | `ACRYLIC` |

The acrylic's face is at 14.54, `ACRYLIC_FACE_UNDER` (0.10) under the rim, whatever its
thickness: the frame's height follows `ACRYLIC`. A 1 mm sheet's underside is at 13.54,
0.50 over J2 and J4's shells (13.04), so it rests on the frame alone. Only a 1.50 sheet
would also land on the shells, as the Pro's panel does; 3 mm would have to sink 1.50
past them, so it stands proud. A frame that prints up to 0.10 tall leaves the face no
prouder than the rim; one that prints short leaves the glue more gap to fill. The first
version left 0.50 for VHB tape; glue in a gap that deep could let the back of the sheet
settle before it cured.

## The acrylic

`hardware/panel/airwriter-lite-top.svg`, from `lite_top.py`: the panel's own outline cut
(62.00 × 104.27, R2.0), the test hole cut at the panel's 3.8 mm, and the Lite's own art,
`hardware/panel/svg/lite.svg` (Wells's Figma Frame 17), frosted: "AirWriter LC", "airprint
to serial bridge", rev 2026.11, the 18 gold dots and the test dot, the serial and power
labels and the three rules, the power slash. The art draws the Pro's 37 lit dots in the
background's colour, and they stay clear: the Lite has no LEDs. The frosted slash sits
over D3, so the power light shows in it.

On rev 2026.11 (15 mm SW3) the test plunger stands 0.46 above the acrylic; on rev A
(16 mm) 1.46.

## The frame

One printed part, in `enclosure/lite_frame.py`:

- **Legs:** the Lite lid's four, factored out as `case.lite_legs()`: Ø6 at H1–H4, an
  M2.5 heat-set insert in each foot, trimmed round J1, J2 and J4's hulls, feet relieved
  for C22. They stop `GLUE_LINE` under the acrylic, and their tops are the glue pads.
- **Rim:** `FRAME_RIM_H` (2.50) deep at the top, from `RIDGE_CLEAR` short of the pocket wall
  to `FRAME_RIM_IN` (1.50) inside the board edge, like the lattice's lip. It makes the
  four legs one part, so the acrylic goes on square, and it shows as a thin black line
  just inside the tray's rim. J2 and J4's hulls cut it at the front. The short piece
  between them would be loose, so it's left off.
- **Arms:** one `FRAME_ARM_W` (2.40) wide from each leg out to the side rim, at rim depth.
- **Light tube:** under a clear top D3 would light the whole corner, so the frame carries
  a tube round the Lite's slash (`hardware/panel/lite_slash.polys.json`, written by
  `lite_top.py` from the art) and D3's envelope: on rev 2026.11 a 3 mm LED on a 4 mm
  spacer standing to 11.85, with R17 nearby outside. Walls run from the board to the
  top, `LATTICE_WALL` thick where there is room; on the east the spacer stands 0.38 from
  the board edge, so that wall runs out to the rim's outer line and is one printed line
  (`FRAME_TUBE_MIN_WALL`, 0.45), as the lattice's was let be on that board. Its inside
  corner follows the rim's rounded one, so the corner keeps that thickness too. A
  `FRAME_CAP` (0.60) cap closes it but for a slit the slash's own shape, right under the
  frost, 0.99 over the LED. D3's grown shadow is left out of the frame's part voids: the
  tube clears D3 itself, and the shadow would cut the east wall away.
- **Clearances:** the rim, arms and tube clear every other part's STEP envelope grown by
  `PART_CLEAR`/`PART_TOP_CLEAR` (`components.py`, as the lattice does).

Printed rim-down (the glue face on the bed, the flattest face), legs up, no supports: the
same orientation as the Lite lid, and the same printability gate.

## Assembly

1. Board into the tray. Heat-set inserts into the frame's legs.
2. Frame onto the board, four M2.5 × 10 from below.
3. Scuff the four leg tops with sandpaper and put a small dab of UV-cure glue (395–405 nm)
   on each. Squeeze-out shows through the clear top at the front legs.
4. Peel the acrylic's film off both faces, drop it into the pocket frosted face up, and
   press it down onto the frame. The pocket lines it up.
5. Cure through the acrylic, longer than the glue's label says: many clear acrylics block
   some UV. The back legs sit under frosted dots, which scatter the light; give them
   longer still. Try the glue and light on an offcut of the same sheet first.

To open, unscrew from below: the frame and acrylic come out together.

## Calls made here

- Glue on the four leg tops only. The acrylic is captive in the pocket and the frame is
  screwed down, so the glue only has to stop the acrylic lifting off the frame.
  UV-cure over two-part epoxy: clear, and it sets only when lit, so the acrylic is lined
  up in the pocket first. Not superglue, whose fumes haze clear acrylic, nor acrylic
  cement, which doesn't take to the printed frame.
- The legs' tops at H1 and H2 sit under the frosted corner dots, so two of the four glue
  pads don't show. H3 and H4's do, as Ø6 discs below the subtitle.
- The printed Lite lid stays, for anyone without a laser. Its plunger bore goes from
  2.25 to 3.80, the panel's hole: 2.25 dated from the old switch and stopped the
  measured 3.1–3.2 plunger.

## What only a print settles

- Whether the glue cures through this acrylic, and whether the acrylic then sits flush.
- How the black rim and the two front leg pads read through the acrylic.
- How brightly the slash glows through the slit, and whether the frost scatters light
  along the sheet into the other frosted marks.
