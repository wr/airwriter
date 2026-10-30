# AirWriter

AirPrint bridge for Apple serial printers. An ESP32-S3 dongle on the ImageWriter II's
mini-DIN-8 serial port presents the printer as a native IPP Everywhere printer, and shares it,
and any LaserWriter on AppleTalk, with phones, Macs and vintage Macs.

**This file holds instructions: what to read, how to build and test, and the rules. Edit it in
place; never append dated entries.** What the hardware and firmware do is `docs/SPEC.md`,
amended in place; designs are `docs/superpowers/specs/`; history is `git log`; open work is
Linear. When a fact here stops being true, change or delete it in the same commit that made it
false.

## Docs

| | |
|---|---|
| `docs/SPEC.md` | The build spec, amended in place. Read before touching hardware or firmware. |
| `docs/traps.md` | Gotchas still true, by topic. Read the section for what you are about to touch. |
| `docs/voice-and-tone.md` | How AirWriter writes: settings page, wiki, README, commits. Read before writing any of them. |
| `docs/cad-routine.md` | How to design, check and iterate printed parts. Follow it for any `enclosure/` change. |
| `docs/hardware-rev-a.md` | Rev A as built: geometry, deviations from the spec and why, rev B list. |
| `docs/hardware-rev-2026-11.md` | Rev 2026.11 as drawn: U3's channels, the GPIOs, what moved from rev A, the scripts that build the board. |
| `docs/library.md` | The custom KiCad library and 3D models. |
| `docs/sourcing.md` | Fab, parts, mini-DIN and PIC decisions. |
| `docs/prior-art.md` | TashTalk, TailTalk, AirTalk, ImageScribbler. |
| `docs/ORDER-NOTES.md` | PCBWay order notes for rev 2026.11. |
| `docs/KICAD-TOOLING.md` | General KiCad 10 / konnect / pcbnew playbook. |
| `docs/superpowers/specs/` | One design per feature, each amended in place. Read the dot-panel cover's amendment block first. |
| `enclosure/README.md` | Printing and assembling the case, and how the shop's 3D models are built from it. |

## Source of truth
- GitHub: github.com/wr/airwriter (public), remote `origin`. Everything committed is public.
  Never commit the Apple manuals, third-party files without a licence (3D models above all),
  Wi-Fi credentials or other private details; history was rewritten once to take such things
  out (`tools/release/purge-history.sh`). Run `gitleaks git` before pushing anything unusual.
  Plans and dated reviews are kept outside this repo.
- Linear project: AirWriter (id: 9c428072-e8eb-4c3d-88e6-b3eadf604eed), team Personal
- Branch prefix: wells/
- PR mode: none

## What's built

Rev A boards are built and print from iPhones, Macs and vintage Macs. Rev 2026.11 (called rev B
in Linear and older docs) is drawn and routed, not yet made, and needs firmware of its own
(ESP32-S3-WROOM-1-N16R8, no DIP switches, a handshake GPIO per port), not yet written.

- **Printers.** Tested on the ImageWriter II. The original ImageWriter and the ImageWriter LQ
  take the same stream and are expected to work, untested; docs say so rather than excluding
  them. The LQ's self-ID reply is taken from GSport's emulator; the original ImageWriter sends
  none, so it never prints in colour. PostScript LaserWriters on AppleTalk are shared as
  printers of their own, tested on a Personal LaserWriter NTR only.
- **Ways in.** AirPrint (`image/urf`, Print-Job only) for phones and Macs. For vintage Macs, the
  LocalTalk port (TashTalk 2.1.3 in U7, which the ESP32 flashes over LVP) and three EtherTalk
  links: the board's own Wi-Fi, Basilisk II's UDP tunnel (port 6066) and Infinite Mac's zone
  relay (off until switched on). One job at a time across all of them, under one job lock.
- **Not built:** Mini vMac (LToUDP), PWG raster, HP LaserJets over serial (designed), AirPrint to
  an AppleTalk ImageWriter, colour on a colour LaserWriter (it prints gray), StyleWriters.

Where things are (`firmware/components/` unless a path is given):

