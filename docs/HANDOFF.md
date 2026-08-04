# AirWriter handoff

Written 2026-08-04. Everything below reflects the repo at that date.

## Where the project stands

**Hardware rev A is finished and not yet ordered.** The board routes clean, the fab package is
generated, and the only thing stopping an order is which mini-DIN connector to buy.

**No firmware exists.** Not a line. The nine components listed in `docs/SPEC.md` §3.1 are all
still to write.

| | State |
|---|---|
| Schematic | Complete, ERC clean, zero errors |
| PCB | Routed, 52 x 54mm, DRC 5 (all cosmetic), every pad connected |
| Fab package | Gerbers, drill, BOM, pick-and-place in `hardware/airwriter/fab/` |
| Firmware | Nothing |
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

**Board is 52 x 54mm, not 55 x 35.** The aspect flipped to portrait because the WROOM-1 wants its
antenna overhanging a short edge while both mini-DINs and the USB-C need the opposite one. It grew
from 32 x 54 when the LocalTalk port was added.

**Reference designators live on F.Fab, not the silkscreen.** At this density the silk text was
unreadable and generated 75 of 82 DRC violations. They still print on the assembly drawing.

## The unverified assumptions, current status

Spec §4 lists five. Only one is resolved.

1. **Bit order within a graphics byte** — open. Bring-up task: print a strip where only the top pin
   fires, observe, encode the answer in one `#define`.
2. **Density escape codes** — open. Verify the letter-to-dpi mapping for 144 dpi before building on it.
3. **Differential polarity** — **resolved by research**, and it found the spec error described above.
   Not yet confirmed against hardware.
4. **HSKo electrical levels** — open. Scope it before trusting the MAX3232 receiver assumption. If the
   swing turns out modest, a divider and clamp into a GPIO could delete U3 entirely.
5. **URF page header field offsets** — open. Take them from cups-filters, not from memory.

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

**`check_freerouting` hangs and wedges the whole MCP server.** The Java process it spawns never
exits. Recovery is `pkill -f freerouting.jar`. Do not call it.

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

**KiCad 10 removed Specctra DSN export and SES import from `kicad-cli`.** Routing therefore cannot be
automated; the Freerouting round trip is GUI-only.

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
- **`Mini-DIN-8_Shielded`** — the stock symbol has no shield pin, so there is no way to net the shell.
  Pin 9 is SHIELD, tied to all three mounting posts in the footprint.
- **`SN65HVD1473DGSR`** — reused with credit from FeralFirmware/TailTalk. No longer placed on the
  board after the transceiver swap, but kept because it is the part TashTalkUSB ships.

## Sourcing

**Fab is PCBWay, parts are China-sourced.** Price against LCSC, not Digi-Key — the gap is large
enough to change design decisions. MAX3232 is $9.98 at Mouser and $0.91 at LCSC. MAX3490 is $6.61 at
Digi-Key and $0.687 at LCSC.

Order the board as **52 x 54mm, 2 layer, 1.6mm FR4, 1oz copper, HASL**. Nothing needs a premium
process.

**Protect the 0.30mm drill floor.** PCBWay's standard minimum is 0.30mm and 0.20mm is an upgraded
spec — a documented case went from $5 to $65 for ten boards. KiCad's stock ESP32-S3-WROOM-1 footprint
puts twelve 0.20mm thermal vias in its ground pad, and they were enlarged to 0.30mm **in the board
file, not the library**. Re-importing U1's footprint will silently put them back. That is also why U1
shows a library mismatch in DRC. Before every order:

```
kicad-cli pcb export drill --output fab/ --format excellon airwriter.kicad_pcb
grep -E '^T[0-9]+C' fab/airwriter.drl        # nothing below 0.300
```

**Key part prices, LCSC, verified 2026-08-04:**

| Part | Price | Stock |
|---|---|---|
| ESP32-S3-WROOM-1-N8R2 | $3.57 | good |
| MAX3490ESA+T (printer port) | $0.687 @10 | 3,788 |
| SP3485EN-L/TR x2 (LocalTalk) | $0.21 each | 37,970 |
| PIC12F1840-I/SN | $0.70 | good |
| Kycon KMDGX-8S-BS mini-DIN | $2.35 (Digi-Key) | **backordered** |

**The open sourcing question is the mini-DIN, and you need two.** Three candidates:

- **Kycon KMDGX-8S-BS** — what the footprint was drawn from, currently backordered at Digi-Key.
- **CUI MD-80S** — the spec's listed alternative, in stock, ships same day. Verify CUI's drawing
  against the footprint before switching.
- **Legion DIN-802, LCSC C41409494** — $0.24, but only 5 in stock, and its footprint (taken from
  TashTalkUSB) has **different pin numbering** to mine while the pad positions match within 0.15mm.
  One of us has the numbering wrong. Do not use it without resolving that.

Note that full-duplex RS-422 transceivers are a niche part and keep going out of stock on LCSC —
ISL3177EIBZ and MAX3491ESD+ were both zero. Half-duplex RS-485 is commodity. That is why the
LocalTalk transceiver is two half-duplex parts rather than one full-duplex one.

## Projects this builds on

**TashTalk** (lampmerchant) — a LocalTalk data-link engine that fits in a PIC12F1840, bit-banging
SDLC and FM0 framing and presenting it over a 1 Mbps UART with CTS flow control. The board carries
one running his firmware. It needs flashing; that is what J5 is for.

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

ESP-IDF is **not installed**.
