# EtherTalk: print from Infinite Mac and Basilisk II — design

Linear: `W-1076 EtherTalk: print from Infinite Mac and Basilisk II`.
Written 2026-10-19. Adds a SPEC §3.13 (AppleTalk), which also covers the LocalTalk node as built
(W-1051, W-1053); there is no AppleTalk section today. Lifts "EtherTalk" from AGENTS.md's
"Not built" line, except EtherTalk on the Wi-Fi radio (below).

## Intent

The board prints from a real Mac over its LocalTalk cable (W-1053). Wells wants the same from
Macs that network over Ethernet rather than a LocalTalk cable, above all emulated ones:

- **Infinite Mac** (infinitemac.org): Basilisk II and SheepShaver in a browser tab.
- **The Macintosh Mini**: Wells's Raspberry Pi Zero 2 W in a Maclock shell running Basilisk II
  (`~/Projects/macintosh-mini`, sold as a kit in his shop).
- **Basilisk II on a desktop**.

Each opens the Chooser, picks AppleTalk ImageWriter, sees "ImageWriter II" and prints, the way
the SE/30 does over the cable, with the same driver and the same printout. No driver is
modified and nothing is installed in the emulated Mac beyond Apple's own AppleTalk ImageWriter.

Those Macs all speak EtherTalk (AppleTalk Phase 2 on Ethernet), so the board learns EtherTalk.
The Ethernet frames reach it two ways, neither of them the Wi-Fi radio itself:

1. **Infinite Mac's zone relay.** Infinite Mac carries each emulated Mac's Ethernet frames over
   a WebSocket to a Cloudflare Durable Object per named zone, which hands them to every other
   member of the zone. The board joins a zone as one more member.
2. **Basilisk II's UDP tunnel.** Basilisk II (not SheepShaver) can send its Ethernet frames as
   UDP datagrams on the local network instead of using slirp. The board listens for them.

## Goals, non-goals, anti-goals

Goals
- From Infinite Mac started with `?appleTalk=<zone>`, and the board given the same zone, the
  Chooser shows "ImageWriter II" under AppleTalk ImageWriter and a page prints, byte for byte
  what the Mac's driver sent.
- The same from Basilisk II with `udptunnel true` on the same network as the board, on the
  Macintosh Mini and on a desktop.
- The cable, both EtherTalk ways, AirPrint and the test page share the printer one job at a time:
  a Mac that finds the printer taken is told "busy" and waits, as a cable Mac is today.
- Nothing about the cable LocalTalk node, AirPrint or printing changes.

Non-goals (this round)
- **Internet passthrough** for tunnel Macs (the board as their router). While the tunnel is on
  in Basilisk II, the emulated Mac has no internet; that is Basilisk II's trade, not ours.
  Possible later.
- **EtherTalk on the Wi-Fi radio** (802.2 SNAP frames in and out of the ESP32's station
  interface), which would reach real Ethernet Macs through the access point. Uncertain on the
  ESP32 and not needed by any of the three targets. The EtherTalk code is written so this is a
  third transport later.
- **Routed AppleTalk networks.** Every network here has no router: nodes live in the startup
  range. A network with a router (RTMP heard) is logged and otherwise not handled.
- **The Macintosh Mini installer option** (switching its Basilisk II to the tunnel). That is the
  macintosh-mini repo's job; for the bench the Mini is set up by hand.
- **LocalTalk over UDP** (LToUDP), considered and set aside for this. It is what Mini vMac 37
  speaks (Mini vMac has no Ethernet) and what AirTalk boxes speak, so it is the next link after
  this one: the cable's LLAP half over a UDP socket, answering ENQ itself. The split below is
  made with it in mind.

Anti-goals
- Anything that can stall or break the cable node, AirPrint or a job in progress: a dead relay,
  a flood of zone traffic or a malformed frame must cost only that link.
- Connecting to infinitemac.org when no zone is set. The board makes no new connection off the
  network unless asked.
- The zone name in logs or anywhere public: it works like a password (anyone with it can print).

## What the other side does (facts, from source)

### Infinite Mac's relay (github.com/mihaip/infinite-mac at `f988f537fff0`, `worker/index.ts`,
`worker/ethernet-zone.ts`, `src/net/CloudflareWorkerEthernetProvider.ts`)

