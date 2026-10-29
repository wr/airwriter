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
| `docs/voice-and-tone.md` | How AirWriter writes: settings page, wiki, README, posts. Read before writing any of them. |
| `docs/cad-routine.md` | How to design, check and iterate printed parts. Follow it for any `enclosure/` change. |
| `docs/hardware-rev-a.md` | Rev A as built: geometry, deviations from the spec and why, rev B list. |
| `docs/library.md` | The custom KiCad library and 3D models. |
| `docs/sourcing.md` | Fab, parts, mini-DIN and PIC decisions. |
| `docs/prior-art.md` | TashTalk, TailTalk, AirTalk, ImageScribbler. |
| `docs/ORDER-NOTES.md` | PCBWay order notes for rev A. |
| `docs/KICAD-TOOLING.md` | General KiCad 10 / konnect / pcbnew playbook. |
| `docs/superpowers/specs/` | Printer scope, dot-panel cover (read its amendment block first), enclosure, status language, signed updates, laser printers, StyleWriters. |
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
  IPP integration finds it. A Print-Job that finds the printer taken (another phone's job, a
  Mac's, the test page, an update) is told `server-error-busy` at once and the phone asks
  again; it never waits with its document unread (W-1076 bench; an iPhone waited out a Mac's
  three pages and printed).
- `printer-icons`: Apple's colour ImageWriter icon at 48, 128 and 512 px, served on port 631
  (SPEC §3.5). A Mac takes it only when the printer is added.
- The client is answered when its upload ends
  (`W-1042 Buffer the whole job in PSRAM and reply when the upload is done`), from a 1 MB
  PSRAM job buffer. An ImageWriter job splits it (`W-1066 Full-page colour photos overflow the
  1 MB job buffer`): 512 KB of URF ahead of the rip, 512 KB of printer bytes behind it, which
  the `prn` task feeds to the port, so the rip keeps pace with the upload and the printer
  prints from memory. Any one page fits, a full-page colour photo included (~5.7 MB of URF,
  250–350 KB to print; the worst page, colour noise, 460 KB). The upload waits on the printer
  past 512 KB of printer bytes (a second colour photo page, or about five gray pages), and
  when the ImageWriter is not ready as the job starts, past 512 KB of URF (the rip asks the
  ribbon first). A LaserWriter job keeps the whole 1 MB for URF. `/api/status` `pipe` shows
  the job in the pipeline, or the last one: bytes in and out, `queued` (still to print), and
  when its upload, rip and last byte ended (`upload_ms`, `rip_ms`, `ms`, 0 until then).
- Out of paper or SELECT off pauses the job; clients show "Needs attention: check paper and
  SELECT" (processing-stopped, `media-empty-error`); the job aborts after
  `CONFIG_AIRWRITER_STALL_ABORT` (600 s).
- After an aborted job the next one resyncs the printer first (1280 NULs + CTRL-X); a printer
  left busy by line noise at power-up gets a CTRL-X.
- Five screens (SPEC §3.7, W-1068): Atkinson, Floyd–Steinberg (both serpentine), blue noise
  (`halftone/bluenoise64.h`, generated), Bayer, threshold, with an optional dot-gain tone curve
  (`CONFIG_AIRWRITER_DOT_GAIN` seeds NVS `gain`; `POST /api/general {"gain":N}` 0–45, shown in
  `/api/status` `gain`, no UI yet). Default 0: on paper Wells found the uncorrected dusk page
  closest to the screen, no contest against 10 and 20. On a colour page the inks are screened black, cyan,
  magenta, yellow and kept off each other's dots. A screen or gain saved from the page reaches
  the next job at once (`job_pipe_set_screen`); before W-1068 it waited for a restart. The
  dashboard's halftone sheet screens its art the same way, with the board's gain, and shows it
  as ink on paper.
- Colour with Apple's colour ribbon (`W-1064 Colour printing over AirPrint with the ImageWriter
  II colour ribbon`): the board reads the ribbon with `ESC ?` self-ID (when the printer comes
  back after a second or more down, and before every AirPrint job), the ImageWriter II's
  "IW10CF" and, since `W-1087 Read the ImageWriter LQ's self-ID so a colour ribbon prints in
  colour`, the LQ's "LQ1CF1" (the shape GSport's emulator sends, untested on a real LQ;
  NJRoadfan on 68kMLA has offered the LQ technical reference's breakdown, which may correct
  it), saves it, reports it in
  `/api/status` `ribbon`; the web page names it on the nameplate and draws its halftone preview
  in the four inks while it is colour. While it is colour, IPP and TXT advertise colour under the same UUID,
  and sRGB pages print one pass per ink per strip, yellow, magenta, cyan, black (SPEC §3.8). On a
  black ribbon an sRGB page prints as gray. Gray pages keep their bytes.
- The 37-dot status panel per the status-language spec, in the palette picked from the panel's
  measured colour map (red only for failures, sky blue for Wi-Fi, pink for updates): the boot
  wave (a rainbow in diagonal bands on all 37 dots, as the shop's 3D model draws it; it replaced
  the Spin on 2026-10-28), discrete printer and Wi-Fi states, per-dot white Ready, DIP7 gamer
  RGB, and after 5 min idle one dot breathing like an old Mac laptop's sleep light, a new dot
  each breath. W-1049 puts the board's actions on it in FeatherFrame's colours: tasks (firmware
  update, TashTalk flash) beside the reset, moments opening states (printer gone and back, page
  out, cancelled, could not join), short acknowledgements (LocalTalk on, a Mac heard, Busy,
  Saved), and LocalTalk jobs inked in the Apple stripes without red and purple.
  `led_grid_action()` per family, `led_grid_blip()`, `led_grid_set_source()`. A quiet power-on:
  the boot wave alone until Wi-Fi is up and the printer line has settled (3–30 s), then straight
  into the state; the Wi-Fi green only after a fresh setup or a real rejoin.
- DIP switches read by their printed numbers; a tap of the `test` button prints a test page, or
  with the printer not ready runs the cable test; a second tap cancels the test page; a tap during
  another job shows Busy.
- The test page (`components/testpage`, design `docs/superpowers/specs/2026-10-20-test-page-design.md`):
  a full Letter page from the tap, DIP4 at power-on and the page's Print Test Page. Letterhead
  (the icon and Apple Garamond as dots, `tp_art.h` from `testpage/tools/mkart.py`), the board's
  details, every font, pitch and style in the printer's own type, the seven colours and a
  ray-traced still life in four inks (gray with a note on a black ribbon), the five screens side
  by side, a ruler and four one-inch squares. It asks the ribbon first, as a job does; about
  64 KB of stream in colour, and never `ESC F` (`docs/traps.md`). The colour ink order and phases live in `ink.c` (`ink_screens_*`),
  shared with jobs.
- Recovery (SPEC §3.11): three equal app slots and a `known_good` record naming the last image
  that was kept and finished a real print; updates never write the running or known-good slot.
  A new OTA image runs on trial and rolls back unless healthy (`/api/status` `slot`, `trial`,
  `rolled_back`, `known_good`); with the console on board 1 (2026-10-28, W-1108), a restart 2 s
  into a trial was back on the previous image 6 s later. Holding `test` 8 s erases the settings
  with a countdown on the panel; holding it through power-up for 5 s makes the bootloader hook
  erase them and boot the known-good slot.
- Signed updates (SPEC §3.12): only images signed with the release key install, from GitHub
  releases or `/update`; the web page's firmware section checks, installs and switches
  auto-update (off by default). An update holds the job lock to its restart and gives way to a
  job that arrives first. Releases are cut only by `tools/release/release.sh`.
- Cable test (`components/cable_test`): one cable from J2 to J4 and a press of test (or
  `POST /api/cable`) tells a crossover (any, not only Apple's) from anything else. TashTalk sends a frame out J4
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
  `/api/localtalk/flash`, `/api/cable`, `/api/restart`, `/api/erase`, `/api/test`,
  `/api/mem` (what holds internal RAM at its lowest, W-1094; `POST /api/mem/reset`),
  `/api/printers`, `/api/panel` (the LED frame being driven: 37 × RGB, little-endian 8.8,
  222 bytes; 404 with no strip).
- The settings page (`W-1079 Settings page: organise by port, like macOS System Settings`,
  design `docs/superpowers/specs/2026-10-20-settings-redesign-design.md`): a settings app, a
  sidebar of Printers (W-1088: notices only for what needs a decision, with a caution mark
  there and on the sidebar row; a found printer just joins the list; a row per printer, a
  printer's detail in the pane, `#printers/<id>`), AirPrint (the switch and each shared printer's address), AppleTalk, Wi-Fi
  and About, each row showing its state; one home per setting. Setup mode is three steps,
  sized for a phone (printers, Wi-Fi, "You're All Set": print from a phone, the test button for
  a test page) and the first visit after it opens on "AirWriter is ready". Beside the name in
  the sidebar is the status panel itself (W-1092): its 7 × 8 dots from `panel_layout.json`, lit
  live from `/api/panel` in the colours board 1's panel was measured showing (the wiki GIFs'
  model, `tools/lights/render.py`), the gold muted so a lit dot never reads as gold. The page
  asks about 6 times a second while the mark is on screen, once a second during a job (6 a
  second cost board 1 7% of a core and no heap), and draws at 30 fps. The setup and About
  heroes keep AirWriter's render (`tools/webui/airwriter.webp`, from the shop); the ImageWriter
  icon is the ImageWriter's (IPP, the tab). Printer rows draw an ImageWriter or a LaserWriter. Upright system type, Apple Garamond Light titles, platinum, icon tiles in the
  ribbon's inks, status as lit bar lamps, low platinum keys; in a window on a big screen, the
  sidebar first on a phone. AppleTalk is one card per way in (AppleTalk Port, Networked Macs,
  Basilisk II, Infinite Mac) with its switch and the Macs heard in the last 10 minutes, each by
  its Macintosh name when it gives one, its address beside the time. A node that answers the
  board's printer lookups (a printer, another AirWriter) is left off unless it gives a Macintosh
  name, and the panel's "a Mac heard" skips it (W-1096); a printer switched on while the board
  runs shows as a Mac until the next look (up to 5 min). Every link asks each node it
  hears for `=:Workstation@*` (System 7 registers the Sharing Setup name so), a LkUp straight to
  that node: 1 s after first hearing it, then backing off from 5 s to 10 minutes until it
  answers, and 1 s after it is seen registering its name; then every 10 minutes. Names are in
  PSRAM. Benched 2026-10-20: the SE/30 over BlueSCSI ("SE30") and Basilisk II ("SE/30") answer.
  On the cable the SE/30 opens AppleTalk long before it registers its name, and the first build
  gave up first; with the fix, the cable card named it "SE30" on board 1 (2026-10-20). The
  board asks U7 at boot, at every node start and after a flash whether it runs TashTalk (the
  sync drains, else its device ID; `/api/status` `lt_chip`, W-1081); without it the node never
  starts, and the AppleTalk Port card shows "No LocalTalk chip" or "The LocalTalk chip needs
  TashTalk" in place of its switch, the latter with a Flash TashTalk key. A good flash starts a
  wanted port at once. The cable test's flash key still shows after it answers `no_tashtalk`. A
  build with no PIC pins hides the card and the TashTalk credit. The page makes no
  request off the board. Sources in `tools/webui`; `mkheader.py` builds `webui_page.h`.
