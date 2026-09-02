# Pre-order board review #4 — 2026-09-01

Fourth adversarial pass over the rev-A main board, on the three axes Wells asked for:
layout and schematic correctness, LCSC part selection (availability + cost), and circuit
correctness. Three reviewers ran in parallel (circuit, layout via scripted pcbnew, parts via
the JLCPCB/LCSC database and live cart API); every finding that reaches this page was
re-verified by the controller with an independent check, and the reviewers' own refuted
candidates are listed at the end so nobody re-derives them.

Source of truth for this pass: `hardware/airwriter/airwriter.kicad_sch` / `.kicad_pcb` at
commit 152ffa9, netlist exported 2026-09-01, ERC/DRC re-run tonight (ERC 0 errors, 44
lib-mismatch/isolated-label warnings; DRC 0 errors, 0 unconnected, 5 benign lib notes,
80 field-parity notes).

## Verdict

**The copper is sound. Nothing found tonight is a dead-on-arrival defect.** The shipped
package (`hardware/combined-fab.zip`) is byte-identical to a fresh `make_fab.sh` run and
matches the source board with zero footprint or track diffs. What the earlier passes missed
is at the edges of the copper: the order paperwork, the hand-solder joints, the cable, and
four floating inputs.

Three things should change before the order is placed; two are paperwork, one is a
ten-minute board edit that needs a package regen.

## Do before ordering

> **Applied 2026-09-02** (uncommitted on `claude/adversarial-pcb-review-ccb17d`): items 1–4
> are done — ORDER-NOTES carries the 0.12 mm exception and the corrected via-fill answer;
> both GND zones use THT thermal reliefs (one stranded island re-tied by `r12_fix.py` with a
> GND via at 119.626, 131.048; GND is one cluster, 153/153 pads); D3 is C12624; the combined
> package is regenerated and re-verified (pos 127, BOM C12624, drill floor 0.30, seam copper
> ≥ 0.5 mm, source DRC 0/0). Item 5 (silk legends) was NOT done — rev B. New benign DRC
> baseline in the combined file: 1 `starved_thermal` on U7.8's F.Cu sliver.
>
> **Also applied 2026-09-02, at Wells's request: J2 flipped to Mac-style wiring** (schematic
> labels 1↔2, 3↔5, 6↔8; six nets hand re-routed with three new vias + one GND stitch via at
> (111.45, 152.6); R5/R7 rotated 180°). The Apple crossover cable is now the right cable and
> a straight cable is the wrong one — the finding below is kept for the record but its
> "buy a straight cable" action is superseded. Source DRC 0/0, GND one cluster, package
> regenerated and re-verified.

1. **ORDER-NOTES: add the 0.12 mm space exception.** `airwriter.kicad_dru` relaxes clearance
   to 0.12 mm inside the `GND_STITCH_012` rule area, and its own comment says "PCBWay quote
   must allow 0.12 mm (4.7 mil) at these points". The measured minimum different-net gap on
   the board is 0.1205 mm at four spots around U4's SOT-223 tab (x 115–120, y 113–120 in
   the board frame). `docs/ORDER-NOTES.md` says only "6/6 mil". Add one line to the board
   spec so PCBWay does not raise a fourth engineering query or etch the LDO island marginal.
   Also say "0.15 mm" rather than "6 mil": the general minimum is 0.1516 mm.
2. **ORDER-NOTES: correct the via-fill answer.** It says the vias on SMD pads are "U1's
   center paddle, U4's tab". Verified tonight: 13 vias touch SMD pads, and four 0.30 mm
   drills sit fully inside ordinary pads — U3.10 (1.27 mm-pitch SOIC), C9.2, C12.2 and
   U1.1. Those are the GND stitch vias dropped straight into pads on 08-25. Board tenting
   does not cover them because the pad's own mask aperture opens them. Tell PCBWay the
   real count, that no fill is wanted, and that minor paste wicking is accepted. If the
   board is reopened anyway (item 3), nudge those four vias just outside the pad outline.
3. **Give THT pads thermal reliefs.** Both /GND zones are set to solid pad connection
   (`connect_pads yes`, KiCad ZONE_CONNECTION FULL) with no per-pad overrides. J4's three
   2.3 mm shield posts sit in a full 1 oz flood on BOTH layers; J2.4, J4.4, S1's eight GND
   pins, SW3's two and U7.8 are the same. J2 and J4 are exactly the parts Wells hand-solders
   after delivery. Set the zones to "thermal reliefs for THT", refill, DRC, regenerate the
   package with `make_fab.sh`. Ten minutes; SMD pads stay solid.

