# Laser printers: AppleTalk LaserWriters first, LaserJets second — design

Linear: `W-1082 AirPrint to LaserWriters on AppleTalk` (phase 1), `W-1083 AirPrint to HP
LaserJets over serial (PCL)` (phase 2). Written 2026-10-21, approved by Wells 2026-10-22. Design
only: no firmware has changed. Supersedes the
"LaserWriter — later" paragraph of `2026-08-04-printer-support-scope-design.md` (left untouched
here because it was being edited in parallel). No laser printer is on the bench; everything this
needs a printer to settle is listed under "What only a printer settles".

## Intent

Wells owns no laser printer and wants AirWriter to support any that would reasonably work. So
this answers three things: which laser printers can reasonably be driven from the board, by
which road, and what can be built and proven before one is bought. An iPhone or Mac should see
the laser printer in its print sheet, print a page, and be told when the printer is out of
paper, the way it already is for the ImageWriter.

**Goals.** AirPrint to every PostScript LaserWriter that has a LocalTalk or Ethernet port. Later,
AirPrint to HP LaserJet-style PCL printers over serial. Everything but the cable and the printer
itself testable on a computer.

**Non-goals.** A PostScript or PDF interpreter on the board. Colour lasers. QuickDraw-only
LaserWriters (LaserWriter SC, II SC, Personal LaserWriter SC/LS/300, Select 300): they need the
Mac to render every page. Duplex. A printer-driver framework (the scope spec's rule stands: one
`printer_caps` struct, no abstraction beyond the output stage below).

**Anti-goals.** Anything that changes how the ImageWriter prints today, its AirPrint identity
(`printer-uuid`), or the regression fixtures. A design that only works once a printer is
bought: if it cannot be tested on a computer it is not phase 1.

## The answer

1. **Build first: AirPrint to a LaserWriter on AppleTalk** (direction 2 of the four below). The
   board becomes a PAP *client* and sends each page as a PostScript `imagemask`. Every
   PostScript LaserWriter but the Select 310 has a LocalTalk port, AppleTalk carries binary at
   an estimated 12–20 KB/s (the serial port manages 0.96 KB/s at 9600 baud, and Level 1
   PostScript must send images over it as hex, doubling them), and PAP reads back the
   printer's status and errors. The board is already a LocalTalk and EtherTalk node. It can be
   proven with Ghostscript and a fake LaserWriter in `tools/ethertalk/etpeer.py`.
2. **Second: PCL raster over serial to LaserJets** (direction 4), aimed at the LaserJet 4
   family, best-effort on the II and III. It reuses the ImageWriter's port and handshake
   unchanged but needs an RS-232 cable and a check on the receive line (see "Hardware").
3. **Not now:** PostScript over serial (direction 1), and a Mac printing through the board to a
   serial PostScript printer (direction 3). Both are written down below so they can be picked up
   if a printer turns up that needs them.

**Starting `printer_caps` without a second printer on the bench.** The scope spec said to revisit
the abstraction only once a second printer is on the bench. This design starts it anyway, and
only as far as the scope spec already proposed: `printer_caps` does not exist yet (resolution,
margins, media and the URF string are constants in `ipp_attrs.c`/`ipp_attrs.h`, and `ripjob.c`
hardwires `RIPJOB_DPI_*`, `IW_MAX_DOTS` and the 0.25 in crop), so phase 1 creates it, plus one
output-stage hook in `ripjob` (ImageWriter strips or PostScript bands). The reason to go first is
that the PostScript page can be proven against Ghostscript bit for bit, which the scope spec did
not foresee. Nothing further: no driver framework.

## Which printers reasonably work

From Apple's spec sheets and the 1988 *LaserWriter Reference* (sources at the end).

