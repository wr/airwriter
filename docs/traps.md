# Traps

Gotchas that are still true, each of which cost real time at least once. Edit in place: when
a trap is fixed in code, delete it here (or move the reason into a comment at that code).
Traps already explained in a code comment are listed only when the comment is easy to miss.

## Firmware, FreeRTOS, ESP-IDF

- **A semaphore taken on one task and given on another must be binary, not a mutex.** A mutex
  given by a non-holder trips `xTaskPriorityDisinherit` and panics the board after every job.
  Both carry the comment now. Bit `s_job_lock`
  (`W-1033 Test button reboots the board after the test page prints`) and `s_print_lock`
  (`W-1042 Buffer the whole job in PSRAM and reply when the upload is done`).
- **`pdMS_TO_TICKS()` rounds down at 100 Hz.** 3 and 6 ms become 0 ticks, so a "wait" ends at
  the next tick, 0–10 ms later. Round up to whole ticks plus one (`pic_sleep_ticks()`); the PIC
  config write failed verify until it did
  (`W-1035 PIC flash: config word verify fails because ms waits round to one tick`).
- **`uart_get_tx_buffer_free_size` is not a capacity probe, and `uart_write_bytes` has no
  timeout.** The probe sees only the ring item in service. That is why `serial_out` has no TX
  ring and writes FIFO-direct (comment in `serial_out.c`).
- **`sdkconfig.defaults*` only seed a new `sdkconfig`.** A value added there (the lwIP
  `CONFIG_LWIP_MAX_SOCKETS=16` and `CONFIG_LWIP_TCP_ACCEPTMBOX_SIZE=8`, for one) must also be
  set in an existing `sdkconfig`, or the build silently keeps the old one.
- **Floating pins from power-on until firmware claims them.** IO48 (U9's input, no pull) and IO35
  (SP3232 T1IN, a driver input with no pull) are driven low as the first statements of
  `app_main()`. IO48 floating can brownout-loop the board on a 500 mA port, not just flicker the
  LEDs. Keep the clamps first; rev B adds the resistors (`docs/hardware-rev-a.md`).
- **Rev A has no hardware LED current limit.** `LG_BRIGHTNESS_CAP` and friends in
  `led_grid_core.h` are the only limit on 37 SK6812s; raise them knowingly.
- **A raw pyserial open on macOS drops the S3 into download mode** (the driver asserts DTR at
  open). Read the console only with `idf.py monitor` under `script` (AGENTS.md, bench rules).
- **An image built without W-1046 is rolled back at its next reboot** when a W-1046 image
  installs it over the air: the installer marks it on trial, and nothing in it ever keeps
  itself. It runs fine until then. Rebase a branch on W-1046 before OTAing it to a W-1046 board,
  or USB-flash it (which also puts the old bootloader and table back).
- **A board running a signed image refuses every image not signed with the same key**
  (`W-1048 OTA updates from GitHub releases: signed, verified, optional auto-update`), and
  `/update` says "not signed with this board's key, or damaged". A branch without W-1048 needs
  no rebase to OTA board 1: the five `CONFIG_SECURE_*` lines of `sdkconfig.defaults`, with the
  key's absolute path, in its own sdkconfig sign its build. An image running unsigned (one
  flashed as `airwriter-unsigned.bin`) refuses everything; USB-flash a signed one.
- **Never publish an image built with `idf.py` by hand.** The local sdkconfig may carry Wi-Fi
  credentials and bench options. `tools/release/release.sh` builds from `sdkconfig.defaults`
  alone and checks the image before it tags. Never rebase a branch after a release tag lands
  on it.
- **W-1048's TLS client and certificate bundle cost flash:** the S3 image leaves 37% of its 2 MB
  slot free. Check the size when adding anything large.
- **Any reboot while an OTA image is on trial rolls it back** (up to 2 min after it boots;
  `/api/status` `trial`): a crash, a Wi-Fi change from the web UI, the long-press reset, and
  `idf.py monitor` without `--no-reset`, which resets the chip when it attaches.
- **`idf.py flash` over USB leaves the printer not-ready until SELECT is toggled.** The S3 sits
  ~10 s in download mode with IO17 (UART1 TX → U2 DI) floating. Power-cycling does not do it.
  Dev-only; rev B candidate pull-up on IO17.

