# Log — design

`W-1136 Settings page: a technical log that survives crashes and being unplugged`. Agreed with
Wells in chat on 2026-11-08 and built that day. Amended in place for what the bench and Wells's
look at it on a board changed. `docs/SPEC.md` §3.17 says what it does.

## Why

Owners have no way to find out why something went wrong. The firmware writes about 230 log
messages (57 error, 66 warning, 110 info), but only to the USB console, which takes a cable and
ESP-IDF's monitor, and nothing survives a restart. After a crash, `/api/status` can give only the
reason for the last reset. Once boards are in other people's hands, whoever helps an owner has
nothing to go on.

Wells chose a technical log over a plain-language job history: "i think keeping it technical is
ok. with a typical pattern of info, warn, error etc". Its readers are owners comfortable reading
one, and the people those owners ask for help.

## Goals

- An owner opens a log on the settings page, at desktop or phone width, and sees what the board
  did, at info, warning and error levels.
- The log covers the time before a crash, a restart or being unplugged.
- An owner can copy or download it and post it publicly without giving away anything personal.
- It reaches existing boards through a normal update: no USB flash, no partition table change.

## Non-goals

- A plain-language history of jobs.
- Sending logs anywhere automatically, or any new connection off the board. No syslog, no network
  console.
- Real clock times on the lines. The board has no clock (no SNTP, no time calls anywhere in the
  firmware). The one real time is when the current start began, from the viewer's own clock.
- A debug level, a log level setting or an on/off switch. `CONFIG_LOG_MAXIMUM_LEVEL` is info and
  only three `ESP_LOGD` lines exist, so there is nothing to turn up.
- Changing what a person with a USB cable sees, other than the name removals below.

Failure even if it works: a job that slows, fails or loses dots because of logging; a log
containing a document, person or network name; internal RAM getting tighter
(`W-1094 Internal RAM nearly runs out during AirPrint jobs`); a setting that needs explaining.

## What people see

**The row.** A **Log** row on About, in the group with **On for** and **Last started**. Its value
is the number of errors since this start ("1 error"), in the failure red, and nothing when there
are none. It opens in place the way a printer's details do, with a `‹ About` button to go back.
Chosen by Wells over a sidebar item and over download-only.

**The screen.** At the top: `‹ About`, a **Show** popup (Everything, Warnings and Errors, Errors),
**Copy** and **Download**. Below, one box of monospaced lines, newest first, updating while it's
open. There is no auto-scroll: new lines arrive at the top, and someone who has scrolled down
stays where they are. Wells chose this over oldest first with following, on seeing that one on a
board. Each line has the time since that start (`+04:53`, `+26:14:32`), the level (`INFO`, `WARN`,
`ERROR`), the source and the message. At phone width the time, level and source sit on one line
with the message wrapping below. It shows the newest 500 lines. Under the box, "Times count from
each start." Wells first read the old `00:14:32` as a clock time.

Each start sits under a divider saying how that start came about, and for the latest one when it
began: "This start · after a crash · today at 5:46 PM", "Earlier start · ended in a crash". The
list can begin partway through a start whose first line is gone, which gets a plain "Earlier
start". Below the box, in About's list style, **Wiki** opens the Troubleshooting page, which says
where to send a log, and **Report an issue** opens a new GitHub issue.

**Copy** takes what's shown under the current filter, at most the newest 300 lines, plus a header.
It uses the page's existing copy helper, because a page on plain http gets no clipboard API.
**Download** saves the whole log with the same header as `airwriter-xxxx-log.txt`. Both are oldest
first, how a log file reads, and say "26 h 14 min ago" where the screen gives a time of day: a
time of day in a posted log gives away a time zone.

The header makes a pasted log stand alone: firmware version and slot, board revision, time on,
last reset reason, each printer's model and connection, and free memory.

**The setup screens.** Setup hides the sidebar, so About can't be reached by someone whose board
can't join Wi-Fi, which is a likely reason to want the log. In the "Couldn't join" state only, the
Wi-Fi step gets a small **View Log** link to the same screen. Back returns to setup.