- A Mac joins zone `Z` by a WebSocket to `wss://infinitemac.org/zone/Z/websocket`. Infinite
  Mac's own page sets the zone from the URL parameter `appleTalk=Z`. Only its Basilisk II and
  SheepShaver Macs have networking (`emulatorSupportsAppleTalk`).
- Messages are JSON text frames:
  - client → relay `{"type":"init","macAddress":"aa:bb:cc:dd:ee:ff"}`: registers the client's
    MAC (lowercase hex, colons). Sent again after every reconnect.
  - client → relay `{"type":"send","destination":D,"packetArray":[…bytes…]}`: D is `"*"`
    (Ethernet broadcast), `"AT"` (the AppleTalk multicast) or a MAC string. The relay forwards
    to every other open client when D is `*` or `AT`, else to the client registered with that MAC.
  - relay → client `{"type":"receive","packetArray":[…]}`.
  - client → relay `{"type":"close"}` before closing.
- A packet is a whole Ethernet frame: destination MAC, source MAC, then type/length.
- Emulators answer a "ping" frame (payload `pingPING` + 4-byte time, 26 bytes in all) with
  `pingPONG!` + the same time, to show latency in Infinite Mac's network status.
- The Worker routes `/zone/<name>/…` straight to the zone's Durable Object: no `Origin`,
  cookie or zone-name check, and no keepalive of its own. Tested 2026-10-19: a plain Python
  WebSocket client with no `Origin` joined a zone, sent `init` and a ping to `*`, and the
  System 7.5.3 Mac running in that zone in a browser answered with a pong to the client's MAC
  (its own MAC was `b2:…`, the same `B2` scheme as the tunnel). An idle Mac sent nothing else in
  20 s.
- This format is Infinite Mac's internal plumbing, not a published interface.

### Basilisk II's UDP tunnel (github.com/kanjitalk755/macemu, `BasiliskII/src/ether.cpp`)

- Prefs `udptunnel true` and `udpport <n>` (default 6066). Basilisk II then uses the tunnel and
  not its `ether` device (slirp). Compiled in where `SUPPORTS_UDP_TUNNEL` is set: the Unix build
  sets it, SheepShaver's does not.
- Each Ethernet frame is one UDP datagram to port `udpport`, payload = the frame as is.
- The emulated Mac's MAC is `42 32` ("B2") + its host's IPv4 address. A frame to a MAC starting
  `B2` goes unicast to the IP in it; a frame to the Ethernet broadcast or the AppleTalk
  multicast `09:00:07:FF:FF:FF` goes to `255.255.255.255`; any other destination is dropped.
- Basilisk II drops received frames whose source MAC is its own, and hands the rest to the
  Mac's protocol handlers by type (any 802.3 length field counts as type 0, which is where
  AppleTalk Phase 2's SNAP frames go).
- So the board's tunnel MAC must be `B2` + the board's IPv4 address, and it must change when
  that address does.

### EtherTalk (Inside AppleTalk 2nd ed., cited in `docs/reference/README.md`: ELAP and AARP
ch. 2–3, DDP ch. 4, NBP ch. 7)

- Phase 2 frames are 802.3 with an 802.2 SNAP header: `AA AA 03` + `08 00 07 80 9B` for DDP,
  `AA AA 03` + `00 00 00 80 F3` for AARP. DDP uses the long header (network numbers).
- Broadcasts go to the AppleTalk multicast `09:00:07:FF:FF:FF`.
- AARP: a node picks a provisional address, probes it (10 probes, 1/5 s apart), and keeps it if
  nobody answers; it answers probes and requests for its own address; it keeps a table of
  AppleTalk address → MAC, filled from AARP and optionally by gleaning from frames received.
- With no router, a node lives in the startup range: network `FF00`–`FFFE`, node 1–253.

## The design

### One AppleTalk node per link

Today `lt_node.c` is the whole LocalTalk node: LLAP node acquisition, then NBP, ATP and PAP.
The printer-side half (NBP name, ATP exactly-once cache, PAP connection and reads, status word,
AEP) is untied from LLAP so it runs over either link layer:

- **LLAP** (the cable, as today): node ID acquisition by ENQ, short DDP headers, TashTalk.
- **ELAP + AARP** (new): startup-range address acquisition by AARP probe, the address map,
  SNAP framing, long DDP headers.

