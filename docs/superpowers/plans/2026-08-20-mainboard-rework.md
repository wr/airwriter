# Main-Board Rework Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Rework `hardware/airwriter/` for the dot-panel cover: strip the dead show face, grow the board to the panel's footprint, add the 37-LED array (5V, level-shifted), the 8-position DIP bank, and the test switch — ending ERC/DRC-clean, routed, with a regenerated fab package.

**Architecture:** Four serial phases: schematic surgery (remove show-face circuits, add LED array + U9 shifter + S1 DIP + SW3 test switch), then outline/placement (62 × 104.27 board, LED field scripted from `panel_layout.json`, circuits in the opaque lower band), then routing (the repo's `route.py`/`stitch.py` pipeline, in the handoff's mandatory order), then verification + fab regen. The panel generator's `hardware/panel/panel_layout.json` is the single source of LED coordinates.

**Tech Stack:** KiCad 10.0.2 (`/Applications/KiCad/KiCad.app/Contents/MacOS/kicad-cli`; pcbnew python at `/Applications/KiCad/KiCad.app/Contents/Frameworks/Python.framework/Versions/3.9/bin/python3`), Freerouting 2.2.4 via `hardware/airwriter/route.py`, python3 for helper scripts.

**Spec:** `docs/superpowers/specs/2026-08-20-dot-panel-cover-design.md` §2 (read the amendment block first). Fact base: the schematic/pin map and parts research recorded in W-573's comments and below.

## Global Constraints

- **PRECONDITION: KiCad must be closed.** `hardware/airwriter/~airwriter.kicad_pcb.lck` / `~airwriter.kicad_pro.lck` must not exist when any task starts (KiCad silently reverts external edits — the handoff's top trap). If lock files exist, stop and report BLOCKED.
- Board: **62.0 × 104.27 mm**, 2-layer, 1.6 mm, matte black, ENIG, corner radius 2.0 mm — same outline as the panel. Grid zone (panel coords x 2.8–59.2, y 2.8–67.6) carries ONLY the LED field and parts under ~4 mm tall; everything taller lives in the lower band (y 67.6–104.3) or on the back face. Ports exit the bottom edge (y = 104.27), under the panel's label row. WROOM antenna overhangs a SIDE edge from the lower band — never buried under copper, never in the grid.
- Fab floors: drill ≥ 0.30 mm, clearance ≥ 0.15 mm (net class already 0.15/0.2 signal/0.5 power — do not lower). Protect U1's 0.30 mm thermal vias (board-local edit; re-importing U1's footprint regresses them).
- LED power: **5 V (`/VBUS`)**. SK6812MINI-E is specified 3.7–5.5 V (3.3 V is out of spec — this is why D1/D2 die rather than move). Data path: IO48 `/LED_DATA` → U9 74AHCT1G125 (5 V supply, TTL VIH) → `/LED_DATA5` → the chain.
- New parts (all stock-verified 2026-08-20, LCSC unless noted):

| ref | part | LCSC | price @10 | role |
|---|---|---|---|---|
| D10–D46 (37) | SK6812MINI-E | C5149201 | $0.0805 | dot LEDs, top-firing |
| C30–C66 (37) | 100 nF 0603 | generic | — | one per LED, on /VBUS |
| U9 | 74AHCT1G125SE-7, SOT-353 | C151417 | $0.13 | 3.3→5 V data buffer |
| S1 | DP-08RP 8-pos piano DIP, THT | C458097 | $0.51 | side config (fallback: E-Switch KAP1108E, Digi-Key) |
| SW3 | TS-1002-BR16026 6×6 THT, 16 mm actuator | C920211 | $0.047 | test switch under the printed plunger (10.5 mm variant C455102 if the lattice lands short) |
| C21 ×2 | 22 µF 0805 | generic | — | /VBUS bulk at the LED field corners |