**Addresses.** The screen is at `/about/log`, so a reload stays on it. Wells asked for real
addresses over `#` ones, so every pane has one now (`/about`, `/printers/2`), and an old `#` link
forwards.

## What's recorded

Everything that goes through `ESP_LOG` at info and above, including ESP-IDF's own Wi-Fi, mDNS and
HTTP messages, captured with `esp_log_set_vprintf` and passed on to the console unchanged.
Messages keep their existing wording. The Wi-Fi library sends a line in three calls, which the
capture joins per task.

- **Starts.** Every start writes a first record: firmware version, reset reason, a run number.
- **Crashes.** One error line from the panic path, with the reason, the task, the core and the
  backtrace addresses. The panic handler writes straight to the USB peripheral
  (`panic_print_char`), not through `esp_log` or `esp_rom_printf`, so it needs a capture of its
  own.
- **Chatter.** The per-request line in `ipp_server` (`op 0x… asks …`) is kept in RAM but not saved
  to flash for routine status polls, so a phone polling all day can't push out the useful lines.
  RAM keeps it because it's the only evidence of a phone that asks and never sends a job.

## What's left out

The settings page has no password and these logs will be posted in public. So the rule is: **a name
somebody typed or chose, or anything that could locate them, never reaches the log.**

| Name | Instead |
|---|---|
| Document | The job number |
| Person printing (`requesting_user` in the IPP request line) | Nothing |
| Printer's custom name | The printer's number and model |
| Wi-Fi network, board's host name | `<network>`, `<host>` |
| MAC address of the router, or of a phone on the setup network | `<mac>` |
| A vintage Mac's name, if the AppleTalk event lines carry one | Nothing |
| A printer's uuid, which holds the board's MAC address | Nothing |

About 30 log statements carry a quoted string or the requesting user; each needs reading. Call
sites change for the names the firmware passes in. ESP-IDF's Wi-Fi library prints the network name
and MAC addresses itself (`connected with %s, … bssid = …` and `station: … join`), so the capture
hook also replaces the saved network and host name in every line, and anything shaped like a MAC
address in lines tagged `wifi`. The firmware's own messages contain no MAC addresses. A router's
MAC can locate a home through public Wi-Fi databases, which is why it goes too. Names go at
recording time and never reach flash. Local IP addresses, printer models and firmware details
stay. No message contains a password today, and that must stay true.

## How it's kept

**RAM.** A 32 KB ring in PSRAM holds every line (about 400) and survives a panic or software
restart. That needs a `.noinit` placement (`CONFIG_SPIRAM_ALLOW_NOINIT_SEG_EXTERNAL_MEMORY`, off
today) with a magic number and check on each boot. `CONFIG_SPIRAM_MEMTEST` is on, and ESP-IDF's
test skips that segment. On the bench, PSRAM kept its contents through a panic restart on rev A's
module.

**Flash.** The first 256 KB of the `storage` partition (1.9 MB, used by nothing: the icons are
embedded in the app), about 3,000 lines. Raw `esp_partition` access, append-only records, a ring of
4 KB sectors. A blank or unreadable region starts afresh, which is how every existing board will
begin. Records carry a format version; an unknown version starts afresh too.