## IPP, iOS, macOS, mDNS

- **iOS keys a printer on `printer-uuid`, cached capabilities included.** Changing
  `operations-supported` under the same UUID left every iPhone job at "Waiting"; a per-boot UUID
  (dev mode) left two "ImageWriter II" entries. Bump `CONFIG_AIRWRITER_UUID_EPOCH` only when
  advertised values change: a new UUID orphans the Mac's CUPS queue, whose device-uri pins
  `uuid=` (Kconfig help has the detail).
- **Stale UUIDs linger as twins.** ESP mDNS does not cache-flush the previous boot's TXT record,
  so a UUID or name change shows two printers until client caches expire (~75 min).
- **Never advertise Create-Job / Send-Document.** With them in `operations-supported` the
  iPhone held every job at "Waiting" (SPEC §3.4).
- **iOS and macOS open 25–50 parallel Get-Printer-Attributes connections.** Workers parked on
  idle keep-alives starved them and prints timed out before Print-Job. Fixed in `ipp_server.c`
  (4 workers, backlog 8, 300 ms idle close when no worker is free); keep it when touching workers.
- **`tools/ipp/build/` holds committed binaries.** `git checkout` leaves them newer than the
  sources and `make test` runs the stale ones. Use `make -B -C tools/ipp test`, which in turn
  rewrites the committed binaries: `git checkout -- tools/ipp/build` before committing unless
  you mean to update them.
- **macOS's `get-printer-attributes-suite.test` has a broken fifth test** (asks for `all`,
  expects only `media-col-database`). No printer passes it (SPEC §3.5).
- **A slow upload is usually the link, not the firmware.** Read the per-job `rssi` (`doc_begin`)
  and `run_document`'s upload-time / longest-wait / longest-push logs before debugging code.
  At rssi −80 an upload crawled at 270 B/s and a cut was seen 16 s late.
- **Killing `ipptool` does not cut a transfer on macOS.** The kernel delivers the buffered file,
  and with the PSRAM job buffer a small upload is already in. To cut mid-document use Cancel-Job, starve
  `ipptool` through a FIFO, or `curl --limit-rate` with a hand-built IPP request.
- **Documents over 1 MB bypass the PSRAM buffer** and stream with TCP backpressure (an iPhone
  photo page is 6.4 MB of URF). For those a dropped upload during a pause still looks like a
  healthy pause until paper returns: the client's FIN waits behind its unsent data.
- **`fixtures/checker.urf` cannot show printer desync.** Its 0x00/0xFF bytes print nothing when
  read out of sync; test resync with a mid-gray band.

## Wi-Fi and network

- **The home mesh: a fast scan joins the first node that answers.** It took a far node on
  channel 1 (−80 to −87 dBm) over a −46 dBm node in the same room; uploads ran at ~6 KB/s and an
  OTA failed. Firmware now scans all channels and joins the strongest AP; don't revert that.
- **Never OTA while a job prints.** It cut two jobs on 2026-10-15. Since W-1048 `/update` refuses
  (409) while a job holds the job lock and gives way to one that arrives mid-upload, but an
  image from before W-1048 still on a board does not. Bench rule in AGENTS.md.

## Serial, printer, LocalTalk

- **J2 needs Apple's crossover cable (590-0552-A / M0197).** Many reproduction "ImageWriter II"
  cables are wired straight and give a false "out of paper"; buzz pin 1 → pin 2 first (SPEC §2.4),
  or plug it from J2 to J4 and press test (the cable test, W-1055). The test can't reach pins 1/2
  (J4's are not connected), so a cable crossed on data but straight on the handshake passes.
- **TashTalk can go deaf to a fresh UART2 link with the node off** (seen once on board 1, W-1055):
  no transmit and no relay, while its CTS read ready and it took the 1024-NUL sync. A node-mode
  boot in between cleared it; the PIC is never reset by an ESP32 reboot. The cable test retries
  from a reset TashTalk (MCLR low 2 ms); its `dbg.reset` is 2 when the reset is what passed.
- **`uart_set_pin` releases every pin the UART had, CTS included, before routing new ones.**
  Re-routing TX and RX alone (after lending them to RMT, which also moves IO18 off the IOMUX)
  leaves CTS tied to "clear" and overruns the printer; `serial_out_lend(0)` passes all three.
- **An aborted mid-strip write leaves the printer counting `ESC G` bytes**, so it prints the next
  job's header as dots. The next job resyncs with 1280 NULs + CTRL-X (`iw_resync`,
  `W-1041 Resync the printer after an aborted job so the next job doesn't print garbage`).
  Not covered: a board reboot mid-strip.