| | Code | Design |
|---|---|---|
| AirPrint, IPP attributes, icons | `ipp_server`, `ipp_attrs`, `printer_caps` | SPEC §3.2–3.5 |
| Job pipeline (1 MB PSRAM buffer) | `job_pipe`, `ripjob` | SPEC §3.4 |
| Raster, halftone screens, colour inks | `urf_decode`, `resample`, `halftone`, `ripjob/ink.c` | SPEC §3.6–3.8 |
| Serial port, flow control, printer state | `serial_out`, `imagewriter`, `printer_state` | SPEC §3.8–3.9 |
| Recovery, factory resets | `ota_health`, `firmware/bootloader_components/` | SPEC §3.11 |
| Signed updates | `fw_update` | SPEC §3.12 |
| LocalTalk, EtherTalk, PAP server, client and relay | `localtalk` | SPEC §3.13 |
| LaserWriters | `laser`, `ps_page` | SPEC §3.14 |
| The printer list | `printers` | SPEC §3.15 |
| Status panel | `led_grid`; `tools/lights` renders every effect | status-language design |
| Test page | `testpage` | test-page design |
| Cable test, TashTalk flashing | `cable_test`, `pic_prog` | |
| Settings page | `webui`; sources in `tools/webui` | settings-redesign design |

The web UI and API are on port 80: `/api/status`, `/api/general` (settings), `/update` (OTA
upload), `/api/update` (GitHub releases), `/api/printers`, `/api/lt`, `/api/et`, `/api/laser`,
`/api/cable`, `/api/test`, `/api/restart`, `/api/erase`, `/api/localtalk/flash`, `/api/mem` and
`/api/panel` (the LED frame: 37 × RGB, little-endian 8.8). Two settings have no control on the
page: Top margin (`{"top_margin":N}`, 1/144 in, -216 to 144, default -144) and dot gain
(`{"gain":N}`, 0–45, default 0). A board with no Wi-Fi credentials runs an open setup network,
`AirWriter-xxxx`, at http://192.168.4.1/.

## Rules
- Write UI text, wiki pages, the README and commit messages by `docs/voice-and-tone.md`. Name
  Linear issues by ID and title.
- Keep the docs true in the commit that changes what they describe: `docs/SPEC.md`,
  `docs/traps.md`, the relevant design's amendment block, and this file when a command or rule
  changes.
- The settings page makes no request off the board. Never hand-edit `webui_page.h`;
  `tools/webui/mkheader.py` builds it.
- Never change `fixtures/checker.expected.bin` (see Verification).
- Generate `.kicad_sch` and `.kicad_pcb` files with the tools, never by hand.
- Releases are cut only by `tools/release/release.sh`, by a maintainer.

