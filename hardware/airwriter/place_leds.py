#!/usr/bin/env python3
"""Place the dot-panel LED field: D10-D46 (37x SK6812MINI-E, top-firing) and
their per-LED 100nF decoupling caps C30-C66, on hardware/airwriter/airwriter.kicad_pcb.

Run with the pcbnew-bundled interpreter:
    /Applications/KiCad/KiCad.app/Contents/Frameworks/Python.framework/Versions/3.9/bin/python3 \
        hardware/airwriter/place_leds.py

Reads hardware/panel/panel_layout.json for the dot grid (37 of 56 dots carry
an LED) and the schematic netlist (kicad-cli sch export netlist) for the
per-LED chain nets, so the physical walk order can be asserted against the
actual wiring rather than assumed.

Coordinate mapping, CORRECTED round 5 (the round-2/3/4 mirror was a controller
error -- the panel sits face-up ABOVE the board and both are viewed from the
same side, so nothing flips):
    board position = (board_origin_x + dot.x, board_origin_y + dot.y)
board_origin is the board outline's top-left corner, (100, 60).

Chain order: serpentine by rows, starting at the row nearest U9 (row 7 --
closest to the lower band where U9 lives) and walking outward to row 0.
Within a row the walk alternates direction so consecutive LEDs are always
adjacent, minimizing total chain trace length. Under the corrected (unmirrored)
mapping, the physical corner nearest U9's east-side position is now panel
column 6 (high x_p), not column 0 -- so the row-7 leg walks column DESCENDING
first (round 5 fix; round 2-4 walked ascending, which was correct only under
the old, wrong mirror math). D10 is chain position 0 (net /LED_DATA5 in),
D11..D46 are positions 1..36 (nets /LED_CH01../LED_CH36 in, matching T1's
schematic wiring exactly -- see the assertion below).
"""
import json
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET
from pathlib import Path

import pcbnew

REPO = Path("/Users/wells/Projects/airwriter")
BOARD = REPO / "hardware/airwriter/airwriter.kicad_pcb"
SCH = REPO / "hardware/airwriter/airwriter.kicad_sch"
PANEL_LAYOUT = REPO / "hardware/panel/panel_layout.json"
KICAD_CLI = "/Applications/KiCad/KiCad.app/Contents/MacOS/kicad-cli"

BOARD_ORIGIN_X = 100.0
BOARD_ORIGIN_Y = 60.0
PANEL_W = 62.0

# LED footprint pad-local geometry (airwriter:LED_SK6812MINI-E_..._TopFiring,
# derived from the stock ReverseMount footprint): pad "3" (VDD) sits at
# local (2.725, -0.75); the F.CrtYd rectangle is -3.65..3.65 x -1.87..1.87.
LED_VDD_LOCAL = (2.725, -0.75)

# Every bypass cap sits at one offset, 2.6mm north of its own LED (Wells,
# "Line the LED caps up for the light grille"). This replaced a per-LED search
# that put each cap on whichever side was geometrically nearest the VDD pad,
# which scattered them around the cells and under the grille's lattice walls.
# One uniform offset keeps every cap inside its own cell.
CAP_OFFSET = (0.0, -2.6)


def export_netlist():
    tmp = tempfile.NamedTemporaryFile(suffix=".xml", delete=False)
    tmp.close()
    subprocess.run(
        [KICAD_CLI, "sch", "export", "netlist", "--format", "kicadxml",
         "-o", tmp.name, str(SCH)],
        check=True, capture_output=True,
    )
    t = ET.parse(tmp.name)
    nets = {}
    for n in t.getroot().find("nets"):
        name = n.get("name")
        nets[name] = [(node.get("ref"), node.get("pin")) for node in n]
    return nets


def net_of_pin(nets, ref, pin):
    for name, nodes in nets.items():
        if (ref, pin) in nodes:
            return name
    return None


def serpentine_order(dots):
    """dots: list of panel dicts with r, c, led. Returns the 37 led:true
    dots in chain walk order, starting at row 7 (nearest U9/lower band),
    alternating direction per row so the walk stays column-adjacent."""
    by_row = {}
    for d in dots:
        if d["led"]:
            by_row.setdefault(d["r"], []).append(d)
    rows_present = sorted(by_row.keys())
    # walk rows in distance-from-7 order: 7,6,5,4,3,2,1,0 (all 8 rows exist)
    row_walk = sorted(rows_present, key=lambda r: -r)
    order = []
    for i, r in enumerate(row_walk):
        # round 5: descending column first (matches U9's board-east position
        # under the corrected, unmirrored coordinate map -- see module docstring)
        row_dots = sorted(by_row[r], key=lambda d: -d["c"])
        if i % 2 == 1:
            row_dots = list(reversed(row_dots))
        order.extend(row_dots)
    return order


def board_pos(dot):
    bx = BOARD_ORIGIN_X + dot["x"]
    by = BOARD_ORIGIN_Y + dot["y"]
    return bx, by