Cheap and worth doing in the same regen:

4. **D3 → LCSC C12624 (KT-0603G).** C72043 has 1 unit in stock and will bounce as an EQ
   again; C12624 has 256k, is cheaper, and is already pre-authorised in the order notes.
   Edit the LCSC field on D3 in the schematic and re-export the BOM.
5. **Port legends are printed under the connector bodies.** SERIAL, LOCALTALK, USB-C and
   PWR all fall inside J2/J4/J1's F.Fab outline and will be invisible on an assembled
   board. SW1 (reset) has no silkscreen at all. Move the legends north of the connectors
   (y ≈ 152–154) if the silk is touched.

## Bring-up traps (no copper change; must be written down)

### The printer cable must be straight-through. Apple's own cable is a crossover.

J2 is wired as the mirror image of the printer's socket: J2.3/J2.6 (TxD−/TxD+) go to the
SP3490 receiver, J2.5/J2.8 (RxD−/RxD+) come from its driver, J2.1 (HSKo) goes to the SP3232
receiver. That is correct only with a pin-N-to-pin-N cable. The Technical Reference's Table
E-2 makes the printer's pins 1, 3 and 6 outputs, the same directions as a Mac port, and
Apple's ImageWriter II cable (590-0552-A / M0197, "System/Peripheral-8") is therefore a
null-modem: 1↔2, 3↔5, 6↔8, 4↔4, 7↔7. Confirmed from two independent sources (pinouts.ru's
590-0552-A table; a 2026 bench trace at colino.net, which also reports that cheap
"ImageWriter II" reproduction cables are frequently wired straight instead).

