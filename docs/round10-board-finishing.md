# Round 10 — board finishing guide (KiCad GUI)

The round-10 schematic is committed (`a118623`): SW1→EN reset rewire, R20 series-R,
C67 input cap, SW2 BOOT switch, UART0 breakout. **The board is NOT synced yet** — it
stays at round-9 HEAD. The place+route below needs the interactive (push-shove) router,
because two of the new traces are long routes across a dense, fully hand-routed board and
scripting/freerouting could not close them without destroying the existing route.

Everything new goes on the **component (F.Cu, hidden) face**. Keep clear of: the 37 lit LED
dots and their windows; the back-art layers (F.Cu SMD pads don't touch them — no
through-holes on the test points); and the enclosure-fixed parts — S1 DIP keys + SW1 reset
(west edge), J2/J4 mini-DINs + J1 USB-C (bottom edge), SW3 plunger at (131, 91). Free space
is ample (~380 clear spots mapped); the lower band (y≈150–163) and the unlit **gold**
inter-dot cells are opaque and open.

## Step 1 — Update PCB from Schematic
In pcbnew: **Tools → Update PCB from Schematic** (F8). This pulls the 3 new schematic parts
(R20, C67, SW2) with ratsnest and applies the net changes (SW1→EN, U9.4→LED_DATA5R,
U1.37→TXD0, U1.36→RXD0). The old U9.4→D10 LED_DATA5 track will now be a net-mismatch —
delete it; it gets re-routed through R20.

## Step 2 — Place + route the 3 schematic parts

| Ref | Value / Footprint | Nets | Suggested placement | Route |
|---|---|---|---|---|
| **R20** | 47R / `Resistor_SMD:R_0603_1608Metric` | pin1 `/LED_DATA5R`, pin2 `/LED_DATA5` | In the LED_DATA5 cut, ~**(151.0, 124.6)** between U9.4 (148.74,125.15) and D10.2 (153.64,124.07) — over gold cells, opaque | Short F.Cu: U9.4→R20.1, R20.2→D10.2 (both ≤2mm, the old-track corridor is clear) |
| **C67** | 1µF / `Capacitor_SMD:C_0805_2012Metric` | pin1 `/VBUS`, pin2 `/GND` | Within 3mm of U4.1 (116.65,113.9), e.g. ~**(117.5, 111.3)**, clear of U4 courtyard | VBUS: short F.Cu to U4.1 or a VBUS pad. GND: yields to the pour (refill) |
| **SW1** | B3U-1000P / `Button_Switch_SMD:SW_SPST_B3U-1000P` | pin1 `/EN`, pin2 `/GND` | Component face **near U1** (relocated from the west edge), bottom-access. Silk **'RESET'**. A clear spot lands ~10mm from EN copper (near ~(135,121)). | **EN: interactive route to EN copper** (nearest U1.3/R3.2/C6 ~9–11mm, needs a B.Cu detour + push-shove). GND: pour |
| **SW2** | B3U-1000P / `Button_Switch_SMD:SW_SPST_B3U-1000P` | pin1 `/BOOT_CFG` (=IO0), pin2 `/GND` | Component face **near U1**, bottom-access. Silk **'BOOT'**. Clear spot ~(116,130), ~19mm from BOOT_CFG copper (U1.27 area is congested). | **BOOT_CFG: interactive route to U1.27/BOOT copper** (~19mm B.Cu detour, push-shove). GND: pour |

**Both switches are now B3U-1000P (C231329), near U1, case-BOTTOM access — the west edge is free (only DIP S1 remains). Plan-4: no west-edge reset pinhole; two bottom buttons by the ESP32.**

**Scripted-finishing status (mb-r8):** R20 and C67 route clean by script (short direct F.Cu). The two switch traces (`/EN` SW1, `/BOOT_CFG` SW2) each need a B.Cu detour with vias on this fully-hand-routed density — a scripted router produces via-dangling / near-miss artifacts, so they want the interactive push-shove router. Same for a few test points whose nets only have copper in congested spots (EN, RXD0, LED_DATA5) — place those on their net in the GUI where you can nudge.

## Step 3 — 8 board-only test points
These are **bare pads, not BOM parts** — add them directly in pcbnew (Place → Footprint),
footprint `TestPoint:TestPoint_Pad_D1.0mm`, one pad each, assign the net, add an F.Silkscreen
label. Easiest: drop each on an existing track of its net in a clear component-face spot (the
pad on same-net copper = connected, no extra route). GND ones sit anywhere on the F.Cu GND
pour. Give each a ref like `TP1…TP8` (they'll show as benign `extra_footprint` in
schematic-parity, exactly like the mounting holes).

| Label | Net | Note |
|---|---|---|
| VBUS | `/VBUS` | many pads/tracks; pick a clear spot |
| 3V3 | `/+3V3` | near U4 output / U1 rail |
| GND | `/GND` | on the pour |
| GND | `/GND` | second ground, opposite side |
| EN | `/EN` | near R3/C6/U1.3 |
| TXD0 | `/TXD0` | near U1.37 (150.7,148.54) — pairs with the UART route |
| RXD0 | `/RXD0` | near U1.36 (149.43,148.54) |
| LED_DATA5 | `/LED_DATA5` | chain-entry debug (post-R20, at D10 side) |

(Dropped vs the original 13: the four RS-422 TPs — probe at J2's mini-DIN through-holes; USB_DP/DM — probe at their vias; plain LED_DATA — LED_DATA5 covers chain-entry.)

## Step 4 — U4 SOT-223 thermal via array
Drop a **4–6 via GND array** under U4's tab pad (pad 2, GND) into the back GND flood. Tab pad
spans ~**(116.65 … 122.95, 116.2)**. Use 0.3mm drill / 0.6mm pad (the board's stitch via
size). Then refill.

## Step 5 — Restore the antenna GND stitch grid
The antenna region lost stitching. Target the documented **~44-via** state (currently 25 GND
vias board-wide). The WROOM antenna rule area is the rectangle **(156.0,127.6)–(162.0,154.54)**
(it bars vias itself, so the grid drops around it). Re-run `hardware/airwriter/stitch.py`
AFTER routing (it adds one via per pour fragment + a 5mm lattice, reading the pour) — or drop a
manual 5mm GND-via grid into the pour near the antenna base. Do **not** strip signal tracks;
`stitch.py` only adds vias. Refill via `ZONE_FILLER` (not `kicad-cli --refill-zones`).

## Step 6 — verify
- DRC `--refill-zones --schematic-parity --severity-all`: 0 real breaks (split real vs
  Zone↔Zone); benign baseline ≈ 11 (5 silk-edge + 5 lib_footprint_mismatch + 1 TASHTALK
  silk_overlap) + the 8 test-point `extra_footprint` parity entries.
- Drills ≥0.30mm, clearance ≥0.15mm. All 3D model paths resolve.
- Then regenerate `fab/` + the zip + fresh STEP/STL, and update the BOM (new lines: R20,
  C67, SW2 = B3U-1000P/C231329; test points do NOT appear — board-only).