| Printer | PostScript | Ports | Road |
|---|---|---|---|
| LaserWriter (1985), Plus | Level 1, 68000, 1.5 MB | LocalTalk (DB-9), RS-232 (DB-25) | AppleTalk; LocalTalk needs a DB-9 connector box |
| LaserWriter II NT, II NTX | Level 1, 68000 / 68020, 2 MB | LocalTalk or RS-422 (mini-DIN-8), RS-232 (DB-25) | AppleTalk |
| LaserWriter IIf | Level 2 | LocalTalk, RS-232/422 | AppleTalk |
| LaserWriter IIg, Pro 630, Pro 810 | Level 2 | adds Ethernet | AppleTalk (LocalTalk or EtherTalk) |
| Personal LaserWriter NT, NTR, 320 | NT Level 1(?), NTR and 320 Level 2 | LocalTalk (+ serial, parallel on NT/NTR) | AppleTalk |
| LaserWriter Select 360, Pro 600, 4/600 PS | Level 2 | LocalTalk (+ serial, parallel) | AppleTalk |
| LaserWriter 16/600 PS, 12/640 PS, 8500 | Level 2 / 2 / 3 | LocalTalk, Ethernet, parallel | AppleTalk (either) |
| LaserWriter Select 310 | Level 1 | serial, parallel; no LocalTalk | serial PostScript only (not now) |
| HP LaserJet 4, 4M, 4 Plus, 4M Plus, 4P/4MP, 5/5M | PCL 5 (M models add PS Level 2) | RS-232 serial (DB-25 on the 4 and 4P, DB-9 on the Plus and 5), parallel, MIO slot | PCL over serial (phase 2); 4M/4ML/5M with an AppleTalk card: AppleTalk |
| HP LaserJet II, IID, IIP, III, IIID | PCL 4 / 5 | RS-232 serial up to 19200, parallel | PCL over serial, best effort at 150 dpi |
| HP LaserJet 4000, 4050, 5000 | PCL 5e/6 (PS optional) | 9-pin RS-232, EIO (JetDirect) | PCL over serial; a JetDirect card speaks EtherTalk (PS models) |
| Other PostScript printers with LocalTalk or EtherTalk (NEC Silentwriter 95, QMS PS 410, GCC Elite XL, JetDirect PostScript LaserJets) | Level 1–2 | varies | AppleTalk, untested, expected to work |

Owning one is a restoration hobby either way (scope spec): fuser gears, capacitors, weight.

## The four directions

### 1. AirPrint to a PostScript printer over serial (J2) — not now

**What it takes.** The same PostScript page writer as direction 2 in its hex form, plus three
things in `serial_out`: XON/XOFF flow control (the LaserWriter II's mini-DIN-8 port has no DTR,
so the printer paces by sending XON/XOFF on its TxD; the ESP32's UART can obey XON/XOFF in
hardware, `uart_set_sw_flow_ctrl`, unverified here), a baud setting, and draining what the
printer sends back (`%%[ ... ]%%` status lines, the ^D it sends at the end of a job). A job ends
with ^D.

**Why not now.**
- Every PostScript LaserWriter but the Select 310 has LocalTalk, and on the LaserWriter, Plus,
  II NT and II NTX the mode switch picks serial *or* AppleTalk, never both.
- Level 1 serial cannot carry binary. Apple: ^C, ^D, ^Q, ^S and ^T are always acted on and
  "true binary data, such as images… is transmitted in hexadecimal" (Reference p. 145). Hex
  doubles the bytes.
- The switch gives 9600 or 1200 baud. 57600 is possible but only by writing it to the printer's
  NVRAM with `serverdict begin 0 exitserver statusdict begin 9 57600 <params> setsccbatch end`
  (channel 9 is the mini-DIN-8, 25 the DB-25), and one user reported hangs at 57600.
- Level 2 adds ASCII85 (1.25× instead of 2×) and decode filters, but Apple's Level 2 LaserWriters
  all have LocalTalk anyway, and whether they support Adobe's binary serial protocol (TBCP) is
  unknown.

**How long a page takes** (the II NT's 8.00 × 10.78 in imageable area at 300 dpi is about
970 KB as a 1-bit bitmap; 10 bits per byte on the wire; a text page about half that once blank
bands are skipped):

| Baud | Full page, hex | Text page, hex |
|---|---|---|
| 9600 | 34 min | ~15 min |
| 57600 | 5.6 min | ~2.5 min |

**Electrically** the II NT/NTX mini-DIN-8 port is RS-422 like the ImageWriter's, so J2 with
Apple's crossover cable is safe. The DB-25 ports are RS-232 (see "Hardware").

**Pick it up** if someone has a Select 310 or a serial-only non-Apple PostScript printer.

### 2. AirPrint to a LaserWriter on AppleTalk — build first

**What it takes.** A PAP client beside the board's PAP server, a PostScript page writer, a second
AirPrint printer, and a query job that asks the LaserWriter what it is. Designed in full under
"Phase 1".

**Risks and unknowns.**
- PAP throughput to a LaserWriter has never been measured anywhere we found. LocalTalk's wire
  rate is 28.8 KB/s; file copies manage about 20 KB/s. Estimate 12–20 KB/s, so 50–80 s for a
  full 970 KB page and 20–35 s for a text page in binary, twice that in hex. The printer's
  engine is 4–8 pages a minute, so the wire sets the pace.
- Binary over PAP is well attested on Level 2 printers; for a Level 1 LaserWriter on AppleTalk
  no document says either way. The query job tests it (below) and falls back to hex.
