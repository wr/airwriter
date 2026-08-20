# Dot-panel cover design

2026-08-20 · Linear W-573 · supersedes the LCARS okudagram direction and, before it,
the dithered-mist cover. Approved by Wells in the brainstorm of 2026-08-20; scale
settled in hand with true-scale thermal prints (62 × 100 mm, variant D lineage).

## What changes, in one paragraph

The show face stops being the main board's back and becomes a separate, fully passive
**art panel** — a second PCB ordered snap-apart in the same fab panel. It carries a
7 × 8 grid of same-size dots built from four PCB materials, a gold serif AirWriter
lockup, white silk port labels, and one physical `test` button plunger. The main board
loses every show-face feature, shrinks, and gains a top-firing RGB LED under each
translucent dot plus an 8-position DIP bank for persistent config. A 3D-printed black
lattice between the boards guides each LED to its dot. Firmware animates printer
state on the grid.

## 1. Art panel

**62 × 100 × 1.6 mm FR4, matte black soldermask, ENIG, 2-layer, zero components,
zero nets.** It is pure materials. Corner radius and mounting features follow the
enclosure design (§3); panel proportions and exact element spacing are Wells's,
exported from Figma.

### Source of truth: Figma

Wells owns the artwork. He exports SVGs (as with `Frame 25.svg` / `Frame 27.svg` /
`Frame 7`); a generator — `hardware/panel/gen_panel.py`, same philosophy as the old
`artwork.py` — converts them into the `.kicad_pcb`:

- One SVG (or layer-tagged group) per material class. The generator maps them to
  mask openings, copper zones, silk polygons, and drills.
- Regeneration is the only supported edit path. Hand edits to the panel board file
  get overwritten.
- The generator enforces the fab floors and fails loudly when artwork violates them:
  drill ≥ 0.30 mm, clearance ≥ 0.15 mm, silk line ≥ 0.153 mm / text ≥ 0.8 mm,
  mask sliver ≥ 0.10 mm.

### The dot materials (all verified on coupon #1 except one)

| code | build | powered off | lit from behind |
|---|---|---|---|
| G | copper + mask opening (exposed ENIG) | gold | gold — opaque, gets no LED |
| P | bare FR4, mask open both faces, no copper | tan | warm glow, follows LED colour (full RGB) |
| B | far face open, viewer face masked, no copper | invisible (mask) | bright teal — mask is the filter, hue fixed |
| L | mask both faces, no copper | invisible (mask) | dimmer, more saturated teal — hue fixed |
| . | mask over copper | black, nothing | nothing |

Mask-dot columns must be copper-free on both layers (trivial here — the panel has no
copper except under G dots and the lockup).

**Unverified:** single-mask teal (B) was reasoned from coupon #1, not observed.
The panel margin (the snap-off rail) carries a swatch line of B, L, and P dots at
final dot size so the material reads are confirmed on the same order that uses them.
Teal hue varies with mask pigment batch — accepted fab risk, noted on W-568's log.

### Grid

7 columns × 8 rows on a uniform pitch, dot map from Frame 7 (transcribed in
`hardware/panel/dotmap.txt` as rows of `GPBL.`). At 62 mm width the pitch lands
near 7.9 mm and dots near 5.2 mm; final numbers come from the Figma export and are
not constrained by anything except the fab floors and the LED package (§2) — dots
down to ~4 mm are safe.

### The test dot

One gold dot in the grid is the test button: a **3.5 mm NPTH** through its centre,
gold ring around it, white silk `test` beneath. The plunger of a tact switch on the
main board passes through it (§2). The hole is plain drilled fibreglass — no plating,
so no electrical path to the panel.

### Lockup and labels

- AirWriter lockup (`Frame 27.svg` lineage): gold — mask openings over a copper zone,
  the coupon-proven trick. Includes the two microprint subtitle lines; smallest
  strokes must clear the 0.10 mm mask sliver floor, which the generator checks.
- Port labels (`Frame 25.svg` lineage): white silkscreen at the bottom edge,
  including the rules above each label.

## 2. Main board rework

### Removed (the whole show face)

- The 12 mm artwork band and the back-face artwork rule areas — the board shrinks
  back toward ~52 × 66 mm; exact outline falls out of re-placement.
- MODE capacitive touch pad (SW2), its guard ring, R20, and C19's touch-revert role.
  IO14 is released.
- BOOT gold bullseye on the show face. USB download mode plus two plain component-side
  pads keep the insurance at zero visible cost.
- D1–D3 reverse-mount indicators and their bare-FR4 windows. Status lives on the grid.
  D3's power-LED role moves to a component-side 0603 (bring-up still wants a
  no-firmware sign of life; it faces inside the case).

### Added

- **LED array:** one SK6812MINI-E per translucent dot — **31** in the current Frame 7
  map (50 dots, of which 19 are gold and unlit; the count follows Wells's final
  artwork) — top-firing, placed at the mirrored dot coordinates.
  All on the existing single chain from IO48 (D1, D2, D4–D6 predecessors collapse
  into this array); firmware pixel count is one constant.