- GPIO assignments (from the verified pin map; all currently no-connect, none strapping, none console): DIP1–8 → **IO38, IO39, IO40, IO41, IO42, IO47, IO21, IO13**, each switch common to GND, internal pull-ups (no resistors). Test switch → **IO14** (freed when touch dies), net `/BTN_TEST`, to GND, internal pull-up, no cap (firmware debounces).
- Removals (verified exclusive support — nothing else hangs on these nets): SW2 + R20 + C19 (nets /BTN_MODE, /TOUCH_SENSE die), D1 + C8, D2 + C20, D4, D5, D6 (nets /LED_CHAIN, /LOGO_CH1/2/3 die; /LED_DATA survives, rerouted to U9). D3 + R17 STAY but D3's footprint changes reverse-mount 1206 → plain `LED_SMD:LED_0603_1608Metric` (inward-facing power light; its bare-FR4 window disappears with the old face). SW1 (BootPads_Service) + C7 STAY as-is. R18/R19, C9, C14, C16–C18 are NOT show-face — do not touch.
- The back face stops being a display: every back-face artwork rule area, mask polygon, and the `TOUCH GUARD` area is deleted. `artwork.py` is retired from the flow (git history keeps it).
- LED coordinate mapping (T2's script): panel dot (x, y) → board (62.0 − x, y). One mirror flip, X only — the panel is face-up above the component side. `led: true` dots only (37). Chain order: serpentine by rows (row 0 left→right, row 1 right→left, …) starting nearest U9 — minimizes inter-LED trace length; exact chain net names `/LED_CH01`…`/LED_CH36`.
- Routing order is the handoff's, verbatim: strip wiring → `route.py` → `stitch.py` → prune vias in keepouts → refill. `--refill-zones` on every DRC. Split `unconnected_items` into real breaks vs pour fragments before believing anything (the handoff's python snippet).
- Schematic edits are python text-surgery on `airwriter.kicad_sch` using existing symbol instances as templates (D1's SK6812MINI-E block for the array; R18's block for nothing — no new passive styles needed beyond copies). After EVERY schematic edit round: `kicad-cli sch erc` must pass with zero errors. Balance-check parens on any generated block (the malformed-footprint trap).
- Mounting: four M2 holes move to the lower band + grid-gap positions chosen in T2 (they index the lattice; keep them clear of dots and the antenna). Non-plated stays the rule.
- The mini-DIN/USB-C 2 mm proud-of-edge stand rule carries over to the new bottom edge. Kycon footprint origin quirks and the J1 3D-model offsets are in the handoff — J1/J2/J4 move as footprints, never re-imported.

---

### Task 1: Schematic surgery

**Files:**
- Modify: `hardware/airwriter/airwriter.kicad_sch`
- Create: `hardware/airwriter/tools_sch/` helper scripts if needed (committed, small)

**Interfaces:**
- Produces: ERC-clean schematic containing exactly the removals/additions above; net names as specified (Global Constraints) — T2 and T3 depend on those names verbatim.

- [ ] **Step 1: Verify preconditions** — no `.lck` files; `git status` clean apart from known dirty `hardware/airwriter/airwriter.kicad_pcb{,.zip}` (those two files are about to be legitimately reworked; note their dirty state in the report before touching them, and `git diff --stat` them so the pre-existing drift is recorded).
- [ ] **Step 2: Baseline ERC** — run `kicad-cli sch erc` on the untouched schematic; record the result (should be clean per the handoff).
- [ ] **Step 3: Removals** — delete SW2, R20, C19, D1, C8, D2, C20, D4, D5, D6 symbol instances and their now-dead local labels (/BTN_MODE, /TOUCH_SENSE, /LED_CHAIN, /LOGO_CH1/2/3). Keep /LED_DATA's label on IO48. ERC after.
- [ ] **Step 4: D3 footprint swap** — change D3's footprint field to `LED_SMD:LED_0603_1608Metric`; leave symbol and nets alone. ERC after.
- [ ] **Step 5: Additions** — add U9 (74AHCT1G125: A ← /LED_DATA, Y → /LED_DATA5, OE̅ → GND, VCC → /VBUS, GND; plus 100 nF C29 at its VCC), D10–D46 with per-LED 100 nF C30–C66 (VDD → /VBUS, chain /LED_DATA5 → D10 → /LED_CH01 → D11 → … → D46 DOUT no-connect), S1 (8 × SPST to GND; /DIP1–/DIP8 to the GPIO map), SW3 (/BTN_TEST ↔ GND on IO14), two 22 µF /VBUS bulk caps C21/C22. Use D1's old symbol block as the LED template (same lib_id `airwriter`-or-`LED`-lib as it used; copy exactly, renumber). ERC must end **zero errors**.
- [ ] **Step 6: Netlist sanity** — export netlist (`kicad-cli sch export netlist`) and assert with a small script: /VBUS now reaches U9 + 39 caps + 37 LED VDDs; /LED_DATA has exactly U1.25 and U9.A; chain nets each have exactly 2 nodes; /DIP1–8 and /BTN_TEST each have exactly 2 nodes; dead nets absent.
- [ ] **Step 7: Commit** — `git add hardware/airwriter/airwriter.kicad_sch` (+ helper scripts) with message "Rework schematic for the dot-panel face" + `Refs: W-573`.

### Task 2: Outline, placement, LED field

**Files:**
- Modify: `hardware/airwriter/airwriter.kicad_pcb`
- Create: `hardware/airwriter/place_leds.py` (reads `../panel/panel_layout.json`, places D10–D46 + their caps at mirrored coords, serpentine chain order)