- The exact `PrinterError:` texts (paper out, jam, toner) are not documented anywhere we found.
- A 68000 LaserWriter's time to image a full-page bitmap: one 1992 report of "5–10 min to send
  and render" a 2 MB bitmap on a II NTX, link unstated.

**Needs a printer.** Throughput, the error texts, how Level 1 machines take binary, how the image
lines up on the paper, which halftone screen looks best in toner.

**Worth it: yes.** Biggest set of printers, best speed, real status, and everything but the
cable and the printer is testable on a computer. TailTalk (GPL-3.0) already does this on a desktop and has
printed on a LaserWriter 4/600 PS through TashTalk, the same chip as U7.

### 3. A classic Mac printing to a serial PostScript printer through the board — not now

**What it takes.** The board registers `<name>:LaserWriter@*`, takes PAP from the Mac's
LaserWriter driver and passes the PostScript out J2. The driver first sends query jobs (`%!PS-Adobe-3.0
Query`: printer, fonts, feature and procset queries); netatalk's `papd` and TailTalk's
`lw_bridge` both show canned answers are enough for LaserWriter 5.2 through 8, with traps:
pre-8 drivers never send EOF on a query and must be answered as soon as its `%%?End…Query`
line arrives; an `RBIUAMListQuery` must be answered `*` or the driver thinks it needs a login;
the font list must not be empty; LaserPrep's `serverdict begin 0 exitserver` must be stripped.
Over serial the Mac's binary data would also have to be turned into hex, or the driver told
`ADOIsBinaryOK?` false (unverified that LaserWriter 8 honours it).

**Why not now.** A Mac reaches any LaserWriter directly over LocalTalk; only non-Apple
serial-only PostScript printers need this, and none is known to be wanted.

**A better version, later.** With phase 1's PAP client, the board could relay PAP between an
EtherTalk Mac (Basilisk II, Infinite Mac, a BlueSCSI Mac on Wi-Fi) and a LaserWriter on the
LocalTalk cable, so an emulated Mac prints to a real LaserWriter. A relay passes the query
conversation through untouched, so no canned answers. Not designed here.

### 4. PCL LaserJets over serial — second

**What it takes.** A PCL raster page writer (small: a handful of escape sequences and PackBits),
a `printer_caps` entry per LaserJet family, a baud setting, and a cable. Sketched under
"Phase 2".

**Why it is reasonable.** The LaserJet 4 family has an RS-232 port up to 57600 baud with
DTR/DSR pacing, which is exactly the board's existing HSKi hardware handshake; PCL 5 raster
data is binary and needs no escaping because pacing is on a wire; PCL 5 compresses raster
(PackBits, delta row) and skips blank rows with `ESC*b#Y`. These are healthy, plentiful
printers with toner still made.

**Risks.** RS-232 levels both ways (see "Hardware"); the II and III are slow and short of memory
at 300 dpi; no status read-back before the LaserJet 4 (PJL), and none at all until the receive
line is safe.

**Worth it: yes, after phase 1.**

### Network printers over TCP — an aside

Ethernet LaserWriters and JetDirect LaserJets also take jobs over TCP (LPR on port 515, JetDirect
raw on 9100). Once the page writers exist a TCP transport is small, and it would AirPrint any
pre-AirPrint network laser. It also makes the board an AirPrint server for printers it is not
plugged into, which is a different product from a dongle on the printer's port. Wells's call;
not in either phase.

## Phase 1: AirPrint to an AppleTalk LaserWriter

### What Wells sees

A LaserWriter on a LocalTalk network (or plugged into J4 with one Mac-to-printer cable, if that
works; a bench item) or an Ethernet LaserWriter on the home network. On the settings page's
AppleTalk section, a LaserWriters card lists every LaserWriter the board hears, each with its
product name and status, and a switch to share one over AirPrint. Switched on, iPhones and Macs
see a second printer named after the LaserWriter ("LaserWriter II NT"), beside the ImageWriter.
A page prints in about the time a Mac would take; out of paper shows on the phone as it does for
the ImageWriter.

### Two printers on one board (my call; Wells may overrule)

The LaserWriter is a second AirPrint printer, not a mode of the first:
- The ImageWriter's identity, UUID and attributes stay exactly as they are. Changing what one
  UUID advertises is a known trap (`docs/traps.md`), and a board serving its ImageWriter should
  not stop because a LaserWriter appeared.
- **This is a refactor of `ipp_server`, not an addition.** It is a singleton today: one URI,
  UUID, job slot, print lock, stop reason, mDNS instance and set of `ipp_server_set_*` globals.
  Those become per printer. The ImageWriter must come through byte-identical: the raster
  regression, `tools/ipp` tests and CUPS's `get-printer-attributes-suite.test` against both
  printers before anything goes to board 1. mDNS already allows several instances
  (`CONFIG_MDNS_MULTIPLE_INSTANCE=y`).
