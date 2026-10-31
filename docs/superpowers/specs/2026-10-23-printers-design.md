# Printers: AirWriter finds, sets up and shares whatever printers you have (W-1088)

`W-1088 Printers: AirWriter finds, sets up and shares whatever printers you have`. Agreed with
Wells in chat on 2026-10-23. Builds on `W-1082 AirPrint to LaserWriters on AppleTalk` (merged),
`W-1087` (the ImageWriter LQ's self-ID, merged) and the settings redesign (`W-1079`).

**Amended 2026-10-30** by `W-1116 Printer port: several printers, the one plugged in shows
online` (`2026-10-30-port-printers-design.md`): the printer port keeps a record per model, the
one answering online and the rest offline, so "the printer port holds one printer" and the
"changed" notice (accept or dismiss) below no longer hold; a different printer answering is a
record of its own, once it has answered twice.

## Why

AirWriter assumes an ImageWriter. Its host name is `imagewriter-xxxx`, its default printer is
"ImageWriter II", AirPrint advertises an ImageWriter whether or not one is plugged in, the
settings page and the docs talk about "the ImageWriter", and a LaserWriter is an extra shared
"beside the ImageWriter". That is Wells's bench, not the person who bought the box. Someone
with only a LaserWriter on the AppleTalk port sees a phantom ImageWriter on their phone.

Wells, in his words: AirPrint should only show detected printers; there should be a printers
section where you say what is where; people will have one or two printers, with a long tail;
whatever their devices, AirWriter should feel personalised and specific to them, "not strongly
defaulted to some guy named Wells's config". Printer setup belongs in first-run setup, because
anyone setting AirWriter up has a printer to use right now. The person should feel confident
they are following the right steps, at ease that it is simple, and excited for the first test.

## Goals

- AirPrint, and AppleTalk for old Macs, offer exactly the printers this person has: the ones
  AirWriter found and identified, plus any they told it about. No phantom printer, ever.
- A Printers section in the settings page is the one place to see what is where, rename a
  printer, say what an unidentified one is, and remove one.
- First-run setup finds the printers, joins Wi-Fi, and ends with a test page and two lines on
  finding the printer on an iPhone or Mac.
- A printer AirWriter can identify is shared on its own, at setup or months later, the way a
  network printer just works today. One it cannot identify still shows up, for the person to
  say what it is.
- Old Macs can print to every printer AirWriter can carry a job to, from every way in that is
  switched on, including a LaserWriter on one AppleTalk link reached from another.
- Nothing in the product, the page or the docs presumes an ImageWriter, or Wells's setup.

## Non-goals

- Perfect detection. AirWriter guesses well when it can and asks when it cannot (Wells: "I
  don't think we should be expected to magically detect printers with perfect accuracy").
- Following changes on its own after setup. Adding an identified printer is automatic; moving
  or removing one is the person's to do in the Printers section.
- Printing to two printers at once. One job at a time across the box, as today.
- TCP/IP network printers, StyleWriters, and PostScript sent to a non-PostScript printer.
- The HP LaserJet over serial (`W-1083`), beyond leaving room for it.

## Anti-goals

- **Wells hand-holding the build.** His words: "spending 6 hours waiting for you to finish, and
  then having to spend another 6 hand-holding you stepwise through sweating the details." This
  document settles the whole experience, including the words on screen, and the build is checked
  end to end on board 1 (below) before he is asked to look.
- A change for anyone whose printing works today: an update keeps their printer, its name and
  identity on their phone, and their host name.
- A settings page that reads like network administration, or setup that asks more than it must.
- AirWriter sending ImageWriter commands to something that is not an ImageWriter.

## What the hardware and the network allow

Facts the design rests on (survey of 2026-10-23 at `b68d4e2`):

- **The printer port (J2)** holds one printer. The board sees one line from it, HSKo (ready).
  Unplugged, switched off, deselected and out of paper all read the same: not ready. A printer
  that is on and selected answers `ESC ?`: the ImageWriter II with `IW10C` (C: colour ribbon),
  the LQ with `LQ1CF1` (W-1087, from GSport, untested on a real LQ). The original ImageWriter
  is not known to answer. So the port can say "an ImageWriter II is here", "an LQ is here",
  "something ready that does not answer", or "nothing ready".
- **AppleTalk** (the LocalTalk cable on the AppleTalk port, EtherTalk on the Wi-Fi network, the
  Basilisk II tunnel, the Infinite Mac zone) holds many printers. Each announces a name and a
  type over NBP: `LaserWriter` for PostScript printers (Apple's and other makers'),
  `ImageWriter` for an ImageWriter with the LocalTalk Option Card. A LaserWriter answers the
  query job with its exact product (W-1082). The cable is searchable before Wi-Fi is set up;
  the network, the tunnel and the zone only after.
- **Phones** cache a printer by its Bonjour name and `printer-uuid`. A printer whose advert
  comes and goes or changes under the same name confuses iOS (the iPhone stall on the W-1082
  bench). Identities must be stable.
- **Memory.** Every AirPrint printer costs an IPP printer record (PSRAM), two mDNS services
  (`_ipp`, `_printer`) and a little internal RAM; every way in for old Macs costs a node. Jobs
  share one 1 MB job buffer, one rip and one PAP client. Internal RAM is Wi-Fi's
  (`docs/traps.md`).

## The printer record

AirWriter keeps a list of printers in NVS (namespace `printers`), replacing today's single
ImageWriter settings and `laser`'s single shared LaserWriter. Each record:

| Field | What |
|---|---|
| `id` | Small integer, stable for the record's life |
| `where` | `port` (J2), or an AppleTalk link (`cable`, `network`, `tunnel`, `zone`) plus the printer's NBP name |
| `kind` | `imagewriter` (with model II, LQ or original), `laserwriter` (PostScript), `appletalk-imagewriter`, or `unknown` |
| `name` | What phones and old Macs show; defaults from the printer (below) |
| `uuid` | Its `printer-uuid`, fixed when the record is made |
| `shared` | Offered over AirPrint (default on once identified). AirPrint only: old Macs are offered every identified printer AirWriter can carry, shared or not (see Old Macs) |
| `found` | `auto` (found and identified) or `person` (told by the person) |
| per kind | Halftone and gain; top margin and ribbon for ImageWriters; the query's answers for LaserWriters |

Up to four records are shared over AirPrint at once (one on the port, three on AppleTalk). More
are listed but not offered, with a line saying AirWriter shares up to four. Wells's "one or two,
with a long tail" fits; four keeps the mDNS service count and memory bounded.

**Names.** From the printer, never from Wells: an AppleTalk printer's own NBP name ("Office
LaserWriter"); a port printer's model ("ImageWriter II", "ImageWriter LQ", "ImageWriter"). Two
the same get " 2". The person can rename any. The name is the Bonjour instance, the IPP
`printer-name` and, for old Macs, the NBP object.

**The same printer.** An AppleTalk printer is its NBP name and type. Heard on two links (a
bridged network), it is one record on the preferred link: the cable, then the network, then the
tunnel, then the zone, as W-1082 does today.

**Other AirWriters.** Every AirWriter also registers `<host name>:AirWriter@*` on each link it
runs. A lookup skips every printer registered by a node that also answers as an AirWriter, and
its own, so a second box on the same network never shows up as printers, relayed or not.

**Identity.** A record's `uuid` never changes. The port printer of a board updated from before
keeps today's (`41575254-4552-<epoch>-<mac>`); an AppleTalk printer's is W-1082's (hash of its
NBP name and the MAC); a new port record on a new board takes the W-1082 form with the port's
own tag in place of the name hash. IPP paths are per record: `ipp/print` for the port printer
(as today), `ipp/p<id>` for the others; `ipp/laser` keeps answering for the LaserWriter of an
updated board.

## Finding printers

- **The port.** At boot, when HSKo comes up after a second or more down, and when setup asks:
  `ESC ?`, then CR, as `ribbon_check` does today. `IW` reads as an ImageWriter II, `LQ` as an
  LQ: identified. Ready with no answer reads as "something on the printer port": listed,
  not shared, and AirWriter sends it nothing else until the person says what it is. In
  particular the idle CTRL-X, the resync and `ESC ?` before jobs go only to a port printer whose
  kind is an ImageWriter.
- **AppleTalk.** On every link that is running: an NBP lookup for `=:LaserWriter@*` and
  `=:ImageWriter@*` at boot, every 30 s for the first 5 minutes, then every 5 minutes, and at
  once when the Printers section is open. A LaserWriter gets W-1082's query job to identify it
  and learn its paper and binary support. An `ImageWriter`-type node that is another AirWriter
  (its NBP name matches the box's own, or it answers a status that says AirWriter) is skipped.
- **An AppleTalk printer that won't say what it is.** A `LaserWriter`-type printer whose query
  job gets no answer is asked again twice over two minutes. Still nothing: it is listed with a
  notice, "*Name* isn't responding. Make sure it's on and connected. It isn't shared until it
  responds.", and the keys Try Again, Use Anyway (as a PostScript printer: Letter, 300 dpi,
  hex) and Remove. Add a Printer… covers the printer port
  only: an AppleTalk printer must answer a lookup to be found (printers in other zones are out).
- **Identified means shared.** A new identified printer gets a record, `shared` on, and appears
  on phones under its own name, and appears in the Printers list; no notice (a "found" notice
  that stayed until dismissed was cut: it told nothing the list didn't). A new unidentified one
  gets a record, `shared` off, and a notice asking what it is.
- **Gone is not removed.** A record whose printer stops answering stays, shown offline on the
  page and to phones (`stopped`, `offline-report`), until the person removes it. AirWriter
  cannot tell "unplugged for good" from "off for the evening", and a printer that comes and
  goes on a phone is worse than one that says offline.
- **Changed is not followed.** If a different identified printer turns up on the port (an LQ
  where the II was), AirWriter does not swap it: it shows a notice ("The printer port now has an
  ImageWriter LQ. Use it instead of ImageWriter II?") and keeps the old record until the person
  says yes. An AppleTalk printer under a new name is a new printer.

## First-run setup

The captive page on `AirWriter-xxxx` (today: one Wi-Fi form) becomes three short steps. Setup
mode starts the LocalTalk node (if TashTalk is present), the PAP client and the port check, so
step 1 can show what is plugged in before the board has Wi-Fi. The words below are the words.

**1. Your printers.** Title "Let's find your printer". While it looks: "Looking on the printer
port and the AppleTalk port…" (a few seconds; the cable lookup runs twice). Then one row per
printer found, each with its icon, name (editable), and where it is ("Printer port", "AppleTalk
port"). Under the list: "Don't see your printer? Make sure it's switched on and connected, then
Look Again. A printer on your home network shows up once AirWriter has joined it." Keys: Look
Again, Add a Printer… (the port's model picker, for an original ImageWriter or one that is off),
Continue. With nothing found, Continue reads "Continue Without a Printer" and the list says
"No printers found yet."

**2. Wi-Fi.** As today: the network list, password, Join. Above it: "AirWriter joins your Wi-Fi
so your iPhone, iPad and Mac can print to it."

**3. Joining.** "AirWriter is joining *Network*. Your phone will go back to *Network* by itself.
Then open **airwriter-xxxx.local** to print a test page." with Copy beside the address. The board
then restarts and joins, as today.

**Finish, on the home network.** The first time the settings page is opened after a fresh setup
(the board knows: `settings_take_wifi_fresh`), it opens on a sheet: "AirWriter is ready",
each printer with its state lamp and a Print Test Page key, any printer found on the network
since, and two lines: "On an iPhone or iPad: tap Share, then Print, and choose *name*. On a Mac:
choose File > Print, then *name* from the Printer menu. If it isn't listed, add it in System
Settings > Printers & Scanners." Done closes it for good.

## The settings page

The sidebar becomes **Printers, AirPrint, AppleTalk, Wi-Fi, About**. The Printer pane and the
AppleTalk pane's LaserWriters card are replaced by Printers; nothing else moves.

**Printers** (sidebar row: "Ready", "2 printers", or the most urgent state). The hero: a printer
icon, "Printers", "The printers AirWriter shares with your iPhone, iPad, Mac and old Macs."
Notices first, only what asks something (unidentified, changed, didn't answer, more than four),
each with a caution mark and its keys. Then one row
per printer, in the W-1079 row shape: icon by kind, name, where it is, a state lamp, and a
chevron to its detail. Keys at the foot: Look for Printers, Add a Printer….

**A printer's detail** (a sheet on a phone, the pane's content on a big screen): status lamp and
words (Ready, Printing, Needs attention: *the printer's own words*, Offline; W-1116,
2026-10-31: Offline alone for every printer, what to check in the wiki's Offline row); for an ImageWriter its ribbon;
Name; Model (port printers: ImageWriter II, ImageWriter LQ, ImageWriter, Something else);
Share with iPhone, iPad and Mac (switch); Halftone with its printed sample; Print Test Page;
Test Cable… (port only); Remove Printer… ("Remove *name*? It stops appearing on your iPhone,
iPad and Mac. If AirWriter finds it again, it comes back."). A port printer of kind "Something
else" shows "AirWriter can't print to this printer yet." and no Share switch.

**AirPrint** keeps its on/off for the whole box (the W-1079 switch, now all printers) and lists
each shared printer's address with Copy.

**AppleTalk** becomes the old Macs' pane: its hero says "Let old Macs print to your printers
over AppleTalk." The four ways in (AppleTalk Port, Networked Macs, Basilisk II, Infinite Mac)
keep their cards, switches, Set Up… sheets and Macs heard. The LaserWriters card goes (to
Printers). A line under the cards lists what old Macs see: "Old Macs see: ImageWriter II, Office
LaserWriter."

**About** and **Wi-Fi** are unchanged except the host name default.

A new board's ways in default to: AppleTalk Port on (setup starts the cable's node to look for
printers, and a node cannot stop once started), Networked Macs on, Basilisk II on, Infinite Mac
off, as today.

`tools/webui/states.json` gains the states this needs (no printer, one port printer, a port
printer and two AppleTalk printers, an unidentified port device, a changed notice, more than four, a printer offline, the finish sheet, each setup step), and every one is
looked at in `preview.py` at desktop and phone width, light and dark, before the page goes to
the board.

## Old Macs

One global choice, as now: the four ways-in switches. Every identified printer is offered to
old Macs on every way in that is on, whether or not it is shared over AirPrint (the AirPrint
switch and Share are AirPrint only, as the AirPrint switch is today), where AirWriter can carry the job, and never where the printer
already speaks for itself:

| Printer | Offered to old Macs | How |
|---|---|---|
| ImageWriter on the port | On every way in that is on, as an `ImageWriter` | As today: the Mac's ImageWriter stream to J2 |
| LaserWriter on link A | On every way in that is on except A, as a `LaserWriter` | Relayed (below) |
| AppleTalk ImageWriter on link A | On every way in that is on except A | Relayed |
| Unknown, or a future LaserJet | Not offered | A Mac would send a language it can't print |

**The relay.** For a printer on link A offered on link B, AirWriter registers its NBP name on B
and, when a Mac opens a PAP connection there, opens one to the printer on A with the PAP client
and passes everything through both ways: the Mac's data to the printer, the printer's output
(PostScript query answers, errors) back to the Mac, and SendStatus answered with the printer's
own status. Busy at either end is busy to the Mac. The Mac's LaserWriter driver talks to the
printer itself (its queries, its Laser Prep), so AirWriter does not rip or rewrite anything.
One relayed job at a time, and it takes the box's job lock like any job.

**AppleTalk ImageWriters** (the LocalTalk Option Card) are kept minimal: AirPrint pages go
through the ImageWriter rip and out over PAP instead of J2, with an ImageWriter II's caps; the
card's status word stands in for the port's line (bit 0x20 paper out: needs attention; bit 0x80
colour ribbon: colour). No self-ID, resync, form feed or top-margin calibration, which are the
card's business. It is built last, and cutting it touches nothing else.

This needs the PAP server to answer more than one name per link (one SLS socket per offered
printer) and to hand a connection to a relay instead of the ImageWriter spool.

## The panel, the test button, the test pages

- **Panel.** One state for the box: the most urgent of its shared printers (failed, needs
  attention, printing, ready, offline), so a quiet box with one ImageWriter looks as it does
  today. The printer-gone and printer-back waves start at the socket the printer is on (J2 for
  the port, the LocalTalk socket for the cable; none for network printers). Job animations run
  for whichever printer prints.
- **Test button.** A tap prints the test page on the port printer, or with no port printer on
  the first shared printer; with the port printer not ready it runs the cable test, as today.
- **Test pages.** The ImageWriter keeps its test page. A LaserWriter (and any PostScript printer)
  gets a new one written in PostScript, using the printer's own Times and Helvetica the way the
  ImageWriter's uses its own type: the AirWriter mark, the printer's name and product, the
  board's details, a gray ramp and the five screens at 300 dpi, a ruler and one-inch squares.
  An AppleTalk ImageWriter gets the ImageWriter test page over PAP.

## Updating a board from before

At first boot on the new firmware, a board that already has Wi-Fi credentials and no
`printers` namespace gets records made from what it has, and nothing on its phones changes:

- The port printer: kind ImageWriter, model from NVS `model`, name from `name` (or the model's),
  today's `uuid`, `ipp/print`, halftone, gain, top margin and ribbon as they were, `shared` from
  the AirPrint switch, `found` person.
- A LaserWriter shared under W-1082: its record from NVS `laser`, its W-1082 uuid, `ipp/laser`.
- The host name: a board that never set one keeps `imagewriter-xxxx`, stored explicitly, so
  bookmarks and Mac queues keep working. New boards default to `airwriter-xxxx`.
- The four ways-in switches as they were, so a board with AirPrint off and the AppleTalk Port on
  keeps its ImageWriter in the Chooser: the port record gets `shared` off and is still offered
  to old Macs.

## Docs

- README's opening: "AirWriter lets your iPhone, iPad and Mac print to classic Apple printers:
  an ImageWriter on its printer port, and PostScript LaserWriters on AppleTalk." Everything
  after follows: no "the ImageWriter" as if it were the only one.
- Wiki: Home (what you need: a printer, not an ImageWriter), Setup (the new three steps and the
  finish sheet), a new Printers page (finding, naming, unidentified printers, removing, the
  four-printer limit; it replaces the unpublished LaserWriters draft), Vintage Macs (old Macs
  see all your printers; the relay), Status Lights (the box's one state), Settings and Updates
  (rewritten for the W-1079 page it never caught up with, with Printers), Troubleshooting.
  Drafted in the wiki repo and shown to Wells before they go up.
- SPEC, AGENTS.md, traps.md as usual, in the commits that change what they describe.

## Checked end to end before Wells looks

On board 1, with its ImageWriter, the fake LaserWriter (`etpeer.py`, kept running and checked
before each step), a fake AppleTalk ImageWriter (a new `etpeer.py` mode), the Mac and the
iPhone, in this order, each step's result written down. The build stands in for people where it
can: `ipptool` with the iPhone's captured URF for the iPhone, the Mac's own CUPS queue for the
Mac, `etpeer.py` for old Macs and printers, `dns-sd` for what phones see. Board 1's NVS is read
over USB before the erase in step 2 and written back after it, so its Wi-Fi, top-margin
calibration, zone name and switches come back. Wells is needed once, at the end, in one sitting:
to type his Wi-Fi password into the real setup flow on his phone, print once from the iPhone,
and look at the pages. During the build the checker and the test pages may print on board 1's ImageWriter
without asking, in the daytime only (Wells, 2026-10-23); every other print waits for that sitting.

1. Update from the current firmware: nothing changes on the iPhone or the Mac (same printers,
   same names), the ImageWriter prints its checker (crc32 `8ee84844` at top margin 0, read on
   the USB console) and the shared fake LaserWriter prints a page.
2. Erase settings: setup shows the ImageWriter on the port; with the ImageWriter switched off,
   setup shows nothing and Add a Printer… works; join Wi-Fi; the finish sheet; a test page.
3. The fake LaserWriter appears on the iPhone by itself, unasked, and prints from it.
4. A second fake (AppleTalk ImageWriter) appears; removing it takes it off the iPhone.
5. The ImageWriter switched off: offline on the page and on the iPhone; on again: ready.
6. An old Mac (the SE/30 on the cable, or `etpeer.py` on the tunnel) prints to the ImageWriter,
   and through the relay to the fake LaserWriter on another link.
7. Every `preview.py` state, both widths, light and dark; the wiki drafts read through.

Host tests cover the record list, finding and naming rules, migration, the relay's PAP
passing, and the PostScript test page (Ghostscript renders it).

## What is left to the builder

How the record list, the per-printer IPP state and the per-printer mDNS services are coded;
the relay's buffering; the PostScript test page's layout within the contents above; the
lookups' timing within the limits above; how setup mode fits the LocalTalk node in its memory.

## Calls I made (Claude), for Wells to check

1. **Up to four printers shared** (one port, three AppleTalk); more are listed but not offered.
2. **AppleTalk ImageWriters** (LocalTalk Option Card) are supported, through the PAP client and
   the ImageWriter rip, since the pieces exist.
3. **A changed port printer is offered, not swapped**, so a misread self-ID never renames
   someone's printer.
4. **Updated boards keep `imagewriter-xxxx`**; only new boards are `airwriter-xxxx`.
5. **One job at a time across the box**, relayed Mac jobs included.
6. **The panel shows the most urgent printer's state**; the test button prefers the port printer.
7. **A new PostScript test page** for LaserWriters, in the printer's own fonts.
8. **The finish step lives on the settings page's first visit** after setup, because the setup
   page cannot follow the phone back to the home network.
9. **AirWriter sends ImageWriter commands only to an identified or named ImageWriter**, which
   changes today's CTRL-X-to-anything-on-J2.
10. **AirPrint's on/off stays one switch for the box**, with Share per printer in its detail.
11. **Old Macs are offered every identified printer, shared over AirPrint or not**, so AirPrint
    and old Macs stay independent, as today.
12. **An AppleTalk printer is its name and type**: heard on two links, one record on the
    preferred link (cable, network, tunnel, zone).
13. **Every AirWriter registers `<host>:AirWriter@*`**, and lookups skip printers registered by
    another AirWriter, so two boxes on one network don't show each other's printers.
14. **A LaserWriter that won't answer the query** can still be used as a plain PostScript printer
    (Letter, 300 dpi, hex) with Use Anyway; AppleTalk printers can't be added by hand.
15. **AppleTalk ImageWriters are minimal and built last**: the ImageWriter rip over PAP, status
    from the card's status word, nothing serial-side.
16. **New boards' ways in default as today** (Infinite Mac off), with the AppleTalk Port on.
17. **The end-to-end check uses stand-ins** (`ipptool`, the Mac's CUPS, `etpeer.py`, `dns-sd`)
    and restores board 1's settings from a USB copy after the erase, so Wells is needed once,
    at the end.