The board runs one node per link: the cable (DIP mode 2, as today), the tunnel and the zone.
Each has its own AppleTalk address and its own PAP connection; all three answer NBP as the queue
name, type `ImageWriter`. Every node belongs to one task, so node state stays single-threaded:
the cable node stays on the LocalTalk task as today, and one EtherTalk task owns the tunnel and
zone nodes, taking frames from the UDP socket and the WebSocket. The cable node's behaviour,
timers and logs do not change, and its host tests pass unchanged.

### One printer

All three nodes print through the same printer hooks in main.c (`lt_print_begin/write/end`
today) and the existing spool and print task: one job at a time under `s_job_lock`. A node whose
OpenConn finds the printer taken (an IPP job, the test page, another node's job) answers busy,
exactly as the cable node does now. The status word reaches every node. `/api/lt/job` keeps the
last job from whichever link printed it, and the job is named by its link
("LocalTalk job", "Infinite Mac job", "Basilisk II job").

**How long an EtherTalk Mac may hold the printer.** The printer is taken at OpenConn, and today
a connection that opens and then only tickles holds it with no limit until data arrives (the
300 s quiet rule counts only after the first bytes). On the cable that is a Mac on the desk; on
the relay it can be anyone with the zone name, or a browser tab that was throttled or
abandoned, while an iPhone's upload waits behind it. So the two EtherTalk links close a
connection that has sent no data for 60 s while the printer is ready (status word 0), before
or after its first bytes; a Mac waiting out paper-out or off line is waiting on the status word
and is never cut off, as on the cable. A job that is sending keeps the printer as long as it
sends. The cable keeps today's timers unchanged. Nothing evicts a job that holds the printer:
AirPrint and the test button wait, as they do today.

### The tunnel

- A UDP socket bound to port 6066 on the station interface. Fixed: it is Basilisk II's default
  and the Macintosh Mini's, and nobody has asked for another; no port setting.
- The node's MAC is `42 32` + the board's IPv4 address; on a new address the node takes the new
  MAC and acquires its AppleTalk address again.
- Out: frames to a `B2…` MAC go to that IP, broadcast/multicast frames to the subnet's broadcast
  address (or `255.255.255.255`; the builder checks which the Mini and a Mac host receive),
  frames to any other MAC are dropped (no Basilisk II Mac can have one).
- In: drop frames from our own MAC and anything not 802.3 + SNAP AppleTalk/AARP.
- On by default, with a setting to turn it off. Listening costs a socket and the broadcast
  chatter of whatever Basilisk II Macs are on the network.

### The zone relay

- If the relay ever refuses the board (it does not today, see above), the board does not
  pretend to be a browser: the zone link reports the refusal and Wells asks Mihai.
- Off until a zone name is set. With one, the board holds a WebSocket over TLS (the
  certificate bundle the firmware already uses for GitHub releases) to
  `wss://infinitemac.org/zone/<zone>/websocket`, sends `init` with its zone MAC after every open,
  and reconnects with backoff (1 s doubling to 60 s) while Wi-Fi is up.
- The zone MAC is a fixed locally administered unicast address derived from the board's own MAC,
  so it is stable across restarts.
- Out: AppleTalk-multicast frames as destination `"AT"`, Ethernet broadcasts as `"*"`, unicast
  as the destination's MAC string. In: `receive` frames go to the zone node; anything else is
  ignored. Malformed JSON or packets are dropped and counted.
- Answers Infinite Mac's ping frames with a pong, so the board shows up in its latency list.
- The zone name is stored in NVS, shown on the settings page (the board's own page on the home
  network) and nowhere else: not in logs, `/api/status` or `/api/lt`.

### Settings and the page

- NVS: `tunnel` (on by default) and `zone` (empty = off), applied at once without a restart, as
  W-1068's screen settings are.
- A new settings-page section, laid out by the W-1072 rules (heading, settings, then one row of
  text left and keys right): a checkbox for the tunnel, a field for the zone, a save key, and a
  line saying what each link is doing (listening, connected to the zone, a Mac seen, the last
  error). Its copy is Wells's to approve on the board before it is committed.
- The JSON API: `GET /api/et` gives each EtherTalk link's state, address and counters (zone name
  excluded), beside `/api/lt`, which stays the cable node's as it is. `GET`/`POST
  /api/et/settings` read and write the two settings for the page.

### The panel

An EtherTalk job is inked in the Apple stripes as a cable LocalTalk job is
(`LG_SRC_LOCALTALK`). The acknowledgements the cable node raises (a Chooser looking, a new Mac
heard) are raised by the EtherTalk nodes too. No new effects; any change to what the panel shows
is shown to Wells on the board before it is committed.

## Testing

- **Host tests** (in `tools/rip` beside the LocalTalk ones): SNAP framing and long DDP round
  trips; AARP acquisition (clean, a clash, a probe answered); the address map; NBP LkUp from a
  startup-range Mac answered to its MAC; a whole PAP job of `fixtures/pap-iw701-page.bin` over an
  EtherTalk node with a fake link, bytes out equal to the fixture; the cable node's existing
  tests unchanged; the relay's JSON in and out; the tunnel's MAC↔IP rules.
- **A desktop peer** (`tools/ethertalk/`, Python): an EtherTalk Mac in software that, over the
  tunnel or the relay, looks the board up by NBP and sends a fixture by PAP. It lets board 1 be
  tested without an emulator and gives the failure logs a known sender.
- **Bench, board 1** (AGENTS.md bench rules; never OTA during a job):
  - Infinite Mac in the desktop app's browser, `?appleTalk=<test zone>`, System 7.5 or Mac OS 8:
    the Chooser lists "ImageWriter II", a page prints, its crc32 matches `/api/lt/job`.
  - Basilisk II on this Mac (being built under `~/Applications/BasiliskII/`, ROM and disk image
    from a Macintosh Mini) with `udptunnel true`: the same. Then the Macintosh Mini itself
    when it is on.
  - The cable node still prints from the SE/30, and an IPP job during an EtherTalk job waits
    (and the reverse: the Mac is told busy).
  - Pull Wi-Fi or kill the relay mid-job: the job ends abandoned, the next one resyncs, the cable
    node never notices.
- **Early check:** whether Infinite Mac's System disks include the AppleTalk ImageWriter driver.
  If not, the user has to drag it in; the design still holds, the instructions change.

**What has to pass before the branch merges** (on Wells's "ship it", as any branch): the host
tests; the desktop peer printing a fixture to board 1 over both the tunnel and the relay; a real
print from Infinite Mac and from Basilisk II on this Mac, each byte for byte; the cable node
printing from the SE/30 unchanged; and Wells's OK on the settings section and the panel, seen on
the board. The Macintosh Mini is a target but not a gate: it is tested when it is on (its
Basilisk II is built from the same source with the tunnel compiled in), and its result is
recorded either way.

## Calls made without Wells (his to overturn)

- EtherTalk Macs lose the printer after 60 s of sending nothing while it is ready; nothing
  evicts a job; the cable's timers stay as they are.
- The tunnel port is fixed at 6066.
- The Macintosh Mini is tested when it is on but does not hold up the merge.
- The branch merges whole; the Mihai note gates the release, not the merge.
- If the relay starts refusing non-browser clients, the board does not fake a browser.
- Jobs are named by link in the status and logs ("Infinite Mac job", "Basilisk II job").

## Left to the builder

- How to cut `lt_node` into the shared printer half and the two link halves, and whether the
  component is renamed (prefer not: churn). The cable node's host tests are the guard.
- The AARP table's size and ageing, gleaning or not, and whether outgoing long headers carry a
  checksum.
- The WebSocket client (ESP-IDF's `espressif/esp_websocket_client` component is the obvious
  one) and the JSON handling (cJSON or by hand; a 600-byte frame is ~2.4 KB of JSON).
- Task, queue and stack sizes, and keeping TLS memory in check beside an OTA's own TLS session.
- Whether the tunnel's broadcasts use the subnet broadcast or `255.255.255.255`.
- How NVS hints carry each EtherTalk node's last address into the next boot.

## Outside the code

- Before this ships in a release, Wells gives Mihai Parparita a heads-up: the feature leans on
  his relay and its internal message format, and a change there breaks it without warning.
  Merging is not shipping (releases are cut by hand with `tools/release/release.sh`), so the
  branch merges whole when it passes, and AGENTS.md's open work says no release goes out with it
  until Wells has sent that note.
- The Macintosh Mini installer option, in the macintosh-mini repo, once this works.
