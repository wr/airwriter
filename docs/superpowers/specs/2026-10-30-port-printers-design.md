# Several printers on the printer port — design

`W-1116 Printer port: several printers, the one plugged in shows online`. Agreed with Wells in
chat on 2026-10-30. Builds on `W-1103 StyleWriters on the printer port` (the branch
`ww/stylewriters-plan`, not yet merged) and `W-1088 Printers: AirWriter finds, sets up and
shares whatever printers you have` (merged). Reverses W-1088's "the printer port holds one
printer" and "changed is not followed", and the StyleWriter design's swapped-port refusal.

## Why

The printer port holds one printer record. Add a Printer… replaces it. When a different printer
answers on the port, W-1088 shows a notice ("The printer port now has an ImageWriter LQ. Use it
instead of ImageWriter II?") and the StyleWriter branch also refuses the old record's jobs, test
page and cleaning until the person accepts. That fits one printer per port. It does not fit
someone who owns an ImageWriter II and a StyleWriter and moves the cable between them.

Wells, in his words: "i'd prefer to just tell airwriter i have both, and then be able to see from
the web portal or airprint that one is online, the other is offline, or both are offline."

And the reason behind every rule below: "i want airwriter to work in a predictable way, and a
sensible way." A new owner adds their printers on the settings page, sees them offline, and plugs
one in to try it end to end. "if the device shows as 'offline' (nothing changes) when i plug in
my printer, i'm going to guess that there's something wrong. do i need to reboot it? is the cable
bad? is my printer broken? i have a reasonable expectation (driven by using modern devices,
mostly) that plugging in (or unplugging, or turning something off, on) is in itself a signal.
if we can't match that perfectly, that's fine. but the *experience* should match. ideally
without having to go into web portal and click a button."

## Goals

- The printer port keeps a record for every printer the person uses on it, found or added: one
  per model.
- The printer answering on the port is online. Every other port printer is offline. Nothing
  answering: all of them offline. Phones and the settings page show the same.
- Plugging a printer in, unplugging it, or switching it on or off shows on the settings page and
  on phones by itself, within seconds, with no button. The original StyleWriter is the one named
  exception.
- Each port printer stays its own AirPrint printer, with its own name, `printer-uuid` and
  capabilities, advertised whether or not it is plugged in.
- AirWriter shares up to six printers, any mix of the printer port and AppleTalk (was four: one
  on the port, three on AppleTalk).

## Non-goals

- Sensing electrically that something is plugged in. Neither board can (below), and adding it is
  tabled (Wells: "this is probably too fussy... so let's table the idea for now").
- Two printers of the same model on the port. Nothing on the wire tells two ImageWriter IIs
  apart, so they are one record and print as one.
- Printing to two port printers at once. One job at a time across the box, as today.
- Finding the original StyleWriter on its own. It answers only after a reset (below); it is
  found at start-up and on Look for Printers, as built (Wells: "if there's a 1991 stylewriter in
  the mix (ie: added by user) then we can deal with the complexity").
- Adding an AppleTalk printer by hand. A LaserWriter still appears on its own when it is switched
  on, as W-1088 and `W-1098 Find a printer the moment it's switched on, and keep a renamed one`
  built.
- StyleWriter phase 2 (vintage Macs through AirWriter to a StyleWriter). Its design is the
  StyleWriter design's.

## Anti-goals

- A printer that comes and goes on a phone. Every port printer stays advertised; only its state
  changes (W-1088: "a printer that comes and goes on a phone is worse than one that says
  offline").
- A printer shown online that is not there, or offline when it is.
- One family's bytes reaching the other: ImageWriter commands or page data to a StyleWriter, or a
  StyleWriter job's page data to an ImageWriter (the StyleWriter design's rule, kept). The
  StyleWriter's questions and its reset may reach an ImageWriter; the bench showed them harmless
  (below).
- A notice or a button for something the person did on purpose. Swapping printers is not an
  error.

## What the board can see

Facts the design rests on (survey of 2026-10-30 at `4e798b8`):

- **No presence sense on either board.** Every J2 line goes through a receiver that gives a plain
  yes or no. The handshake (J2.2, HSKi) is an RS-232 receiver: up means an ImageWriter that is on
  and selected; unplugged, switched off, deselected and out of paper all read down. The receive
  pair (J2.5, J2.8) on rev 2026.11 is the THVD1451, whose fail-safe reads an open or idle line as
  mark, on purpose. J2.7 (GPi) is not connected. Rev 2026.11's spare pads (IO38–IO40) could carry
  a trial later; the idea tabled was J2's shell to a spare pad in place of R8, read through the
  cable's shield and the printer's chassis.
- **An ImageWriter** is seen by its handshake line at once and answers `ESC ?`: `IW10`… the II,
  `LQ`… the LQ. The original ImageWriter answers nothing; its line up is its answer.
- **A StyleWriter** may never raise the line. The II, 1200 and colour models answer the plain
  question (`?` at 57600): `SW`, `SW3`, or `CS` and then the byte after `p` for the colour
  member. The original StyleWriter answers only after the full sequence, which starts with a
  reset (`00 FF FF FF 49`, eject and reset).
- **Asking is cheap.** The plain question is one byte, then up to 250 ms for a first byte back,
  under the job lock. Sent to an ImageWriter II at 9600 it is not heard: 300 of them a second
  apart, then the full sequence once, left the paper blank on the bench (`docs/traps.md`,
  2026-10-27).
- **An ImageWriter's model is known about 2 s after its line comes up.** `ESC ?` goes 2 s after a
  line-up that followed a second or more down (`main.c`, W-1064: ask once the printer settles),
  then at the port task's next 500 ms tick. Today's Ready comes from the line alone, after its
  1 s settle.
- **Bonjour** allows 10 services (`CONFIG_MDNS_MAX_SERVICES`, the component's default; not
  pinned in `sdkconfig.defaults`). Each shared printer is one `_ipp` service; the printer at slot
  0 adds `_printer`; the settings page is `_http`. Six printers use 8.

## Port records

The printer port holds any number of records within the list's 8, at most one per model:
ImageWriter II, ImageWriter LQ, ImageWriter (the original), and each StyleWriter model. A
"Something else" record stays one per port, as built.

**Which one is on the port.** At most one port record is *answering* at a time:

- An ImageWriter II or LQ record: its line is up and `ESC ?` named its model since the line came
  up.
- An original ImageWriter record: its line is up, and `ESC ?` and the plain question got nothing.
- A StyleWriter record: the plain question (or the full sequence, for the original) named its
  model, and its status checks still answer.

The answering record shows its own state (Ready, Printing, Needs attention). Every other port
record is Offline. An ImageWriter record reads Ready once its `ESC ?` answer is in, not from the
line alone, so the page never shows the wrong ImageWriter for a moment; that is about 2–3 s
after the line comes up, and the 2 s settle stays. It goes offline 3 s after its line drops (the
debounce today). A StyleWriter record goes offline at its first unanswered status check.

**A printer AirWriter has no record for.** An answer naming a model with no record makes one,
named by its model, shared at once if there is room (W-1088's "identified means shared"). It
takes the same answer twice in a row, so a garbled reply never adds a printer. A line up with
nothing said is the "Unknown printer" notice, as built, unless an original ImageWriter record is
in the list. An Unknown printer record goes when an identified printer answers on the port (it
was that printer, not yet identified); today that answer converts it, which can't happen when
the model already has a record. The notice's model list marks models already in Printers as
added.

**Something else.** The person's "leave this device alone" for the printer port. While a
Something else record is in the list, AirWriter asks nothing on the port on its own, as built,
and every other port record reads Offline with the line "The printer port is set to Something
else." One change: Look for Printers, the person's own request, asks once even then (as built it
asks nothing). A printer that answers it replaces the Something else record. Add a Printer…
replaces it too, as `pr_add_port` does today, rather than adding beside it.

**What goes.** W-1088's "changed" notice (accept or dismiss) and the StyleWriter branch's
swapped state: the port conflict, the refusals that follow from it, `/api/printers` `swapped`,
the remembered dismissal, and the notice's line "Or connect the StyleWriter again and press
Look for Printers." A different printer answering is that printer online and the last one
offline. A misread can at worst add a record the person removes; it can no longer rename or
take over someone's printer, which was W-1088's reason for the notice.

**Adding by hand.** Add a Printer… adds a port record for a model not in the list (an original
ImageWriter, or a printer that is off right now). Models already in the list show as added and
can't be picked. The same holds for a record's Model in its detail. Removing the record of the
printer that is answering brings it back at the next look, as the Remove sheet's own words say.

## What the port is asked

The StyleWriter design's `port_policy`, with "the record" read as "the printer that answered
last", and three changes (in bold). In order:

1. A Something else record in the list: nothing on AirWriter's own, as built. **Look for
   Printers asks once** (above).
2. A StyleWriter answering: `1` and `2` every 10 s, as built. **Each check also asks `?` (and `p`
   after `CS`, as a look does), so a different StyleWriter put in its place shows within one
   check.**
   **Amendment (R11, 2026-10-30): a line-up edge or a look while a StyleWriter answers asks its
   status at once, not at the next 10 s check, so an ImageWriter put in its place (its line
   coming up) is found in about 1.5 s. The status bytes at 57600 are harmless to an ImageWriter
   (bench 2026-10-27). The board no longer drops the line-up edge for an answering StyleWriter
   (W-1116 task 5).**
3. A line-up edge after a second or more down, or a look with the line ready: the plain question
   first where any StyleWriter record is in the list, the port last answered as a StyleWriter, or
   an original ImageWriter record is in the list; `ESC ?` first everywhere else; then the other
   if the first got nothing. As built, with "a StyleWriter record" read as any of them: a
   StyleWriter that raises its line and hears `ESC ?` first may stop listening until a reset,
   and the plain question costs an ImageWriter about 250 ms at line-up.
4. **Nothing answering: the plain question every 5 s** (was 60 s), whatever is in the list, so a
   StyleWriter plugged in is found in seconds by someone who never added it.
5. An original StyleWriter record in the list: the full sequence at start-up and on Look for
   Printers when nothing else answered, as built.
6. The CTRL-X kick, as built: only after a plain question since the line went down got nothing,
   and never while a StyleWriter answers.

Never while a job runs on the port, as built.

**How long things take.**

| Printer | Switched on or plugged in → Ready | Switched off or unplugged → Offline |
|---|---|---|
| ImageWriter II or LQ | 2–3 s (line up, the 2 s settle, `ESC ?`) | 3 s |
| Original ImageWriter | About 3 s (line up, the settle, `ESC ?` and the plain question unanswered) | 3 s |
| StyleWriter II, 1200, Color 1500–2500 | Within 5 s | Within 10 s |
| Original StyleWriter | On Look for Printers or at start-up | Reads Ready while quiet once found, as built |
| LaserWriter on AppleTalk | In seconds, as built (W-1098) | 30 s, as built |

## Jobs to a printer that isn't plugged in

**Another printer answers on the port.** The job's printer is certainly not there. Print-Job is
answered `server-error-busy`, its body read to the end, as when another job holds the printer
(SPEC 3.4): the phone or Mac keeps the job and asks again, and it prints once the person swaps
the cable. Nothing is held on the board, so the printer that is there, and every other printer,
keep printing. The absent printer reads `stopped`, `offline-report`, "Offline", never why
(R27).

This rests on how iPhones and Macs retry, which nobody has watched for minutes on end. Gate: on
the bench, an iPhone and a Mac print to the absent printer and the bench records how long each
keeps the job, how often it asks, and what it shows. It passes if both keep the job for 10
minutes or more (today's wait for a switched-off ImageWriter) and show something a person reads
as "offline" or "waiting". Otherwise the fallback: the job is taken and ends aborted at once,
with the same message, as the StyleWriter branch's refusal does today.

**Nothing answers on the port.** AirWriter can't tell a switched-off ImageWriter from one that
is out of paper or deselected, so this stays as today: the job is taken, the phone shows it
paused, and an ImageWriter job waits up to 10 minutes (`CONFIG_AIRWRITER_STALL_ABORT`); a
StyleWriter job ends after its 30 s of silence, as built. Two additions keep the anti-goal:

- An ImageWriter job that starts on a quiet port sends no page data until its own model has
  answered `ESC ?` (an original ImageWriter record: its line up and the plain question
  unanswered). Another ImageWriter model answering ends the job aborted ("Another printer was
  connected. Print again."), since its page was ripped for the model it was sent to.
- While it waits with nothing sent, it asks the plain question every 5 s. A StyleWriter that
  answers ends the job the same way and comes online.
- A StyleWriter job already sends page data only after its own model has answered its `?`
  (as built). Its reset at the start of the job may reach an ImageWriter that is deselected or
  out of paper; on the bench the reset printed nothing on an ImageWriter II
  (`docs/traps.md`, 2026-10-27, once), and the overnight run below repeats it.

**Vintage Macs.** Macs see one ImageWriter for the printer port, as today: the node's own NBP
name, from the oldest ImageWriter record, fixed across swaps so a Chooser selection keeps
working. (As built, `printers_offers` lets the last port ImageWriter overwrite the name, and the
queue name is set once at boot from the first port record; both follow the oldest record now.)
A Mac's ImageWriter job goes to whichever ImageWriter answers on the port (the II, LQ and
original take the same stream). While a StyleWriter answers, the Mac hears busy and waits, as on
the StyleWriter branch. While nothing answers, as today. The AppleTalk pane's line "In the
Chooser on vintage Macs: …" lists what Macs are offered: that one ImageWriter and the AppleTalk
printers. As built it lists every identified printer, which would name each port printer, and
StyleWriters that phase 1 doesn't offer to Macs.

**Test page and cleaning.** The test button and DIP4 go to the printer answering on the port,
else the first shared printer that is ready; with port records and nothing answering, the
button runs the cable test, as today. The page's Print Test Page and Clean Print Head on a port
printer that isn't answering, while another one is, answer 409: "Connect *name* to the printer
port first."

## Six shared printers

`PR_SHARE_MAX` 6, any mix; the separate AppleTalk cap goes. `IPP_PRINTERS` 7 (ruling R14,
2026-10-30: slot 0 is the oldest port record's alone and stays empty while that record is not
shared or there is none, so the six others need slots of their own; at most six are on at
once). Bonjour 8 of 10
services, with `CONFIG_MDNS_MAX_SERVICES` pinned in `sdkconfig.defaults` (releases build from it
alone). The list stays at 8 records. A seventh identified printer is listed, not shared, with
the "more than six" notice (today's "more than four", renumbered).

**Paths.** A record's `printer-uuid` never changes while it exists, and nor does its own path:
every record answers at `ipp/p<id>`, port records included. The oldest port record is also
advertised at `ipp/print`, with the legacy routing of any other path, so the port record a
board had before keeps the path its phones know. Removing the oldest port record moves
`ipp/print` to the next one, whose `ipp/p<id>` keeps working (ruling R9, 2026-10-30: the
`rp` in `/api/printers` names the advertised path; the board routes both). Each port record
needs its own uuid:
today's port form is the list's epoch and MAC with a family tag (`0001` ImageWriter, `0002`
StyleWriter), the same for an ImageWriter II and an LQ, so a new port record's uuid also carries
its model. Existing records keep theirs.

**Costs to check.** Each shared printer adds one Bonjour service, its IPP state (PSRAM), and the
status polling of every phone and Mac that has it added, against 16 sockets for the whole box.
The bench check below covers it on a rev A board (N8R2, 2 MB PSRAM), the smaller of the two.

## The settings page and phones

- **Printers list.** Every port printer has its row: icon, name, "Printer port", its lamp. The
  one answering shows its state; the others show Offline. No notice for a swap.
- **A port printer's detail**, offline for any reason (another printer answers, nothing does,
  a Something else record is listed): "Offline", the phone's word exactly, as for every
  offline printer (R28); never why (R27). Print Test Page and Clean Print Head on a printer
  whose port another is on still answer "Connect *name* to the printer port first." (an
  action's refusal, not a status line).
- **Add a Printer…** adds. Its model list marks models already in Printers as added.
- **Notices** left: the unknown port printer, a LaserWriter that won't answer, more than six.
- **Phones** see every shared port printer all the time, each `stopped` + `offline-report` while
  it isn't the one answering.
- **Panel.** As built: idle, the box is ready while any shared printer is. A swap plays the
  printer-back wave at J2 once, not printer-gone then printer-back.
- **API.** `GET /api/printers`: each port record's live state says whether it is the one
  answering; `swapped` goes. `POST /api/printers {"add_port":"<model>"}` adds a record, 400 with
  a sentence when that model is in the list (as does `model` on a record). `/api/status`
  `model`: the answering port printer's, else as built.

`tools/webui/states.json` gains: two port printers with one ready and one offline; two port
printers both offline; a swap in progress (one offline, the other just found); six shared
printers and a seventh with the notice; Add a Printer… with a model already added. **Mocks of
these go to Wells before the page is built** (his rule: anything visual is seen first), at phone
and desktop width, light and dark.

## Storage and updates

- The list's blob gains nothing per record; a second port record is what's new. `pr_save` writes
  today's version until the list holds a second port record, so a rollback leaves a one-port
  list untouched. A board rolled back with two port records keeps the newer list in NVS
  unwritten, as built since W-1103 (`pr_blob_newer`, `s_keep`): the older firmware runs with a
  list of its own in RAM, its printers found again each boot, and the two-port list is there
  for the next update.
- A stored "changed" notice is dropped on load; its printer becomes a record at the next look if
  it answers.

## Where it's built

On the StyleWriter branch, as `W-1116` commits, merged with or right after W-1103's phase 1, so
the swapped-port refusal never ships to anyone. The "changed" notice, on `main` since W-1088,
goes in the same merge.

## Docs

In the commits that change what they describe: SPEC 3.15 and 3.16 (the port, notices, sharing,
the asking order, jobs), an amendment block in the printers design and the StyleWriter design
pointing here, `docs/traps.md` for whatever the bench teaches. After Wells approves the change:
the wiki's Printers page (several printers on the printer port, the one connected is Ready; six
shared), Troubleshooting ("A printer shows Offline": connect it and switch it on), and the
shop's AirWriter description wherever it says four or one printer per port.

## Checked before Wells looks

Host tests (`make -C tools/rip test`, `make -C tools/ipp test`):

- `port_policy` against scripted printers for ten minutes, every swap among an ImageWriter II,
  an LQ, an original ImageWriter, a StyleWriter II, a Color StyleWriter 2500 and an empty port:
  each printer's record is answering within the table's times and every other port record
  offline; a quiet port gets the plain question every 5 s and nothing else; a StyleWriter swapped
  for another model within one 10 s check is caught at that check; a Something else record
  silences the port.
- The list: one record per model, a duplicate refused, Add a Printer… adds, a reply has to come
  twice to make a record, no "changed" notice, the blob's version only with a second port record,
  six shared and the seventh's notice.
- IPP: busy for a port printer that isn't answering while another is, its state and message;
  six printers routed by path; `tools/ipp/test/golden/` updated with the reason.
- An ImageWriter job on a quiet port sends nothing until an ImageWriter answers, and ends when a
  StyleWriter answers.

On the bench board, with its ImageWriter II, the fake LaserWriter (`etpeer.py`), the Mac and the
iPhone:

1. A StyleWriter II record added by hand: both rows show, the StyleWriter Offline (on the
   page and the iPhone), the ImageWriter Ready.
2. The ImageWriter switched off: Offline within 3 s on the page and the iPhone; on: Ready
   within 3 s, and never the wrong ImageWriter on the way.
3. The busy gate above: the iPhone and the Mac print to the StyleWriter record; how long each
   keeps the job, what it shows, written down.
4. Six shared printers (the two port records and four `etpeer.py` fakes): `/api/mem` steady over
   ten minutes, and every printer answers Get-Printer-Attributes in under 2 s with the iPhone,
   the Mac and `ipptool` polling.
5. A night of the 5 s plain question into the ImageWriter while it is deselected, with the
   StyleWriter's full sequence (the reset) 20 times along the way, then the checker prints crc32
   `8ee84844` at top margin 0 (one print, with Wells's yes).
6. If a USB serial adapter is to hand, `swfake.py` on J2 in the StyleWriter's place: the swap
   both ways inside the table's times. Without one, the host tests carry the StyleWriter side
   and an owner confirms on paper, as the StyleWriter design already plans.
7. Every new `preview.py` state at both widths, light and dark.

## Decided by Wells (2026-10-30)

1. Several printers on the port, each remembered; the one connected online, the others offline.
2. Share up to six printers ("Raise limit to 6").
3. Hardware presence sensing is tabled.
4. Plugging in, unplugging and switching on or off show by themselves, with no button; an exact
   match isn't needed, the experience is.
5. The original StyleWriter may stay the exception.
6. Vintage Macs keep one ImageWriter for the printer port, named from the oldest ImageWriter
   record and fixed across swaps, rather than one per ImageWriter as phones get.
7. (2026-10-31, from the mocks) One kind of Offline. A status line is one line of help, the
   phone's words exactly, never a variant for one situation; the Offline lines that named the
   printer on the port, or the port's Something else record, were colon reveals with a hidden
   "not X, but Y". Why a printer is offline belongs in the wiki's Offline troubleshooting row
   (R27).

## Calls I made (Claude), for Wells to check

1. **Busy, not refuse,** for a job to a port printer while another answers, gated on the bench;
   refusing at once is the fallback.
2. **Nothing answering keeps today's wait**, because a switched-off ImageWriter and one out of
   paper look the same; an ImageWriter job sends nothing until an ImageWriter answers.
3. **One record per model** on the port.
4. **The 5 s look runs whatever is in the list**, so a printer nobody added is still found; the
   overnight run on the ImageWriter is its gate.
5. **Each StyleWriter status check confirms the model**, so a quick swap between StyleWriters
   can't print to the wrong one.
6. **A new record takes the same answer twice.**
7. **Every port record answers at `ipp/p<id>`**, and the oldest one, the record a board had
   before, also at `ipp/print`, which moves to the next port record when it is removed (Paths,
   ruling R9).
8. **A Something else record still silences the port**, but Look for Printers asks once and a
   printer that answers replaces it. It's rare, and the person chose it.
9. **A swap plays one printer-back wave.**
10. **Built on the StyleWriter branch and merged with it**, so the refusal never ships.
11. **An ImageWriter reads Ready once its `ESC ?` answer is in** (2–3 s), not from the line
    (about 1 s today), so the page never shows the wrong ImageWriter.
12. **The plain question goes first at a line-up whenever any StyleWriter record is in the
    list**, so a StyleWriter never hears `ESC ?` first.
13. **A waiting ImageWriter job ends when a different printer answers**, ImageWriter or
    StyleWriter: "Another printer was connected. Print again."
14. **An Unknown printer record goes when an identified printer answers on the port.**

## Left to the builder

How port records and the answering one are held in `printers_core` and `port_policy`'s inputs;
the IPP slot each shared record takes; where the twice-seen rule lives; the shortest line-down
that still re-asks `ESC ?` while idle (an A/B switch box flipped quickly should be re-asked, and
flow control mid-job must not); the exact words, in the copy pass, within the voice guide.

## Amendments (2026-10-30, while building)

Rulings made during the build that change what the sections above say:

- **R11** (What the port is asked, rule 2): a line-up edge or a look while a StyleWriter
  answers asks its status at once, not at the next 10 s check; an ImageWriter put in its
  place is found in about 1.5 s from the edge.
- **R16** (Port records, a printer AirWriter has no record for): the first answer that would
  change the list is asked again even when the model has a record of its own, so one answer
  never replaces a Something else record or drops an Unknown printer record; the second in a
  row does.
- **R17** (Which one is on the port): a line-up while the link is dirty after a job cut short
  keeps the ImageWriter record `ESC ?` named, since the idle `ESC ?` waits for a job's resync;
  so does a line-up mid-job (a paper reload).
- **R19** (Adding by hand): removing the record on the port forgets the port's last answer, as
  a model change does, and the port is asked again at once.
- **R20** (What the port is asked, rule 4): an ImageWriter II or LQ record on the port whose
  `ESC ?` answer was lost gets the 5 s plain question and then `ESC ?` when it got nothing,
  while its line is up; not while a Something else or Unknown printer record is listed, not
  on a dirty link, not before the line-up's own question has run (the 2 s settle), and not
  while a StyleWriter answered last.
- **R27** (Wells, 2026-10-31, The settings page and phones; Jobs to a printer that isn't
  plugged in): one kind of Offline. A port printer whose port is another's reads its family's
  one Offline line on the page and the phone, the same as with nothing answering; the two
  reason sentences go from the firmware (`pr_port_say_words`, `PR_SAY_MAX`) and the page.
  The phone's reason keyword stays `offline-report` for a taken port. The 409 "Connect *name*
  to the printer port first." on Print Test Page and Clean Print Head stays: it refuses an
  action, it is not a status line. The reasons go in the wiki's Offline troubleshooting row.
- **R28** (Wells, 2026-10-31, from the bench): Offline is a status name alone, "Offline", on
  the page and the phone, for every printer of every kind (ImageWriter, StyleWriter,
  LaserWriter), not only port printers: "check power and cable" pointed a printer that isn't
  plugged in, because another is, at the wrong fix. What to check goes in the wiki's Offline
  row. Print Test Page's own refusal on an offline printer keeps its line of help.
