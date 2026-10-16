# AirWriter

AirPrint bridge for Apple serial printers. ESP32-S3 dongle on the ImageWriter II's
mini-DIN-8 serial port, presenting as a native IPP Everywhere printer.

**This file is current state only. Edit it in place; never append dated entries.** History is
`git log` and Linear; designs are `docs/superpowers/specs/` and `plans/`. When a fact here stops
being true, change or delete it in the same commit that made it false.

## Docs

| | |
|---|---|
| `docs/SPEC.md` | The build spec, amended in place. Read before touching hardware or firmware. |
| `docs/traps.md` | Gotchas still true, by topic. Read the section for what you are about to touch. |
| `docs/cad-routine.md` | How to design, check and iterate printed parts. Follow it for any `enclosure/` change. |
| `docs/hardware-rev-a.md` | Rev A as built: geometry, deviations from the spec and why, rev B list. |
| `docs/library.md` | The custom KiCad library and 3D models. |
| `docs/sourcing.md` | Fab, parts, mini-DIN and PIC decisions. |
| `docs/prior-art.md` | TashTalk, TailTalk, AirTalk, ImageScribbler. |
| `docs/ORDER-NOTES.md` | PCBWay order notes for rev A. |
| `docs/KICAD-TOOLING.md` | General KiCad 10 / konnect / pcbnew playbook. |
| `docs/superpowers/specs/` | Printer scope, dot-panel cover (read its amendment block first), enclosure, status language, signed updates. |
| `docs/review-*.md`, `docs/firmware-audit-*.md` | Dated reviews. Historical; their findings are applied. |

## Source of truth
- GitHub: github.com/wr/airwriter (public), remote `origin`. Push `main` there directly. History
  was purged on 2026-10-15 (`tools/release/purge-history.sh`, W-1054): never commit the Apple
  manuals, third-party 3D models without a licence, or the private strings listed in
  `~/.config/airwriter/purge-scrub.txt`. Run `gitleaks git` before pushing anything unusual.
- Linear project: AirWriter (id: 9c428072-e8eb-4c3d-88e6-b3eadf604eed), team Personal
- Branch prefix: wells/
- PR mode: none

## State

**Rev A board 1 is built and prints from an iPhone and a Mac.** Its stream for
`fixtures/checker.urf` is byte-identical to `checker.expected.bin` (2672 bytes, crc32 `8ee84844`).

What works on `main`:
- AirPrint discovery and printing (`image/urf`, Print-Job only, SPEC §3.4); Home Assistant's
  IPP integration finds it.
- The whole job is buffered in 1 MB PSRAM and the client is answered when the upload ends
  (`W-1042 Buffer the whole job in PSRAM and reply when the upload is done`).
- Out of paper or SELECT off pauses the job; clients show "Needs attention: check paper and
  SELECT" (processing-stopped, `media-empty-error`); the job aborts after
  `CONFIG_AIRWRITER_STALL_ABORT` (600 s).
- After an aborted job the next one resyncs the printer first (1280 NULs + CTRL-X); a printer
  left busy by line noise at power-up gets a CTRL-X.
- The 37-dot status panel per the status-language spec: Spin at boot, discrete printer and Wi-Fi
  states, per-dot white Ready, DIP7 gamer RGB, and after 5 min idle one dot breathing like an
  old Mac laptop's sleep light, a new dot each breath. W-1049 puts the board's actions on it in FeatherFrame's colours: tasks (firmware update, TashTalk flash) beside the
  reset, moments opening states (printer gone and back, page out, cancelled, could not join),
  short acknowledgements (LocalTalk on, a Mac heard, Busy, Saved), and LocalTalk jobs inked in the
  six Apple stripes. `led_grid_action()` per family, `led_grid_blip()`, `led_grid_set_source()`.
  A quiet power-on: the Spin alone until Wi-Fi is up and the printer line has settled (3–30 s),
  then straight into the state; the Wi-Fi green only after a fresh setup or a real rejoin.
- DIP switches read by their printed numbers; a tap of the `test` button prints a test page, or
  with the printer not ready runs the cable test; a second tap cancels the test page; a tap during
  another job shows Busy.
