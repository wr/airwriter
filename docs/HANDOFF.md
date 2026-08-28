# AirWriter handoff

Written 2026-08-04, last updated 2026-08-28.

## Where the project stands

**2026-08-28 (later still): the no-hardware-needed firmware is written — W-586, branch
`wells/w-586-firmware-status-controls` (stacked on W-585's branch, both unmerged).**
Four new pieces, every core pure C with host tests in `tools/rip` (subagent-driven, each
task adversarially reviewed; plan in
`docs/superpowers/plans/2026-08-28-firmware-status-controls.md`):

- **`printer_state`**: flags-in, one-derived-state-out reducer (setup > no-wifi > blocked
  > printing > error > ready) + a mutex-guarded ESP glue; main.c posts events from wifi,
  AP fallback, doc begin/end and the serial watchdog. `status_json` gained a machine
  field `"ps"`.
- **`led_grid`**: chain-order material map GENERATED from the schematic netlist + board
  positions (`hardware/panel/gen_led_map.py` -> committed `led_map.c/h`; the netlist walk
  caught KiCad-10 regex traps — see the script's comments). Pure renderer `lg_render`
  (state x time -> 37xRGB; FR4 dots full RGB, mask dots forced monochrome, ERROR drops
  mask dots dark, **global cap 48/255 applied as the last multiply — the board has no
  hardware current limit**). Glue drives the chain over RMT via `espressif/led_strip`
  3.0.3 at ~33 fps; devkit strips use `AIRWRITER_LED_COUNT` + `AIRWRITER_LED_DATA_PIN`.
- **`dip_config`**: decode + 3-sample debounce core; glue polls DIP1-8 + the test button
  at 100 ms with internal pull-ups (S3 pins 38/39/40/41/42/47/21/13, button 14; all −1
  off-S3). Provisional bit semantics live in `dip_config_core.h`: DIP1-2 mode, DIP3 dev,
  DIP4 test-on-boot, DIP5-6 dither override, DIP7-8 spare — firmware-level defaults, the
  silk only numbers the switches.
- **Test page**: button (or DIP4 at boot) prints a 64-row checkerboard+gradient page at
  1152 dots through the real ht/iw/printer_out path. **A new `s_job_lock` mutex in
  main.c serializes it against real IPP jobs** — review caught that `s_printing` was
  never the real lock (ipp_server's `s_print_lock` was), and a mid-test-page phone job
  would have interleaved bytes on the wire.

What the devkit can demo now: every LED state pattern on a strip, DIP/button on jumpers,
the full IPP->raster->serial path in loopback. Still hardware-gated: everything through
the transceivers. Not started (unchanged): PIC/LocalTalk bridge (PAP-gated), consumers of
`mode`/`dither_ovr`.

**2026-08-28 (late): the firmware audit's must-dos are implemented and verified —
W-585, branch `wells/w-585-firmware-preboard-hardening`.** All three must-dos plus the
host-test seam landed the same day the audit was filed:

1. **ESP-IDF v5.5.1 is reinstalled** at `~/esp/esp-idf` and the project is retargeted:
   `idf.py set-target esp32s3` + `fullclean` done, regenerated `sdkconfig` verified
   TX=17 / RX=18 / CTS=8, `LED_DATA_PIN=48`, loopback off. The C6 devkit build lives in
   its own tree now — `idf.py -B build_c6 -D SDKCONFIG=sdkconfig.c6 build` (config
   verified TX=4/RX=5, CTS −1, loopback on). Both targets build clean. The old devkit
   sdkconfig was preserved as `sdkconfig.old`.
2. **The IO48 clamp is in**: first statements of `app_main()`, before the log line and
   `nvs_flash_init()`, compile-guarded by the new Kconfig `AIRWRITER_LED_DATA_PIN`
   (48 on S3, −1 elsewhere — the C6 block compiles out entirely). `esp_driver_gpio`
   added to main's REQUIRES.
3. **The stalled-printer brick is dead, and NOT by the audit's sketched fix.** Two
   adversarial review rounds shaped this (9 agents, then 3 clean skeptics):
   - **Trap, recorded for good: `uart_get_tx_buffer_free_size` is not a capacity
     probe.** In IDF v5.5.1 it returns `tx_buf_size - tx_len_tot`, and `tx_len_tot`
     counts only the ring item the ISR is currently draining — queued items behind it
     are invisible. A first implementation that bounded `uart_write_bytes` (which has
     NO timeout — it blocks in `xRingbufferSend(portMAX_DELAY)`) by that probe still
     parked the rip task forever when CTS stalled mid-job. The audit's clear_to_send
     poll had the same class of hole.
   - **Shipped design: no TX ring at all** (`uart_driver_install` tx size 0). Writes go
     FIFO-direct through `uart_tx_chars` — a spinlocked `uart_hal_write_txfifo` that
     takes what the 128-byte FIFO fits and returns the count; it cannot block. The loop
     lives in `serial_out/serial_writer.c`, pure C behind injectable ops, host-tested
     (28 checks incl. a no-busy-spin invariant): fail fast once the watchdog says
     BLOCKED, delay one 100 ms watchdog period on any remainder. Pacing is lossless at
     9600 baud (96 bytes drain per period < 128 FIFO).
   - **Failure now propagates**: `printer_out()` checks the write, sets a failure flag,
     **purges the FIFO** (`serial_out_purge`, `uart_ll_txfifo_rst`) so a dead job's
     ≤128-byte tail can't print garbage ahead of the next job when paper returns, and
     calls `job_pipe_abort()`; the rip task unwinds, `s_task` clears, the next job is
     accepted. `doc_end` says "printer timeout".
4. **Host suite is 277 checks across 8 binaries** (`make test` in `tools/rip`), stream
   regressions byte-identical. `tools/rip/Makefile` now compiles `serial_writer.c`
   (named explicitly — the rest of serial_out is IDF-bound).

Audit items 5–6 (MCLR: resolved-no-code; brightness cap: waits for `led_grid`) and the
feature gaps (dip_config, PIC bridge) are unchanged. Order state below is unchanged:
W654223ASS53 still needs Wells's file-replacement email; nothing is paid.

**2026-08-28 (evening): boards waiting on the PCBWay quote; firmware audited for the
wait; parts sourcing settled.** The session after the review below did four things:

1. **The in-flight PCBWay order W654223ASS53 must not build as uploaded.** Wells started
   it 08-24 with the pre-review files (they still have the ten stranded-ground DOAs) and
   it has sat at an engineering query since 08-25. The reply is drafted and in
   `docs/ORDER-NOTES.md`: replace the files with `hardware/combined-fab.zip` +
   `combined-top.png`, and answer the soldermask-bridge question with "gang-open the
   USB-C pin rows" (its 0.20 mm dams are under their 0.22 mm matte-black minimum;
   everything else clears). Wells sends that email; nothing is paid until he sees the
   re-quote (standing rule). Pay only through the PCBWay site, never to bank details
   from an email signature.
2. **Firmware was audited for pre-board work — read `docs/firmware-audit-2026-08-28.md`
   before touching firmware.** Headline: no pin is wrong anywhere; three must-dos are
   (a) reinstall ESP-IDF (it is NOT on this Mac — the "v5.5.1 installed" stratum below
   is stale) and later `idf.py set-target esp32s3` + `fullclean` (sdkconfig is still the
   C6 devkit build), (b) the IO48 output-low clamp as app_main's first statements,
   S3-guarded (C6 has no GPIO48), `esp_driver_gpio` added to main's REQUIRES,
   (c) a deadline in `serial_out_write()` — as written, one job to a stalled printer
   permanently bricks the print service ~10 minutes later (traced through job_pipe's
   never-exiting task; the audit doc has the minimal fix design). None of it is written
   yet; Wells asked for the audit to be filed, not implemented.
3. **Parts Wells buys himself (links in the session log/artifact):** Kycon KMDGX-8S-BS
   ×6 from Mouser (sole channel with stock — order first), PIC12F1840-I/P DIP ×3 from
   Digi-Key, Wurth 9774030151R ×6 if consigning. Programmer decision: a ~$26 PICkit 3
   clone (eBay, US seller tommyvdp) driven by **PICkitminus** (kair.us; macOS CLI;
   PIC12F1840 confirmed on its device list) — chosen because TashTalk's config word sets
   `_LVP_OFF`, which requires high-voltage programming and rules out the MPLAB Snap and
   every LVP-only cheapie; the TL866II universal programmer was checked and does NOT
   support the 12F1840 at all. The PICkit 5 ($78) is the fallback if the clone is a dud.
4. The teal swatch was cancelled and the test hole went gold-plated in the review pass
   below; W-573's memory and the spec amendments carry both decisions.

**2026-08-28: third pre-order review (machine battery + 21 adversarial agents); every
finding fixed the same day; the order package is final.** The verified review report is
an artifact: https://claude.ai/code/artifact/906e3ccc-a3d1-490d-bf9d-19e78b7fe92b.
What changed, in one pass over the source board and the panel generator:

- **GND pour pulled back from the V-score**: seam edge of both /GND zone outlines moved
  y 60.3 → 60.5 (PCBWay rejects copper within 0.4 mm of a score; KiCad's DRC checks the
  wrong rule and stays green either way). Verified in the shipped gerbers: closest copper
  is now Y-60500000 on both layers.
- **The second VBUS via** (added 08-25 at 153.967, 126.846) sat 0.186 mm wall-to-wall from
  its partner — under PCBWay's 11 mil and the project's own 0.25 mm rule, hidden because
  `hole_to_hole` was severity *warning*. Moved to x = 153.82 (0.733 mm centres). Both
  projects now set `hole_to_hole` to **error** — run `drc --severity-all` anyway.
- **J2/J4 excluded from the position file** (`exclude_from_pos_files`, same mechanism as
  H_TEST); pos is 127 placements and PCBWay's line can no longer try to place the
  hand-soldered mini-DINs. They stay in the BOM with their DO-NOT-ASSEMBLE notes.
- **The test hole is a 3.8 mm PLATED bore now, gold to the edge** (Wells: "a big via").
  `emit_board.py` emits a netless plated pad (Ø4.4 pad / Ø3.8 drill, copper layers only),
  the black hole-surround (`keep_dia`) is gone, and the pad + annulus ring + tab art share
  a decorative net **PANEL_AU** so their deliberate contact doesn't DRC as a short.
  `combine_boards.py` keeps that one net and strips all others — do not "fix" either end.
- **Swatch rail cancelled** (Wells, 2026-08-28): the single-mask-teal risk is accepted
  unmitigated for this order; the spec's swatch references are amended out.
- **SPEC §4 assumptions 3 and 4 are both resolved from the Technical Reference already in
  `docs/reference/`** (Appendix E pp. 204-209): TxD polarity corroborated, HSKo is half a
  26LS30 off ±5 V — the SP3232 receiver is sound on paper. Board-1 scoping is confirmation.
- **U8's shutdown-on-transmit wiring is deliberate and safe** — ~RE rides LT_DE, so U8
  drops to shutdown while the PIC transmits; wake is ≤1.8 µs against LocalTalk's ~200 µs
  minimum inter-dialogue gap. It mirrors the U6 bug fixed on 08-22 but is NOT that bug;
  do not "fix" it.
- **Firmware must** (new item, alongside the brightness cap and CTS deadline): first
  statements of `app_main()` drive **IO48 low** — U9's input has no pull and floats from
  power-on until firmware claims the pin; a brief LED flicker at boot is expected on this
  rev. Rev B: 100 k from U9.2 to GND, filed with the LT_DE 10 k.
- **Stale packages deleted**: `hardware/airwriter-fab.zip` and
  `hardware/combined/combined.kicad_pcb.zip` (a mislabeled 08-24 PCBWay-plugin export) are
  gone from the tree. Upload **only** `hardware/combined-fab.zip` +
  `hardware/combined/combined-top.png`. The paste-ready order notes live in
  **`docs/ORDER-NOTES.md`** (supersedes the notes inside the 08-25 review doc).
- U2/U3's schematic MPN/Manufacturer fields finally match the parts bought since 08-22
  (SP3490EN-L / SP3232EEN, MaxLinear).

Full battery after all of it: ERC 0; source-board DRC 0/0/0 with parity under
`--severity-all` (5 benign lib notes); combined board under the real rules 0 clearance /
0 unconnected (the 6 seam `invalid_outline` notices are the two-outlines-sharing-an-edge
construction, traced unambiguous); drill floor 0.30; pos 127 rows, no J2/J4; the 3.8 in
the PTH file, NPTH is pegs + mounting only.

**2026-08-25: second adversarial pre-order review; two DOAs found and fixed; package
regenerated.** Read `docs/review-2026-08-25-pre-order-2.md`. Short version: ten top-side
GND islands had no via to the plane — SP3490, both SP3485s, LED 20 onward, the WROOM's
100 nF caps and CC1's pulldown were all ungrounded (KiCad reported them as ten
`Zone<->Zone` unconnected items, which the 08-22 rule wrongly called benign — treat ANY
unconnected item as real from now on). Fixed with 12 vias and three short bottom-side
track jogs; DRC now 0 errors / 0 unconnected and the GND net is one copper cluster. The
second DOA: SW1's EVQP44B3M footprint was a guess (its descr said so) and would have
bonded EN to a frame tab — redrawn from Panasonic ANCTB27E, SW1 sits inboard of the case wall where
Wells put it (origin 157.225, 99.454; plate face at x=159.35). Also
fixed: 3V3 track and VBUS via under the H4 screw head, a second via on the 5 V layer
crossing, "V-CUT" text moved off Edge.Cuts, H_TEST out of the pos file, and
`CONFIG_AIRWRITER_INVERT_CTS` in firmware. Regenerate the package with
`sh hardware/combined/make_fab.sh` (after `python3 hardware/combine_boards.py`); upload
`hardware/combined-fab.zip` + `hardware/combined/combined-top.png`. Order notes are in the
review doc. Wells decided to keep the panel's back copper over the WiFi antenna; measure range on board 1. ESP-IDF is no longer installed on this Mac; the
firmware edit is uncompiled and `firmware/sdkconfig` is still a C6 build.

**2026-08-22: full pre-order board review done; three blockers found and fixed; the fab
package is regenerated and order-ready.** Read `docs/review-2026-08-22-pre-order.md` —
short version: the SK6812MINI-E was a reverse-mount part that would have fired into the
board (all 37 now SK6812MINI-C, C7423117, stock PLCC4 footprint, positions unchanged);
four "GND" pads were routed to 5V (U9 ~OE among them — no LED data, dead USB ESD);
U6's ~RE-high shutdown trap and inverted LocalTalk A/B polarity are fixed; U2/U3 are now
SP3490/SP3232; the fab BOM's R8-in-the-DNP-row regression is fixed and every line has an
LCSC number. Board re-routed from scratch, DRC 0 errors / 0 parity / 0 real unconnected.
Order package: ~~`hardware/airwriter-fab.zip` + production CSVs~~ [superseded — the
single-board package is deleted; the order is the combined panel, see `docs/ORDER-NOTES.md`]
(J2/J4 hand-solder, U7 socket-only). Quote to Wells before ordering, as ever. Case-plan
flags: SW3 is 16mm tall (not 13), LED body is now 3.5x3.7mm.


**2026-08-20: the cover pivoted to a dot-grid art panel; the panel is BUILT and the main
board is deep into its rework (W-573, branch `wells/w-573-dot-panel-cover`).** Everything
below this section about the back-face artwork, MODE touch pad, BOOT bullseye and indicator
windows is superseded. The spec is
`docs/superpowers/specs/2026-08-20-dot-panel-cover-design.md` — **read its amendment block
before its body**; several working values in the body are marked `[amended]`. The SDD
execution ledger with every ruling is
`.superpowers/sdd/2026-08-20-mainboard-rework/progress.md` (git-ignored scratch — read it
before resuming, it is the true sequence of what happened).

### What exists now

**The art panel is done.** `hardware/panel/generate.sh` turns Wells's Figma export
(`hardware/panel/svg/panel.svg` — the single source of truth; fill colour IS the material)
into a DRC-clean, byte-deterministic `panel.kicad_pcb`, 62 × 104.27 mm, matte black, ENIG,
zero components except four blind reflow-standoff seats. Current export lineage: **Frame 9b**
— 56 dots = 19 gold (one is the `test` dot, 3.8 mm plated bore, black mask-knockout letters),
14 bare-FR4 (`#E7BA9B`, full-RGB pixels), 9 single-mask bright teal (`#14BABA`), 14
double-mask deep teal (`#176767`); both top corners are gold so mounting bosses hide under
them; **37 lit dots total**. The back face is a solid copper flood (the black mask transmits
teal — without copper the LEDs bleed across the whole face) with windows only at the 37 lit
dots, the fr4 slash, and the test NPTH. Four exposed rings in the flood take **PEM
SMTSO-M25-3ET-class blind nuts** so M2.5 screws climb from rubber feet through case floor,
main board, and printed lattice into the panel — no hardware on the art face; hole/seat
coaxiality between the boards is load-bearing, change them together or not at all
(`STANDOFFS` in `hardware/panel/emit_board.py`).

**The main board rework is placement-complete pending one in-flight adjustment.** 62 ×
104.27 (matches the panel; it grew, it did not shrink), ports on the bottom edge at 19.5 mm
even centers, LED field placed from `panel_layout.json` (identity mapping
`board = panel + (100, 60)` — an earlier mirror rule was WRONG and is dead), each LED's
100 nF cap at one uniform offset 2.6 mm north (Wells's light-grille needs the cells clear),
LED and passive designators on F.Fab. West edge is the control edge: S1 (DP-08RP right-angle
piano DIP, real measured footprint, keys overhang to x=97.8 — case wall 2.25 mm thick puts
key tips flush with the outside; DIP 1 at the BOTTOM) and SW1 (Panasonic EVQP44B3M side
reset, actuator flush at x=100, case pinhole). SW3 test switch's PLUNGER (not origin —
3.95 mm offset, footprint origin is pin 1) centred at (131, 91) under the panel's test dot.
U9 level shifter lives inside the grid under a gold dot; U4 stays near (144.3, 130.8) by
ruling (gold cells cost 25–100 mm of power run). U5 ESD sits at its geometric floor from J1
(~6.3 mm — J1's own courtyard makes < 5.7 impossible). WROOM antenna is FLUSH at x=162
(no case bump — Wells chose flush), keepout narrowed to module width + 5 mm/side.

**In flight when this was written** (agent `mb-r7`): restoring the LED chain to **37** (a
controller misread briefly "retired" two — Wells said *move*, and his 9b art did the moving),
lifting **U1 north ~3.5 mm** so mounting hole H4 lands at Wells's intended SE corner
(156.364, ~155), antenna keepout riding along trimmed clear of dot (7,6). Mount pattern
after that lands: H1 (105.636, 65.636) under the gold corner dot, H2 (156.364, 65.636) under
the other, H3 (105.636, ~152) west-low, H4 (156.364, ~155) east-low — all M2.5 clearance
(2.75 mm), non-plated, panel seats coaxial. Verify with the alignment proof
(37/37 LEDs + plunger + holes at 0.0000 mm) before believing anything.

### The working pipeline (all of it exists, use it)

`hardware/panel/generate.sh` (panel; idempotent, floors.py advisory warnings are expected —
8 known, incl. the accepted 0.101 mm microprint pinch and 0.089 mm test-glyph mask ridge).
Main board: `place_leds.py` (re-sync LEDs to the panel json; asserts serpentine chain and
plunger), then **strip → `route.py --passes 60` (run DETACHED with nohup; tool-shell resets
kill plain background runs; it wipes and re-routes from scratch every time) → `stitch.py`
ONCE (its lattice box is hardcoded around U1 — update it when U1 moves) → prune vias in
via-barred areas → refill → DRC `--refill-zones --schematic-parity --severity-all`** and
split unconnected into real-vs-Zone↔Zone (the handoff snippet below still applies). Benign
DRC baseline: 5 silk-edge + 5 lib_footprint_mismatch (U1's vias + the four enlarged holes)
+ Wells's own TASHTALK silk overlap. Routing lesson that repeatedly paid: every "impossible"
net was a single-track keyhole — **widen the approach, never shorten the path**.

### New traps from this rework (each cost real time)

- svgelements yields `<mask>` *content* as ordinary elements (the plates bug) and ignores
  mask attributes; the generator strips defs and rect-crops masked paths. The Figma export's
  final `<rect fill="white">` lives inside `<clipPath>` — hiding or deleting it clips the
  whole SVG to nothing.
- Fabs clip silk over exposed copper; neither DRC nor renders show it. That is why the
  `test` tag is a mask knockout.
- `board.Save()` randomizes every UUID (panel generator derandomizes; keep it), and KiCad's
  `Fracture()` converts polygon holes to slit outlines on save — `HoleCount()` reads 0 on
  reload; verify by vertex count or render.
- A board footprint keeps its OWN copy of the 3D model reference — adding a model to the
  library does nothing for already-placed parts. Hand-written `.wrl` bodies render in KiCad
  but are absent from STEP/STL exports; the LED model needed a +0.84 mm z-lift because the
  reverse-mount package hangs through a cutout our top-firing footprint doesn't have.
- pcbnew: `board.Remove()` corrupts later `FootprintLoad`/`GetNetInfo` in-process (split
  scripts); `FootprintLoad` drops the library nickname from the FPID; footprint-owned rule
  areas are not in `board.Zones()`; deleting routed tracks leaves their clearance cut-outs
  frozen in the stored zone fill (refill via `ZONE_FILLER` — `kicad-cli --refill-zones`
  does not write back).
- The stock WROOM courtyard fuses the module body with a 48 mm RF-recommendation rectangle;
  splitting them (body = courtyard, RF = rule area notched around fixed parts) is what made
  the flush antenna placeable at all.
- The SMTSO seats need SOLID back-flood copper — a dot window under a seat leaves nothing
  to solder to (corner dots being gold in 9b is what makes the top seats work).

### Next, in order

1. Land the in-flight U1-lift/37-LED restore; Wells reviews renders; then the committed
   route + full battery (this file's checks) — that closes the board plan's Task 3.
2. Task 4: regenerate `hardware/airwriter/fab/` (BOM must show C5149201 ×37, C151417,
   C458097, C920211, EVQP44B3M, and none of the removed parts — J6/R18/R19/J3 are GONE,
   IO36/37 freed), renders/STEP checks, whole-branch final review, fresh Desktop exports.
3. Plan 3, firmware: `led_grid` (37 px, material map from `panel_layout.json`, mask dots
   brightness-only teal, FR4 dots full RGB), `dip_config` (IO38/39/40/41/42/47/21/13,
   pull-ups, DIP1=mode…), `/BTN_TEST` on IO14. Runs on the devkit.
4. Plan 4, lattice + case: every number it needs is in the ledger and §2/§3 of the spec —
   hole positions (from the final board), SMTSO stack, DIP key tips x=97.75 / z 8.11–9.01 /
   2.25 mm wall, reset pinhole at x=100, plunger through the 3.8 mm plated panel hole, carve-outs
   for S1's top corner, U4, R3, U9 (1.1 mm tall under its gold dot).
5. Fab: panel + board snap-apart at PCBWay; quote to Wells before ANY order (standing
   rule). [swatch rail cancelled 2026-08-28 — teal risk accepted unmitigated, see top entry]

**Hardware rev A is finished and not yet ordered.** The board routes clean, the fab package is
generated, and the mini-DIN is settled — Kycon KMDGX-8S-BS from Mouser. Nothing blocks the order.

**An artwork swatch coupon is ordered ahead of it (PCBWay, 2026-08-09).** Wells was on the fence
about the cover's whole visual language — logo, mist, touch pad — and the coupon
(`hardware/coupon/`, black mask, ENIG, 60 x 45mm) shows all of it in real materials for ~$40:
the harvested wordmark, the mist, all three MODE pad candidates A/B/C, and the LED window sizes.
When it arrives: judge the aesthetic, pick the MODE pattern (the main board currently carries C),
and only then order rev A. Note the main board's fab package predates the SW1/SW2 rework and
needs regenerating before that order.

**Six of the nine firmware components exist and are tested.** `urf_decode`, `resample`,
`halftone` and `imagewriter` do the raster path; `ipp_attrs` and `ipp_server` do AirPrint.
All six build for the ESP32 and run there — ESP-IDF v5.5.1 is installed and the project
flashes. 569 checks pass on the host. Still to write: `serial_out`, `printer_state`,
`net_provision` and `webui`, all of which want hardware or are M6.

| | State |
|---|---|
| Schematic | Complete, ERC clean, zero errors |
| PCB | Routed, 52 x 78mm, DRC down to one known violation |
| Fab package | Gerbers, drill, BOM, pick-and-place in `hardware/airwriter/fab/` |
| Firmware | M2 and M4 done: raster pipeline and AirPrint, 569 checks green |
| Devkit | ESP32-C6 stands in for the board; it discovers, accepts jobs and prints from an iPhone |
| Boards ordered | No |

Linear project **AirWriter** on the Personal team tracks this. W-562 through W-567 are milestones
M1–M6 from the spec; **W-568** is the board and carries the full decision log in its comments.

## What the board is

An ESP32-S3 dongle with two mini-DIN-8 ports and a USB-C power inlet.

- **Printer port** drives an ImageWriter II over RS-422 at 9600 baud with DTR handshaking. This is
  the reason the project exists.
- **LocalTalk port** puts the board on an AppleTalk network as a node, using a PIC12F1840 running
  Tashtari's TashTalk firmware as the LocalTalk data-link engine.
- **AirPrint** runs over WiFi, concurrently with either.

Three modes were scoped. Mode 1 is AirPrint only. Mode 2 adds EtherTalk over WiFi and needs no
hardware, only firmware. Mode 3 adds the physical LocalTalk port. **The board is built for mode 3**,
so all three are reachable — but only one AppleTalk transport can be live at a time unless someone
writes an AppleTalk router. AirPrint is IP and runs alongside any of them.

## Deviations from the spec, and why

`docs/SPEC.md` has been amended in place where these apply. Read it as the current design, not the
original draft.

**The receiver polarity in spec §2.4 was wrong.** The draft mapped transceiver A to the printer's
TxD- and B to TxD+. Apple's convention is that a binary 1 has TxD- more negative than TxD+, so the
`+` line is non-inverting. With A low at mark, the receiver would idle low and every byte would
arrive inverted. Corrected to **A ← pin 6 (TxD+), B ← pin 3 (TxD-)**. Confidence is good but not
certain: both sources document the Macintosh serial port rather than the printer, and they are
secondary. The ImageWriter II Technical Reference would settle it.

**The polarity jumpers are gone.** The spec wanted solder jumpers so the differential pairs could be
swapped without a respin. The ESP32 UART does the same job in firmware — `uart_set_line_inverse()`
with `UART_SIGNAL_TXD_INV` / `UART_SIGNAL_RXD_INV`. Inverting DI flips Y and Z together, which is
electrically identical to swapping the pair, and the same holds for RO. Four parts and eight pads
saved, insurance kept.

**U2 is a MAX3490, not a MAX3491.** The spec tied DE high and RE low permanently, so the enable pins
were dead weight on the printer port.

**SW1 is an Omron B3U-1000P.** The closest KiCad stock footprint to the spec's "4.5mm tactile" was
the B3S-1100, whose courtyard is 10 x 8.9mm. That had become the binding constraint on the pocket
beside the mini-DIN.

**R9 added**, a DNP 0R in series between the MAX3232's T1OUT and the printer's HSKi. The spec wanted
that path wired but not populated; a DNP series resistor expresses that without U3 driving HSKi by
default.

**Four parts are fitted only if the printer port misbehaves.** They are the bare pad pairs in the row
under U3 and U2, and they are marked DNP in both the schematic and the board.

| | Value | What it does | Fit it when |
|---|---|---|---|
| R5 | 120R | Differential termination across the printer's TxD pair, at U2's receiver | The cable is long enough to ring. At 9600 baud on a printer cable it only loads the line |
| R6 | 4.7k | Failsafe bias, +3V3 to TxD+ | The receiver reads garbage with the printer off or unplugged |
| R7 | 4.7k | Failsafe bias, TxD- to GND | Same — R6 and R7 are a pair, fit both or neither |
| R9 | 0R | Series jumper from the MAX3232's T1OUT to the printer's HSKi pin | You decide the board should drive HSKi rather than only listen on HSKo |

**The BOM had three parts wrongly marked DNP.** It grouped rows by value and footprint but not by the
DNP flag, so R8 landed in the same row as R9 and R18/R19 landed in the same row as R6/R7 — and the
row took the DNP label. An assembler working from it would have left the printer port's shield
ungrounded and the I2C bus without pullups. Regenerated with DNP in the group-by. The pick-and-place
file is now exported with `--exclude-dnp`, so it lists only parts that should actually be placed: 53
of the 57.

**Nine board footprints carried placeholder values** — R17 read "R17", C19 read "C19", J6 read "J6" —
because the board was never fully re-synced from the schematic. Those show up in the pick-and-place
file's value column, so an assembler would have seen "R18" where "4.7k" belongs. Synced against the
BOM.

**Board is 52 x 78mm, not 55 x 35.** The aspect flipped to portrait because the WROOM-1 wants its
antenna overhanging a short edge while both mini-DINs and the USB-C need the opposite one. It grew
from 32 x 54 when the LocalTalk port was added, to 66 tall when the indicators, the buttons and
the I2C header needed a band of their own, and to 78 for the artwork band below. Growing it sideways
was tried first and undone: the ports live on one edge, so the room has to come from the axis they
sit on.

**A blank band runs across the board at y 86 to 98, for a logo and whatever else wants silkscreen.**
Roughly 50 x 12mm, clear of copper on both layers apart from stitching vias, sitting between the
ESP32 row and the transceiver row. The AirWriter wordmark sits just above it at y 82 to 86. Adding
the band is what took the board from 66 to 78mm tall: every footprint below y=87 moved down 12mm and
nothing above it moved, so the antenna keepout, the front connectors and their 2mm proud stand are
all untouched.

C9 and C14 came down with the band. They bypass U2 and U3, and leaving them in the top row would
have put them 22mm from the chips they serve. They now sit at the top edge of the lower block,
about 5mm from their pins — closer than before the board grew.

**The two remaining connectors carry function labels, not pinouts.** J3 is `USB` and J6 is
`I2C DISPLAY`. Per-pin labels do not fit: J3 has 1.35mm between the board edge and U1's pads, and
J6's 1mm pitch cannot hold text at the 0.8mm minimum height with legal spacing. J6 is wired in Qwiic
pin order — GND, 3V3, SDA, SCL — so the word is the pinout for anyone with a Qwiic cable.

**U7 sits in a socket and J5 is gone.** The PIC is a **PIC12F1840-I/P** in PDIP-8, dropped into an
8-pin DIP socket, and it gets TashTalk flashed into it in a chip programmer before it is inserted.
That deletes the ICSP header, five pads and a part from the board, and it means a PIC that will not
take firmware never gets soldered down.

What changed with it:

- **MCLR now runs to the ESP32 on IO2**, net `LT_RESET`, instead of to J5. It was only ever wired
  to J5 pin 1, so in normal operation it floated — held up by nothing but the PIC's internal weak
  pull-up. Firmware can now hold the LocalTalk engine in reset, which is worth having on a headless
  dongle: it recovers a wedged PIC without a power cycle, and it keeps the PIC quiet in the modes
  where LocalTalk is not the live transport. Driving it low asserts reset; leaving the GPIO as an
  input lets the PIC's internal pull-up run it, which is the state through ESP32 boot. No external
  resistor. IO2 was picked because it is a plain GPIO — not a strapping pin, not JTAG, not the
  console UART — and it is the closest free module pad to U7, 5.1mm away.
- **The socket is 10.7 x 10.8mm against the SOIC-8's 7.8 x 4.9**, so the top-right block spread out:
  U7's body centres on (133.5, 69), C17/R14/R18/R19 moved right to x=141.5, and R15/R16 dropped to
  y=76. The DIP-8 footprint's origin is pad 1, not the body centre — offset by 3.81mm in both axes
  when placing it.
- **It stands about 9mm proud** once a PIC is seated, against roughly 12mm for the mini-DIN bodies.
  Still inside the tallest thing on the board, but check it against the case lid.
- **The socket is not a BOM line.** kicad-cli only emits parts that have a schematic symbol, and the
  socket has none — it is the footprint U7 sits on. Buy it separately: LCSC `C72124`, CONNFLY
  DS1009-08AT1NX-0A2, $0.024. U7's BOM description says so.

**"Schematic parity clean" was never true.** Nothing had been running the check — `kicad-cli pcb
drc` only does it with `--schematic-parity`, and none of the earlier runs passed it. The board sits
at 15 parity issues and sat at 16 before the socket work, so removing J5 accounted for the one
that went away. None of them is a wiring error:

- `extra_footprint` on H1–H4, because mounting holes have no schematic symbol. Normal.
- `footprint_symbol_field_mismatch` on D2, D3, J4, J6, SW2, U6, U7, U8 — field text that differs
  between the two files, not connectivity.
- `net_conflict` on D2 pad 1 (the last WS2812's DOUT, genuinely unconnected) and on J6's two
  mounting-post pads.

Run it with the flag before ordering if you want them gone; none of them changes the copper.

**Firmware owns the PIC's reset line.** Pulling RA3/MCLR low holds TashTalk in reset and tristates
its outputs. Nothing else shares the RS-422 driver here — U6 and U8 are the LocalTalk pair, U2 is
the printer's — so this is not about bus arbitration; it is about being able to restart the LocalTalk
engine on a board with no reset button and no console. The wire was free at fab time and impossible
to add afterwards, which is why it went in before the order rather than after the first bug.

**The mounting holes are not grounded, deliberately.** A ground ring buys nothing in a printed
plastic case, and it would create a second path to ground that cannot be broken. The mini-DIN shells
already reach GND through R8, which is a 0R precisely so that tie can be lifted; the ImageWriter II
is mains-powered with a metal chassis, so a second, unbreakable ground path is exactly what puts
noise on the serial link. The holes stay non-plated.

**All three front connectors stand 2mm proud of the board edge**, so their faces sit flush with the
outside of a 3mm-wall printed case rather than with the PCB. Move them together or not at all. They were not coplanar with each other
before either — the mini-DINs stood 1.25mm proud while the USB-C sat 0.55mm back. The mini-DIN
origin sits 2.75mm past the edge; the USB-C origin 2.15mm short of it.

**Resistor and capacitor designators live on F.Fab; everything else is on the silkscreen.** Those
two classes are the dense ones, and their silk text was the source of 61 of the 75 DRC violations
the board carried. They still print on the assembly drawing, so pick-and-place and hand assembly
lose nothing. Chips, connectors, LEDs, buttons and mounting holes keep silk designators, and the
parts a user touches carry a function label above and the designator below: PWR, NET, JOB over the
LEDs, BOOT and MODE over the buttons.

**The placement is block-per-function, not row-per-part-type.** The first layout put every passive
into three long rows across the middle of the board regardless of what it served — USB-C CC
pulldowns 40mm from the connector, RS-232 charge-pump caps far from the MAX3232 — while the whole
bottom-right quadrant sat empty. Each block now sits with its chip:

| Block | Where | Parts |
|---|---|---|
| ESP32 rail and EN | directly under U1 | C4 C5 R3 C6 |
| LocalTalk digital | top right | U7 PIC in a DIP socket, C17, R14, R15, R16 |
| I2C expansion | right edge, top | J6, R18, R19 |
| Printer analog | left, above J2 | U3, U2, C9–C15, R5–R9 |
| LocalTalk line drivers | centre, above J4 | U6, U8, C16, C18, R10–R13 |
| Power chain | right, above J1 | C2, C3, U4, U5, C1, R1, R2 |
| UI strip | band above the ports | D3 D1 D2, SW1 SW2, R17, C7, C8, C19, C20 |

Moving U6 and U8 down beside J4 cut the four LocalTalk differential runs from about 30mm to about
14mm. Nothing enclosure-critical moved: U1, J1, J2 and J4 are exactly where they were.

**The four mounting holes are a symmetric 45 x 50mm rectangle.** H2 used to sit at x=161.5, which is
9.5mm off the right edge of a board that ends at 152 — it was outside the outline entirely, so rev A
had three usable holes and one that would have been drilled into nothing.

**The ground pour is stitched, with 44 vias, not the 82 a blanket grid wanted.** Freerouting never
stitches, so a fresh route leaves the front pour cut into 17 fragments. `stitch.py` places one via
per fragment first, at the roomiest point inside it, then fills in a 5mm lattice. The counts, all
measured on this board:

| stitching vias | fragments left unjoined |
|---|---|
| 0 | 14 |
| 8, one per fragment | 8 |
| 22, blanket 5mm grid | 12 |
| 44, one per fragment plus a 5mm grid | 8 |
| 82, blanket 2mm grid | 9 |

Targeted placement is what does the work; a blanket grid mostly drops vias into fragments that were
already stitched and misses the small ones. The 8 that remain are slivers too small to take a 0.6mm
via with clearance — each is tied to GND through its own pads, so they are harmless. The grid pass
is there for return-path continuity under the 2.4GHz module, not for connectivity, and it is free:
0.3mm is the same drill tool the board already uses.

`stitch.py` has no undo — pcbnew's `Remove()` on a via does not survive a `Save`, so running it
twice just accumulates vias. To re-stitch, strip every segment, via and arc from the `.kicad_pcb`,
run `route.py`, then run `stitch.py`.

## The unverified assumptions, current status

Spec §4 lists five. Three are now resolved, all from primary sources.

1. **Bit order within a graphics byte** — **resolved.** Bit 0 is the top pin, bit 7 the bottom,
   read off the ImageWriter II Technical Reference p. 107 and independently confirmed against
   Ghostscript's `gdevadmp.c`. A reviewer re-checked it against the scan. Implemented in
   `imagewriter.c`.
2. **Density escape codes** — **resolved.** `ESC p` is 144 dpi at 1152 dots; `ESC P` is 160 dpi.
   Table 8-2, p. 106, cross-checked against `gdevadmp.c` and re-verified against the scan by a
   reviewer. The full table is in `SPEC.md` §4.
3. **Differential polarity** — **resolved by research**, and it found the spec error described above.
   Not yet confirmed against hardware.
4. **HSKo electrical levels** — open. Scope it before trusting the MAX3232 receiver assumption. If the
   swing turns out modest, a divider and clamp into a GPIO could delete U3 entirely.
5. **URF page header field offsets** — **resolved.** Taken from `cups/raster-stream.c` in
   OpenPrinting/cups, not from memory, and implemented in `urf_decode.c` with the offsets cited
   in comments. Still unconfirmed against a real iOS capture — see below.

## Traps that cost real time

These all bit at least once. Several bit twice.

**KiCad silently reverts file edits.** If a KiCad session was open before an external edit, saving
from KiCad writes its stale copy back. This ate the board outline once and the design rules twice.
Always close KiCad before editing files, and re-verify afterwards.

**Two konnect MCP tools corrupt this project.** `set_design_rules` writes constraints into the
`.kicad_pcb` setup block, but KiCad 10 reads them from `.kicad_pro` — the result is duplicate keys
and an unloadable board. `set_board_size` appends an outline instead of replacing it, leaving two
overlapping rectangles on Edge.Cuts. Edit `airwriter.kicad_pro` directly for rules; rebuild the
outline by hand.

**Freerouting hangs on any DSN KiCad exports, and the cause is fractional coordinates.** KiCad
writes coordinates with decimals while declaring `(resolution um 10)`; Specctra wants whole units.
Freerouting turns them into exact rationals, the numerators explode in `Line.intersection`, and it
spins at 200% CPU inside `BasicBoard.normalize_traces` forever, still printing "Opening ...".
Rounding every number in the `.dsn` to an integer drops the load from never to 0.9 seconds.
`route.py` does this for you. This is also why `check_freerouting` appeared to wedge the MCP
server — same root cause, not a tooling quirk. Recovery is `pkill -f freerouting.jar`.

**Freerouting also blows up re-reading its own previous output.** Same arithmetic, so `route.py`
drops the existing wiring and routes the whole board from scratch each time; a full route takes
about 15 seconds. `--keep-wiring` routes only what is missing but will hang on a board Freerouting
has already routed.

**Freerouting never repours zones.** DRC without `--refill-zones` showed 278 violations that dropped
to 3 once refilled. Always pass it.

**`add_zone` created the ground pour on net `"GND"` while every pad is on `"/GND"`.** KiCad uses the
hierarchical name with a leading slash. The pour was floating copper for its entire life and DRC
reported it as 28 isolated-copper islands rather than as a disconnected zone. Check zone nets by
string, not by eye.

**Footprint zones store absolute coordinates.** Moving a footprint by editing `(at ...)` in a script
moves the pads but strands any keepout polygon. The WROOM-1's antenna keepout sat 46mm off the board
until corrected, so DRC had never been enforcing it.

**konnect's placement tools resolve `lib_id` only against KiCad's install directory**, ignoring both
project and global library tables. The project libraries are symlinked into the app bundle as a
workaround. Those symlinks vanish on a KiCad upgrade.

**A generated `.kicad_mod` contained unescaped double quotes** inside its `(descr ...)` string, which
makes the s-expression malformed. KiCad failed to load the footprint and dropped the part from the
board without complaint. Balance-check any generated library file.

**KiCad 10 removed Specctra DSN export and SES import from `kicad-cli`** — but not from the
`pcbnew` Python module, which still has `ExportSpecctraDSN` and `ImportSpecctraSES`. Routing is
therefore fully automatable; run `hardware/airwriter/route.py` with the interpreter inside the app
bundle and it does export, round, route, import, and DRC in one go, no GUI:

```
/Applications/KiCad/KiCad.app/Contents/Frameworks/Python.framework/Versions/3.9/bin/python3 \
    hardware/airwriter/route.py --passes 30
```

**A footprint's rotation does not rotate its pad shapes.** Each pad carries its own angle in
`(at x y ANGLE)`, and KiCad writes the footprint rotation into it. A generated footprint that omits
the angle places its pads correctly but leaves every pad shape unrotated — on J6 that turned a
1mm-pitch connector into four shorted pads. Set the angle on every pad of a rotated footprint.

## Custom library

`hardware/airwriter/lib/` holds three things KiCad does not ship.

- **`MiniDIN-8_Kycon_KMDGX-8S-BS_RightAngle`** — KiCad has no mini-DIN footprint at all. Geometry
  read off Kycon engineering drawing KMDGX-xS-BS rev C6 (ECO#18-013), 8-position "Recommended PCB
  Layout (Top View)", cross-checked against three dimension chains. **Pin 7 sits at +0.90mm from the
  centreline while pin 2 directly above it sits at +1.30mm** — the lower row is deliberately
  asymmetric and easy to get wrong by eye. Kycon's own STEP model, fetched later, has the same
  asymmetry, which is independent confirmation the footprint is right.
- **`lib/airwriter.3dshapes/MiniDIN-8_Kycon_KMDGX-8S-BS_RightAngle.step`** — Kycon's official model,
  from `kycon.com/website/Extranet/3D/KMDGX-8S-BS_rC6.STEP`. That Extranet path is not linked from
  the product page; it was found through a search index. Attached to the footprint at
  `rotate -90 0 180`, offset zero. Verified by rendering the bottom of the board: all eight pins and
  all three posts land centred in their holes. Without it the board's STEP export had two holes
  where the ports should be.
- **`lib/airwriter.3dshapes/USB_C_Receptacle_HRO_TYPE-C-31-M-12.step`** — KiCad 10 ships this
  footprint but not its model, and the stock footprint points at a `${KICAD10_3DMODEL_DIR}` path
  that does not exist, so J1 vanished from the 3D export without any warning. Upstream
  `kicad-packages3D` does not have it either; KiCad issue #22139 tracks it. Taken from
  NabuCasa/yellow-3dshapes, which processed the vendor model through KiCad StepUp. That repo
  carries no licence file — check provenance before publishing the hardware.
  The model's origin is a corner, not the footprint centre, hence `rotate -90 0 0` with
  `offset -4.47 -3.65 0`. Derived by matching the four shield tabs (8.64mm apart, rows 4.18mm
  apart) and the two 0.65mm legs (5.78mm apart) to the pads, then confirmed by render: all six
  through-board features seat in their holes, and the asymmetric shield-tab pattern rules out a
  180-degree error.

**J1's model lives on the board, not in a library.** The footprint comes from KiCad's stock
`Connector_USB`, so re-importing it from the library will restore the dead `${KICAD10_3DMODEL_DIR}`
path and J1 will silently disappear from the STEP export again. Same class of trap as U1's vias.

**D4–D6 borrow a stand-in 3D model.** `LED_SK6812MINI_PLCC4_3.5x3.5mm_P1.75mm.step` does not exist
in KiCad 10, so the logo LEDs pointed at nothing and rendered as bare pads — which read as
"not populated" and was only ever a picture problem. They now reference
`LED_WS2812B-Mini_PLCC4_3.5x3.5mm.step`, the same 3.5mm PLCC4 package, so the preview and STEP
export show the part. (D1/D2's older missing-model note is obsolete: as SK6812MINI-E they use a
model KiCad does ship.)

Before trusting a STEP export, check every model path resolves — a missing one is silent:

```
python3 - <<'EOF'
import re, os
b = open('hardware/airwriter/airwriter.kicad_pcb').read()
base = '/Applications/KiCad/KiCad.app/Contents/SharedSupport/3dmodels'
proj = 'hardware/airwriter'
for p in re.findall(r'\(model "([^"]+)"', b):
    real = p.replace('${KICAD10_3DMODEL_DIR}', base).replace('${KIPRJMOD}', proj)
    if not os.path.exists(real): print('MISSING', p)
EOF
```
- **`Mini-DIN-8_Shielded`** — the stock symbol has no shield pin, so there is no way to net the shell.
  Pin 9 is SHIELD, tied to all three mounting posts in the footprint.
- **`SN65HVD1473DGSR`** — reused with credit from FeralFirmware/TailTalk. No longer placed on the
  board after the transceiver swap, but kept because it is the part TashTalkUSB ships.

## Sourcing

**Fab is PCBWay, parts are China-sourced.** Price against LCSC, not Digi-Key — the gap is large
enough to change design decisions. MAX3232 is $9.98 at Mouser and $0.91 at LCSC. MAX3490 is $6.61 at
Digi-Key and $0.687 at LCSC.

Order the board as **52 x 78mm, 2 layer, 1.6mm FR4, 1oz copper, matte black soldermask, ENIG**.

**ENIG is not vanity here.** J1 is a USB-C receptacle at 0.5mm pitch, and HASL is molten solder
blown flat with an air knife — the surface it leaves is uneven enough that plenty of fabs will not
guarantee it below 0.5mm pitch. ENIG is flat. The board wants it regardless of how the back looks.
It also stops exposed copper tarnishing, which matters now that the back is a display surface and
may carry capacitive touch pads. Reckon on about **+$25** on the order — roughly one pair of
mini-DINs, against a build with $13 of parts per board.

**The finish and mask colour are recorded in the board's stackup**, so `kicad-cli pcb render` and
the STEP export show what will actually arrive. Gerbers carry no colour; the mask is an order
option, and the stackup is the thing that stops it being forgotten at the cart.

**The artwork palette is now four materials**, which is worth designing to rather than discovering:

| | |
|---|---|
| Matte black | soldermask, the ground |
| White | silkscreen |
| Gold | exposed copper — a mask opening over the pour |
| Translucent tan | bare FR4, where copper and mask are both absent. This is the one that glows |

Note that `kicad-cli pcb render` tints bare FR4 by the mask colour, so the logo window looks nearly
black in renders. It will not look like that on the board — tan fibreglass against matte black is a
hard contrast, and it is the whole point of the window.

**Protect the 0.30mm drill floor.** PCBWay's standard minimum is 0.30mm and 0.20mm is an upgraded
spec — a documented case went from $5 to $65 for ten boards. KiCad's stock ESP32-S3-WROOM-1 footprint
puts twelve 0.20mm thermal vias in its ground pad, and they were enlarged to 0.30mm **in the board
file, not the library**. Re-importing U1's footprint will silently put them back. That is also why U1
shows a library mismatch in DRC. Before every order:

```
kicad-cli pcb export drill --output fab/ --format excellon airwriter.kicad_pcb
grep -E '^T[0-9]+C' fab/airwriter.drl        # nothing below 0.300
```

**Protect the 0.15mm clearance floor too — this one was being violated.** PCBWay's own guidance is
"design trace width/spacing above 6mil (0.15mm) to save cost", and the board's only net class had
`clearance` set to **0.12mm**. The autorouter happily used it: raising the rule to 0.15 and
re-running DRC turned up **98 clearance violations**, every one a pair of traces sitting between
0.12 and 0.15mm apart. That is the same trap as the drill floor — a rule quietly permitting an
upgraded spec — and it would have been invisible until the quote came back.

The rule is 0.15mm now and the board re-routes clean against it. Check it the same way:

```
kicad-cli pcb drc --format json -o /tmp/d.json airwriter.kicad_pcb   # no clearance violations
```

**MODE is a capacitive touch pad in exposed gold on the back face.** SW2 is no longer a switch: it
is `airwriter:TouchPad_Core_D9mm`.

**The electrode is a solid 9.2mm copper disc and the pattern lives in the soldermask.** That is the
whole trick. Self-capacitance wants unbroken area, so cutting the copper into a comb or a halftone
would have cost sensitivity for looks. Instead the copper stays whole and the mask carries the
image: a solid Ø4mm gold core whose edge dissolves outward into the cover's mist, drawn in the
mist's own cells — **0.72mm cells on the grain's 0.91mm pitch**, not the 0.455mm logo grid an
earlier revision used. Wells judged that revision a mismatch: its fine dense dither read as a
clump, not as the mist condensing. All openings stay on the electrode; the touch guard's bare
annulus (4.6–6.1mm radius, no pour, so no gold) reads as a quiet dark ring around the control.

**SW1 is not a control and no longer pretends to be one.** It moved to the component side as
`airwriter:BootPads_Service` — two plain bare rectangles in a silk box labelled BOOT, at the same
(139.7, 111.3) so the existing F.Cu track lands on pad 1 unchanged. The old back-face bullseye and
its 0.3mm through-hole are gone; the visible face now carries exactly one control, and mist cells
backfill where the bullseye sat.

MODE's pattern comes out of `lib/gen_touchpads.py`, so core size, fade curve and cell size are
numbers in one place. Re-run it and re-place the footprint to change the look.

- **MODE moved from IO38 to IO14.** The S3 only does capacitive touch on IO1–IO14. The old pin is
  released and capped.
- **R20, 1k, sits in series** between the pad and the GPIO. Exposed metal wired straight to a pin is
  an ESD path; the S3 is rated about 2kV and a person can carry far more.
- **C19 is DNP, not deleted.** 100nF across a touch pin would swamp a sense capacitance measured in
  picofarads. Leaving the pads means fitting one part reverts MODE to a mechanical switch if touch
  disappoints.

**On the research, because the eurorack precedent is split.** Make Noise's René uses *resistive*
touch — skin completing a circuit — and is known for not reading reliably on dry hands. That is the
failure mode worth avoiding, and it is a different technology from what Synthux's Simple Touch does,
which is capacitive via an MPR121. Capacitive does not care about skin conductivity. An MPR121 is a
12-channel part and would be silly for one button when the S3 has the peripheral built in.

**Bare gold is correct here, and an earlier note in this file was wrong to say otherwise.**
Soldermask is about 25 microns and barely affects sensitivity, so either works — but exposed ENIG is
what the synth world does, it is more sensitive, and it is the entire point of the finish.

**The logo is lit by a light box, not by LEDs behind the letters.** Reverse-mount LEDs make a local
pool of light where they sit — they do not spread sideways — so four of them behind a 33mm wordmark
would give four bright patches and dim gaps. Instead **D4, D5 and D6 are top-firing SK6812MINI**
that shine *down* into the printed housing. A white cavity floor bounces the light back up through
the window, arriving already diffused. That is what a wide even glow wants, and it needs no special
parts.

They sit outside the logo keepout — at (105.5, 93), (146.5, 93) and (115, 86.3) — because anything
under the window would block the light coming back up. Position barely matters for a light box; the
cavity floods.

**All six pixels are one chain on one wire.** U1 IO48 to D1, D1 to D2, then D2's DOUT — which was
unconnected — carries on to D4, D5, D6 over `LOGO_CH1/2/3`. No new GPIO, and the logo is addressable:
any colour, any animation, same protocol. Firmware needs one constant changed from 2 pixels to 6.

**SK6812 rather than WS2812B for a real reason.** WS2812B wants 3.5V minimum and this rail is 3.3V —
the original D1/D2 were out of spec and nobody had noticed. Every SK6812 variant on the board is
specified down to 3.3V.

**The indicators fire up through the board, and BOOT is two pads.** Every component sits on the
face that becomes the underside, so a normal SMD LED points away from the viewer. All three
indicators are reverse-mount now — they emit *toward* the board, and instead of the hole their
footprints expect, they shine into a bare-FR4 window:

| | was | is | window |
|---|---|---|---|
| D1, D2 | WS2812B-2020 | **SK6812MINI-E**, LCSC `C5149201`, $0.045 | 3.4 x 3.0mm |
| D3 | 0603 green | **LED_1206 ReverseMount** | 1.6 x 2.2mm |
| SW1 | Omron B3U-1000P | **two bare pads**, `SolderJumper-2_P1.3mm_Open` | — |

**The pin numbering changed but the wiring did not.** WS2812B-2020 numbers its pins DOUT-VSS-DIN-VDD
and SK6812 numbers them VSS-DIN-VDD-DOUT — but both KiCad symbols put the same *functions* at the
same coordinates, so swapping `lib_id` moved every net correctly and no label had to be touched.
Verified against the exported netlist. Firmware is unaffected: SK6812 speaks the same one-wire
protocol at the same 800kbit/s.

**Each window is the logo recipe at small scale** — a `LED WINDOW <ref>` keepout barring tracks,
vias and pour on both layers, with a mask opening 0.4mm smaller on both faces so no exposed copper
sits at the edge.

**BOOT is gone as a switch.** Now the USB-C carries data, `esptool` can drive the S3 into download
mode over the wire; the two pads are insurance you bridge with tweezers. It also removes the last
part that would have needed a hole in the visible face. MODE (SW2) stays a real switch and still
faces into the housing — that one is unresolved.

**The strip re-spaced because the new LED is 7.3mm wide against the WS2812B's 3.0.** It now runs
from x 108.9 to 136.3, fitting between mounting hole H3 and the power column at x 139.8.

**D1, D2 and D3 now show `lib_footprint_mismatch` in DRC, deliberately.** Their library footprints
carry a board cutout — Edge.Cuts segments on the SK6812MINI-E, an NPTH slot on the 1206 — because
they are drawn for shining through a hole. Those were removed so the fibreglass stays. **Re-importing
any of these three footprints from the library will silently punch the holes back in**, exactly the
trap U1's 0.30mm vias already set.

**Read this before touching the routing: `unconnected_items` is not all pour fragments.**
`kicad-cli pcb drc` reports pour islands and genuinely unrouted signals in the same list, and
`route.py` prints `Found N unconnected pads` for both. For several commits this board carried
**12 to 16 real net breaks** — UART_TX, UART_RX, UART_CTS, LED_DATA, USB_DP/DM among them — while
DRC's *violations* count stayed at four and everything looked fine. Split them before believing a
board is routed:

```
real = [u for u in drc['unconnected_items']
        if ' <-> '.join(i['description'] for i in u['items']).count('Zone') != 2]
```

The router also says so plainly if you read the whole line: `completed ... (11 unrouted and 10
violations)`. A finished board has no `unrouted` clause at all.

**CORRECTION (2026-08-21, W-573): a `Zone↔Zone` unconnected item is NOT automatically benign.**
The rule above splits them out as harmless "pour islands" — that is only true when the island
carries no pad. KiCad's ratsnest calls a pad *connected* the moment it touches ANY GND copper,
even a stranded island that reaches nothing else — so a fragmented GND pour can strand real pads
(their only ground is an island wired to nothing) while DRC shows just a few benign-looking
`Zone↔Zone` lines. This board had exactly that: **44 GND fill islands → 18 disconnected
components; 22 GND pads stranded off the main plane**, including decoupling caps and the U5 ESD
ground (J1's USB-C GND turned out fine — on the main plane). Each `Zone↔Zone` ratsnest edge is a
real GND disconnection: N edges ⇒ N+1 GND clusters. **Verify GND properly**: compute connected
components of GND copper (zone-fill islands + pads + tracks + vias, overlap = edge, vias bridge
layers) and confirm every GND pad shares the main-plane component — `tools_pcb/r12_gnd.py` does
this and its component count matches KiCad's ratsnest exactly. Fix = one GND stitch via per
stranded F.Cu fragment down to the main B.Cu flood (`r12_fix.py`); fragments over a B.Cu window /
back-signal routing get an F.Cu GND track to a nearby over-main-B via spot (`r12_bridge.py`). The
back flood itself is not one solid island — dense B.Cu (LocalTalk) routing splits it, so some F.Cu
fragments sit over non-GND back copper and are genuinely hard to reach.

**What actually broke it: the wordmark reserve was 49mm wide on a 52mm board.** Barring tracks on
*both* layers across a band that wide leaves 0.5mm at one edge and 2.5mm at the other — the router
simply could not cross. The fix is two-part and both halves matter:

- **Three separate reserve blocks**, split at natural gaps in the wordmark (x 115.4–117.4 and
  138.4–140.4), so traces have channels through the middle rather than only round the ends.
- **B.Cu only.** The visible face has to stay clear; the component side does not. Letting F.Cu route
  under the wordmark is what took it from 9 unrouted to fully routed. The cost is that a wordmark
  cell with a trace beneath it stays dark instead of glowing — which in a dither is invisible.
  Retention landed at 198 of 293 lit cells.

**Every rule area exports to Freerouting as a hard keepout**, whatever its tracks/vias settings say.
A pour-only guard still blocks the router. That is why `artwork.py` lays down the artwork *and* the
touch guard, and why both go on after routing.

**`SW1` was sitting on top of `D2`.** The BOOT pad is on B.Cu, so the courtyard checks skipped it —
but its centre via is through-hole, and it landed 1.5mm from a LED. Back-side controls need checking
against *front*-side footprints, which is not automatic.

**`C19`'s ground pad is starved by the touch guard.** It is DNP so nothing is fitted and nothing
floats today, but if MODE ever reverts to a mechanical switch, delete the `TOUCH GUARD` rule area at
the same time or that pad has no ground.

**The back carries a dithered cover artwork, imported from `~/Downloads/kicad/`.** Eight SVGs at
100 x 150mm — exactly 1.923x the board, same aspect — scaled by **0.52** and **mirrored in X** onto
the back layers. Smallest cell lands at 0.455mm, silk at 0.364mm; both clear the fab floor.

Three departures from the artwork's own README, and they matter:

- **It goes on B.\*, not F.\*.** The back is the display face, so everything mirrors.
- **The `*_gold_copper` layer is not imported at all.** The back is already a solid ground pour, so
  copper is everywhere; 503 separate copper islands would have shorted across traces. **The mask
  openings alone give the gold**, because the pour underneath is the copper. Same trick as the
  touch pad and the logo.
- **The artwork's wordmark replaced the old rectangular logo window.** They landed in the same
  place, and a dithered wordmark beats a plain slab.

**Order of operations is the whole game here, and getting it wrong cost several passes.** Freerouting
only sees keepouts that are *separate single-outline zones* — a zone holding 320 cell outlines
exported to the DSN as **one** keepout, so the router drove straight through the artwork. What works:

1. Reserve the wordmark with **one rectangular keepout** (`WORDMARK RESERVE`, 49 x 8.8mm) and give
   each LED window **its own zone**. Sixteen keepouts reach the router and it honours them.
2. Route, stitch, then **prune stitching vias that landed in a via-barred area** — `stitch.py` reads
   the pour, not the rule areas, so it drops vias into reserves.
3. Only then filter the artwork cells against the finished copper and emit them, as rule areas that
   bar **copper pour only**. They constrain nothing, so no re-route is needed.

Cell retention after filtering: wordmark FR4 **282/293**, grain FR4 23/27, gold 625/625 and 162/198.

**The TashTalk credit line is gone from B.Silkscreen.** It ran the width of the board and was eating
a band of the grain — silk over exposed copper does not adhere, so every cell beneath it had to give
way. The attribution still stands in this file, in `SPEC.md` and in the licence; it does not need to
be on the show face.

**`artwork.py` regenerates the whole thing** and is the only supported way to re-import. It reads the
SVGs, filters every cell against the copper actually on the board, and emits rule areas that bar
copper pour only — so it never forces a re-route. Run it last:

```
strip wiring  ->  route.py  ->  stitch.py  ->  prune vias in keepouts  ->  artwork.py  ->  refill
```

**BOOT is a gold bullseye on the back, not a pair of rectangles on the hidden side.** `SW1` was
`SolderJumper-2_Open` sitting on the component face, where you would have had to open the case to
reach it — useless for something that exists as insurance. It is now
`airwriter:BootPads_Ring_D5mm`: a 2.2mm disc inside a 0.9mm-wide ring, 0.5mm apart, both exposed
ENIG, on the visible face beside the MODE pad. Bridge the gap with tweezers to force download mode.
Ø5mm against MODE's Ø9.2mm, so it reads as the secondary control it is.

**The logo is a backlit window, not printed artwork.** The wordmark is 32.3 x 7.1mm at x 110.07 to
142.33, y 90.07 to 97.13, inside the blank band. Three things make it glow:

- **A keepout rule area, `LOGO WINDOW`, on F.Cu and B.Cu** covering the wordmark plus 0.6mm —
  33.5 x 8.3mm. Tracks, vias and copper pour are all barred, so nothing crosses the light path.
  It has to exist *before* routing: Freerouting honours keepouts and funnels the crossings into the
  9.5mm channel on the left and the 9.1mm on the right. Verified clear afterwards — zero tracks,
  vias or pour samples inside the box.
- **Mask openings on B.Mask and F.Mask**, not silkscreen. The logo arrived on B.Silkscreen, which is
  white ink and would have covered the very window it was meant to be. Light crosses the whole
  stack, so the mask opens on *both* faces; leaving the front closed would put green mask in the
  path and dim it.
- **No copper on either layer** in the window, from the keepout above.

The result in daylight is a copper-free panel that reads as a slightly different green, with bare
FR4 letterforms inside it. The original silkscreen version is in git if the printed look is wanted
instead — the two are mutually exclusive in the same place.

**The board is 1.6mm and that is a brightness ceiling.** Thinner FR4 passes more light, but the
board is also the case lid and carries two through-hole mini-DINs that take plug and unplug force,
so it stays at 1.6mm.

**Still unsolved: the LEDs point the wrong way.** Components sit on F.Cu, which becomes the
underside, so a normal SMD LED there fires away from the board. Backlighting the window needs one
of: side-firing LEDs at the window edge, a white reflective cavity in the housing acting as a light
box, or reverse-mount LEDs firing through a board cutout. The third suits point indicators — it is
what would let the status LEDs drop their light pipes — but not a glowing logo.

**The USB-C is a USB port now, not a power inlet.** Spec §2.5 originally read "USB-C is power
only", with flashing and console going to a 4-pin 1.27mm header instead. That left a USB connector
on the front panel with its data pins tied off, a debug header nobody can reach without a custom
cable, and a two-line USB ESD part with four of its six pins no-connected. D+ and D- now run from
J1 through U5 to the S3's native USB peripheral:

| net | pads |
|---|---|
| `/USB_DP` | J1.A6, J1.B6, U5.1, U5.6, U1.14, J3.3 |
| `/USB_DM` | J1.A7, J1.B7, U5.3, U5.4, U1.13, J3.2 |

Both USB-C flip orientations are tied together (A6+B6, A7+B7), which is what a USB 2.0-only device
does. U5's I/O1 pins are 1 and 6 and its I/O2 pins are 3 and 4 — each pair is the same node inside
the package, so both carry one net and the layout routes through the part.

**U5 moved to sit 2-4mm from J1's data pins**, swapping places with the CC pulldowns. Trace
inductance ahead of a clamp is what lets a strike through, so the ESD part goes closest to the
connector; R1 and R2 only carry a DC level and can sit further back. It was 16.7mm away before.

J3 stays, tapping the same two nets, as a bring-up fallback for the first spin. The silkscreen on
J1 now reads `USB-C` rather than `5V IN`, which was no longer true.

Only the ESP32's native USB is on this — full speed, 12 Mbps — so the ~60mm run from J1 to U1
needs no length matching and no impedance control.

**The regulator sits next to the ESP32, not next to the USB-C inlet.** Freerouting treats a power
net like any other signal — hop to the nearest unconnected pad, repeat — so the 3.3V rail came out as
one long daisy chain. With U4 down by J1, the loads measured like this from the regulator's output
pin, on a board 78mm tall:

| | before | after moving U4 |
|---|---|---|
| U2, printer transceiver | 145mm, 14 pads | **34mm, 2 pads** |
| U3, RS-232 | 147mm, 14 pads | **39mm, 3 pads** |
| D1 / D2, status LEDs | 165-170mm, 15-16 pads | **44mm, 2 pads** |
| U1, the ESP32 | 113mm, 10 pads | 90mm, 6 pads |

U4 moved up beside U1 with C1, C2 and C3; U5 and the CC pulldowns stayed at J1, because ESD
clamping and USB-C configuration belong at the connector. VBUS now makes the long diagonal instead
of 3.3V, which is the right way round — 5V into an LDO with 300mV of dropout does not care about
30mV of trace drop, and the 3.3V rail does.

**U1 is the one that did not really improve, and the reason is geometry.** Its only power pin is
module pad 2 at (107.25, 62.76) — the top-left corner — boxed in by H1, J3 and the board edge, with
the module body filling everything to its right. There is no lane the router will take, so it goes
the long way round regardless of where U4 sits. Fixing that one properly means either pouring 3.3V
as a zone on F.Cu, or moving H1 and J3 to open a channel up the left edge. Neither is done.

At 90mm of 0.5mm trace the path is about 88 milliohms, so a 500mA WiFi burst sags the rail ~44mV
against a 3.0V floor. It works with margin, and U1's own bypass caps sit beside it in the chain
rather than at the far end, so the local decoupling is genuinely local.

**There is a Power net class now: 0.5mm track for `/+3V3`, `/VBUS` and `/GND`, against 0.2mm for
signals.** Everything used to be 0.2mm, including the 3.3V rail feeding a module whose WiFi
transmit current peaks near 500mA. 0.2mm in 1oz copper carries that, but with no margin and a
10-15°C rise; 0.5mm makes it a non-question. Signals stay at 0.2mm, which is 7.9 mil and
comfortably inside PCBWay's standard.

**Every named part sources. Prices and stock, LCSC unless noted, verified 2026-08-05:**

| Ref | Part | Price | Stock |
|---|---|---|---|
| U1 | ESP32-S3-WROOM-1-N8R2 | $3.75 | 9,842 |
| U2 | MAX3490ESA+T (printer port) | $0.687 @10 | 3,788 |
| U3 | MAX3232ESE+T | $0.91 | 7,956 for the plain +, the +T line is its own SKU |
| U4 | AP7361C-33E-13 | $0.52 @10, $0.40 @100 | 1,526 |
| U5 | USBLC6-2SC6 | $0.099 ST, $0.018 UMW clone | good |
| U6, U8 | SP3485EN-L/TR | $0.21 each | 37,970 |
| U7 | PIC12F1840-I/P (PDIP-8) | ~$1.50 | Mouser or Digi-Key, **not LCSC** — LCSC stocks only the SOIC |
| U7 socket | CONNFLY DS1009-08AT1NX-0A2, LCSC `C72124` | $0.024 | good |
| D1, D2 | WS2812B-2020 | $0.092 @10 | 35,115 |
| J1 | TYPE-C-31-M-12 | $0.096 | good |
| J2, J4 | Kycon KMDGX-8S-BS mini-DIN | ~$2.50 (Mouser) | 1,032 — Digi-Key is the backordered one |
| J6 | SM04B-SRSS-TB(LF)(SN) | $0.13 | 60,960 |
| SW1, SW2 | Omron B3U-1000P | $0.128 | 112,590 |

That is about **$13 of parts per board**, and $5 of it is the two mini-DINs. Passives are generic
0603 and 0805 with no MPN in the BOM — buy whatever is cheap and in stock.

**J3 is the only awkward line.** LCSC does not stock 1.27mm single-row male headers as 1x4; it
stocks strips. Buy `C28952` (1x10P, $0.025) or `C3408` (1x50P, $0.123) and cut. It does not have to
be fitted at all — it is bring-up only.

**U2 and U3 carry the `+T` tape MPNs, not the plain `+`.** Same silicon; the plain MAX3232ESE+ is
$1.70 at LCSC against $0.91 for the +T. Cut tape is what a small order gets anyway.

**The WS2812B-2020 footprint is right despite its name.** `LED_WS2812B-2020_PLCC4_2.0x2.0mm` draws a
2.2 x 2.0mm body, which is the real package — LCSC lists it as 2.2x2mm too. The pin order matches the
chain as wired: 1 DOUT, 2 GND, 3 DIN, 4 VDD, with D1's DOUT feeding D2's DIN.

**The mini-DIN is settled: Kycon KMDGX-8S-BS, bought from Mouser.** The part was only ever a
problem because Digi-Key is backordered on it; Mouser holds 1,032 and ships immediately. It is the
exact part the footprint was drawn from and cross-checked against Kycon's own STEP model, so the
geometry risk is zero. Buy six — you need two, it is a through-hole part you can damage, and the
spares cost about $2.50 each.

**Solder them by hand rather than having PCBWay fit them.** PCBWay keeps no parts library and buys
every component per order, so a through-hole connector becomes a manually quoted line item with a
5-10 day sourcing delay. Twenty-two joints across two large, well-anchored parts is a few minutes
with an iron, it keeps the connector off the critical path of the board order, and a wrong part
then costs $5 instead of a scrapped assembly run.

The alternatives were checked and rejected, so nobody needs to re-do this:

- **Kycon KMDGX-8S-AS** — in stock at Digi-Key, but it is a "Board Guide, Board Lock" push-pull
  variant and Digi-Key does not list it as a substitute for the -BS. Different mounting scheme.
- **Adam Tech MDJ-008-FS** — a genuine drop-in; its drawing matches all eight signal positions and
  the ±6.75 posts. Out of stock at Digi-Key. Keep as the fallback if Kycon ever dries up.
- **Same Sky (ex-CUI) MD-80SM** — signal pads match, mounting does not. The shield tabs are
  rectangular slots and there is a 2.5 x 1.0mm earth blade where the footprint has a ø2.3 round
  hole; a 2.5mm blade has a 2.69mm diagonal and will not fit. MD-80S is worse — unshielded, no
  side posts. The spec's old suggestion of MD-80S was wrong.
- **HOOYA / Legion DIN-802 (LCSC C20611694 / C41409494)** — cheapest by far and 780 in stock at
  HOOYA, but the pads sit 0.15-0.20mm out almost everywhere and the centre feature is a 1.5 x 3.0
  oval slot rather than a round hole. Not usable without redrawing the footprint.

**The pin-numbering discrepancy is resolved: our footprint is correct and the DIN-802 one is
mirrored.** Derived from the socket-face view (the mirror of the plug convention) combined with
KiCad's +Y-downward frame, it predicts all eight pad positions including the 0.90/1.30 asymmetry.
The DIN-802 footprint transposes pins 1 and 2, puts 3 on the wrong side, and places centre pin 4
in the front row with outer pin 5 behind it, which is backwards for a right-angle fan-out.

Worth knowing: **the 0.90mm asymmetry is an industry pattern, not a Kycon quirk.** Adam Tech and
Same Sky both show 0.90 against 1.30 on their 7- and 8-pin layouts and neither shows it on their
6-pin ones. It is the extra lower-row pin offset to clear the one above it.

Note that full-duplex RS-422 transceivers are a niche part and keep going out of stock on LCSC —
ISL3177EIBZ and MAX3491ESD+ were both zero. Half-duplex RS-485 is commodity. That is why the
LocalTalk transceiver is two half-duplex parts rather than one full-duplex one.

## Projects this builds on

**TashTalk** (lampmerchant) — a LocalTalk data-link engine that fits in a PIC12F1840, bit-banging
SDLC and FM0 framing and presenting it over a 1 Mbps UART with CTS flow control. The board carries
one running his firmware. It needs flashing, which now happens in a chip programmer before the PIC
goes into its socket — see the socket note above.

**TailTalk / TashTalkUSB** (FeralFirmware) — the LocalTalk section's wiring was copied from
TashTalkUSB's KiCad netlist node for node rather than derived from prose. TailTalk itself is a
userspace AppleTalk toolkit for desktop OSes and is the best reference for the protocol work ahead.

**AirTalk** — LocalTalk over WiFi dongles, ESP32-based. Relevant if mode 2 or a WiFi tunnel is ever
built; you would need to match its wire protocol to interoperate.

**ImageScribbler** (polpo) — a 1:1 clone of Apple's ImageWriter II/LQ LocalTalk Option Card, running
Apple's original ROM on a 65C02 and Z8530. CC0, and JCM-1 sells it assembled. It does what mode 3
does, better and with zero protocol risk, but needs vintage silicon and goes inside the printer.
**Its ROM image (341-0034-B) is publicly archived and is the authoritative answer to what PAP
actually delivers to an ImageWriter** — the question gating all the AppleTalk work. Disassembling it
serves mode 2 and mode 3 equally.

## What to do next

**As of 2026-08-09, the board waits on the swatch coupon.** Ordered from PCBWay that day; expect
it 1–2 weeks out. When it arrives, Wells judges the cover language in hand and picks the MODE
pattern — A, B or C, all three are on the coupon; the main board carries C. Then regenerate the
main fab package (`hardware/airwriter/fab/` predates the SW1/SW2 rework and the D4–D6 model fix)
and order rev A. Firmware is not blocked meanwhile: M5 needs no board.

Also from that session: the webui key clicks were tuned by ear (latch and strike now 4500Hz,
values in the `SND` table in `tools/webui/app.js`; the debug tuner that did it is retired but
one commit away), and the sections above on the touch pad, BOOT pads and LED models were
rewritten to match the current board.

Older next-steps below are kept for context; several are already done (mini-DIN settled,
ESP-IDF installed, M2 and M4 done).

**The iPhone capture is done.** `fixtures/ios-20260804-232652.urf` came off a real phone through
`tools/ippcap`, and iOS honoured `RS72-144` — it rendered at exactly 144 dpi rather than sending
300, so no downsample fallback is needed and `RS300` need not be advertised. The capture is in
`make regress` and `test/test_ios_capture.c` asserts its header fields, so assumption 5 now has a
real-world check. 249 checks green.

**M4 is done and was verified from a real iPhone.** The phone found the printer in its print
sheet and sent a job, which arrived complete. CUPS `ipptool` passes `get-printer-attributes`
conformance, and a Print-Job carrying the captured fixture moved all 143,470 bytes in 240ms.

**M5 is next and needs no board.** Swap the byte-counting sink in `main.c` for the real chain —
`urf_decode` → `resample` → `halftone` → `imagewriter` — and write the printer stream to flash
or the console instead of a UART. That makes the whole pipeline verifiable on the devkit, and
what remains for the real board is only `serial_out`: the UART, the RS-422 levels and the DTR
handshake. `printer_state` and the M1/M3 work are the parts genuinely gated on boards, which
are not ordered.

**Printer scope is settled.** See `docs/superpowers/specs/2026-08-04-printer-support-scope-design.md`.
Short version: v1 is the ImageWriter II over serial and nothing else. StyleWriter and
ImageWriter LQ are cut on consumables. LocalTalk is built second, because its job goes out
through the serial port and that has to work first. Two of the five unverified assumptions
in `SPEC.md` §4 are now resolved from Apple's technical reference.

**Settle the mini-DIN.** It blocks ordering and you need two.

**Then M2, which needs no hardware and no ESP-IDF.** The spec calls `tools/rip` the highest-value
item for development speed, and it is a prerequisite for every mode:

1. Stand up a stub IPP server that answers `Get-Printer-Attributes` and dumps a `Print-Job` body to
   disk. Point an iPhone at it and commit the result under `fixtures/`.
2. Port the PackBits-style RLE from cups-filters (`rastertopwg.c`, `urftopdf.cpp`) and cite the source
   file in a comment. Do not derive it from first principles.
3. Build `urf_decode`, `halftone` and `imagewriter` as a host binary. `stream.bin` diffs
   byte-for-byte between changes, which is the whole point.

**In parallel:** install ESP-IDF 5.x, which is not on this machine and is needed for everything after
M2; and pull the option-card ROM to start reading the PAP side.

**Then the milestones in order** — M1 wire, M3 dots, M4 discovery, M5 print. M5 is the first video.
Mode 3's AppleTalk stack comes after that; it is weeks of work and gated on the PAP question.

## Toolchain

`kicad-cli` is not on PATH: `/Applications/KiCad/KiCad.app/Contents/MacOS/kicad-cli`, version 10.0.2.

Freerouting 2.2.4 is at `~/.local/share/freerouting/freerouting.jar`, Java 25 is present. To route:
export Specctra DSN from the PCB editor, run
`java -jar ~/.local/share/freerouting/freerouting.jar -de board.dsn -do board.ses`, import the
session. Then DRC with `--refill-zones`.

ESP-IDF **v5.5.1 is installed** at `~/esp/esp-idf`, with tools for both esp32s3 and esp32c6.
Source `. ~/esp/esp-idf/export.sh` before any `idf.py`. `sdkconfig.defaults` is split per
target because the devkit on hand is a C6 and the board is an S3; the C6 has no PSRAM, which
makes it the stricter of the two for memory.

WiFi credentials live in the gitignored `sdkconfig` (`idf.py menuconfig`, AirWriter menu) and
are overridden by anything in the `wifi` NVS namespace.