- Top margin (NVS `topmargin`, `/api/status` `top_margin`, `POST /api/general
  {"top_margin":N}`; no control on the page: with the paper loaded as the guide says the default
  is right, and a menu and then a line-up sheet only confused), in 1/144 in, -216 to 144: positive adds
  paper ahead of each AirPrint page and the test page, negative drops their first rows
  (`ripjob_cfg_t.top_margin`, `rip --top-margin`). Not LocalTalk jobs: the Mac's driver keeps
  its own. Default -144 (`CONFIG_AIRWRITER_TOP_MARGIN`): paper loaded as the Owner's Guide says
  (leading edge under the bail) starts printing about 1 in down, and an AirPrint page arrives
  as the whole sheet, so dropping that inch lands it where the iPhone laid it out. The board
  advertises a 1 in top margin (bottom 0.5 in, SPEC §3.5), so iOS draws nothing in that inch.
  Board 1 is set to -7/8 in (-126): there its calibration lines land exactly 1 in from the top
  and 0.5 in from the bottom.
- A job starts at top of form: when the last stream to move the paper was cut, or was a Mac's
  that stopped mid-page, the next job sends a form feed first (`iw_link_need_ff`). ESC c leaves
  top of form alone. A Mac's stream never form-feeds (it feeds by line, forward and back, to
  exactly its 11 in page), so the LocalTalk scanner follows the paper (`iw_scan_at_tof`).