- **Test switch:** 6 × 6 mm through-hole tact switch with a long round actuator,
  length chosen as lattice height + 1.6 mm panel + ~1.5 mm proud (the TS1102-style
  family runs 4.3–25 mm; exact MPN picked with the enclosure stack in the plan).
  Plunger ≤ 3.2 mm diameter to pass the 3.5 mm panel hole.
- **DIP bank:** 8-position right-angle (piano) DIP switch at a board edge, actuators
  facing out through a slot in the case side. 8 GPIOs, internal pull-ups, closed = GND.
  Boot-read and runtime-polled. Initial map: 1–2 transport mode (AirPrint /
  +EtherTalk / +LocalTalk), 3 test-page-at-boot, 4 dev mode (regenerating printer
  UUID per SPEC §3.5), 5–8 reserved. SW1 stays inside as factory reset.
- **Component orientation flips in the case:** components now face the panel
  (LEDs fire up into the lattice); the ports still exit the front. Enclosure §3
  absorbs this.

### Power

31 LEDs at full white is ~1.9 A — far beyond the AP7361C's 1 A and the point.
Firmware enforces a global brightness cap; the states are dim animations by design.
The plan includes a real budget: worst-case animation current vs the 3.3 V rail,
and whether the LED VDD moves to VBUS with a level-shifted data line or stays on
3.3 V capped. This is decided by measurement/datasheet in the plan, not here.

## 3. Light-guide lattice and enclosure

A 3D-printed matte black lattice sits between the boards:

- One square-taper tube per lit dot: LED cavity (≥ 4 × 4 mm for the 3.5 mm package)
  at the bottom, dot-sized opening at the top. Walls ~1 mm; black, opaque —
  bleed control is its whole job.
- A clear channel for the test plunger.
- Indexing posts into the main board's mounting holes and matching features under
  the panel, so LED–dot alignment is mechanical, not assembly skill.
- Lattice height is the master variable: it sets the switch actuator length and the
  case height. Chosen in the plan alongside the tallest component (the DIP socketed
  PIC at ~9 mm proud is the current ceiling).

The case itself: panel is the lid (62 × 100 outer), main board sits below
component-side-up, ports out the front wall, DIP slot in one side wall. The old
"board is the case lid" assumption is dead; the mini-DIN plug forces now land in the
case walls, not the panel.

## 4. Fabrication

- One PCBWay order: main board + art panel, snap-apart (mouse bites), both matte
  black + ENIG, 1.6 mm, 2-layer. If PCBWay's multi-design panel fee beats two
  separate proto orders, order separately instead — decided at quote time, and the
  quote is shown to Wells before any order (standing rule: nothing is ordered
  without the render and price).
- The panel margin rail carries the material swatch line (§1).
- All existing pre-order checks apply to both boards: drill grep, clearance DRC,
  parity DRC, 3D model path check, render review.

## 5. Firmware

New component `led_grid` (host-testable like the raster components):

- Frame buffer over the dot map; per-dot material class from `dotmap.txt` baked in.
  Mask dots clamp to brightness-only (their hue is physical); FR4 dots are RGB;
  gold/empty cells are skipped.
- States: `idle` slow breathe · `printing` row-sweep pulse · `wifi_connecting`
  radial rings · `error` red blink (FR4 dots carry the red; mask dots drop dark —
  the physics is the aesthetic). Global brightness cap from §2.
- Driven from `printer_state` events; SK6812 protocol already proven on D1/D2.

`dip_config`: read 8 GPIOs, expose as config struct, re-read on change; consumed by
`net_provision` (mode) and `printer_state` (test page, dev mode).

Test button: debounced GPIO → print an internal test page through the existing
raster path. Test page content is a later decision; a placeholder pattern is fine
for bring-up.

All of this runs on the devkit with a strip of SK6812s before any board exists.

## 6. Verification

- `gen_panel.py` output passes `kicad-cli pcb drc`; floors enforced in the generator.
- Main board: ERC, DRC with `--refill-zones` and `--schematic-parity`, the
  unconnected-items split from the handoff, drill grep, model-path check.
- `led_grid` and `dip_config` get host-side checks in the existing test harness;
  `tools/rip` regression must stay byte-identical (this work never touches the
  raster path).
- Physical: swatch rail on the panel margin validates material B before the panel
  is trusted; light-guide bleed judged by eye on the first print.

## Open items (tracked in the plan, not blockers to it)

1. Lattice height → switch actuator MPN → case height (one chained decision).
2. LED supply rail: 3.3 V capped vs VBUS + level shift (measure/budget).
3. PCBWay multi-design fee vs two orders (at quote).
4. Final dot size/pitch and all element spacing (Wells, in Figma).
5. Test page contents (any time before M6 polish).