- **`LT_DE` floats whenever the PIC is not driving it** (no PIC, or PIC held in reset, which
  tristates it). Tack 10 k across U6 pins 3–5 before connecting a LocalTalk network.
- **Never high-voltage program the PIC in its socket.** VPP (~9 V) lands on ESP32 IO2
  (`LT_RESET`). The ESP32 flashes it over LVP and keeps CONFIG2.LVP set so it stays
  reprogrammable without high voltage (`pic_prog.h`).
- **TashTalk never says whether a frame went out.** It does the RTS/CTS handshake itself and
  drops a frame silently after 32 failed attempts, and it relays every frame but its own. Prove
  the transmit path with `POST /api/lt/echo` (body: a Mac's node number) and look for the echo
  reply in `GET /api/lt`.
- **TashTalk's CTS (RA2 → IO6) is low when it can take bytes**, the ESP32 UART's own CTS
  polarity: no inversion. Its queue is 128 bytes and a frame can be 605, so flow control is not
  optional.
- **A LocalTalk job with no printer on J2 cannot finish.** The line reads off line and the status
  word says so; whatever the Mac sends fills the spool and holds its reads, and after
  `CONFIG_AIRWRITER_STALL_ABORT` the job is aborted (CloseConn). To take a job on the bench without
  a printer, build with `CONFIG_AIRWRITER_UART_LOOPBACK`: the line reads ready and the log checks
  the job's bytes as they come back (never on a board that should print).
- **The LocalTalk node owns IO4/IO5, which are also the PIC's ICSP lines.** The web UI's
  TashTalk flash refuses while the node runs: set DIP1-2 off mode 2 (and build without
  `CONFIG_AIRWRITER_LT_BENCH`), restart, then flash.
- **The Option Card's PAP status is a two-byte word, low byte first, not a string** (Apple TN
  NW20; `LT_IW_*` in `lt_node.h`). A LaserWriter-style `%%[ status: ... ]%%` string is wrong here.
- **Never report busy (bit 15) to a Mac about its own connection.** Mac OS 8.1's AppleTalk
  ImageWriter 7.0.1 opened PAP, read 0x8008 (busy + off line) and sent nothing until the
  connection timed out. Busy is only for the board printing someone else's job.
- **AppleTalk ImageWriter 7.0.1 ends a job with CloseConn, never PAP EOF**, after its last Data.
  It never reads back (no SendData to us), polls SendStatus about twice per chunk, writes ~88
  bytes per read with a flow quantum of 1, and sends the same native ImageWriter stream as serial
  (one short page: 11968 bytes, identical across prints). Once, the Mac sent a whole page and
  then tickled the connection for 10+ minutes without closing. Cause not found. Its next job's
  OpenConn now supersedes a connection silent for 10 s (`LT_PAP_STALE_MS`), and a job that has
  owed data for 5 min with the printer ready is closed (`LT_PAP_QUIET_MS`), so the printer is
  never held indefinitely.
- **The bench Mac's 8.1 install had no AppleTalk ImageWriter driver**, and the plain
  "ImageWriter" Chooser driver is serial only.
  Apple's `AppleTalk_ImageWriter_7.0.1.sea.bin` (Wayback copy of download.info.apple.com) works.
- **U8's ~RE rides `LT_DE`, so U8 shuts down while the PIC transmits.** Deliberate: wake is
  ≤ 1.8 µs against LocalTalk's ~200 µs inter-dialogue gap. It mirrors the U6 bug fixed on 08-22
  but is not that bug; don't "fix" it.

## KiCad, board, fab

**Editing**
- **KiCad silently reverts external edits.** A session open before a scripted edit writes its
  stale copy back on save (ate the outline once, the design rules twice). Close KiCad (no
  `~*.lck` files) before editing, re-verify after.