With the Apple cable: our driver fights the printer's 26LS30 through the printer's 54 Ω
series resistors (~70–80 mA per line, under the SP3490's 250 mA limit — no damage), the
receivers float, and our HSKo input lands on the printer's DSR input, so R1IN sits at 0 V
on the SP3232's internal 5 kΩ pull-down, CTS never asserts, and after 30 s the firmware
reports **out of paper**. Every doc in the repo points debugging at differential polarity
and CTS polarity, not at the cable.

Actions: state in HANDOFF/SPEC and on the case legend that J2 takes a straight 8-conductor
mini-DIN-8 M-M cable (C2G 09569 class; avoid S-Video-style cables that drop conductors);
buy it before board 1 arrives; or build a 1↔2/3↔5/6↔8 adapter so the Apple cable also
works. SPEC §2.4's justification ("female on the board, so a straight cable works") is
wrong in its reasoning — gender has nothing to do with it — and will mislead a future
reviser. Rev B: four 0 Ω option pads make the port cable-agnostic.

### LT_DE floats as shipped, not just during reset.

PCBWay fits the DIP socket and nothing else, so every board leaves the fab with `LT_DE`
(U6.DE + U8.~RE + PIC RA4) undriven. The SP3485 datasheet is silent on floating DE, R14
pulls U6.DI high, and if DE drifts high U6 puts an enabled RS-485 driver across a live
LocalTalk bus. Bodge on board 1: 10 k across U6 pins 3 and 5 (DE and GND are adjacent
SOIC pins). Until then, do not plug a LocalTalk box into a board with an empty socket.
This was filed on 08-25 as a rev-B pulldown for the "PIC in reset" case; the ship state is
the stronger version.

### U3.11 (T1IN) floats from power-on.

`HSKI_DRV` is IO35 → U3.11 and nothing else. IO35 has no internal pull at reset and the
firmware never touches it. The SP3232 datasheet: "the driver's inputs do not have pull-up
resistors; connect unused inputs to VCC or GND" — the same sentence the 08-25 pass applied
to T2IN when it fixed the U3.10 island. T1OUT goes only to the DNP R9, so nothing leaves
the board, but the driver stage sits at threshold on the rail that feeds the CTS receiver.
Fix: drive IO35 low in the same first-statements-of-`app_main()` block as the IO48 clamp.
Rev B: tie T1IN to GND.

### IO48 float can be a brownout loop, not a flicker.

The window is power-on → `app_main()` (hundreds of ms). A floating AHCT input at 5 V picks
up noise from the LED field it sits in; SK6812s latch on an 80 µs low plus roughly-valid
bits. Enough latched white LEDs on a 500 mA port collapse VBUS in ~40 µs, the S3 browns
out, and the loop repeats. The rev-B 100 k on U9.2 is required, not cosmetic; on board 1 a
100 k from U9 pin 2 to GND is a hand bodge (0402 across SC-70 pins 2–3 is feasible).

### Program the PIC out of the socket, always.

TashTalk's config word sets `_LVP_OFF`, so ICSP entry puts ~9 V on MCLR. MCLR is wired
straight to ESP32 IO2 with no series resistor (abs max 3.6 V). A PICkit on the socket with
the PIC seated kills the module. Not warned anywhere. Rev B: 1 k in series in `LT_RESET`.

## Degraded (accept for rev A, record)

- **Single 0.4 mm VBUS via carries everything.** Nodal solve of the VBUS network (37 LEDs at
  36 mA + LDO at 600 mA) puts 1.94 A through the via at (151.250, 150.171) — the whole
  board's supply. A 0.4 mm barrel rates ~2.2 A, so there is no margin at the theoretical
  all-white corner. The firmware brightness cap remains the only limiter. A 0.6 mm partner
  via fits south-west of it clear of H4's screw head if the board is reopened.
- **The USB ESD clamp is on an 8.1 mm stub on D−.** D− is routed J1 → 12.5 mm → via, and
  U5.3/4 hang off that via on a branch; D+ is in-line (U5.6 sits 0.2 mm off the trace).
  Prior passes measured U5's placement (~6.3 mm) but never the routed path. Full-speed USB
  works regardless; the clamp is just less effective on D−. Also: D+ and D− are not a
  routed pair (different layers, 1.95 mm skew, 0 vs 2 vias) — irrelevant at 12 Mbit/s, but
  the docs should stop calling them one.
- **SP3490 fail-safe covers open inputs only.** The 08-25 verified line "R6/R7 DNP so the
  fail-safe holds RO high with the printer off" overstates the datasheet: the guarantee is
  for unconnected inputs; "printer off, cable attached" is ~0 mV differential through the
  printer's 54 Ω, inside the ±200 mV undefined window, and fitting R6/R7 lifts A by only
  ~37 mV. In practice the trip point is offset and RO reads high; it is just not guaranteed.
  Free fix: gate UART RX on CTS asserted. Low impact (the RX path carries only self-ID and
  XON/XOFF).
- **U9's supply can legally fall under the 74AHCT1G125's 4.5 V minimum.** USB permits 4.40 V
  at a bus-powered hub; subtract the VBUS spine drop under LED load. AHCT's real threshold
  (~1.5 V) and the SK6812's ratiometric VIH keep it working, but the "2.0 V VIH vs 3 V VOH"
  margin claim only holds above 4.5 V. Rev B: 74LVC1G125 (1.65–5.5 V).
- **VBUS bulk is ~4× the USB inrush budget.** ~71 µF nominal, ~40 µF effective at 5 V
  (C29277 confirmed 10 V-rated, so the rating half of the 08-25 concern is refuted), against
  USB 2.0's 10 µF / 50 µC. Some hubs will trip. Board-1 experiment: depopulate C21/C22 and
  see whether the LED field still runs clean at the capped brightness.
- **J4's shell is hard-tied to GND; only J2 goes through R8.** HANDOFF says both do. Either
  deliberate (a LocalTalk box is transformer-isolated) or an omission; write down which.
  Lifting R8 also buys less than the doc claims for J2, because J2.4 signal ground is
  hard-wired to board GND anyway.
- **U6's decoupling cap is 5.3 mm away by copper** (C14); every other IC is under 2 mm
  except U8 at 3.6 mm. Fine at 230 kbaud; the one outlier.
- **VBUS necks to 0.35 mm for 0.96 mm at U9's VCC** (netclass says 0.5). µA of real
  current; cosmetic.

## Documentation drift the case plan depends on

| HANDOFF says | Board is |
|---|---|
| LED bypass caps at a uniform **2.6 mm** north | **2.87 mm** north, all 37, zero variance — a lattice cut to 2.6 fouls every cap |
| H4 at (156.364, ~155) | H4 at (153.364, 151.930); panel seats match H1–H4 to 4 dp (the asymmetry is intentional) |
| SW1 "actuator flush at x = 100", "reset pinhole at x = 100" | SW1 is on the **east** edge, plate face at x ≈ 159.4, 2.6 mm inboard of x = 162 |
| "Mini-DIN shells reach GND through R8" | J2 only; J4 is direct |
| "strapping pins safe floating" (08-22) | IO45/IO46 yes; **IO3 no** — Espressif says it must not be high-Z. Harmless with factory eFuses; 10 k pulldown in rev B |
| J1/J2/J4 "2 mm proud" | J1 1.55 mm, J2/J4 2.05 mm past the edge — case cut-outs differ by 0.5 mm |

Also: KiCad is now 10.0.6 (the shipped gerbers came from 10.0.2) and both `airwriter`
library symlinks in the app bundle are gone; recreate them before the next konnect
placement session.

## Part selection (LCSC, verified 2026-09-01)

Every populated line resolves to the right MPN, package and value. Per-board component
cost at LCSC prices is about **$13.50 at 5 boards, $12.21 at 10** — the BOM is already close
to cost-optimal; this pass is about stock risk, not savings.

| Line | State | Action |
|---|---|---|
| D3 C72043 green 0603 | **1 in stock** | swap to C12624 KT-0603G (256k, cheaper) |
| D10–D46 C7423117 SK6812MINI-C | 5.5–6k in stock, $0.064 @185+ — the biggest line ($2.35/board) | keep; re-check stock the day the order is placed. The 3535 look-alikes (XL-3535RGBC-2812B) are pricier and unverified for chamfer/pin-1 orientation — do not substitute |
| U1 C2913204 | 14k, $5.06 → $4.53 @10; confirmed N8R2 quad PSRAM | keep |
| U5 C7519 USBLC6-2SC6 (ST) | 30k, $0.18 | optional: C2827654 clone at $0.044, same SOT-23-6 — safe, saves $0.14/board |
| U6/U8 C8963 SP3485EN-L | 335k, $0.28 → $0.21 | optional: UMW C668205 $0.25; not worth it at 10 boards |
| U2 C26735 SP3490, U3 C9378 SP3232 | 13.5k / 96k | keep; no cheaper drop-in worth qualifying |
| U4 C500795 AP7361C-33E | 6k, $0.81 → $0.64 | keep; the -33ER (C3743528) is the reverse-pinout SOT-223R, AMS1117 is not a drop-in |
| U9 C151417 74AHCT1G125SE-7 | 1.6k, sole SC-70-5 source in the DB | keep; fine at this volume |
| SW3 C920211 TS-1002-BR16026 | 649 in stock; 16 mm height confirmed | keep; thin, watch it |
| S1 C458097, SW1 C79164 | 2.7k / 2.5k | keep |
| J1 C165948 | 316k; 16-pin HRO confirmed | keep; the 60%-cheaper SHOU HAN C2765186 has an unverified pad layout — a mismatch is a scrapped panel |
| C29277 22 µF 0805 | 10 V X5R (not 6.3 V) | keep; the 25 V alternative is 81% dearer for nothing |
| R3/R14 C25804 | restocked, 31M | keep |
| Passives | all Basic-class, 1M–79M stock | keep |
| J2/J4 C5780060 Kycon | 0 at LCSC | Mouser ×6, as planned; DNP for PCBWay |

## Verified OK this pass (numbers, so nobody re-derives them)

- Package freshness: `combined.kicad_pcb` = source board, 138/138 footprints and 1080/1080
  segments identical; `make_fab.sh` reproduces all 29 shipped files byte-for-byte apart
  from the version/timestamp header; pos = 127 top-side rows, J2/J4/H_TEST/DNP excluded.
- GND is one copper cluster (`r12_gnd.py`: 38 islands, 1 component, 153/153 pads on plane).
  VBUS and 3V3 are each one component. 3V3 run U4.3 → U1.2 is 52 mΩ over 51 mm at 0.5 mm.
- Antenna keepout (x 156–162, y 127.6–154.5, both layers) has 0.0000 mm² of copper in it;
  the pour and the 1.9 A VBUS trunk abut its boundary at 0–0.15 mm. Combined with the
  accepted panel flood overhead, budget for detuning on the board-1 range test.
- V-score setback holds: main-board copper starts at y = 60.500 on both layers.
  Copper-to-edge ≥ 0.30 mm. Hole-to-hole min 0.333 mm (the 08-28 VBUS via fix holds).
  Smallest mask dam 0.200 mm, only on J1 (the gang-open answer covers it); next 0.269 mm.
- Silk: no silk on any mask aperture, no text under 0.8 mm / 0.15 mm stroke; pin-1 marks
  present on U2/U3/U6/U8/U4/U9, socket notch on U7, cathode bar on D3, chamfer corner on
  all 37 LEDs at top-left as the order note says.
- Drill inventory exactly as ORDER-NOTES claims (PTH 0.3 ×109 … 3.8 ×1; NPTH 0.65 ×2,
  2.75 ×4). The PTH file declares two 0.300 tools (T1 vias, T2 U1 paddle) — harmless.
- SW3 plunger at exactly (131.000, 91.000). S1 key tips at x = 97.75.
- Test pads TP2/TP4/TP5/TP7/TP8 (3V3, EN, LED_DATA5, TXD0, RXD0) exist as board-only 1.0 mm
  B.Cu pads at x ≈ 153.9; the schematic's two `isolated_pin_label` ERC warnings are those
  labels naming the nets. No exposed GND within 22 mm of them — add one in rev B.
- Circuit, re-confirmed from primary sources: J4 is a Mac-style port and a LocalTalk box
  plugs straight in; U6/U8 A/B polarity right despite the swapped `LT_A/LT_B/LT_Y/LT_Z`
  net names (rename, do not rewire); U8.~RE on LT_DE reproduces TashTalk's truth table;
  SP3485 guarantees RO=1 for open AND shorted inputs, which the transformer-coupled idle
  bus needs; TashTalk RA0 out / RA1 in / RA2 active-low CTS match the board; `_MCLRE_ON`
  means IO2 floating leaves the PIC running; IO35 is legal on N8R2; DIP on JTAG pins does
  not break USB flashing; EN/IO0 timing (13.9 ms release, IO0 at 3.15 V by then) is fine;
  HSKo path deterministic in every cable state; DSR (J2.2) is ignored by the printer per
  the TRM; SP3232 unused-channel and 100 nF charge-pump handling per datasheet; 47 Ω on
  DIN is correct (the datasheet's 500 Ω is for DIN-driven-without-VDD, impossible here);
  full-white LED current is 1.33 A (12 mA/channel), not 2.2 A; AP7361C pinout, caps,
  dropout (140 mV max @300 mA) and current limit all match — but its only datasheet θJA is
  110 °C/W, so the 08-25 "Tj 88 °C at 600 mA" figure is an estimate, and 600 mA sustained
  is not a real operating point (realistic 145–250 mA → Tj 72–92 °C in a 45 °C case).
- Mis-plugging a LocalTalk box into J2 or the printer into J4 is non-destructive.

## Rev-B list (new items this pass)

10 k pulldown on LT_DE; 100 k on U9.2; tie T1IN or pull HSKI_DRV low; 1 k series in
LT_RESET; 10 k pulldown on IO3; 1 k series in every switch/DIP leg (ROM drives GPIO40 =
DIP3 high in USB-OTG download boot); partner VBUS via at the H4 crossing; U5 in-line on D−;
74LVC1G125 for U9; R/C shell tie on J4 or document why not; port legends off the connector
bodies and a silk mark for SW1; a GND test pad beside TP7/TP8; rename the LT_* nets; 0 Ω
cable-option pads on J2; inrush soft-start if USB compliance ever matters.

## Refuted this pass (do not re-raise)

SP3490 not pin-compatible with MAX3490 (it is); IO35 illegal on WROOM-1 (only on octal
PSRAM modules); 22 µF caps 6.3 V (they are 10 V); 37 × 60 mA (12 mA/channel → 1.33 A);
crossover cable destroys the SP3490 (54 Ω limits it to ~75 mA); R14 wrong for TashTalk
(redundant but harmless, and it defines DI when the PIC is absent); RO/RA5 contention at
the DE edge (TashTalk sequences LATA5 before LATA4); MCLR needs an external pull-up
(`_MCLRE_ON`); HSKi open stalls the printer (ignored per TRM); LocalTalk box needs pins
1/2/7 (passive transformer, no); fitting R6/R7 makes the printer-off RX state
deterministic (37 mV, no); 47 Ω violates the SK6812 500 Ω note; DIP on JTAG breaks
flashing; LDO cooks in the case; U9 input clamp back-feeds VBUS (ms, trivial); 38 caps for
37 LEDs means one is missing (one spare); "TXD0/RXD0 have no pads" (they do — board-only
TP7/TP8); LED/USB-C cheaper "equivalents" (pricier or pad-layout unverified).