TEST_BUTTON_RC = (3, 3)  # SW3 sits here (panel dot (31,31) -> board (131,91)) though led:false


def main():
    layout = json.loads(PANEL_LAYOUT.read_text())
    dots = layout["dots"]
    dots_by_rc = {(d["r"], d["c"]): d for d in dots}
    led_dots = [d for d in dots if d["led"]]
    assert len(led_dots) == 37, f"expected 37 led dots, got {len(led_dots)}"

    # The walk is what assigns the reference designators: D10 is chain position
    # 0 and the walk order IS the electrical order, which the chain check below
    # then proves against the schematic rather than assuming.
    order = serpentine_order(dots)
    assert len(order) == 37
    populated = [(f"D{10 + i}", f"C{30 + i}", d) for i, d in enumerate(order)]

    # SW3 sanity check (round 5): the test dot (31,31) is led:false and not
    # walked, but its board position must still land at the centre column
    # (131, 91) under the corrected mapping, matching SW3's fixed placement.
    test_dot = dots_by_rc[TEST_BUTTON_RC]
    assert not test_dot["led"]
    tx, ty = board_pos(test_dot)
    assert abs(tx - 131.0) < 1e-6 and abs(ty - 91.0) < 1e-6, (
        f"test dot mapped to ({tx},{ty}), expected (131,91)")

    nets = export_netlist()

    board = pcbnew.LoadBoard(str(BOARD))
    fps = {fp.GetReference(): fp for fp in board.GetFootprints()}

    # SW_PUSH_6mm_H13mm's origin is pin 1, not the button centre -- the plunger
    # is the F.Fab circle 3.25/2.25mm away from it. Rounds before r6 aligned the
    # origin to the test dot, which left the physical button ~3.95mm off it.
    # Check the plunger, never the origin.
    sw3 = fps["SW3"]
    plunger = None
    for item in sw3.GraphicalItems():
        if (item.GetClass() == "PCB_SHAPE" and item.GetLayer() == pcbnew.F_Fab
                and item.GetShape() == pcbnew.SHAPE_T_CIRCLE):
            c = item.GetCenter()
            plunger = (c.x / 1e6, c.y / 1e6)
    assert plunger, "SW3 has no F.Fab plunger circle to check against"
    assert abs(plunger[0] - tx) < 1e-4 and abs(plunger[1] - ty) < 1e-4, (
        f"SW3 plunger centre is at {plunger}, must be on the test dot ({tx},{ty})")

    # Chain check, read off the live netlist rather than from a name formula:
    # each LED's DIN must be on the net the previous LED's DOUT drives, and the
    # first one's DIN must be the buffer output. Retiring a dot leaves gaps in
    # the position-equals-number rule would pass on a schematic whose chain was
    # wired out of designator order; following the links cannot.
    upstream = "/LED_DATA5"
    for led_ref, _cap_ref, _dot in populated:
        din = net_of_pin(nets, led_ref, "2")
        if din != upstream:
            print(f"FATAL: chain break at {led_ref}: DIN is {din!r}, expected "
                  f"{upstream!r} from the previous LED's DOUT. Refusing to place.")
            sys.exit(1)
        upstream = net_of_pin(nets, led_ref, "4")  # this LED's DOUT feeds the next

    placements = []
    for led_ref, cap_ref, dot in populated:
        bx, by = board_pos(dot)

        led_fp = fps.get(led_ref)
        cap_fp = fps.get(cap_ref)
        if led_fp is None or cap_fp is None:
            print(f"FATAL: missing footprint {led_ref} or {cap_ref} on board "
                  f"-- run the footprint sync stage first.")
            sys.exit(1)

        led_fp.SetPosition(pcbnew.VECTOR2I(pcbnew.FromMM(bx), pcbnew.FromMM(by)))
        led_fp.SetOrientationDegrees(0.0)

        dx, dy = CAP_OFFSET
        cap_x, cap_y = bx + dx, by + dy
        cap_fp.SetPosition(pcbnew.VECTOR2I(pcbnew.FromMM(cap_x), pcbnew.FromMM(cap_y)))
        cap_fp.SetOrientationDegrees(0.0)

        dist_from_vdd = ((dx - LED_VDD_LOCAL[0]) ** 2 + (dy - LED_VDD_LOCAL[1]) ** 2) ** 0.5
        placements.append((led_ref, dot["r"], dot["c"], cap_ref, dist_from_vdd))

    board.Save(str(BOARD))

    print(f"Placed {len(placements)} LEDs + caps.")
    worst = max(placements, key=lambda p: p[-1])
    print(f"Worst-case cap-to-VDD distance: {worst[0]} -> {worst[3]}, {worst[-1]:.2f}mm")
    over_2mm = [p for p in placements if p[-1] > 2.0]
    print(f"{len(over_2mm)} of {len(placements)} caps sit over the 2mm VDD-distance "
          f"guideline (the uniform grille offset is 2.71mm from the VDD pad by "
          f"construction, so this is expected, not a packing failure).")


if __name__ == "__main__":
    main()