- **konnect `set_design_rules` and `set_board_size` corrupt this project.** The first writes
  rules into `.kicad_pcb` (KiCad 10 reads `.kicad_pro`: duplicate keys, unloadable board); the
  second appends a second outline. Edit `airwriter.kicad_pro` directly; rebuild outlines in script.
- **A generated `.kicad_mod` with unescaped `"` in `(descr ...)`** is malformed; KiCad drops the
  part from the board without complaint. Balance-check generated library files.
- **A footprint's rotation does not rotate its pad shapes.** Each pad carries its own angle in
  `(at x y ANGLE)`; omit it and pads land right but unrotated (four shorted pads on a 1 mm part).
- **Footprint zones store absolute coordinates.** Moving a footprint's `(at ...)` in a script
  strands its keepout (the WROOM keepout once sat 46 mm off the board).
- **pcbnew in-process:** `board.Remove()` corrupts later `FootprintLoad`/`GetNetInfo` (split
  scripts); `FootprintLoad` drops the library nickname from the FPID; footprint-owned rule areas
  are not in `board.Zones()`; deleting routed tracks leaves their clearance cut-outs frozen in the
  stored fill (refill with `ZONE_FILLER` in script — `kicad-cli --refill-zones` does not write back).
- **`board.Save()` randomizes every UUID** (the panel generator derandomizes; keep it), and
  `Fracture()` turns polygon holes into slit outlines, so `HoleCount()` reads 0 on reload. Verify
  by vertex count or render.
- **Zone nets are hierarchical.** Pads are on `/GND`; a pour made on `"GND"` was floating copper
  that DRC reported only as isolated islands. Check zone nets by string.
- **The stock WROOM courtyard fuses the module with a 48 mm RF rectangle.** Split them (body =
  courtyard, RF = rule area notched round fixed parts) or the flush antenna won't place.
- **Back-side parts' courtyards are not checked against front-side footprints.** A B.Cu part's
  through-hole landed 1.5 mm from a front LED once; check by hand.
- **The DIP-8 socket footprint's origin is pad 1**, 3.81 mm off the body centre in both axes;
  SW3's origin is pin 1, its plunger 3.95 mm away.
- **Re-importing a footprint from the library silently undoes board-level edits**: U1's
  0.20 → 0.30 mm thermal vias (drill floor) and J1's 3D model path (`docs/library.md`).
- **A board footprint keeps its own copy of the 3D model path**, so adding a model to the library
  fixes nothing already placed. Hand-written `.wrl` bodies render in KiCad but are absent from
  STEP/STL exports. A missing model is silent; run the check in `docs/library.md`.

**Routing**
- **The main-board order is fixed:** `place_leds.py` → strip wiring → `route.py --passes 60`
  (default 25) run detached → `stitch.py` once → prune vias in via-barred areas → refill with
  `ZONE_FILLER` → DRC `--refill-zones --schematic-parity --severity-all`. A finished route has no
  "unrouted" clause in Freerouting's completion line.
- **Freerouting hangs on KiCad's DSN** (fractional coordinates under `(resolution um 10)`; it
  spins forever in `normalize_traces`). `route.py` rounds every number. Recovery:
  `pkill -f freerouting.jar`.
- **Freerouting also hangs re-reading its own output**, so `route.py` wipes and routes from
  scratch every time (~15 s); `--keep-wiring` hangs on an already-routed board.
- **Run `route.py` detached with `nohup`.** Tool-shell resets kill plain background runs.
- **Every rule area exports to Freerouting as a hard keepout**, whatever its settings; a zone
  holding many outlines exports as one keepout the router drives through. Give each its own zone.
- **`stitch.py` has no undo and runs once per fresh route.** pcbnew's via `Remove()` does not
  survive `Save`, so a second run accumulates vias. It reads the pour, not rule areas, so prune
  vias it drops into via-barred areas. Its lattice box is hardcoded around U1; move it with U1.
