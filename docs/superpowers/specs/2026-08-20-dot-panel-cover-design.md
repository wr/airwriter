# Dot-panel cover design

2026-08-20 · Linear W-573 · supersedes the LCARS okudagram direction and, before it,
the dithered-mist cover. Approved by Wells in the brainstorm of 2026-08-20; scale
settled in hand with true-scale thermal prints (62 × 100 mm, variant D lineage).
[amended: panel is 62.0 × 104.27 mm - see amendment block below]

## What changes, in one paragraph

The show face stops being the main board's back and becomes a separate, fully passive
**art panel** — a second PCB ordered snap-apart in the same fab panel. It carries a
7 × 8 grid of same-size dots built from four PCB materials, a gold serif AirWriter
lockup, white silk port labels, and one physical `test` button plunger. The main board
loses every show-face feature, shrinks, and gains a top-firing RGB LED under each
translucent dot plus an 8-position DIP bank for persistent config. A 3D-printed black
lattice between the boards guides each LED to its dot. Firmware animates printer
state on the grid.

## Amendment, 2026-08-20 (post-generator)

The generator (Tasks 1-4, `hardware/panel/`) is built and DRC-clean; several
working values below are superseded by what actually came out of Wells's
Figma export and the generator's own measurements. `[amended]` marks the
stale lines in place; this block is the correction:

- **Panel is 62.0 × 104.27 mm**, not 62 × 100/101.51 — the Frame 9 re-export's
  viewBox is `0 0 1100 1850` px, not 1801.
- **Grid pitch 8.4545 mm, dots Ø5.6364 mm** (not "~7.9 mm pitch / ~5.2 mm dots"
  — those were pre-export estimates; the real numbers come straight out of
  the SVG's circle spacing).
- **Test hole is 2.25 mm NPTH, not 3.5 mm.** A tact switch plunger must be
  **≤ ~2.0 mm** to pass it, not ≤ 3.2 mm — this changes the main-board switch
  selection in §2.
- **Generator is `panel_layout.py` + `svg2poly.py` + `floors.py` +
  `emit_board.py`, run via `generate.sh`** — not a single `gen_panel.py`.
  `floors.py` is the fab-floor advisory pass (Task 4 follow-up): it warns,
  but does not fail the build, on any same-class poly gap or hole-to-outline
  pinch between 0.02 mm and 0.101 mm.
- **Microprint risk, accepted pending quote:** the AirWriter subtitle's
  thinnest strokes measure ~0.10 mm at their tightest hole-to-outline pinch
  (e.g. near x 6.87, y 83.93) — `floors.py` warns on this every run. Below
  the 0.10 mm mask-sliver floor stated in §1, so it's a real risk, not a
  false positive; whether a given fab can actually resolve it is a
  quote-time question, not resolved here.
- **The `test` tag is a mask knockout, not silk.** Superseding the silk-
  clipping caveat this amendment block originally carried: Wells's design is
  black letters on exposed gold, not white silk over the dot. `emit_board.py`
  detects any white art fully inside a gold-family dot's disc and cuts it
  into that dot's F.Mask instead of emitting it on F.SilkS — mask stays kept
  (dark) over the glyph strokes, punched back open (gold) for each glyph's
  own counter (e.g. inside the `e`). Data-driven: the current Frame 9 export
  already places the tag fully inside its dot, so no further re-export was
  needed. The silk-clipping risk is resolved by construction, not pending
  anything. `floors.py` carries the equivalent floor check under the label
  "mask ridge" (the solid mask between letter strokes has the same practical
  minimum as any other feature).

## 1. Art panel

**62 × 100 × 1.6 mm FR4, matte black soldermask, ENIG, 2-layer, zero components,
zero nets.** [amended: 62.0 × 104.27 mm - see amendment block above] It is pure materials. Corner radius and mounting features follow the
enclosure design (§3); panel proportions and exact element spacing are Wells's,
exported from Figma.

### Source of truth: Figma

Wells owns the artwork. He exports SVGs (as with `Frame 25.svg` / `Frame 27.svg` /
`Frame 7`); a generator — `hardware/panel/gen_panel.py`, same philosophy as the old
`artwork.py` — converts them into the `.kicad_pcb`.
[amended: the generator is `panel_layout.py` + `svg2poly.py` + `floors.py` +
`emit_board.py` via `generate.sh` - see amendment block above]

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
[amended: the panel now also carries a solid B.Cu flood everywhere except the
P/B/L dot windows (oversized 0.15mm), the fr4 slash, and the test NPTH's
clearance - the black soldermask alone still transmits some teal, so without
a copper backstop an LED would glow through the whole back face and bleed
between dots; the flood confines each LED's light to its own window. Dot
material at the dot location itself is unchanged - see `emit_board.py`'s
`back_copper_flood()`.]

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
[amended: pitch is 8.4545 mm, dots Ø5.6364 mm - see amendment block above]

### The test dot

One gold dot in the grid is the test button: a **3.5 mm NPTH** through its centre,
gold ring around it, white silk `test` beneath. The plunger of a tact switch on the
main board passes through it (§2). The hole is plain drilled fibreglass — no plating,
so no electrical path to the panel.
[amended: hole is 2.25 mm NPTH, not 3.5 mm; `test` is a black-mask knockout
on the gold ring, not white silk beneath it - see amendment block above]

### Lockup and labels

- AirWriter lockup (`Frame 27.svg` lineage): gold — mask openings over a copper zone,
  the coupon-proven trick. Includes the two microprint subtitle lines; smallest
  strokes must clear the 0.10 mm mask sliver floor, which the generator checks.
- Port labels (`Frame 25.svg` lineage): white silkscreen at the bottom edge,
  including the rules above each label.

## 2. Main board rework

### Removed (the whole show face)

- The 12 mm artwork band and the back-face artwork rule areas. *[amended 2026-08-20:
  the board does NOT shrink — the 62 mm-wide dot grid needs an LED directly under every
  translucent dot, so the board grows to the panel's own footprint, 62 × 104.27, with
  the circuits in the opaque lower band and ports out the bottom edge.]*
- MODE capacitive touch pad (SW2), its guard ring, R20, and C19's touch-revert role.
  IO14 is released.
- BOOT gold bullseye on the show face. USB download mode plus two plain component-side
  pads keep the insurance at zero visible cost.
- D1–D3 reverse-mount indicators and their bare-FR4 windows. Status lives on the grid.
  D3's power-LED role moves to a component-side 0603 (bring-up still wants a
  no-firmware sign of life; it faces inside the case).