- The LaserWriter gets its own `_ipp._tcp` instance with the `_universal` subtype on port 631,
  its own resource path (`rp=ipp/laser`), name, icon and attributes. CUPS shares many queues on
  one port this way.
- **Its `printer-uuid` is per LaserWriter:** MAC, a laser epoch, and a hash of the shared
  printer's NBP name. Sharing a different LaserWriter is a different printer to iOS and macOS,
  which is the truth, and the ImageWriter's UUID never moves.
- **The AirPrint switch becomes per printer.** Today's switch (W-1079) takes `_ipp` and
  `_printer` off Bonjour and refuses Print-Job for the whole server. It becomes the ImageWriter's
  switch, and the LaserWriter's share switch is its own. A board with no ImageWriter turns the
  ImageWriter's off.
- **The panel on a board with no ImageWriter** (ImageWriter AirPrint off, a LaserWriter shared):
  the printer states follow the LaserWriter in the same lights (Ready, paper out, offline), and
  the quiet power-on waits for Wi-Fi and the LaserWriter's first NBP answer, at most 30 s, rather
  than for J2's line. With an ImageWriter shared, the panel stays the ImageWriter's.
- **One job at a time across both printers and every link**, because the 1 MB job buffer and
  `job_pipe` are single. An IPP job for either printer waits behind the other as IPP jobs wait
  today (SPEC §3.4: one waiter, then `server-error-busy`); a Mac on AppleTalk is told busy.
- **A LaserWriter busy with someone else's job is a paused printer.** The job holds the board
  while it waits, up to the stall limit (600 s), exactly as a paper-out ImageWriter job does
  today: other jobs wait, an update gives way to it.
- One LaserWriter shared at a time in phase 1.

