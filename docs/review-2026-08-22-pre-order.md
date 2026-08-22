# Pre-order board review — 2026-08-22

Full design review of `hardware/airwriter/airwriter.kicad_pcb` before the PCBWay
mfg+assembly order. Four review passes ran in parallel: datasheet/pinout verification,
BOM availability+cost, PCB layout, plus the controller's own ERC/DRC/netlist audit.
Everything found was fixed the same day; the fab package in `hardware/airwriter/fab/`
and `hardware/airwriter-fab.zip` reflect the fixed board.

## Blockers found (all fixed)

### 1. All 37 LEDs would have fired INTO the board
The SK6812MINI-E (C5149201) is a reverse-mount package: its emitting window is on the
same side as the solder pads, and the body hangs 1.78 mm below the pad plane. It is
made to be soldered on the back of a board and shine through a cutout (Adafruit 4960
sells it exactly that way). The board used it top-side with no cutouts — the lens
would press into the soldermask and the body would not even sit flat.

**Fix:** all 37 swapped to **SK6812MINI-C** (OPSCO, LCSC **C7423117**, ~$0.06/pc,
8.9k stock) — the top-firing member of the same family, same SK6812 timing, same
12 mA/channel. Stock KiCad symbol+footprint pair (`LED_SK6812MINI_PLCC4_3.5x3.5mm`),
verified against LCSC/EasyEDA CAD data: pad 1 UL=DOUT, 2 LL=GND, 3 LR=DIN, 4 UR=VDD,
part orientation dot lands on the footprint's chamfer corner. LED positions unchanged
(panel alignment intact); footprints rotated 180° so DIN still faces the chain;
bypass caps moved 0.27 mm north for courtyard clearance. VIH is still 0.7·VDD, so the
74AHCT1G125 level shifter stays.

*Assembly note for PCBWay: confirm the LED's dot corner lands on the chamfered
footprint corner — the -C's datasheet pin numbers differ from KiCad's pad numbers,
and a 180° rotation is the easy mistake.*

### 2. Four ground pads were routed to 5 V
DRC schematic-parity: **U9 pin 1 (~OE), U5 pin 2 (ESD GND), U3 pin 8 (R2IN), C6
pin 2** were wired into the VBUS network. Consequences on a built board: U9's output
tri-stated → no LED data at all; the USB ESD clamp's ground at 5 V → D+/D− dragged
to ~4.3 V, USB dead and unprotected. Fix: stubs rerouted to GND vias; parity and
unconnected both 0 now.

### 3. Fab BOM DNP regression (again)
`fab/airwriter-bom.csv` grouped R8 (populated, printer-shield tie) into R9's DNP row —
the same grouping bug the handoff already documented once. An assembler working from
it leaves the mini-DIN shield floating. Fixed by adding `DNP` (and `LCSC`) to the
BOM `--group-by`; R8/R9 are separate rows now.

## Real bugs, would have shipped broken LocalTalk

- **U6 (SP3485 driver) had ~RE tied to 3V3.** With ~RE high and DE low the SP3485
  enters low-power shutdown; driver wake from shutdown is ~0.65–0.9 µs, so the first
  LocalTalk sync edge after enable gets clipped. ~RE now tied to GND (RO stays NC).
  TashTalk's reference circuit is one shared transceiver where this can't happen —
  the trap only appeared when TX and RX were split across two chips.
- **Both LocalTalk pairs were inverted vs Apple convention** (A/non-inverting → TxD−
  instead of TxD+). FM0 coding would probably have survived it, but there is no
  documented guarantee TashTalk is polarity-agnostic. U6 and U8 A/B swapped; now
  A→TxD+/RxD+, B→TxD−/RxD− per the ImageWriter II TRM / Mac serial convention.

## Verified correct (datasheet-checked, no action)

- Printer port: ImageWriter II DIN-8 mapping, receiver polarity (A←TxD+ idles RO at
  mark), driver polarity, HSKo→MAX3232 R1IN→IO8 as active-low CTS. All correct.
