# SPDX-License-Identifier: CERN-OHL-S-2.0
#
# J1's 3D model, drawn from dimensions: an HRO TYPE-C-31-M-12 USB-C receptacle
# (16-pin, top-mount, four through-hole shield legs). Replaces a model of
# unknown licence. The outline is the USB Type-C receptacle shell; the leg,
# peg and pad positions match the KiCad footprint's frame, so the case and the
# light-guide lattice, which hull this solid, come out unchanged.
#
# Frame (as KiCad places it with rotate -90 0 0): X across the opening, Y up
# from the board (0 = board top), Z into the board from the mating face (0).
#
#   enclosure/.venv/bin/python hardware/airwriter/lib/make_usbc_model.py
from pathlib import Path

from build123d import (Axis, Box, BuildPart, BuildSketch, Cylinder, Location,
                       Locations, Mode, Plane, RectangleRounded, add, export_step,
                       extrude, fillet)

W, H, R = 8.94, 3.21, 1.255          # shell outline and corner radius
WALL = 0.295                         # shell thickness at the sides
IN_W, IN_H, IN_R = 8.35, 2.56, 1.03  # opening
DEPTH = 7.30                         # mating face to the shell's back
CAVITY = 6.50                        # how deep a plug goes
TONGUE = (6.69, 0.70, 0.25, 1.605)   # width, thickness, radius, centre height
TONGUE_Z = (0.80, 6.50)
LEGS_Z = ((2.195, 2.995), (6.25, 7.30))
LEG_T, LEG_BOTTOM, LEG_TIP_R = 0.30, -1.00, 0.20   # tips rounded front to back
PEGS_X = (1.58, 7.36)                # locating pegs, Ø0.5
PEG_Z, PEG_BOTTOM = 6.25, -0.60
PADS_X = ((1.02, 1.42), (1.82, 2.22), (2.62, 2.82), (3.12, 3.32), (3.62, 3.82),
          (4.12, 4.32), (4.62, 4.82), (5.12, 5.32), (5.62, 5.82), (6.12, 6.32),
          (6.72, 7.12), (7.52, 7.92))
PAD_Z, PAD_T = (7.30, 7.90), 0.15


def _legs():
    """Shield legs, both sides, from below the board up into the shell wall,
    their tips rounded front to back. Built outside the BuildPart context so
    each can be filleted before it joins the shell."""
    legs = []
    for x0 in (0.0, W - LEG_T):
        for z0, z1 in LEGS_Z:
            leg = Box(LEG_T, WALL * 2 - LEG_BOTTOM, z1 - z0).moved(
                Location((x0 + LEG_T / 2, (LEG_BOTTOM + WALL * 2) / 2, -(z0 + z1) / 2)))
            tip = leg.edges().filter_by(Axis.X).group_by(Axis.Y)[0]
            legs.append(fillet(tip, LEG_TIP_R))
    return legs


def build():
    legs = _legs()
    with BuildPart() as p:
        # Shell: rounded-rectangle tube, open at the mating face, closed at the back.
        with BuildSketch(Plane.XY.offset(0)):
            with Locations((W / 2, H / 2)):
                RectangleRounded(W, H, R)
        extrude(amount=-DEPTH)
        with BuildSketch(Plane.XY.offset(0)):
            with Locations((W / 2, H / 2)):
                RectangleRounded(IN_W, IN_H, IN_R)
        extrude(amount=-CAVITY, mode=Mode.SUBTRACT)
        # Tongue, inside the opening.
        tw, tt, tr, ty = TONGUE
        with BuildSketch(Plane.XY.offset(-TONGUE_Z[0])):
            with Locations((W / 2, ty)):
                RectangleRounded(tw, tt, tr)
        extrude(amount=-(TONGUE_Z[1] - TONGUE_Z[0]))
        for leg in legs:
            add(leg)
        # Locating pegs.
        for x in PEGS_X:
            with Locations(Location((x, PEG_BOTTOM / 2, -PEG_Z), (90, 0, 0))):
                Cylinder(0.25, -PEG_BOTTOM)
        # SMD pads behind the shell.
        for x0, x1 in PADS_X:
            with Locations(((x0 + x1) / 2, PAD_T / 2, -(PAD_Z[0] + PAD_Z[1]) / 2)):
                Box(x1 - x0, PAD_T, PAD_Z[1] - PAD_Z[0])
    return p.part


if __name__ == "__main__":
    out = Path(__file__).parent / "airwriter.3dshapes" / "USB_C_Receptacle_HRO_TYPE-C-31-M-12.step"
    part = build()
    part.label = "USB_C_Receptacle_HRO_TYPE-C-31-M-12"
    export_step(part, out)
    bb = part.bounding_box()
    print(out, [round(v, 3) for v in (bb.min.X, bb.min.Y, bb.min.Z, bb.max.X, bb.max.Y, bb.max.Z)],
          round(part.volume, 3))