**AppleTalk roles.** Today a link's switch means "answer as the ImageWriter here", and off means
quiet (the cable's node cannot stop, so off drops every frame). A LaserWriter on J4 needs the
cable node running, but a board with no ImageWriter must not show Macs an ImageWriter. So a link
runs when either role needs it, and the NBP name `<queue>:ImageWriter@*` is registered and
answered (and PAP opens to it accepted) only on the links where the ImageWriter is shared. The
AppleTalk cards keep their switches with that meaning; the LaserWriters card's share switch turns
on the link the chosen printer was found on.

### Finding and choosing a LaserWriter

- NBP lookup of `=:LaserWriter@*` on the cable and the Wi-Fi radio link (the tunnel and the
  relay only for testing), when the LaserWriters card is opened and every 30 s while a
  LaserWriter is shared. Looking starts a link that is not running; the cable's node then runs
  until restart, as it cannot stop, but answers as nothing unless a role needs it. Also SendStatus to each found printer for the card's
  status line. HP JetDirect PostScript printers register the same type.
- Choosing one runs the **query job** (below) and saves the answers in NVS (namespace `laser`:
  NBP name, link, product, version, level, binary or hex, page size, imageable area, resolution).
  The query runs again at each boot once the printer answers NBP, and when the shared printer
  stops answering and comes back.
- The shared printer is remembered by NBP name; if two answer to it, the first on the cable wins.

### The query job

An ordinary PAP job that prints nothing, Level 1 safe, every question inside `stopped` so a
failure answers with a default rather than an error. Its output comes back over PAP (the
client's own SendData to the printer); lines starting `%%[` are status and skipped. It asks:

- `statusdict /product get`, `version`, `statusdict /revision get`;
- the language level (`/languagelevel where {pop languagelevel} {1} ifelse`);
- the device resolution (`72 0 matrix defaultmatrix dtransform`);
- the imageable area (`clippath pathbbox`) and, on Level 2, `currentpagedevice /PageSize get`.
  On Level 1 the page size is inferred from the imageable area (Letter, A4, Legal);
- **a binary probe**: it reads 256 bytes 0x00–0xFF with `readstring` and prints their count and
  a checksum. Binary is used only if they arrive intact; otherwise every page goes as hex. This
  is how a Level 1 LaserWriter's binary question gets answered without a document.

TailTalk's `pap-print query` and `dump-statusdict` are the model; they ran on a 4/600 PS.

### The AirPrint printer

As the ImageWriter's (SPEC §3.2–3.5) except:
- URF `RS300`, `W8` only (no colour), no fanfold sizes. iOS honoured `RS72-144` exactly, so
  `RS300` is expected to be honoured too; a 600 dpi LaserWriter scales the 300 dpi page itself.
- `media-supported` and `media-ready`: only the size the printer reported. Changing trays or
  sizes means `a4`/`letter` tray operators on Level 1 and `setpagedevice` on Level 2, which is
  not worth it before a printer is on the bench. A job whose URF page size differs anyway is
  placed top-left and clipped, and logged. Margins from the imageable area, rounded outward to
  whole 1/100 mm.
- `printer-make-and-model` "Apple " + the product string (whatever the printer says, for
  non-Apple ones); `printer-info` and the Bonjour name the NBP object name.
- `printer-state` and reasons from the status poller (below).
- Icon: a LaserWriter icon in the same style as the ImageWriter's, its licence handled as that
  one is in `LICENSE.md`. Which LaserWriter's icon is the builder's choice.
- A 300 dpi gray URF page is about 4.3 times the pixels of the ImageWriter's 144 dpi one, so an
  iPhone page of 140 KB there is roughly 600 KB here. It still fits the 1 MB buffer; a page with
  photos will not, and streams with TCP backpressure as today (SPEC §3.4), so streaming is a
  normal path for this printer, not a fallback. Whether iOS honours `RS300` with `W8` is
  unproven until a capture (below).

### The page: halftone, then PostScript

- **The rip** runs as today at 300 × 300 dpi (the new `printer_caps` gives `ripjob` its geometry). Same
  five screens and gain setting, kept per printer; the ImageWriter's settings are not shared.
  Default screen: the board's current default until a LaserWriter is benched. The colour path
  and the Top margin setting are ImageWriter-only; a LaserWriter page lands where PostScript
  puts it, with no tractor feed to calibrate.
- **`ps_page`** (new component, pure C, host-built in `tools/rip` like the others) turns 1-bit
  rows into PostScript:
  - DSC header (`%!PS-Adobe-3.0`, `%%Creator: AirWriter <version>`, `%%Title:` the job name,
    escaped), a prolog that defines one procedure, and per page `save`, the bands, `restore
    showpage`. No LaserPrep, no Level 2 operators. Plain PostScript without Apple's procset is
    confirmed to print on a LaserWriter (TailTalk, 4/600).
  - The page is placed so its top-left pixel is the paper's top-left corner, at 72/300 scale.
  - **Bands.** Rows accumulate until a blank row (which is skipped and ends the band) or 64 rows
    (bounded memory: 64 × 320 B = 20 KB, in PSRAM). Each band is trimmed to its first and last
    non-zero byte and sent as one `imagemask` (polarity `true`: a set bit, which the halftoner
    uses for ink, paints) reading `currentfile` row by row into one prolog string with
    `readstring` (binary) or `readhexstring` (hex). Exactly one LF follows the procedure name
    before binary data, never CR LF. A text page comes out at roughly 40–50 % of the full
    bitmap; a photo page barely shrinks.
  - No compression filters in phase 1. Level 2 `RunLengthDecode` (TailTalk uses it on the
    4/600, and notes that firmware crashed on deeper filter chains such as CCITTFax) is a later,
    measured improvement for Level 2 printers.
  - Hex lines are broken every 64 or so digits.

### The PAP client

A second PAP role in `components/localtalk`, link-agnostic like the server's node ops, beside
the server on the same node and sharing its ATP and NBP code. From the PAP research (netatalk,
Apple's CUPS `pap` backend, TailTalk; *Inside AppleTalk* itself was not readable, so every
constant below is to be checked against TailTalk's `pap.rs` before use):

- **One ATP socket** for OpenConn, the data phase and CloseConn; it is also the "responding
  socket" named in OpenConn (TailTalk PR #26: with two sockets the printer's SendData never
  arrived). A separate socket for SendStatus.
- **OpenConn**: ATP exactly-once request, user bytes `[connID, 1, 0, 0]`, data `[our socket,
  quantum 8, wait time u16]`. Reply `[connID, 2, 0, 0]`, data `[printer's session socket,
  its quantum, result u16 (0 ok, 0xFFFF busy), Pascal status]`. Some printers omit EOM on it.
- **Busy**: resend OpenConn every 2 s, the wait time counting seconds since the first try (the
  printer grants the longest waiter). The AirPrint job shows the LaserWriter's status string
  meanwhile ("busy; source: AppleTalk" means a Mac is printing) and gives up after the board's
  stall limit (600 s).
- **Data**: the printer pulls. Its SendData `[connID, 3, seq]` asks for up to 8 packets; we
  answer with up to that many Data packets of at most 512 bytes, user bytes `[connID, 4, EOF,
  0]` (byte 3 always 0: echoing the sequence there stopped a printer at 256). Sequence numbers
  start at 1 and wrap skipping 0; a repeated SendData gets a byte-identical reply, so the last
  reply is kept. Packets over 512 bytes upset a LaserWriter on LocalTalk (TailTalk, on
  hardware).
- **Reading back**: our own SendData to the printer's session socket, from the start, retried,
  brings its PostScript output.
- **End**: EOF on our last Data packet. Answer any SendData still outstanding with empty EOF
  replies, wait for the printer's EOF (with a timeout: netatalk notes some printers never send it
  on big jobs), then CloseConn `[connID, 6, 0, 0]` to the session socket.
- **Tickle** `[connID, 5, 0, 0]` every 30 s; the connection is dead after 120 s of silence
  (sources say 60 s / 120 s / 4 min; 30 s and 120 s are safe against all).
- **SendStatus** `[0, 8, 0, 0]` to the printer's listening socket; reply `[0, 9, 0, 0]`, four
  unused bytes, a Pascal string such as `status: idle` or `job: …; status: busy; source:
  AppleTalk`.
- Backpressure: the rip writes into a spool (as `at_printer.c`'s) that the client drains only
  when the printer asks, so a slow or paused LaserWriter holds the rip, never a link task.

### Status and errors

- **Poll** SendStatus every 5 s while the LaserWriter is shared, more often while printing.
- **Map** loosely, case-insensitive, on `PrinterError:` and on the printer's output during a
  job: paper out or tray empty → `media-empty-error`; jam → `media-jam-error`; cover or door
  open → `door-open-error`; toner low → `toner-low-warning`; no toner → `toner-empty-error`;
  anything else after `PrinterError:` → `other-error`. Each sets `printer-state` `stopped` and
  the job `processing-stopped`, as paper out does for the ImageWriter (SPEC §3.9), with the
  printer's own words as `printer-state-message`. "warming up" and "busy" keep the printer
  `processing`. These strings are guesses until a LaserWriter is benched.
- **A PostScript error** in the job (`%%[ Error: …; OffendingCommand: … ]%%`, then
  `%%[ Flushing: … ]%%`) aborts the job with that text in `job-state-message`.
- **No answer** to NBP or SendStatus for 30 s: the AirPrint printer goes `stopped` with
  `offline-report`, as an ImageWriter that is switched off does.

### Jobs, cancel

- A job takes the job lock, opens PAP (waiting out busy), then rips into the spool. It is
  `completed` when the printer's EOF arrives.
- **Cancel** sends EOF at once with the next Data reply. The printer meets end of file inside an
  `imagemask`, raises an error and flushes; that page does not print and pages already shown do.
- A link that drops mid-job aborts the job, as for AppleTalk jobs today.

### Settings, web and API (sketch for whoever owns `tools/webui` and `settings`)

`GET /api/laser`: the LaserWriters heard (NBP name, link, status), the shared one and its query
answers, its job and status. `POST /api/laser {"share":"<NBP name>"}` or `{"share":null}`;
`{"dither":…, "gain":…}` for its screen. `/api/status` gains a `laser` member. The page's
AppleTalk section gets the LaserWriters card; the design follows the settings redesign spec.
The panel shows a LaserWriter job as any AirPrint job: no new light effect.

### Testing with no printer

- **300 dpi pages.** The only iOS capture is at 144 dpi. Synthesise 300 dpi gray URF fixtures
  (a text page, a photo page) on the host, and capture one real iPhone page by pointing an
  iPhone at `tools/ippcap` advertising `RS300,W8` (a stub server: nothing prints). The capture
  settles whether iOS honours `RS300` and how big a text page really is.
- **The page writer on the host.** `tools/rip` gains `--ps out.ps`. Ghostscript renders it at
  300 dpi to a 1-bit image, and the test asserts it matches the rip's own 1-bit page bit for bit,
  in binary and in hex, for the checker fixture and the 300 dpi pages. The prolog is
  checked against PostScript Level 1's operator list. Ghostscript is a host tool only (it is
  AGPL; nothing of it is linked or shipped).
- **A fake LaserWriter.** `tools/ethertalk/etpeer.py` is a client only today, so this is real
  work (roughly 300 lines: NBP registration, an ATP responder, a PAP server). It gains a
  `laserwriter` mode over Basilisk II's tunnel (not on the same computer as Basilisk II, which
  holds UDP 6066): it registers `<name>:LaserWriter@*`, answers PAP as a LaserWriter does
  (OpenConn, busy on demand, printer-pulled SendData with quantum 8, status strings, its
  PostScript output read back, EOF), saves each job and renders it with Ghostscript, and can be
  told to fake paper out, a jam, a PostScript error or a dropped connection. It also answers the
  query job from canned values, binary probe included, both ways. TailTalk's `PapServer` and
  netatalk's `papd` are the references. The board's whole phase 1 then runs against it: lookup,
  choose, query, print from an iPhone, busy, paper out, cancel.