- AirPrint has an off switch (W-1079): off, `_ipp` and `_printer` leave Bonjour and Print-Job
  is refused as not accepting jobs; on, they come back under the same printer-uuid.
- Wi-Fi scans all channels and joins the strongest AP.
- The printer list (`W-1088 Printers: AirWriter finds, sets up and shares whatever printers you have`,
  design `docs/superpowers/specs/2026-10-23-printers-design.md`, SPEC §3.15, merged
  2026-10-24): records in NVS `printers`/`list` (`components/printers`), each
  with its own printer-uuid, made once. The port's printer is AirPrint printer 0 as its record
  says; `ESC ?` at line-up finds it, and ImageWriter commands (CTRL-X kick, resync, the job's
  self-ID, the test page) go to J2 only when it is an ImageWriter. Identified printers are
  shared at once, up to four; AppleTalk ones at AirPrint printers 1-3. The test button, DIP4 and
  the page go to the port's ImageWriter, else the first shared printer (a LaserWriter gets
  `ps_testpage`). The panel shows the box: ready while any shared printer is; the idle CTRL-X
  follows the port printer's own line (`printer_state_port_line`). Setup mode looks
  at once. AirPrint to an AppleTalk ImageWriter was cut: old Macs' only. `GET`/`POST
  /api/printers`.
  A board's first boot on it makes its records from what it had (the port's old uuid, a
  W-1082 LaserWriter, the AirPrint switch as the port's Share) and stores its host name, so
  `imagewriter-xxxx` stays; a new board is `airwriter-xxxx` with the AppleTalk port on.
  Old Macs on each AppleTalk way in that is on are offered every identified printer AirWriter
  can carry (`printers_offers`, `lt_node_offer`): the port's ImageWriter under its name on
  socket 0x81, printers from other links under their own NBP names on 0x85-0x87, and
  `<host name>:AirWriter` on 0x88 on every running link, which another AirWriter's lookups use
  to skip this one's printers. NBP names are the record's in MacRoman (`nbp_name`). A Mac's
  connection to a printer from another link goes through the relay (`localtalk/pap_relay.c`,
  run by `laser.c` with the one PAP client, under the job lock): the Mac hears busy, in the
  printer's own status words, until the printer's connection is open; then its data goes to
  the printer and the printer's output (query answers, errors) back to its reads, 256 bytes a
  read, nothing rewritten. A connection carries any number of jobs, each EOF passed through
  both ways (`W-1095 Relay: a vintage Mac's LaserWriter job stalls after the driver's query`:
  the LaserWriter driver sends a query, then the document, over one connection). A Mac that
  closes after its last EOF, before the printer's (the SE/30's driver does), has that job
  finished, every byte to the printer; mid-job it is cancelled. Benched 2026-10-24 radio →
  cable with the SE/30 to a real Personal LaserWriter NTR (four jobs: query, Laser Prep by
  `exitserver`, query, document), tunnel → cable with `etpeer.py print-ps --close-early`, and
  zone → tunnel against the fake.
- TashTalk 2.1.3 is in U7, flashed in place by the ESP32 over LVP.
- A LocalTalk node on the AppleTalk port whenever U7 runs TashTalk (W-1088: every look for
  printers runs on it), answering old Macs only while the settings page's switch says so
  (W-1079; until the switch is used, DIP mode 2, and `CONFIG_AIRWRITER_LT_BENCH` always), on UART2 to
  TashTalk at 1 Mbaud with its CTS on IO6 (`components/localtalk`): acquires a server node ID,
  answers NBP LkUp as `<queue>:ImageWriter@*`, and answers PAP as Apple's ImageWriter LocalTalk
  Option Card does (status word, one connection). A PAP job is the Mac driver's own ImageWriter
  stream and goes to the printer as it is, under the job lock: an IPP job or test page makes the
  Mac wait ("busy"), a paused printer holds the Mac's reads (16 KB spool), and a job cut short
  gets W-1041's resync. The last job's bytes are also kept for `GET /api/lt/job`. `GET /api/lt`
  shows state and log; `POST /api/lt/echo` with a node number pings that Mac (AEP). The bench
  Mac has printed on paper through it. Switched off, the node offers old Macs nothing but the
  AirWriter marker and still looks for printers.
- EtherTalk (`W-1076 EtherTalk: print from Infinite Mac and Basilisk II`, `W-1078 EtherTalk over
  Wi-Fi: print from real Macs (BlueSCSI, Ethernet Macs)`, SPEC §3.13): three more links to the
  same printer, each AppleTalk Phase 2 with its own address: the board's own Wi-Fi for Ethernet
  and BlueSCSI Macs, Basilisk II's UDP tunnel (port 6066), and Infinite Mac's zone relay over a
  WebSocket to infinitemac.org (off until switched on; a made-up zone name). One job at a time
  across all four links (`at_printer.c`). `GET /api/et`, `GET`/`POST /api/et/settings`.
  `tools/ethertalk/etpeer.py` is a software Mac for the tunnel and the relay (lookup, print).
  A real Infinite Mac (System 7.5.3) printed over the zone on 2026-10-24; it registers as
  `<Unnamed>`, so its card shows it by address.
- LaserWriters on AppleTalk (`W-1082 AirPrint to LaserWriters on AppleTalk`, SPEC §3.14, design
  `docs/superpowers/specs/2026-10-21-laser-printers-design.md`): **built; benched on one real
  LaserWriter, a Personal LaserWriter NTR on board 1's cable (2026-10-24), and on the fake.**
  Since W-1088 the board looks on every running link
  (at start after 10 s, every 30 s for 5 minutes, then every 5 minutes) and puts each
  LaserWriter and AppleTalk ImageWriter it hears in the printer list; one answering from another
  AirWriter's node is skipped. A printer switched on checks no one has its name (the NTR: 8 s
  of ENQs, then five lookups of its own name 2 s apart); since `W-1098 Find a printer the moment
  it's switched on, and keep a renamed one` any link hearing that looks once, 3 s after the
  last check, so a printer is back in seconds and nothing is sent while it is off (the NTR has
  no sleep mode: off between uses is the power saving). The query also asks the page count; a
  printer new to the list at the address a silent one of the same product had, with as many
  pages or more, is that one renamed and keeps its record (uuid, AirPrint printer, settings).
  A check of a new name from the address another name was heard at ends that name there at
  once: a renamed printer answers the old name's SendStatus, which names nothing. Addresses and
  counts are NVS `printers`/`seen`, beside the list. Benched on board 1 2026-10-24: the NTR,
  off 14 minutes and back on, was ready 3 s after its last name check (19 s after its first
  frame); the fake, booted, was ready 5 s after, and renamed at its address kept its record and
  uuid (two runs). Once, before the stale-name fix, a renamed fake was never found; not since. A LaserWriter is queried (Level 1 PostScript, a binary probe) and,
  once it answers, shared on AirPrint printer 1-3 at `ipp/p<id>` (W-1082's at `ipp/laser`), its
  own Bonjour name and UUID. Jobs rip at 300 dpi into Level 1 `imagemask` bands (`ps_page`) and
  go out through one PAP client beside the server (`localtalk/pap_client.c`); `components/laser`
  holds the query, the status words, the caps and UUID, the policy (`lw_core.c`: lookups,
  status in turn, queries) and the board's side (`laser.c`). A LaserWriter's detail has a
  Start page switch (`W-1099 LaserWriter detail: a switch for the printer's start page`) where
  the printer says whether it prints one; flipping it sends `exitserver` with password 0 and
  `setdostartpage`, and the page waits for the printer's answer. Board 1's NTR took it through
  the relay (2026-10-24, DoStartPage read back false). `GET /api/laser` is diagnostics
  (heard, targets, the last job's figures); the page reads `/api/printers`. A link answers as
  the ImageWriter only where its card's switch is on, and runs for a printer in the list
  whatever it says. `etpeer.py laserwriter` is the fake (`--also NAME` for a second; it checks
  its name coming up, and `--addr`/`--pages` make a renamed one).

**Enclosure** (`enclosure/`, in use on board 1): tray (print in PETG with Arachne walls;
every wall 1.70 below the rim, one thickness all round so no lines show up the outside; the
board goes in by the sequence in `enclosure/README.md`, thumbing the west wall out past the
DIP keys), Lite frame, Lite lid, and the Pro build's light-guide lattice. Cavity 13.04 (measured mini-DIN
height); Pro screws M2.5 × 18. Four 10 mm rubber feet sit in 0.40 pockets in the floor's
underside, each hiding a screw counterbore. The shop's cased GLBs still show the pre-W-1036
tray. The Lite's clear top is 1.5 mm cast acrylic, laser file
`hardware/panel/airwriter-lite-top.svg` (`lite_top.py`, from the panel's art: gold dots, test
dot, wordmark, labels and power slash frosted; outline and Ø3.8 test hole cut), glued with
UV-cure glue to the four leg tops of the printed Lite frame (`lite_frame.py`: the lid's legs, a
rim and arms; spec `docs/superpowers/specs/2026-10-28-lite-acrylic-top-design.md`). Neither
is cut or printed yet. The all-printed Lite lid stays, its plunger bore now the panel's 3.8.

**Printers:** built and tested on the ImageWriter II only. The original ImageWriter and the
ImageWriter LQ take the same stream and are expected to work, untested (the scope spec's
amendment); docs say so rather than excluding them. The board reads an LQ's self-ID too, so a
colour ribbon in an LQ prints in colour; the reply's shape is taken from GSport's emulator and
untested on a real LQ. The original ImageWriter sends no self-ID, so the board never sees a
colour ribbon in it. The port printer's model (found, or picked in its detail) names it in IPP
`printer-make-and-model` and Bonjour `ty` (W-1088); `/api/status` `model` still says Apple
ImageWriter II. StyleWriters are designed, not built (below). PostScript LaserWriters on AppleTalk are shared as
printers of their own (above), benched on a Personal LaserWriter NTR only.

Not built: Mini vMac (LToUDP, `W-1077`), consumers of the dither override, PWG raster, HP
LaserJets over serial (`W-1083`, designed), AirPrint to an AppleTalk ImageWriter (cut from
W-1088: `W-1090 AirPrint to ImageWriters with the LocalTalk Option Card`), colour on a colour
LaserWriter (`W-1089 Colour LaserWriters: print in colour over AirPrint`; it prints in gray),
StyleWriters (`W-1103 StyleWriters on the printer port`, designed: AirPrint to the seven lpstyl
documents on J2, then vintage Macs through AirWriter as Apple's adapter; no StyleWriter on the
bench, an owner confirms on paper; a StyleWriter likely needs R9 fitted on rev A).

**Board 1:** ESP32-S3-WROOM-1-N8R2 (8 MB flash, 2 MB PSRAM). On the home Wi-Fi at
10.0.1.115, mDNS `imagewriter-2434.local`. Console on USB-C as `/dev/cu.usbmodem*` (the number
follows the Mac's port). Its queue name reads "ImageWriter II" in `/api/status`, which is also
its name in the Chooser. U7's PIC calibration words: `16E0 1DA8`. Its LocalTalk node ID is 184
(kept in NVS `lt`/`node`); the bench Mac is node 83. It was erased and set up again on
2026-10-24, with its settings put back over the API. Its port printer's uuid is now
`…4c57-0001-0001-10bda36e2434`, which orphans Mac queues added before that date. Printer: an ImageWriter II on J2 via
Apple's crossover cable, fanfold paper loaded as the Owner's Guide says (top edge under the paper
bail). It runs v0.2.0-246-g5ce940d, a bench build on no pushed branch (local branch
`bench/board1-5ce940d`): the StyleWriter branch at a38820f (W-1103) with W-1108 and the boot
wave. Its known-good slot, ota_0, holds the same without the wave (v0.2.0-245-g56cc466,
USB-installed 2026-10-28, record blank). Auto-update is on; it shares the fake "LaserWriter II
NT" on the Basilisk II tunnel when `etpeer.py tunnel laserwriter` runs on the Mac. On its
AppleTalk cable since 2026-10-24: a
Personal LaserWriter NTR (PostScript 2010.129, Level 2, 3 MB, 300 dpi, takes binary; 10,277
pages that morning), AirPrint printer 3 at `ipp/p3`. Its start page is off (set from the board, 2026-10-24). Its toner is spent (more ordered), so
its pages cannot be judged by eye yet; `statusdict /pagecount` through the relay
(`etpeer.py tunnel print-ps`) shows each page came out. Its Top margin is calibrated to -7/8 in
(`top_margin` -126): a page's line 1 in down and one 0.5 in from the bottom landed exactly there.
Its AppleTalk port has been set on the page (`lt_port` 1), so DIP switches 1-2 no longer decide;
Infinite Mac is on (Wells switched it on, with a made-up zone name). The page reads its DIP switches as 2, 6, 7 and 8 on: not
yet checked against the part. One pin of its print head is dead (a white line in solid fills).

Known issues, open:
- Infinite Mac's relay reconnected every few minutes while idle on v0.2.0-146, when internal
  RAM was short (`W-1094 Internal RAM nearly runs out during AirPrint jobs`). Not seen since
  the W-1094 build, which has not yet sat idle long on board 1; if it recurs, `/api/et`
  `relay` `why` has the client's own reason.
- Board 1 sometimes powers up in the S3's download mode (power LED only, panel dark, no
  Wi-Fi): C7, 100 nF on IO0 with no pull-up but the chip's own, holds IO0 low past EN
  (`docs/traps.md` has how to tell and how to get out without touching it). Lifting C7 cures
  it; rev B adds a 10 k pull-up and drops C7.
- Twice on 2026-10-24, after board 1 lost power (its USB moved) with the ImageWriter on, the
  printer sat busy with SELECT lit: the idle CTRL-X did not free it, switching the printer off
  and on did. A soft restart (`/api/restart`, an OTA) leaves the line ready. A job sent while it
  is stuck pauses as "Needs attention"; switching the printer off and on mid-page loses what its
  buffer held (stray characters, colours out of register).
- With the node off, TashTalk once sent and relayed nothing over the cable test's own UART2 link
  while its CTS read ready; it never recurred after a node-mode boot and the cause is unknown. The
  test retries from a reset TashTalk and logs "TashTalk answered only after a reset" (`docs/traps.md`).
- The Mac's CUPS shows "printer is offline" a few seconds into an out-of-paper pause while the
  board holds `stopped` + `media-empty-error`. Best guess: the CUPS ipp backend's write timeout.
- Wi-Fi range with the panel's copper over the antenna: −91 dBm in one spot, "Excellent" after
  moving the board. Unquantified.
- HSKo never scoped (SPEC §4.4). Board-reboot-mid-strip resync is untested.
- The bench Mac froze solid 8.7 in down its second PAP page (2026-10-19), right after a normal
  SendStatus reply; it then answered nothing, AEP included, and the board timed the connection
  out after 2 min and went Ready. Once before it sat tickling on a finished page instead
  (`LT_PAP_QUIET_MS`). If the Mac freezes again, check whether it is the same job.
- `W-1044 iPhone never submits a job on builds with W-1040's job model` is Done, but its cause
  was never isolated (likely the early reply, the strongest-AP fix and stale mDNS twins aging
  out). It recurred once on the W-1082 bench (Print tapped, 348 Get-Printer-Attributes in four
  minutes, no job) while the shared LaserWriter was offline or the board sat at −86 dBm; once
  both were right the iPhone printed. `ipp_server` now logs each request's path, asked
  attributes and answer size, so the next stall says why. The issue has the bisect plan.
- The 37 LEDs reference a 3D model KiCad 10 doesn't ship, so they are absent from STEP
  exports (`docs/library.md`).
- `W-1086 LaserWriter: a big upload to an unreachable LaserWriter holds the board ~12 minutes`:
  a LaserWriter heard by lookup but deaf to unicast fails a small job in 31 s, but an 8 MB
  upload (an iPhone photo) held the job lock ~12 min, the ImageWriter included, with
  `/api/laser` `job.active` false. Seen once on board 1, when the Mac running `etpeer.py`
  changed IP under it; unexplained, the issue has the repro. The slow photo prints filed with
  it are fixed (dropped sends, `docs/traps.md`): a Mac's photo page to the fake LaserWriter
  now takes ~100 s, bound by the 8.4 MB URF upload (65–100 KB/s) and the rip.

## Open work

Linear is the queue; these are the live items.
- `W-1091 Rev B: the main board's next revision`: the rev B list (`docs/hardware-rev-a.md`),
  with `W-1043 Rev B: through-hole dome LED for the power indicator` under it.
- `W-1046 Factory reset must always work, even with broken firmware` and
  `W-1045 Long-press factory reset with live feedback on the LED panel`: merged 2026-10-15, benched
  on board 1 (all pass). Board 1 has the W-1059 bootloader and partition table, so an image
  without W-1046 OTA'd onto it is rolled back at its next reboot, and one without W-1059 may
  overwrite the known-good slot (`docs/traps.md`).
- `W-1048 OTA updates from GitHub releases: signed, verified, optional auto-update`: merged
  2026-10-16, benched on board 1 (all pass): `/update` and GitHub both refuse tampered, foreign-key
  and unsigned images, auto-update found and kept v0.1.1, and a job mid-download wins.
- `W-1059 Known-good slot: factory reset boots the last version that worked`: benched on board 1
  2026-10-16, all pass. After its one USB flash (Wi-Fi kept), B went to ota_1, printed and became
  known-good, and C went to ota_2. A power cut 61% into D's upload left C running, and D then went
  to ota_0, not B's slot. Three power-up resets: the first booted B (0.25 s after the 5 s hold),
  one with the record blanked booted ota_0, and one with the known-good image damaged booted the
  last kept image. The panel showed the reset. v0.2.0 (2026-10-17) is the first release with it;
  a board still on v0.1.1 runs v0.2.0 over the air but gets the slot only from a USB flash.
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
- `W-1064 Colour printing over AirPrint with the ImageWriter II colour ribbon`: merged
  2026-10-17, benched on board 1: self-ID answered "IW10C"; the iPhone, which had the printer
  cached black-only, offered colour under the same UUID with no "Waiting"; a colour screenshot
  printed (960 KB of URF, just under the 1 MB job buffer). Not yet seen on this build: a Black &
  White print, a Mac queue (likely needs re-adding). Released in v0.2.0.
- `W-1066 Full-page colour photos overflow the 1 MB job buffer`: the split buffer (above),
  merged and benched on board 1 2026-10-24: a colour photo from the iPhone (3.7 MB of URF, 155 KB to
  print) uploaded in 25 s, was ripped at 27 s and printed by 3 min 47 s, fully in colour, with
  no error on the phone. Under the old buffer it would have held the upload for the whole
  print. The fake LaserWriter took a gray page as before. Internal RAM's low mark fell to 567
  bytes during that upload (27 KB free after); that is
  `W-1094 Internal RAM nearly runs out during AirPrint jobs`.
- `W-1067 Web dashboard shows which ribbon is in: colour or black` and `W-1068 Better halftone:
  dot gain curve, serpentine, blue noise, interleaved colour`: merged 2026-10-17, benched on board 1.
  The dusk page printed through Atkinson and blue noise read the same to Wells; the uncorrected
  page beat 10% and 20% dot-gain curves, so the curve ships off. The sheet is drawn at one device
  pixel per dot (half scale on a retina Mac), its type set to the measure. Five tabs wrap 2/2/1 on
  a phone. Released in v0.2.0 (2026-10-17), cut from main `b0fc219` with `--branch` from a clean
  worktree because this checkout carried someone's enclosure edits.
- `W-1076` and `W-1078` reached main on 2026-10-20 with `W-1079` (Done), which was built on
  top of them. `W-1078` is Done (the SE/30 printed over Wi-Fi on 2026-10-20). `W-1076` was
  benched on board 1 on 2026-10-24: busy both ways between the tunnel and AirPrint, a Mac
  vanishing mid-job (cut after the 60 s hold) and the relay dropping mid-job (abandoned within
  1.5 s), a real Infinite Mac print, and Erase plus setup from a phone. Wells waived the SE/30
  on the cable. The bench found two bugs, both fixed: an AirPrint job that waited for the
  printer was aborted, and the page's Erase said AirWriter had not answered. It also turned up
  `W-1094 Internal RAM nearly runs out during AirPrint jobs`. Before any release that turns
  the relay on for users, give Mihai Parparita (Infinite Mac) a heads-up.
- `W-1094 Internal RAM nearly runs out during AirPrint jobs`: 60 KB of statics moved to PSRAM
  (the EtherTalk links, the LocalTalk node, frame, JSON and reply buffers) and `/api/mem` added;
  benched on board 1 2026-10-24 (v0.2.0-150-g8088a2f). Idle: 86.6 KB free, largest piece
  31.7 KB, boot low 60.7 KB, where v0.2.0-146 had ~26 KB, 7.5 KB and a 107-byte low. An iPhone
  page over AirPrint (`fixtures/ios-20260804-232652.urf`, 143 KB, a 5-minute print) never
  polled below 75.5 KB free; `/api/mem`'s low was 68.2 KB with a 31.7 KB piece, no failed
  allocations, and the relay stayed connected. Ranked next steps, if RAM is short again: IPP
  worker stacks to PSRAM (20 KB), Wi-Fi's IRAM options off (18 KB), the rip stack to PSRAM with
  its flash writes on another task (7 KB), mDNS and the NVS cache to PSRAM.
- Not in Linear: scope HSKo during a long job (SPEC §3.9 timing).
- Laser printers (design `docs/superpowers/specs/2026-10-21-laser-printers-design.md`; a
  Personal LaserWriter NTR on the bench since 2026-10-24): `W-1082 AirPrint to LaserWriters on
  AppleTalk` merged 2026-10-22 (plan `docs/superpowers/plans/2026-10-22-laserwriters-appletalk.md`),
  benched on board 1 against the fake, then on the NTR (the design's "What only a printer
  settles" has the figures): the cable, long frames through TashTalk, ~12 KB/s on LocalTalk,
  binary, full pages in its memory, busy both ways, out of paper read mid-job, and the SE/30
  printing through the relay (`W-1095 Relay: a vintage Mac's LaserWriter job stalls after the
  driver's query`, merged 2026-10-24). Still owed on the NTR: placement and the screen by eye once
  its toner is in, and the cover-open, jam and toner texts mid-job (idle, it says "idle"
  whatever is wrong). Owed on a Level 1 machine (Plus, II NT): hex and a 68000's imaging
  time. Next
  `W-1083 AirPrint to HP LaserJets over serial (PCL)`. Network printers over TCP are out. Open
  hardware question for any RS-232 printer, the original ImageWriter included: J2's receiver
  (MAX3490, common-mode −7 to +12 V) against RS-232's −12 V.
- The full test page has printed on board 1 from the page's Print Test Page, on the colour
  ribbon. Still owed: a print from the button and from DIP4, one on the black ribbon, and a
  second tap mid-page.

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
- **ESP-IDF v5.5.1** at `~/esp/esp-idf` (esp32s3 tools). Its Python environment was built on
  Python 3.14 and Homebrew's `python3` is now 3.15: put a directory with `python` and `python3`
  linked to `/opt/homebrew/bin/python3.14` first in `PATH`, then `. ~/esp/esp-idf/export.sh`,
  then from `firmware/`:
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
- **Host tests:** `make -C tools/rip test` (raster path, halftone screens and colour, serial writer, printer_state, led_grid,
  dip_config, pic_prog, localtalk, PAP client, LaserWriter query and policy, PostScript writer,
  cable, fw_update, known_good, testpage) and `make -C tools/ipp test`.
- **Test page preview:** `tools/rip/build/rip --testpage colour --out-stream tp.bin` (or `black`,
  `unknown`), then `python3.14 tools/iwview/iwview.py tp.bin tp.png` draws it as ink on paper.
  Its text needs the printer's font dot patterns, extracted from the Technical Reference's
  Appendix C into `~/.config/airwriter/iw-fonts/*.json` on Wells's Mac and never committed;
  without them text draws as boxes.
- **Raster regression:** `make -C tools/rip regress`; `stream.bin` must match the fixtures byte
  for byte (gray, the iOS capture, and a synthetic sRGB page ripped in colour and as gray). No
  ESP32 needed. `rip --colour --out-preview` shows a colour page in its four inks; `--gain 0`
  turns the dot-gain curve off, `--dither bluenoise` picks the blue-noise screen. The checker
  fixture is pure black and white and must never change; the other three move with any change
  to a screen, the curve, the ink order or how strips are sent, and are regenerated with the
  reason in the commit. A blank strip is only its held line feed, in gray as in colour.
- **LaserWriters:** `make -C tools/rip pscheck` (needs Ghostscript, called by path: `gs` is a
  git alias here) renders the LaserWriter PostScript of the checker and two 300 dpi pages and
  requires the rip's own dots, binary and hex, also with every non-Level-1 operator trapped, and
  runs the query job; `make -C tools/rip interop` prints with the firmware's PAP client
  (`build/lwhost`) to `etpeer.py`'s fake LaserWriter over a pipe (lookup, query, print, busy,
  paper out, PostScript error, cancel, a silent printer). `tools/ipp/test/golden/` pins the
  ImageWriter's IPP answers byte for byte.
- **Firmware:** `idf.py build` clean, then `python3 tools/stackcheck/stackcheck.py` (needs the
  toolchain `export.sh` puts on PATH): every task that makes ESP-IDF's deep calls keeps 1 KB
  of its stack spare by GCC's count (W-1108). A local sdkconfig older than W-1108 fails it on
  `app_main` until its `CONFIG_ESP_MAIN_TASK_STACK_SIZE` is 4608, as in `sdkconfig.defaults`.
- **Web page:** `python3 tools/webui/preview.py`, then http://localhost:8079/_states: every state
  at desktop and phone width, light and dark, before a change to the page goes to the board.
  Its `/api/panel` plays `tools/lights`' scene frames for each state's `ps` (it builds them with
  `make`; without a C compiler the dots show unlit).
- **On board 1:** with Top margin at 0 (`POST /api/general {"top_margin":0}`), an `ipptool`
  Print-Job of `fixtures/checker.urf` prints crc32 `8ee84844`. At the default (-1 in) the
  checker, which sits in the page's top inch, prints blank (crc32 `0e5fc630`, 9 bytes).
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
  and strip ANSI from `LOG`. OpenOCD (`board/esp32s3-builtin.cfg`) beside the monitor reads
  registers live, but a gdb attach halts both cores, and detaching reset board 1 (2026-10-22):
  never with a job on paper. `thread apply all bt` names where every task waits.
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
hardware/panel/       art panel generator (Figma SVG → panel.kicad_pcb), and the Lite's
                      acrylic top laser file (lite_top.py → airwriter-lite-top.svg)
hardware/combined/    the snap-apart order panel and make_fab.sh; combined-fab.zip is the order
firmware/             ESP-IDF 5.5 app; components/ per SPEC §3.1 plus job_pipe, led_grid, pic_prog,
                      ota_health, printer_caps, ps_page, laser …; bootloader_components/ holds the bootloader's pin clamps
                      and power-up reset
tools/rip/            host build of the raster path + host tests for the pure-C cores
tools/ipp/            IPP host tests and gpa_server (the real encoder on localhost for ipptool)
tools/ippcap/         stub IPP server that captures a Print-Job body (how fixtures were made)
tools/ethertalk/      etpeer.py, a software EtherTalk Mac for the tunnel and the relay, and
                      a fake LaserWriter; test_interop.py
tools/webui/          web UI sources and its two emulator icons; mkheader.py generates
                      firmware's webui_page.h (never hand-edit); preview.py serves the page
                      in any of states.json's states (no printer, setup mode, a failing
                      relay, long names...) with no board
tools/icon/           the printer icon as a pixel grid; mkicons.py writes ipp_server/icons/*.png
tools/iwview/         iwview.py draws any ImageWriter stream (test page, a LocalTalk capture) as paper
tools/lights/         the wiki's GIF of every panel light effect, played through led_grid_core;
                      after changing an effect, `make -C tools/lights OUT=<wiki>/images/lights`
tools/release/        release.sh (build, check, sign, publish), tamper.py (refusal-test images)
tools/stackcheck/     stackcheck.py: worst-case stack of the tasks that make ESP-IDF's deep calls
fixtures/             captured URF samples and expected streams
enclosure/            3D printed case (build123d): tray, Lite lid, Pro lattice; out/*.stl committed
```

## Licence
`LICENSE.md` maps paths to licences (texts in `LICENSES/`): firmware, tools and fixtures
GPL-3.0-or-later; hardware and enclosure CERN-OHL-S-2.0; docs CC-BY-SA-4.0. The AirWriter name
and logo are not licensed. New source files carry an SPDX header; anything third-party goes in
`LICENSE.md`'s table before it's committed.