**Interfaces:**
- Consumes: `hardware/panel/panel_layout.json` (dots with `led: true`), T1's net names.
- Produces: a placed, unrouted board — outline 62 × 104.27 with 2 mm radius, ports on the bottom edge with the 2 mm proud stand, LED field exact-mirrored, antenna overhanging a side edge, mounting holes placed, all old artwork/rule areas gone. Zero footprints missing, `kicad-cli pcb drc` reporting only unrouted items (no courtyard/outline violations).

- [ ] **Step 1: Update the board from the schematic** (footprint sync — KiCad python `board.Update…`/netlist import per KiCad 10; new parts arrive with placeholder positions, dead parts leave). U1's board-local via edits and D-footprint cutout-removals must survive — never re-import existing footprints.
- [ ] **Step 2: New outline + zone purge** — rebuild Edge.Cuts as 62 × 104.27 rounded rect (by script, replacing the old outline entirely — the append-don't-replace konnect trap applies to tools, so build by hand in pcbnew python). Delete every back-face artwork rule area, back mask/silk art polys, the TOUCH GUARD, the LOGO WINDOW and LED WINDOW keepouts, the wordmark reserves.
- [ ] **Step 3: `place_leds.py`** — mirror map per Global Constraints, LEDs + caps placed, serpentine order asserted against chain nets from the netlist (fail loudly on mismatch). Caps within 2 mm of their LED's VDD.
- [ ] **Step 4: Block placement** — lower band: ports (J1/J2/J4 bottom edge, proud stand preserved), U1 + antenna overhang on a side edge, U9 adjacent to the LED field's chain entry, S1 on the chosen side edge with keys outboard (confirm DP-08RP key orientation against its datasheet drawing before final position), SW3 under the panel's test dot at board (62 − 31.0, 31.0) — **exception to the tall-parts rule: it lives mid-grid by design; its 16 mm actuator is the plunger's seat**, the lattice gives it a shaft. PIC+socket, transceivers, regulator re-placed per the block-per-function principle. Mounting holes: 4 × M2 at grid-gap/lower-band positions, recorded in the report for the lattice plan.
- [ ] **Step 5: Pre-route DRC** — courtyards, outline, antenna keepout (rebuild the WROOM keepout at the new position — footprint zones store absolute coords, the handoff trap), zero missing-footprint/parity surprises other than the known benign classes. Commit "Place the dot-panel board" + `Refs: W-573`.

### Task 3: Route

**Files:**
- Modify: `hardware/airwriter/airwriter.kicad_pcb`

- [ ] **Step 1:** Strip wiring; `route.py --passes 30` (it rounds DSN coords — never hand-export); expect a full route with **no `unrouted` clause** in Freerouting's completion line.
- [ ] **Step 2:** `stitch.py`; prune any stitching vias inside keepouts; refill zones.
- [ ] **Step 3:** DRC with `--refill-zones --schematic-parity`, split unconnected_items with the handoff snippet: **zero real breaks**; violations 0; drill grep floor 0.30; clearance floor confirmed 0.15. Power-net check: /VBUS to the LED field on the 0.5 mm power class.
- [ ] **Step 4:** Commit "Route the dot-panel board" + `Refs: W-573`.

### Task 4: Verify and regenerate the fab package

**Files:**
- Modify: `hardware/airwriter/fab/` (gerbers, drill, BOM, pick-and-place), `hardware/airwriter/airwriter.kicad_pcb.zip`

- [ ] **Step 1:** Full check battery: ERC, DRC (as T3), 3D model paths resolve (handoff snippet), STEP export + top/bottom renders read by the implementer (LED field pattern must visually match the panel grid mirrored; ports on the bottom edge; DIP keys off the edge).
- [ ] **Step 2:** Regenerate fab outputs exactly as the handoff prescribes (BOM grouped WITH the DNP flag; pick-and-place `--exclude-dnp`; drill grep). BOM must show the new lines (37× C5149201, C151417, C458097, C920211) and none of the removed parts.
- [ ] **Step 3:** Cross-check LED positions in the pick-and-place against `panel_layout.json` mirror math with a script — every led:true dot has exactly one LED within 0.05 mm of its mirrored coordinate.
- [ ] **Step 4:** Commit "Regenerate fab package for the dot-panel board" + `Refs: W-573`. Post the W-573 checkpoint comment (controller may do this).

---

## Out of scope

- Panelizing panel+board for one PCBWay order, the swatch rail, and the quote (fab plan; the quote goes to Wells before any order).
- The light-guide lattice and case (plan 4) — but T2's mounting-hole and SW3-height choices feed it; record them.
- Firmware (`led_grid` count constant becomes 37, `dip_config`) — plan 3.