- **Host unit tests** of the PAP client's state machine, as `localtalk` has today: sequence
  wrap, retransmits, the EOF drain, the busy wait.

### What only a printer settles

On a LaserWriter on the bench, before phase 1 is called done:
- **Cable.** J4 straight into a LaserWriter II's LocalTalk port with one Mac-to-printer cable,
  as the cable test runs J4 to J2; otherwise a pair of connector boxes. The LaserWriter and Plus
  need a DB-9 LocalTalk box.
- **Long frames on the cable.** The board's LocalTalk node has only ever sent short frames; a
  PAP client answers each SendData with up to eight ~540-byte frames through TashTalk. TailTalk
  has printed through TashTalk this way, so the chip can; this board's UART feed to it cannot be
  proven without a LocalTalk peer taking long frames.
- **Throughput** on LocalTalk and on EtherTalk over Wi-Fi, for a text page and a full page, and
  whether a 68000 machine (Plus, II NT) images a full-page bitmap in reasonable time.
- **Binary on a Level 1 machine**: what the probe finds.
- **Placement**: a page with lines 1 in from each edge lands there; margins honest.
- **Memory**: no `VMerror` or `limitcheck` on a 1.5 MB Plus or 2 MB II NT with a full page.
- **The status texts** for paper out, jam, toner and cover open, read off the printer to fix
  the mapping.