- Recovery (SPEC §3.11): three equal app slots and a `known_good` record naming the last image
  that was kept and finished a real print; updates never write the running or known-good slot.
  A new OTA image runs on trial and rolls back unless healthy (`/api/status` `slot`, `trial`,
  `rolled_back`, `known_good`). Holding `test` 8 s erases the settings with a countdown on the
  panel; holding it through power-up for 5 s makes the bootloader hook erase them and boot the
  known-good slot.
- Signed updates (SPEC §3.12): only images signed with the release key install, from GitHub
  releases or `/update`; the web page's firmware section checks, installs and switches
  auto-update (off by default). An update holds the job lock to its restart and gives way to a
  job that arrives first. Releases are cut only by `tools/release/release.sh`.
- Cable test (`components/cable_test`): one cable from J2 to J4 and a press of test (or
  `POST /api/cable`) tells Apple's crossover from anything else. TashTalk sends a frame out J4
  that RMT decodes off J2's RxD; only then does RMT send one out J2's TxD for TashTalk to relay
  back. The panel answers in the bottom three rows between the two sockets: a band sweeps from
  LocalTalk to the printer and back, then the strip glows green for a crossover, or blinks red
  twice for anything else. When TashTalk takes none of the test's bytes it tests nothing and
  says why (W-1063): with the node off, U7's device ID over ICSP tells an empty socket
  (`no_chip`) from a PIC not running TashTalk (`no_tashtalk`); with the node on, only
  `chip_silent`. The panel then skips the sweep and blinks the dots by the LocalTalk socket red
  twice.
- Setup AP `AirWriter-xxxx` (open, 192.168.4.1) on a board with no credentials; web UI on port
  80: `/api/status`, Wi-Fi, settings, `/update` (OTA), `/api/update` (GitHub releases),
  `/api/localtalk/flash`, `/api/cable`.
- Wi-Fi scans all channels and joins the strongest AP; printer-uuid is stable (MAC + epoch).
- TashTalk 2.1.3 is in U7, flashed in place by the ESP32 over LVP.
- A LocalTalk node in DIP mode 2 (or always, with `CONFIG_AIRWRITER_LT_BENCH`), on UART2 to
  TashTalk at 1 Mbaud with its CTS on IO6 (`components/localtalk`): acquires a server node ID,
  answers NBP LkUp as `<queue>:ImageWriter@*`, and answers PAP as Apple's ImageWriter LocalTalk
  Option Card does (status word, one connection). A PAP job is the Mac driver's own ImageWriter
  stream and goes to the printer as it is, under the job lock: an IPP job or test page makes the
  Mac wait ("busy"), a paused printer holds the Mac's reads (16 KB spool), and a job cut short
  gets W-1041's resync. The last job's bytes are also kept for `GET /api/lt/job`. `GET /api/lt`
  shows state and log; `POST /api/lt/echo` with a node number pings that Mac (AEP).

**Enclosure** (`enclosure/`, in use on board 1): tray (print in PETG; the board goes
in by the sequence in `enclosure/README.md`, thumbing the thin west wall out past the DIP keys),
Lite lid, and the Pro build's light-guide lattice. Cavity 13.04 (measured mini-DIN height);
Pro screws M2.5 × 18. The shop's cased GLBs still show the pre-W-1036 tray.

Not built: EtherTalk, consumers of the dither override, PWG raster.

