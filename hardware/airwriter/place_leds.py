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
LED_CRTYD_HALF = (3.65, 1.87)
# Capacitor_SMD:C_0603_1608Metric's own F.CrtYd is -1.48..1.48 x -0.73..0.73
# (courtyard, not body -- read off the stock footprint). Kept unrotated
# (0 degrees) throughout, so these are its half-extents along each axis.
CAP_CRTYD_HALF = (1.48, 0.73)
CAP_MARGIN = 0.15


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


def free_neighbor_dirs(dot, dots_by_rc):
    """Which of the 4 panel-grid neighbors of `dot` are NOT a lit LED
    (either off-grid or led:false). Returns board-space directions --
    round 5: X is no longer mirrored, so a free panel "left" (c-1) neighbor
    is a free BOARD -x direction (not +x, as the old mirrored math had it).
    The test-button cell is led:false but is NOT free -- SW3 (a real,
    fairly large THT part) sits there."""
    r, c = dot["r"], dot["c"]

    def occupied(rr, cc):
        if (rr, cc) == TEST_BUTTON_RC:
            return True
        n = dots_by_rc.get((rr, cc))
        return bool(n and n["led"])

    free = set()
    if not occupied(r, c - 1):
        free.add("-x")   # panel left -> board -x
    if not occupied(r, c + 1):
        free.add("+x")   # panel right -> board +x
    if not occupied(r - 1, c):
        free.add("-y")   # panel up -> board -y
    if not occupied(r + 1, c):
        free.add("+y")   # panel down -> board +y
    return free


def cap_offset_for_dir(direction):
    """Local (dx, dy) offset from the LED footprint origin for a cap placed
    just clear of the LED's own courtyard in the given board-local direction,
    biased toward the VDD pad on whichever axis it doesn't move along."""
    vx, vy = LED_VDD_LOCAL
    hx, hy = LED_CRTYD_HALF
    cap_hx, cap_hy = CAP_CRTYD_HALF
    if direction == "+x":
        return (hx + cap_hx + CAP_MARGIN, vy)
    if direction == "-x":
        return (-(hx + cap_hx + CAP_MARGIN), vy)
    if direction == "-y":
        return (vx, -(hy + cap_hy + CAP_MARGIN))
    if direction == "+y":
        return (vx, hy + cap_hy + CAP_MARGIN)
    raise ValueError(direction)


# preference order: closest to the VDD pad first (measured against the
# footprint geometry above), falling back toward the GND/DIN side last.
DIR_PRIORITY = ["-y", "+x", "+y", "-x"]

# D10 sits in the grid-zone corner right next to U9/C29 (the chain-entry
# level shifter, an allowed grid-zone exception -- see the report). The
# panel-grid neighbor check has no idea U9/C29 are there, so it happily
# offers +y for D10's cap even though that lands C30 on top of U9's
# courtyard under the round-5 (corrected, unmirrored) chain entry. Forbid
# it explicitly rather than teach the whole heuristic about board-level
# fixed obstacles for one cell.
FORBIDDEN_CAP_DIRS = {"D10": {"+y"}}


def main():
    layout = json.loads(PANEL_LAYOUT.read_text())
    dots = layout["dots"]
    dots_by_rc = {(d["r"], d["c"]): d for d in dots}
    led_dots = [d for d in dots if d["led"]]
    assert len(led_dots) == 37, f"expected 37 led dots, got {len(led_dots)}"

    order = serpentine_order(dots)
    assert len(order) == 37

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

    placements = []
    for idx, dot in enumerate(order):
        led_ref = f"D{10 + idx}"
        cap_ref = f"C{30 + idx}"

        expected_net = "/LED_DATA5" if idx == 0 else f"/LED_CH{idx:02d}"
        actual_net = net_of_pin(nets, led_ref, "2")  # pad 2 = DIN
        if actual_net != expected_net:
            print(f"FATAL: chain-order mismatch at position {idx}: "
                  f"{led_ref}.DIN is {actual_net!r}, expected {expected_net!r} "
                  f"for this walk position. Refusing to place.")
            sys.exit(1)

        bx, by = board_pos(dot)

        led_fp = fps.get(led_ref)
        cap_fp = fps.get(cap_ref)
        if led_fp is None or cap_fp is None:
            print(f"FATAL: missing footprint {led_ref} or {cap_ref} on board "
                  f"-- run the footprint sync stage first.")
            sys.exit(1)

        led_fp.SetPosition(pcbnew.VECTOR2I(pcbnew.FromMM(bx), pcbnew.FromMM(by)))
        led_fp.SetOrientationDegrees(0.0)

        free_dirs = free_neighbor_dirs(dot, dots_by_rc)

        def fits_on_board(direction):
            ox, oy = cap_offset_for_dir(direction)
            cx = bx + ox
            return (BOARD_ORIGIN_X + 0.6) <= cx <= (BOARD_ORIGIN_X + PANEL_W - 0.6)

        forbidden = FORBIDDEN_CAP_DIRS.get(led_ref, set())
        candidates = [d for d in DIR_PRIORITY
                      if d in free_dirs and d not in forbidden and fits_on_board(d)]
        chosen_dir = candidates[0] if candidates else None
        if chosen_dir is None:
            # every panel-free direction would run the cap off the board
            # edge (edge-column LEDs choosing -x/+x), or the LED is fully
            # boxed in -- fall back to +y, which never approaches a side
            # edge, even if the direction wasn't reported "free" by the
            # neighbor check (worst case it's merely not optimally close).
            chosen_dir = "+y"
            print(f"WARN: {led_ref} ({dot['r']},{dot['c']}) has no board-safe "
                  f"free neighbor direction; falling back to +y (may need a manual nudge)")
        dx, dy = cap_offset_for_dir(chosen_dir)
        cap_x, cap_y = bx + dx, by + dy
        cap_fp.SetPosition(pcbnew.VECTOR2I(pcbnew.FromMM(cap_x), pcbnew.FromMM(cap_y)))
        cap_fp.SetOrientationDegrees(0.0)

        dist_from_vdd = ((dx - LED_VDD_LOCAL[0]) ** 2 + (dy - LED_VDD_LOCAL[1]) ** 2) ** 0.5
        placements.append((led_ref, dot["r"], dot["c"], cap_ref, chosen_dir, dist_from_vdd))

    board.Save(str(BOARD))

    print(f"Placed {len(placements)} LEDs + caps.")
    worst = max(placements, key=lambda p: p[-1])
    print(f"Worst-case cap-to-VDD distance: {worst[0]} -> {worst[3]}, "
          f"{worst[-1]:.2f}mm (direction {worst[4]})")
    over_2mm = [p for p in placements if p[-1] > 2.0]
    print(f"{len(over_2mm)} of 37 caps landed over the 2mm VDD-distance target "
          f"(courtyard clearance took priority; see report).")


if __name__ == "__main__":
    main()