- **The screen** that looks best in toner, and whether the gain curve helps.
- **Busy**: a Mac printing to the LaserWriter while an iPhone job waits, both ways.

## Phase 2 sketch: PCL LaserJets over serial

Enough to plan from once phase 1 is in; facts from HP's PCL 5 Technical Reference, the PCL 5
Comparison Guide and the LaserJet 4/4 Plus/5 service manual.

- **Targets.** First the LaserJet 4 family (4, 4M, 4 Plus, 4M Plus, 4P/4MP, 5/5M): RS-232 up to
  57600 baud, `PACING=DTR/DSR` with DTR on pin 20, PCL 5 raster compression modes 0–3 and 5,
  `ESC*b#Y` to skip rows, PJL status (`INFO`, `USTATUS`), 2 MB standard. Then best effort: the
  III/IIID (300 dpi, modes 0–3, `ESC*b#Y`; a 1 MB machine prints only about three quarters of a
  full 300 dpi page, so 150 dpi), the IIP (modes 0–2, 150 dpi), the II/IID (no compression,
  512 KB, 150 dpi, blank rows as `ESC*b0W`, 19200 baud at most).
- **The port.** J2, the ImageWriter's port, with the printer set to serial, RS-232, DTR/DSR
  pacing, and the baud the board uses (9600 default; 57600 on the 4 family). The printer's DTR
  reaches HSKi through the cable, exactly as the ImageWriter's handshake does, so `serial_out`
  is unchanged except for the baud. Never RS-422 mode on a LaserJet 4: it has no hardware
  pacing there and falls back to XON/XOFF.
- **The cable.** A Mac-to-DB-25 printer cable of the kind the original ImageWriter uses (DB-25
  models), or that plus a DB-25-to-DB-9 adapter. Printer TxD (DB-25 pin 2) left unconnected
  until the receive line is safe (see "Hardware"), so the printer is one-way: the only status is
  the handshake, as for the ImageWriter.
- **`pcl_page`** (new, pure C, host-built): `ESC E`, `ESC*t300R` (or 150), `ESC*r0F`, the cursor
  at the page's top-left, `ESC*r1A`, then per row `ESC*b2M` once and `ESC*b#W` + PackBits row,
  blank runs as `ESC*b#Y` (mode-0 printers: `ESC*b0W`), `ESC*rC` (`ESC*rB` on the III/IIID), FF,
  `ESC E`. Letter at 300 dpi is 2550 × 3300 with a logical page 2400 wide. Mode 3 (delta row) is
  the next measured step for the 4 family.
- **Which LaserJet** is a choice on the settings page's existing printer Model picker. The port's
  printer (index 0) then serves that LaserJet instead of an ImageWriter. The three ImageWriter
  models keep today's UUID, since they share capabilities; a LaserJet family gets a UUID of the
  MAC, the epoch and the family, so switching between ImageWriter and LaserJet is a change of
  printer to iOS. A LaserJet is not a third printer.
- **Times at 57600**: a text page in PackBits about 10–25 s, a full 300 dpi page up to about
  170 s; at 9600 six times that. The LaserJet's I/O timeout is 15 s by default, so a page must
  never pause that long mid-stream: the rip is fast enough, but a stalled upload would not be,
  and a buffered job (the norm) avoids it.