**Board 1:** ESP32-S3-WROOM-1-N8R2 (8 MB flash, 2 MB PSRAM). On the home Wi-Fi at
10.0.1.115, mDNS `imagewriter-2434.local`. Console on USB-C as `/dev/cu.usbmodem*` (the number
follows the Mac's port). Its queue name reads "ImageWriter II" in `/api/status`, which is also
its name in the Chooser. U7's PIC calibration words: `16E0 1DA8`. Its LocalTalk node ID is 214
(kept in NVS `lt`/`node`); the bench Mac is node 83. Printer: an ImageWriter II on J2 via
Apple's crossover cable.

Known issues, open:
- With the node off, TashTalk once sent and relayed nothing over the cable test's own UART2 link
  while its CTS read ready; it never recurred after a node-mode boot and the cause is unknown. The
  test retries from a reset TashTalk and logs "TashTalk answered only after a reset" (`docs/traps.md`).
- The Mac's CUPS shows "printer is offline" a few seconds into an out-of-paper pause while the
  board holds `stopped` + `media-empty-error`. Best guess: the CUPS ipp backend's write timeout.
- Wi-Fi range with the panel's copper over the antenna: −91 dBm in one spot, "Excellent" after
  moving the board. Unquantified.
- HSKo never scoped (SPEC §4.4). Board-reboot-mid-strip resync is untested.
- `W-1044 iPhone never submits a job on builds with W-1040's job model` is Done, but its cause
  was never isolated (likely the early reply, the strongest-AP fix and stale mDNS twins aging
  out). If the iPhone stalls again, that issue has the bisect plan.
- The 37 LEDs reference a 3D model KiCad 10 doesn't ship, so they are absent from STEP
  exports (`docs/library.md`).

## Open work

Linear is the queue; these are the live items.
- `W-1043 Rev B: through-hole dome LED for the power indicator`, and the rest of the rev B list
  in `docs/hardware-rev-a.md` (not in Linear yet).
- `W-573 Dot-panel cover: passive art panel, LED grid, DIP config` (umbrella, in progress).
- `W-567 M6 — Manners: paper-out, cancel, web UI, provisioning, OTA` (backlog).
- `W-1046 Factory reset must always work, even with broken firmware` and
  `W-1045 Long-press factory reset with live feedback on the LED panel`: merged 2026-10-15, benched
  on board 1 (all pass). Board 1 has the W-1059 bootloader and partition table, so an image
  without W-1046 OTA'd onto it is rolled back at its next reboot, and one without W-1059 may
  overwrite the known-good slot (`docs/traps.md`).
- `W-1053 LocalTalk: print PAP jobs from the Mac's AppleTalk ImageWriter driver`: merged
  2026-10-15. Host-tested against `fixtures/pap-iw701-page.bin`, and on board 1 a real Mac job went
  from PAP through `serial_out` byte for byte (UART loopback, `CONFIG_AIRWRITER_LT_BENCH`). Still
  owed: a paper test, which needs a second crossover cable for J2 while the Mac's is on J4.
- `W-1048 OTA updates from GitHub releases: signed, verified, optional auto-update`: merged
  2026-10-16, benched on board 1 (all pass): `/update` and GitHub both refuse tampered, foreign-key
  and unsigned images, auto-update found and kept v0.1.1, and a job mid-download wins.
- `W-1059 Known-good slot: factory reset boots the last version that worked`: benched on board 1
  2026-10-16, all pass. After its one USB flash (Wi-Fi kept), B went to ota_1, printed and became
  known-good, and C went to ota_2. A power cut 61% into D's upload left C running, and D then went
  to ota_0, not B's slot. Three power-up resets: the first booted B (0.25 s after the 5 s hold),
  one with the record blanked booted ota_0, and one with the known-good image damaged booted the
  last kept image. The panel showed the reset. No release carries it yet: the next one should,
  and the wiki edits (Test Button, Troubleshooting, Settings and Updates) are pushed with the merge.
- `W-1049 Panel feedback for every action: OTA, reset, cables, TashTalk flash`: merged 2026-10-16,
  benched on board 1 (Wells by eye), quiet power-on included: a normal boot shows only the Spin
  until Wi-Fi and the printer settle (3-30 s). Held back by Wells: update kept, update refused,
  rolled back, DIP switch flipped (their acts draw the state). Deferred from review: the amber
  ahead of "printer back" drops from the alert cap to the normal cap at its first frame, and Saved
  does not wake a sleeping panel.
- `W-1060 Boot Spin still stutters a little while Wi-Fi joins`: fixed by running the LED task, and
  its RMT interrupt, on core 1 (a frame-gap log on board 1 showed the WPA3 handshake starving it
  on core 0). Benched 2026-10-16: no frame gap over 15 ms through a power cycle, Wells "perfect
  now".
- `W-1063 Cable test: tell an empty LocalTalk chip socket apart from a bad cable`: Done
  2026-10-16, with Wells's smaller cable answer (the strip between the sockets). Host-tested and
  built, untested on hardware (Wells closed it without a bench run). When convenient on board 1:
  a crossover test (the strip, nothing regressed), then one with U7 pulled (`no_chip`, the blink
  by the LocalTalk socket).
- Not in Linear: scope HSKo during a long job (SPEC §3.9 timing).

## Toolchain
- **KiCad 10.0.6.** `kicad-cli` is not on PATH: `/Applications/KiCad/KiCad.app/Contents/MacOS/kicad-cli`.
  Run `kicad-cli <sub> --help` before each subcommand (KiCad 10's CLI differs from 8/9-era
  training data); generate `.kicad_sch` / `.kicad_pcb` with the tools, never by hand.
- **pcbnew scripts** run on KiCad's bundled interpreter:
  `/Applications/KiCad/KiCad.app/Contents/Frameworks/Python.framework/Versions/3.9/bin/python3`.
  `hardware/airwriter/route.py` does export DSN → round → Freerouting → import → DRC headless.
- **Freerouting 2.2.4** at `~/.local/share/freerouting/freerouting.jar`, Java 25.
- **konnect** resolves `lib_id` only against KiCad's install directory, so the project libraries
  must be symlinked into the app bundle. **The symlinks are gone since the 10.0.6 upgrade**;
  recreate them before any konnect placement:
  `ln -s "$PWD/hardware/airwriter/lib/airwriter.kicad_sym" /Applications/KiCad/KiCad.app/Contents/SharedSupport/symbols/`
  and the same for `lib/airwriter.pretty` into `.../SharedSupport/footprints/`.
- **ESP-IDF v5.5.1** at `~/esp/esp-idf` (esp32s3 tools). `. ~/esp/esp-idf/export.sh`
  first, then from `firmware/`:
  - build: `idf.py build`
  - flash over USB: `idf.py -p /dev/cu.usbmodem1101 flash` writes the bootloader, partition
    table, a blank otadata, a blank known-good record and the app into ota_0, so the board boots
    that and its power-up reset returns to it (leaves the printer not-ready until SELECT is
    toggled)
  - OTA: `curl -m 180 --data-binary @build/airwriter.bin http://10.0.1.115/update` (bench rules
    first). It lands on trial in the slot that is neither running nor known-good: `/update`
    refuses another until `/api/status` shows `"trial":false` (up to 2 min). The image must be
    signed with the key that signed the running one (409 while a job prints, 400 for an
    unsigned or tampered image)
  - release: `tools/release/release.sh vX.Y.Z [notes]` from a clean `main` level with origin
    (`--dry-run` builds and checks only). It builds from `sdkconfig.defaults` alone, never the
    local sdkconfig, and publishes `airwriter-esp32s3.bin` + `airwriter.json`
- **Signing key:** `~/.config/airwriter/firmware-signing-key.pem` (RSA-3072, mode 600; Wells keeps
  a copy in 1Password), never in the repo. Its public half is
  `firmware/keys/airwriter-release.pub.pem`. Every build is signed: an sdkconfig needs the five
  `CONFIG_SECURE_*` lines of `sdkconfig.defaults` with the key's absolute path, and main.c
  refuses to build without them. Losing the key means a USB flash for every board.
- `firmware/sdkconfig` is gitignored and holds the Wi-Fi credentials (menuconfig, AirWriter
  menu); NVS namespace `wifi` overrides it. `sdkconfig.defaults{,.esp32s3}` only seed a new one;
  main.c refuses to build an sdkconfig without rollback, with IDF's own factory reset, or without
  signing.
- **Enclosure:** build123d; `enclosure/build.sh` runs the tests then writes `out/*.{step,stl}`.
  The lattice's part clearances come from kicad-cli's STEP export, so the build needs kicad-cli.
  `enclosure/tools/render.py` (PNG views) and `tools/coupon.py` (fit-test cut-outs) need
  `tools/requirements.txt`. Work by `docs/cad-routine.md`.

## Verification
- **Host tests:** `make -C tools/rip test` (raster path, serial writer, printer_state, led_grid,
  dip_config, pic_prog, localtalk, cable, fw_update, known_good) and `make -B -C tools/ipp test`
  (`-B`: `tools/ipp/build/` is committed).
- **Raster regression:** `make -C tools/rip regress`; `stream.bin` must match the fixtures byte
  for byte. No ESP32 needed.
- **Firmware:** `idf.py build` clean.
- **On board 1:** an `ipptool` Print-Job of `fixtures/checker.urf` prints crc32 `8ee84844`.
  After an OTA, `/api/status` reaches `"trial":false` with the new `version`.
- **Schematic:** `kicad-cli sch erc` → 0 errors; 44 known warnings (42 `lib_symbol_mismatch`,
  2 `isolated_pin_label`).
- **Board:** `kicad-cli pcb drc --severity-all --schematic-parity --refill-zones` on
  `hardware/airwriter/airwriter.kicad_pcb` → 0 errors, 0 unconnected, 5 `lib_footprint_mismatch`
  warnings, 80 field-parity notes (both re-run 2026-10-15). Treat any unconnected item as real;
  `tools_pcb/r12_gnd.py` checks that every GND pad is on the main plane. The combined panel
  under its default rules shows only known
  items (~274 false clearance, 14 copper_edge, 12 hole_clearance, 6 seam invalid_outline, 1
  benign starved_thermal on U7.8).
- **Fab package:** `python3 hardware/combine_boards.py` then `sh hardware/combined/make_fab.sh`;
  drill floor 0.30 (`grep -E '^T[0-9]+C'`), pos file 127 rows with no J2/J4.

## Bench rules (board 1)
- **Never OTA while a job prints.** Tell Wells to hold prints, read `/api/status`, confirm
  `printing=false` with no job in the last ~10 s, then OTA. Two jobs were cut on 2026-10-15.
  Side sessions flash only on the main session's go.
- **Console only through `idf.py monitor` under `script`.** A raw pyserial open drops the S3 into
  download mode. From a non-TTY shell:
  `script -q LOG idf.py -p /dev/cu.usbmodem1101 monitor </dev/null &`, then `pkill -f idf_monitor`
  and strip ANSI from `LOG`. OpenOCD (`board/esp32s3-builtin.cfg`) reads registers live without
  a reset.
- Read `/api/status` (`ps`) before judging the panel: a deselected printer shows offline embers,
  not Ready.
- A slow upload is the link until proven otherwise: check `rssi` in the `doc_begin` log.
- A power-up reset erases board 1's Wi-Fi (no credentials in its sdkconfig): it comes back on the
  open `AirWriter-2434` network, and Wells sets it up again at http://192.168.4.1/. Read its
  console with `idf.py monitor --no-reset` attached before the reset, which keeps the reset's
  own log.

## Layout
```
hardware/airwriter/   KiCad 10 main board (sch, pcb, lib/, route.py, stitch.py, tools_pcb/)
hardware/panel/       art panel generator (Figma SVG → panel.kicad_pcb)
hardware/combined/    the snap-apart order panel and make_fab.sh; combined-fab.zip is the order
firmware/             ESP-IDF 5.5 app; components/ per SPEC §3.1 plus job_pipe, led_grid, pic_prog,
                      ota_health …; bootloader_components/ holds the bootloader's pin clamps
                      and power-up reset
tools/rip/            host build of the raster path + host tests for the pure-C cores
tools/ipp/            IPP host tests and gpa_server (the real encoder on localhost for ipptool)
tools/ippcap/         stub IPP server that captures a Print-Job body (how fixtures were made)
tools/webui/          web UI sources; mkheader.py generates firmware's webui_page.h (never hand-edit)
tools/lights/         the wiki's GIF of every panel light effect, played through led_grid_core;
                      after changing an effect, `make -C tools/lights OUT=<wiki>/images/lights`
tools/release/        release.sh (build, check, sign, publish), tamper.py (refusal-test images)
fixtures/             captured URF samples and expected streams
enclosure/            3D printed case (build123d): tray, Lite lid, Pro lattice; out/*.stl committed
```

## Licence
`LICENSE.md` maps paths to licences (texts in `LICENSES/`): firmware, tools and fixtures
GPL-3.0-or-later; hardware and enclosure CERN-OHL-S-2.0; docs CC-BY-SA-4.0. The AirWriter name
and logo are not licensed. New source files carry an SPDX header; anything third-party goes in
`LICENSE.md`'s table before it's committed.