## Toolchain
- **KiCad 10.0.6.** `kicad-cli` is not on PATH; on macOS it is
  `/Applications/KiCad/KiCad.app/Contents/MacOS/kicad-cli`. Run `kicad-cli <sub> --help` before
  each subcommand (KiCad 10's CLI differs from 8/9-era training data).
- **pcbnew scripts** run on KiCad's bundled interpreter:
  `/Applications/KiCad/KiCad.app/Contents/Frameworks/Python.framework/Versions/3.9/bin/python3`.
  `hardware/airwriter/route.py` does export DSN → round → Freerouting → import → DRC headless.
  Rev 2026.11's board is built from rev A by `tools_pcb/r13_build.sh` then `r13_route.py` and
  `r14_lc_light.py` (`docs/hardware-rev-2026-11.md`); never re-run them over hand edits.
- **Freerouting 2.2.4** on Java 25 (`hardware/airwriter/README.md`).
- **konnect** resolves `lib_id` only against KiCad's install directory, so the project libraries
  must be symlinked into the app bundle before any konnect placement (a KiCad upgrade removes
  the links):
  `ln -s "$PWD/hardware/airwriter/lib/airwriter.kicad_sym" /Applications/KiCad/KiCad.app/Contents/SharedSupport/symbols/`
  and the same for `lib/airwriter.pretty` into `.../SharedSupport/footprints/`.
- **ESP-IDF v5.5.1** (esp32s3 tools). Source its `export.sh`, then from `firmware/`:
  - build: `idf.py build`
  - flash over USB: `idf.py -p /dev/cu.usbmodemNNNN flash` writes the bootloader, partition
    table, a blank otadata, a blank known-good record and the app into ota_0, so the board boots
    that and its power-up reset returns to it (the printer stays not-ready until SELECT is
    toggled)
  - OTA: `curl -m 180 --data-binary @build/airwriter.bin http://<board>/update` (read "On a real
    board" first). It lands on trial in the slot that is neither running nor known-good:
    `/update` refuses another until `/api/status` shows `"trial":false` (up to 2 min). 409 while
    a job prints, 400 for an unsigned or tampered image
  - release: `tools/release/release.sh vX.Y.Z [notes]` from a clean `main` level with origin
    (`--dry-run` builds and checks only). It builds from `sdkconfig.defaults` alone, never the
    local sdkconfig, and publishes `airwriter-esp32s3.bin` + `airwriter.json`
- **Signing.** Every build is signed. `sdkconfig.defaults` signs with
  `firmware/firmware_signing_key.pem` (gitignored; the build says how to make one); an older
  local sdkconfig needs its five `CONFIG_SECURE_*` lines. A board takes an OTA image only if the
  key that signed its running image signed it, so a board flashed with your key takes only your
  builds. The release key never enters the repo; its public half is
  `firmware/keys/airwriter-release.pub.pem`.
- `firmware/sdkconfig` is gitignored and holds the Wi-Fi credentials (menuconfig, AirWriter
  menu); NVS namespace `wifi` overrides it. `sdkconfig.defaults{,.esp32s3}` only seed a new one;
  main.c refuses to build an sdkconfig without rollback, IDF's own factory reset, or signing.
- **Enclosure:** build123d; `enclosure/build.sh` runs the tests then writes `out/*.{step,stl}`.
  The lattice's part clearances come from kicad-cli's STEP export, so the build needs kicad-cli.
  `enclosure/tools/render.py` (PNG views) and `enclosure/tools/coupon.py` (fit-test cut-outs)
  need `enclosure/tools/requirements.txt`. Work by `docs/cad-routine.md`.

## Verification
- **Host tests:** `make -C tools/rip test` (every pure-C core, from the raster path to the PAP
  client and the test page) and `make -C tools/ipp test`.
- **Raster regression:** `make -C tools/rip regress`; `stream.bin` must match the fixtures byte
  for byte (gray, the iOS capture, and a synthetic sRGB page ripped in colour and as gray). No
  ESP32 needed. The checker fixture is pure black and white and must never change; the other
  three move with any change to a screen, the curve, the ink order or how strips are sent, and
  are regenerated with the reason in the commit. A blank strip is only its held line feed, in
  gray as in colour. `rip --colour --out-preview` draws a colour page in its four inks.
- **Test page preview:** `tools/rip/build/rip --testpage colour --out-stream tp.bin` (or `black`,
  `unknown`), then `python3 tools/iwview/iwview.py tp.bin tp.png` draws it as ink on paper. Its
  text needs the printer's font dot patterns, extracted from the ImageWriter II Technical
  Reference's Appendix C into `~/.config/airwriter/iw-fonts/*.json` (or `--fonts`) and never
  committed; without them text draws as boxes.
- **LaserWriters:** `make -C tools/rip pscheck` (Ghostscript at `$GS` or `/opt/homebrew/bin/gs`)
  renders the rip's PostScript and requires its own dots, binary and hex, with every
  non-Level-1 operator trapped; `make -C tools/rip interop` prints with the firmware's PAP client
  to `etpeer.py`'s fake LaserWriter (lookup, query, print, busy, paper out, PostScript error,
  cancel, a silent printer). `tools/ipp/test/golden/` pins the IPP answers byte for byte.
- **Firmware:** `idf.py build` clean, then `python3 tools/stackcheck/stackcheck.py` (needs the
  toolchain `export.sh` puts on PATH): every task that makes ESP-IDF's deep calls keeps 1 KB of
  its stack spare by GCC's count. A local sdkconfig that fails it on `app_main` needs
  `CONFIG_ESP_MAIN_TASK_STACK_SIZE` 4608, as in `sdkconfig.defaults`.
- **Web page:** `python3 tools/webui/preview.py`, then http://localhost:8079/_states: every state
  at desktop and phone width, light and dark, before a change to the page goes to a board. Its
  `/api/panel` plays `tools/lights`' scene frames for each state's `ps` (it builds them with
  `make`; without a C compiler the dots show unlit).
- **On a board:** with Top margin at 0 (`POST /api/general {"top_margin":0}`), an `ipptool`
  Print-Job of `fixtures/checker.urf` prints crc32 `8ee84844` (2672 bytes, the same as
  `checker.expected.bin`). At the default (-1 in) the checker, which sits in the page's top
  inch, prints blank (crc32 `0e5fc630`, 9 bytes). After an OTA, `/api/status` reaches
  `"trial":false` with the new `version`.
- **Schematic:** `kicad-cli sch erc` → 0 errors; 47 known warnings (42 `lib_symbol_mismatch`,
  5 `isolated_pin_label`: TXD0, RXD0 and rev 2026.11's three spare pins).
- **Board:** `kicad-cli pcb drc --severity-all --schematic-parity --refill-zones` on
  `hardware/airwriter/airwriter.kicad_pcb` → 0 errors, 0 unconnected, 5 `lib_footprint_mismatch`
  warnings, no field-parity warnings. Treat any unconnected item as real; `tools_pcb/r12_gnd.py`
  checks that every GND pad is on the main plane. The combined panel under its default rules
  shows only known items (~221 false clearance, 6 copper_edge and 4 silk_edge where J1, J2, J4
  and U1 meet the edge, 16 hole_clearance where NPTH holes meet the pours, 6 seam
  invalid_outline, 1 benign starved_thermal on U7.8). Rev 2026.11's netlist gate:
  `python3 hardware/airwriter/tools_sch/check_rev2026_11.py <kicadxml netlist>`; its board gate,
  on KiCad's python: `hardware/airwriter/tools_pcb/r13_check.py` (pads 0.30 mm from the edge,
  silk on the board, D3's K, SW1's RESET).
- **Fab packages:** `python3 hardware/combine_boards.py` then `sh hardware/combined/make_fab.sh`
  writes AirWriter's panel (`hardware/combined-fab.zip`) and the LC's main board
  (`hardware/airwriter-lc-fab.zip`), and asserts their position-row counts and the 0.30 drill
  floor (`docs/ORDER-NOTES.md`).

## On a real board
- **Never update firmware while a job prints.** Make sure nothing is printing: `/api/status`
  shows `printing=false` and no job ended in the last ~10 s, then OTA. `/update` refuses (409)
  while a job holds the job lock, but an image from before signed updates does not.
- **Read the console only through `idf.py monitor` under `script`.** A raw pyserial open drops
  the S3 into download mode. From a non-TTY shell:
  `script -q LOG idf.py -p /dev/cu.usbmodemNNNN monitor </dev/null &`, then
  `pkill -f idf_monitor` and strip ANSI from `LOG`. OpenOCD (`board/esp32s3-builtin.cfg`) beside
  the monitor reads registers live, but a gdb attach halts both cores and detaching can reset
  the board: never with a job on paper. `thread apply all bt` names where every task waits.
- Read `/api/status` (`ps`) before judging the panel: a deselected printer shows offline embers,
  not Ready.
- A slow upload is the link until proven otherwise: check `rssi` in the `doc_begin` log.
- Holding `test` through power-up for 5 s erases the settings and boots the known-good slot. A
  board whose sdkconfig has no Wi-Fi credentials comes back on its open setup network. To keep
  the reset's own log, attach `idf.py monitor --no-reset` before it.
- Rev A sometimes powers up in the S3's download mode (power LED only, panel dark, no Wi-Fi);
  `docs/traps.md` says how to tell and how to get out.

## Layout
```
hardware/airwriter/   KiCad 10 main board (sch, pcb, lib/, route.py, stitch.py, tools_pcb/)
hardware/panel/       art panel generator (Figma SVG → panel.kicad_pcb), and the LC's
                      acrylic top laser file (svg/lc.svg → lc_top.py → airwriter-lc-top.svg)
hardware/combined/    the snap-apart order panel and make_fab.sh
firmware/             ESP-IDF 5.5 app (components/, above); bootloader_components/ holds the
                      bootloader's pin clamps and power-up reset
tools/rip/            host build of the raster path + host tests for the pure-C cores
tools/ipp/            IPP host tests and gpa_server (the real encoder on localhost for ipptool)
tools/ippcap/         stub IPP server that captures a Print-Job body (how fixtures were made)
tools/ethertalk/      etpeer.py, a software EtherTalk Mac for the tunnel and the relay, and
                      a fake LaserWriter; test_interop.py
tools/webui/          settings page sources; mkheader.py generates firmware's webui_page.h;
                      preview.py serves the page in any of states.json's states with no board
tools/icon/           the printer icon as a pixel grid; mkicons.py writes ipp_server/icons/*.png
tools/iwview/         iwview.py draws any ImageWriter stream (test page, a LocalTalk capture) as paper
tools/lights/         the wiki's GIF of every panel light effect, played through led_grid_core;
                      after changing an effect, `make -C tools/lights OUT=<wiki>/images/lights`
tools/release/        release.sh (build, check, sign, publish), tamper.py (refusal-test images)
tools/stackcheck/     stackcheck.py: worst-case stack of the tasks that make ESP-IDF's deep calls
fixtures/             captured URF samples and expected streams
enclosure/            3D printed case (build123d): tray, LC frame and lid, Pro lattice; out/*.stl committed
```

## Licence
`LICENSE.md` maps paths to licences (texts in `LICENSES/`): firmware, tools and fixtures
GPL-3.0-or-later; hardware and enclosure CERN-OHL-S-2.0; docs CC-BY-SA-4.0. The AirWriter name
and logo are not licensed. New source files carry an SPDX header; anything third-party goes in
`LICENSE.md`'s table before it's committed.