- **Host test**: GhostPCL renders `pcl_page`'s stream and the test compares it bit for bit.
- **Bench**: the cable and RS-232 levels; DTR pacing holding a page; the 15 s timeout; memory
  errors on a stock II/III; which modes each model really takes.

## Hardware

- **J2's receive line and RS-232.** J2's receive pair goes to the MAX3490, an RS-422 receiver
  whose common-mode range is −7 to +12 V. An RS-232 printer's TxD swings to about −12 V. So no
  RS-232 printer (a LaserJet, a LaserWriter's DB-25 port, the original ImageWriter) should have
  its TxD wired to J2's RxD until the MAX3490's absolute maximum is checked against it. Phase 2
  avoids it by leaving that wire out. **Rev B:** route J2 pin 5 to U3's spare RS-232 receiver as
  an option, so RS-232 printers can talk back (PJL status).
- **Sending to RS-232.** The board drives TxD− from 0 to 3.3 V where RS-232 expects a negative
  swing too; most receivers switch at about 1.5 V, so it is expected to work, unverified. Same
  caveat as the original ImageWriter's.
- **The LaserWriter's mini-DIN-8 serial port is RS-422**, electrically the ImageWriter's, so
  direction 1 on a II NT/NTX would be safe with Apple's cable.
- **LocalTalk needs no new hardware.**

## Licences and prior art

- **TailTalk** (FeralFirmware, Rust, GPL-3.0): PAP client, a LaserWriter emulator and an IPP
  bridge, tested on a 4/600 PS. Read as the reference; write our own C. If code is ported, check
  whether it is GPL-3.0-only or -or-later first (AirWriter's firmware is -or-later) and add it to
  `LICENSE.md`'s table.
- **netatalk** (`pap`, `papd`): GPL-2.0. Reference only; nothing copied.
- **Ghostscript / GhostPCL**: AGPL. Host test tools, never linked or shipped.
- **Apple CUPS `pap` backend**: reference for timers and retries.

## Decided by Wells (2026-10-22)

The design is approved as written.

1. **Phase 1 merges before any LaserWriter has printed**, once it passes against the fake
   LaserWriter and board 1's ImageWriter prints unchanged. Sharing is off by default and the
   docs say LaserWriters are untested.
2. **Network printers over TCP are out** for now.

## Calls made in this design (check while reading)

- The LaserWriter is a second AirPrint printer with a UUID per LaserWriter, not a mode of the
  ImageWriter; one job at a time across both.
- One LaserWriter shared at a time; only the size it reports is advertised.
- Phase 1 sends Level 1 PostScript with no compression filters; binary only where the query
  job's probe proves it, hex otherwise.
- A LaserWriter busy with another Mac's job holds the board like a paper-out ImageWriter.
- On a board with no ImageWriter the panel follows the LaserWriter.
- A link's ImageWriter name is answered only where the ImageWriter is shared, so a LaserWriter-only
  board shows Macs no ImageWriter.
- LaserJets come in phase 2 as the J2 printer's model, one-way, at 300 dpi on the 4 family and
  150 dpi on the II/III.
- The web card and `/api/laser` are part of phase 1, built after the settings work in flight now.

## Left to the builder

The NVS layout, the exact PostScript prolog text, band height (64 is a guess), the IPP resource
path's name, the icon, how `ripjob` takes its output stage (function pointers on `printer_caps`
are the expected shape), the NBP poll interval, the order of the SendStatus and NBP polls, and
how the web card looks within the settings redesign.

## Sources

- Apple, *LaserWriter Reference* (1988): bitsavers.org/pdf/apple/printers/laserwriter/Apple_Laserwriter_Reference_1988.pdf
  (ports and switches pp. 54–56, 136–146; imageable area p. 85; reserved characters p. 145).
- Apple spec sheets: support.apple.com/en-il/docs/accessories/pp206; Apple TN NW20 (PAP status),
  TN PR19 (LaserPrep), TN PR05 (memory).
- TailTalk: github.com/FeralFirmware/TailTalk (`tailtalk/src/pap.rs`, `examples/pap-print`,
  `tailtalk-gui/ipp_bridge.rs`, `lw_bridge.rs`; PRs #26, #64, #67; issue #63).
- netatalk: `bin/pap/pap.c`, `etc/papd/{session,queries,ppd,magics}.c`, papd(8).
- HP: PCL 5 Technical Reference (hp.com/ctg/Manual/bpl13210.pdf), PCL 5 Comparison Guide, PJL
  Technical Reference, LaserJet 4/4 Plus/5 service manual (Table 3-8, p. 3-33, p. 3-39).
- HP Jetdirect Administrator's Guide (2004), AppleTalk support per card.
- Adobe, PostScript Language Reference, 3rd ed. (filters, §3.13).
