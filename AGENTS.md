# AirWriter

AirPrint bridge for Apple serial printers. ESP32-S3 dongle on the ImageWriter II's
mini-DIN-8 serial port, presenting as a native IPP Everywhere printer.

**Start with `docs/HANDOFF.md`** — current state, decisions, traps and next actions.

The full build spec is `docs/SPEC.md`. Read it before touching hardware or firmware —
it carries the pinout, part list, escape-code plan, and a list of unverified assumptions
that must not be guessed at.

## Source of truth
- GitHub: none (local only)
- Linear project: AirWriter (id: 9c428072-e8eb-4c3d-88e6-b3eadf604eed), team Personal
- Branch prefix: wells/
- PR mode: none

## Layout
```
hardware/airwriter/   KiCad 10 project (sch, pcb, lib/, fab/)
firmware/             ESP-IDF 5.x app; components/ per spec 3.1
tools/rip/            host build of urf_decode + halftone + imagewriter
fixtures/             captured URF/PWG samples
enclosure/            3D printed case (build123d): tray + Lite lid, out/*.stl committed
docs/SPEC.md          the build spec
```

## Toolchain
- `kicad-cli` is not on PATH. It lives at
  `/Applications/KiCad/KiCad.app/Contents/MacOS/kicad-cli` (KiCad 10.0.2).
- ESP-IDF v5.5.1 is at `~/esp/esp-idf`. Run `. ~/esp/esp-idf/export.sh` first, then
  `idf.py build` from `firmware/`. Target is already set to esp32s3.
- The `konnect` MCP tools resolve `lib_id` only against KiCad's install directory —
  they ignore project and global `sym-lib-table`. The project libraries are therefore
  symlinked into the app bundle so placement tools can see them:
  `/Applications/KiCad/KiCad.app/Contents/SharedSupport/{symbols/airwriter.kicad_sym,footprints/airwriter.pretty}`.
  KiCad itself uses the project table, so the symlinks are a tooling workaround, not a
  dependency — but they will disappear on a KiCad upgrade and need recreating.
- Always run `kicad-cli <sub> --help` before using a subcommand. KiCad 10's CLI differs
  from the 8/9-era flags in most training data. Generate `.kicad_sch` / `.kicad_pcb`
  with the tool rather than hand-writing s-expressions.

## Verification
- Schematic: `kicad-cli sch erc` must pass.
- Board: `kicad-cli pcb drc` must pass.
- Raster pipeline: `tools/rip` runs on the host against `fixtures/` — regression by
  diffing `stream.bin` byte for byte. No ESP32 needed.

## Licence
Hardware CERN-OHL-S, firmware GPLv3.