**When it saves.** Flash writes stall the board, so saving and sector erases happen only when no
job holds the job lock and none ended in the last few seconds (`job_touch()` in `main.c` already
records that for the auto-updater's idle check). On boot, lines from the last start
still in RAM are saved first. The page sees flash and RAM merged by sequence number.

**What can be lost.** Lines not yet saved when the power is cut, which during a job means that job
so far. A stuck job followed by someone unplugging the board is a likely real case, so warnings
and errors are appended straight away to pre-erased flash during a job (`LB_SAVE_DURING_JOB`, a
page write, about a millisecond, no erase). On the bench a StyleWriter page printed normally with
one forced every second.

**Erasing.** **Erase All Settings** and the power-up reset clear both layers, and the new log
starts by saying settings were erased.

## API

- `GET /api/log`: lines after a given sequence number, the newest N, or all of them. The screen
  polls it while it's open. **Copy** and **Download** are built from it in the page, with the header
  from `/api/status` and `/api/printers`.
- `/api/status` gains the error count for this start.

`webui.c` has `max_uri_handlers = 32` and registers 30 routes, this one among them. While a job
runs, `/api/log` answers from RAM only, because reading flash competes with the print. A request
decides that once, when it begins. The screen's once-a-second question never needs flash, because
RAM holds every line after the last one it has.

LocalTalk and EtherTalk each keep a small ring of event lines (20 and 24), served through `/api/lt`
and `/api/et`. The page doesn't read them, and every one of those lines will now be in the log, so
both rings go. The bench step in `docs/traps.md` that reads an echo reply from `GET /api/lt` moves
to the log.

## Constraints

- The hook runs on whichever task logged, including Wi-Fi's. No large stack buffer. It formats into
  PSRAM under its own lock and then forwards to the console, one after the other rather than
  nested, so it adds little more than its own frame. It never waits long: if the lock is busy it
  drops the line and counts it, and the log shows how many were dropped.
- `tools/stackcheck` can't see the hook: it follows three roots and no calls through pointers. The
  gate is `uxTaskGetStackHighWaterMark` on a board, before and after, on the tasks that log most
  (Wi-Fi, the web server, the print tasks).
- No internal RAM. Compare `/api/mem`'s low point through a job before and after.
- Serving the log must not slow a job. Someone can have the screen open during a print.
- The page makes no request off the board. `webui_page.h` is generated by `tools/webui/mkheader.py`.
- Page text in the product voice (`docs/voice-and-tone.md`). The log's own lines are engineering
  text.

## Verification

- Host tests in `make -C tools/rip test` for the pure-C core: ring and record framing, wraparound,
  a corrupt or blank region, name replacement, merging RAM and flash.
- `make -C tools/rip regress` unchanged.
- `tools/webui/states.json` gains the log's states (empty, busy, errors, a failed join), checked at
  `/_states` at desktop and phone width, light and dark.
- `idf.py build` clean, then `tools/stackcheck/stackcheck.py`.
- On a board: a bench-only forced panic shows its reason after the restart; power cut while idle
  leaves the log whole. Both done. A StyleWriter page took the place of the LaserWriter print with
  the screen open, because the LaserWriter was offline. The ImageWriter checker wasn't run, because
  no ImageWriter was connected; the raster path didn't change and `regress` is byte-identical.

## Who decided what

**Wells:** technical, with the usual levels. A row on About that opens in place. Newest line on
top, with no following. Times counted from each start, with the real start time on the current
one. **Wiki** and **Report an issue** rows in About's style. Real page addresses. A window 860 wide.

**Claude, for Wells to check:** always recording with no settings. Names and MAC addresses left
out, and the rule behind that. The **View Log** link on the setup screens. **Wiki** going to the
Troubleshooting page, so where people send logs can change without a firmware update. Erasing
clears the log. The two AppleTalk event rings removed in favour of the log. Copy and Download
staying oldest first. The screen asking every second rather than holding a connection open.

**Left to the builder:** record format, ring sizes within reason, the API's exact shape, how panic
details are captured, the polling rate, the wording of dividers.

## Docs that change with it

After Wells has approved the build: a new section in `docs/SPEC.md`; the stale `storage` comment in
`firmware/partitions.csv`; AGENTS.md's "Where things are", API list and verification; `docs/traps.md`
for anything the bench turns up; the wiki's Troubleshooting page.

## Suggestions, not part of this

- The task watchdog is set not to panic (`CONFIG_ESP_TASK_WDT_PANIC` off), so a stuck task leaves
  the board hung instead of restarting it. Turning that on would make a hang recover and show up as
  a crash in this log. It changes how the board behaves, so it needs its own decision and testing.