- TashTalk wiring: RA5 shared data pin, RA4 DE (+~RE), RA2 active-low CTS→IO6,
  RA0/RA1 UART directions, MCLR internal pull-up, 3.3 V @ 32 MHz OK.
- ESP32-S3: N8R2 keeps IO35/36/37 free; all chosen GPIOs plain; strapping pins safe
  floating; module pin numbering verified.
- U4/U5/U9 pinouts, USB-C CC pulldowns, charge-pump caps, EN/BOOT RC, DIP wiring.

## Component / sourcing changes

| Was | Now | Why |
|---|---|---|
| MAX3490ESA+ $3.46 | **SP3490EN-L** C26735 $0.77 | pin-compatible, 14k stock |
| MAX3232ESE+ $1.29 | **SP3232EEN** C9378 ~$0.25 | pin-compatible, 83k stock |
| SK6812MINI-E | **SK6812MINI-C** C7423117 | blocker #1 |

Every BOM line now carries an LCSC number (passives included). Two lines PCBWay
cannot source from LCSC:

- **J2/J4 Kycon KMDGX-8S-BS** — LCSC out of stock. Plan (unchanged from spec): marked
  DO NOT ASSEMBLE, Wells hand-solders; buy 2+spares from Mouser (~$1.7/pc, ~1k stock)
  or Digi-Key (7k stock).
- **U7 PIC12F1840-I/P** — DIP not on LCSC. PCBWay fits only the socket (C72124,
  $0.02); PIC bought from Digi-Key ($1.47), TashTalk-flashed, inserted by hand.

## Cost picture (5 boards, from live LCSC/PCBWay data)

- Parts as listed: ~$16.6/board list price; expect $30–40/board effective after
  PCBWay sourcing markup and attrition extras.
- PCB 62×104 mm 2-layer matte black ENIG ×5: ~$40–60. Assembly ~700 SMT joints +
  THT (S1, SW3, socket): ~$90–150 + stencil. DHL to EU ~$25–40 + VAT.
- **Ballpark $320–450 total for 5 assembled boards.** Standing rule: quote to Wells
  before ordering. `pcbway.com/quotesmt.aspx` with `hardware/airwriter-fab.zip` +
  `production/bom.csv` + `production/positions.csv`.

## Layout items fixed

- U4 LDO tab: 5×0.3 mm thermal vias into the back GND flood (was spokes-only,
  ~0.85 W at WiFi peak).
- C17 (U3's 100n) was 7 mm from the VCC pin on the wrong side; now 1.7 mm away.
- One 0.148 mm pinch (U4 pad 2 vs an LED trace) covered by the existing
  `GND_STITCH_012` 0.12 mm scoped rule — PCBWay's quote must allow 0.12 mm at these
  marked spots, same as before.
- Full re-route (Freerouting, 0 unrouted), 24 stitch vias, zones refilled.
  Final DRC: **0 errors, 0 parity, 0 real unconnected**; only the 5 known-benign
  lib-mismatch warnings (U1's deliberately enlarged drills) remain.

## Flags that are NOT board problems (case plan inputs)

- **SW3 (TS-1002-BR16026) is 16 mm tall per LCSC, not 13 mm** as the footprint name
  suggests. The test-dot plunger stack in the lattice/case plan (Plan 4) must use
  16 mm. Verify against the physical part before printing.
- LED body is now 3.5×3.7 mm (was 3.2×2.8) and ~1.1 mm tall — grille cell carve-outs
  in Plan 4 should use the -C outline.
- VBUS spine to the LED field is 0.5 mm/1 oz. At the theoretical 1.4 A full-white it
  runs warm (~20 °C rise); at art-panel brightness it is fine. Firmware should cap
  total LED current (~500 mA) — cheaper than copper.

## Open questions for a first article

- LT polarity + -C pin order both rest on secondary sources (TRM is for the Mac end;
  -C CAD data is EasyEDA's). Both are now wired to the documented-most-likely
  convention, and both are firmware-recoverable (UART invert / none) except LT
  polarity, which is fixed in copper — test LocalTalk on board #1 before relying on
  the run.