- **Freerouting never repours zones.** Always DRC with `--refill-zones` (278 violations → 3).
- **Every `unconnected_items` entry is real, `Zone↔Zone` included.** KiCad calls a pad connected
  once it touches any GND copper, even a stranded island; N such edges mean N+1 GND clusters (ten
  ungrounded parts on 08-25). Verify with `tools_pcb/r12_gnd.py` (every GND pad on the main plane).
- **An "impossible" net is usually a single-track keyhole.** Widen the approach; never shorten the path.

**Rules and fab**
- **A rule that quietly permits an upgraded fab spec costs money at quote time.** Drill floor
  0.30 mm (0.20 is a PCBWay upgrade) and clearance floor 0.15 mm (98 violations hid at 0.12).
  Check `grep -E '^T[0-9]+C' fab/*.drl` before an order.
- **`hole_to_hole` was severity warning and hid a 0.186 mm via pair.** It is error now; still DRC
  with `--severity-all`.
- **PCBWay rejects copper within 0.4 mm of a V-score; KiCad's DRC checks the wrong rule** and stays
  green. The GND zones' seam edge sits at y 60.5 for this.
- **Fabs clip silk over exposed copper; neither DRC nor renders show it.** The `test` tag is a
  mask knockout for that reason.
- **`kicad-cli pcb render` tints bare FR4 by the mask colour**, so glowing FR4 dots look black
  in renders.
- **The BOM must group by the DNP flag** or a DNP row swallows fitted parts (R8 was once left off).
- **`hardware/airwriter/fab/` is a stale single-board package.** The order source is
  `hardware/combined-fab.zip` + `hardware/combined/combined-top.png` (`docs/ORDER-NOTES.md`).
- **`combine_boards.py` strips every net except `PANEL_AU`**, the decorative net that lets the
  test pad, annulus and tab art touch without a DRC short. Don't "fix" either end.

**Art panel (`hardware/panel/`)**
- **svgelements yields `<mask>` content as ordinary elements** and ignores mask attributes; the
  generator strips defs and rect-crops masked paths.
- **The Figma export's final `<rect fill="white">` lives inside `<clipPath>`.** Hiding or deleting
  it clips the whole SVG to nothing.
- **`floors.py` warnings are expected**: 7 known, all 0.101 mm pinches (4 gold, 3 silk), accepted
  by Wells. It advises; it does not fail the build.
- **The SMTSO standoff seats need solid back-flood copper.** A dot window under a seat leaves
  nothing to solder to.
- **Board holes and panel seats are coaxial by construction** (`STANDOFFS` in
  `hardware/panel/emit_board.py`). Change them together or not at all.
- **LEDs map to dots by identity, `board = panel + (100, 60)`.** An earlier mirror rule was wrong.

## Enclosure

- **Front connectors are not coplanar.** Measured, the mini-DIN mating faces stand 2.75 mm proud
  of the board edge and the USB-C shell ~1.50 mm (enclosure spec §3). An old "all stand 2 mm
  proud" line was intent, not measurement.
- **The east-wall reset pinhole is at SW1's plate face, x ≈ 159.4**, not on the west wall at
  x = 100 as an old note said.
- **Heights the KiCad models get wrong.** SW3's footprint is `SW_PUSH_6mm_H13mm`, but the
  TS-1002-BR16026 stands 16 mm. U7's model is the empty socket; a seated PIC stands ~9 mm.
- **Measure the parts the case touches; the models are off.** Mini-DIN shells stand 13.04, not the
  STEP's 12.75 (`MINIDIN_TOP`). SW3's plunger is 3.1 at the lattice top tapering to 3.5, and it
  stands 0.2 E / 0.2 S of the model's axis (`TEST_PLUNGER_*`).
- **build123d `Compound & Box` returns an empty Compound** (0.11.1); `Solid & Box` works. Slice
  STEP assemblies one solid at a time (`components._bands`).
- **A boolean whose faces all coincide returns nothing** (OCCT). Let one body overshoot the other.
- **`fillet_2d` fails when the radius is longer than an adjacent edge.** Lengthen the edge (an
  overshoot into air costs nothing).
- **kicad-cli's STEP export exits 2 on every run**, because KiCad 10 ships no SK6812MINI model.
  `components.export()` accepts exactly the NO_MODEL parts and names anything else; don't loosen
  that by trusting a stale `out/components.step`.
