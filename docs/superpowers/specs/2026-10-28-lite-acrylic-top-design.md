# Lite acrylic top and frame design

2026-10-28. Wells chose a clean top held with tape over screws through the acrylic. The
shop sells AirWriter Lite as "the same black bottom case under a clear acrylic top with
the AirWriter mark lasered into it".

## The stack

| Part | Where | Set by |
|---|---|---|
| Tray, board, M2.5 × 10 from below | unchanged | the Lite lid's hardware |
| Lite frame (printed) | board face to `CAVITY - TAPE` | this design |
| Tape, 3M VHB 4905 (clear, 0.50 mm) | on the four leg tops | `TAPE` |
| Acrylic top, 1.50 mm clear cast | `CAVITY` to `CAVITY + ACRYLIC` | `ACRYLIC` |

The acrylic's underside lands at `CAVITY` (13.04), on J2 and J4's shells and the taped leg
tops, as the Pro's panel does. Its face is at 14.54, 0.10 under the rim. Thicker tape
lifts it: 1.0 mm tape puts it 0.40 proud.

## The acrylic

`hardware/panel/airwriter-lite-top.svg`, from `lite_top.py`: the panel's own outline cut
(62.00 × 104.27, R2.0), the test hole cut at the panel's 3.8 mm, and the panel's art
frosted: the 18 gold dots, the test dot, the wordmark and subtitle, the port labels and
rules, the power slash. The other 37 dots are left clear: they are the Pro's lit dots and
the Lite has no LEDs. The frosted slash sits over D3, so the power light shows in it.

On rev 2026.11 (15 mm SW3) the test plunger stands 0.46 above the acrylic; on rev A
(16 mm) 1.46.

## The frame

One printed part, in `enclosure/lite_frame.py`:

- **Legs:** the Lite lid's four, factored out as `case.lite_legs()`: Ø6 at H1–H4, an
  M2.5 heat-set insert in each foot, trimmed round J1, J2 and J4's hulls, feet relieved
  for C22. They stop `TAPE` under `CAVITY`, and their tops are the tape pads.
- **Rim:** `FRAME_RIM_H` (2.50) deep at the top, from `RIDGE_CLEAR` short of the pocket wall
  to `FRAME_RIM_IN` (1.50) inside the board edge, like the lattice's lip. It makes the
  four legs one part, so the acrylic goes on square, and it shows as a thin black line
  just inside the tray's rim. J2 and J4's hulls cut it at the front. The short piece
  between them would be loose, so it's left off.
- **Arms:** one `FRAME_ARM_W` (2.40) wide from each leg out to the side rim, at rim depth.
- **Clearances:** the rim and arms clear every part's STEP envelope grown by
  `PART_CLEAR`/`PART_TOP_CLEAR` (`components.py`, as the lattice does), and stay out of
  the power slash's window, grown by `D3_TUBE_CLEAR`, so D3's light reaches the slash.
  On rev 2026.11 the dome LED's envelope cuts the rim where it needs to.

Printed rim-down (the tape face on the bed, the flattest face), legs up, no supports: the
same orientation as the Lite lid, and the same printability gate.

## Assembly

1. Board into the tray. Heat-set inserts into the frame's legs.
2. Frame onto the board, four M2.5 × 10 from below.
3. A 6 mm square of VHB 4905 on each leg top.
4. Peel the acrylic's film off both faces, drop it into the pocket frosted face up, and
   press over each leg. The pocket lines it up.

To open, unscrew from below: the frame and acrylic come out together.

## Calls made here

- Tape on the four leg tops only. The acrylic is captive in the pocket and the frame is
  screwed down, so the tape only has to stop the acrylic lifting off the frame; four
  6 mm VHB squares hold far more than its 12 g. The rim can take tape too.
- The legs' tops at H1 and H2 sit under the frosted corner dots, so two of the four tape
  pads don't show. H3 and H4's do, as Ø6 discs below the subtitle.
- The printed Lite lid stays, for anyone without a laser. Its plunger bore goes from
  2.25 to 3.80, the panel's hole: 2.25 dated from the old switch and stopped the
  measured 3.1–3.2 plunger.

## What only a print settles

- The tape's real thickness once pressed, and whether the acrylic then sits flush.
- How the black rim and the two front leg pads read through the acrylic.
- Whether the frosted slash lights well enough from D3 with no tube.