### Added

- **LED array:** one SK6812MINI-E per translucent dot — **37** *[amended 2026-08-20:
  Wells ruled the six near-black cells are LED-backed too — dark until lit, physically
  identical to double-mask dots]* in the current map (56 dots, of which 19 are gold and
  unlit; the count follows Wells's final artwork) — top-firing, placed at the mirrored
  dot coordinates from `panel_layout.json`.
  All on the existing single chain from IO48 (D1, D2, D4–D6 predecessors collapse
  into this array); firmware pixel count is one constant.
- **Test switch:** 6 × 6 mm through-hole tact switch with a long round actuator,
  length chosen as lattice height + 1.6 mm panel + ~1.5 mm proud (the TS1102-style
  family runs 4.3–25 mm; exact MPN picked with the enclosure stack in the plan).
  Plunger ≤ 3.2 mm diameter to pass the 3.5 mm panel hole.
  [amended: hole is 2.25 mm, so plunger must be ≤ ~2.0 mm - see amendment block above]
- **DIP bank:** 8-position right-angle (piano) DIP switch at a board edge, actuators
  facing out through a slot in the case side. 8 GPIOs, internal pull-ups, closed = GND.
  Boot-read and runtime-polled. Initial map: 1–2 transport mode (AirPrint /
  +EtherTalk / +LocalTalk), 3 test-page-at-boot, 4 dev mode (regenerating printer
  UUID per SPEC §3.5), 5–8 reserved. SW1 stays inside as factory reset.
- **Component orientation flips in the case:** components now face the panel
  (LEDs fire up into the lattice); the ports still exit the front. Enclosure §3
  absorbs this.

### Power

37 LEDs *[amended]* at full white is ~2.2 A — far beyond the AP7361C's 1 A and the point.
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

The case itself: panel is the lid (62 × 104.27 outer *[amended]*), main board sits below
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
  [amended: generator is `generate.sh` (panel_layout.py/svg2poly.py/floors.py/
  emit_board.py); floors.py asserts hard fab floors and warns (not fails) on
  0.02-0.101mm gaps/pinches - see amendment block above]
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
