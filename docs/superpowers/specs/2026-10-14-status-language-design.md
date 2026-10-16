# Status language v2 — design

Linear: W-1037. Written 2026-10-14, after rev A bring-up. Supersedes the "States" bullet of
`2026-08-20-dot-panel-cover-design.md` §5 and the teal-only material model in its §1 table.

## Goal

Anyone across the room can tell what the AirWriter is doing, without opening the web page:
setting up, joining Wi-Fi, ready, printing, waiting on the printer, needs attention, or a
failed job. A printer that is switched off is not an error. The panel goes quiet when nothing
is happening, and greets you with a short animation at power-on.

## What the hardware can and cannot tell us (rev A)

**One status line from the printer.** HSKo (the printer's DTR) reaches IO8 through the SP3232.
The ImageWriter II Technical Reference, Appendix E, says it "goes false … when the printer is
unable to receive data (deselected or busy)"; paper-out deselects the printer and lights its
error lamp (Chapter 5, Paper-Out Sensor). A printer that is off or unplugged leaves the line
undriven, which the receiver also reads as false. So on rev A **off, unplugged, deselected and
paper-out are electrically identical.** Self-ID (`ESC ?`) needs a selected printer, so it
cannot split them either.

Firmware separates them by context instead:

- not ready **while idle** → the printer is off, unplugged or deselected: *Printer offline*;
- not ready **for 30 s during a job** → paper, jam or SELECT pressed: *Needs attention*.

Rev B could do better: HSKo sits at about −5 V when deselected, 0 V when off and +5 V when
ready, so an ADC tap on it would separate all three. Out of scope here; noted for rev B.

**The panel's materials (measured on board 1, by eye).**

| Material | Count | Brightness vs FR4 | Colour behaviour |
|---|---|---|---|
| P — bare FR4 | 14 | 100% | crisp, true RGB |
| B — single mask | 9 | ~10% | fuzzy, larger halo with a yellow-green rim; red passes well, blue shifts to teal, yellow goes olive |
| L — double mask | 14 | ~5% | as B, dimmer and smaller |

The gap cannot be closed: even at full drive a B dot reaches about half an FR4 dot and an L dot
about a quarter, at a current rev A cannot hold as a steady state (no hardware current limit).
**So information lives on the 14 FR4 dots.** Mask dots are atmosphere: dark in calm states, and
driven hard only in short animations and in the red/amber states, where their colour survives.

## State model

Display states, highest priority first:

| # | State | Entered when | Left when | IPP |
|---|---|---|---|---|
| 1 | **Booting** | power-on | the Spin ends: once the board has settled (below, W-1049's quiet power-on), at the earliest 3.0 s and at the latest 30 s after power-on | idle |
| 2 | **Setup** | no credentials, or the boot-time join failed; sub-state *phone joined* while ≥1 station is on the AP | credentials saved (the board reboots) | — |
| 3 | **Joining Wi-Fi** | the Spin ended without Wi-Fi (the 30 s cap), and after any disconnect once joined | got an IP | — |
| 4 | **Needs attention** | a job is active and HSKo has been false for `STALL_ATTENTION` (30 s) -- or for `OFFLINE_DEBOUNCE` (3 s) if it was already false when the job started: no printer to wait for | HSKo true (the job resumes), or `STALL_ABORT` (600 s) passes → job fails | stopped, `media-empty` |
| 5 | **Job failed** | a job ends in failure (transfer cut, rip error, stall abort) | `FAILED_HOLD` (60 s) passes, or the next job starts | idle; the job is aborted |
| 6 | **Printing** | a job starts | the job ends | processing |
| 7 | **Printer offline** | no job, HSKo false for `OFFLINE_DEBOUNCE` (3 s), counted from the end of the Spin (the ImageWriter reports busy for ~3.5 s after the AirWriter powers up, W-1039) | HSKo true | stopped, `other`¹ |
| 8 | **Ready** | Wi-Fi up, HSKo true, nothing above applies | — | idle |

**Quiet power-on** (W-1049; Wells, 2026-10-16: a normal power-on showed the Spin, Joining, the
green, the printer gone and back, then Ready). The Spin is all a normal power-on shows. It runs
until the board has **settled**, then goes out (1.3 s) straight into the state:

- settled means Wi-Fi is up and the printer line has held still: ready for `BOOT_SETTLE_READY`
  (1 s), or not ready for `BOOT_SETTLE_OFFLINE` (10 s, counted from power-on: a printer switched on
  with the board keeps its line down for a few seconds while it starts). Setup mode or a job
  starting settles it at once;
- the Spin runs at least `BOOT_MS` (3.0 s, its own show) and at most `BOOT_CAP` (30 s), so it
  never looks hung;
- after it: Ready with nothing else, or Printer offline's embers directly (no "gone"), or Setup
  (no credentials), or Setup opened by the two red pulses (the saved network was tried and
  failed), or Joining (still no Wi-Fi at the cap);
- the green Wi-Fi moment opens the Ready after the Spin only on the first power-on after Wi-Fi
  was saved from the web page (the restart that follows; a flag that survives the restart, not a
  power cut), as confirmation. Otherwise it opens a Ready entered from Joining: a real rejoin.

After the Spin, every moment plays for real changes as before.

**Asleep** is a modifier, not a state: after `SLEEP_AFTER` (300 s) with no activity while in
*Ready* or *Printer offline*, the panel fades to a pilot light. States 1–6 never sleep.

**Cable result** (W-1055) is a modifier too: after a cable test, its result shows over Setup,
Joining, Ready or Printer offline for 2.4 s (1 s when no cable was tested); a job hides it. The
test runs from the test button while the printer is not ready (a cable from J2 to J4 holds HSKo
false), or from the web page. It runs through TashTalk, so with no chip in U7, or one not running
TashTalk, no cable is tested and the panel says that instead of red (W-1063).

**Activity** (wakes the panel and resets the sleep timer): any display-state change, a job
operation arriving (Print-Job, Validate-Job; the server has no Create-Job), a test-button press,
a DIP change, a cable test's result.
Get-Printer-Attributes does **not** count: Home Assistant's IPP integration polls it every
30 s and would keep the panel awake forever. (This trades away "lights up when the print sheet
opens"; Validate-Job still lights it the moment you tap Print.)

¹ Checked 2026-10-14: `offline-report` is a CUPS keyword, absent from the IANA IPP registry, so
the registered `other` is used. Check on an iPhone what the print sheet shows in this state.

### Behaviour changes that come with it

- **A stalled job pauses instead of aborting.** Today the 30 s stall fails the job and purges the
  rest of the page. Now the job waits (attention state, phone shows out-of-paper) and resumes
  when HSKo returns, for up to `STALL_ABORT`. This also means raising `job_pipe`'s
  `PUSH_TIMEOUT` (20 s today) to cover the pause, since the upload backs up behind the stalled
  printer. **Risk:** the phone's upload connection is held open for the whole pause; whether iOS
  tolerates a multi-minute stall is unverified. If it does not, the follow-up is to spool the
  ripped printer stream (~115 KB per page) in PSRAM so the upload can finish independently.
- **A printer stuck busy while idle gets a CTRL-X (W-1039).** Serial-line noise (a cable replug,
  power-up) can leave the ImageWriter holding DTR false with SELECT lit -- an unprinted line in
  its buffer. On board 1 it stayed stuck 120 s without help and cleared 4.1 s after a CTRL-X
  (cancel current line), sent past the flow control. Rule: no job, line not ready for 5 s, then
  at most every 60 s while it stays stuck; never mid-job, where a low line is paper or a jam.
- **Wi-Fi reconnects forever after a drop**, with backoff 1, 2, 4 … 30 s. Today the station
  gives up after 8 retries and the panel keeps showing Ready. A drop never flips the board into
  setup mode; only a failed boot-time join does (unchanged).
- **The setup AP reports stations joining and leaving** (`WIFI_EVENT_AP_STACONNECTED` /
  `_STADISCONNECTED`), driving the *phone joined* sub-state.

## Panel language

Colours are drive targets before the cap: `LG_BRIGHTNESS_CAP` 48/255 on FR4 dots, except Needs
attention, which gets `LG_ALERT_CAP` 96/255 (tuned on board 1: at 48 it was too dim to notice).
"FR4 only" means mask dots stay dark.

Every pattern uses the array: position drives sweeps, ripples and the print head, and each dot
carries a fixed per-dot variation (a hash of its row and column) in phase, period and hue, so no
state reads as one big LED. Hue offsets stay inside each state's colour family.

| State | Dots | Pattern |
|---|---|---|
| Booting | all; mask at 4× | **Spin**: a full-saturation hue wheel around the test button, spinning up from dark (dots fade in by distance), then collapsing inward to the test button: the far dots go first and the centre last, over 1.3 s from when the board settles (so a board settled by 1.7 s ends dark at 3.0 s, as before). While it waits longer, the wheel keeps turning at its 2.5 s pace (100°/s) and eases to 60% between 3 and 4 s, which also keeps the draw down while Wi-Fi joins. |
| Setup | FR4 only | blue (hue 220° ±16° per dot, drifting ±6°) **radar sweep**: an arm turns around the test button once per 2.6 s; each dot flares to 88% as it passes and decays behind it (e-folding over 55°), floor 8% |
| Setup, phone joined | FR4 only | the same blues, steady at 58% with a per-dot shimmer of ±8% on its own 3–5.5 s period |
| Joining Wi-Fi | FR4 only | blue (hue 215° ±18°) **rings pulsing outward** from the test button every 0.9 s, 8% floor, 83% at the ring |
| Wi-Fi connected | FR4 only | the first moments of a Ready entered from Joining, or from Booting on the first power-on after Wi-Fi was saved (`greeted`): a green field (hue 130° ±14°) with a ripple outward over 1.4 s, held, then cross-fading to Ready's white between 2.2 s and 3.2 s. Green means "connected, or it worked" (W-1049, FeatherFrame's green): here, Wi-Fi; as a sweep and a pulse, "this cable works" (W-1055). |
| Ready | FR4 only | **neutral white**, each dot slightly warm or cool, breathing on its own 5–9 s period between 30% and 70% (first tuned at 16–38%, which peaked under 6 counts and was invisible in daylight on board 1; 30–70% is 4× the drive). (Colour tweaks made on 10-14/15 were judged while the board was actually in Printer offline, so they were reverted to this.) |
| Ready, gamer mode | FR4 only | DIP7 on: hue from position plus time, a slow RGB drift (one wheel per ~14 s), saturation 75%, the same breathing |
| Printer offline | FR4 only | **warm embers**: hue 25–43°, saturation 50–62%, each dot on its own 7–13 s cycle, mostly at 9% and swelling to 60% (4× the drive of the first 5–32% tune, for daylight) |
| Printing | all; mask at 4× | **print head**: a bright white dot crosses the current row in 1.5 s, inking it warm (hue 28–48°) behind; rows advance top to bottom and alternate direction like the ImageWriter's bidirectional head; the two rows above hold fading ink (25%, 12%), the rest 3% |
| Needs attention | all; FR4 at the alert cap (96/255), mask at 4× | deep amber (hue 18–30°: at 36° the bright green die washed it toward white) **double ripple** from the test button: two wide rings 350 ms apart every 2 s over a 12% amber floor, so the panel is never dark while it needs a person |
| Job failed | all; mask at 4× | red (hue 352–12°): three 200 ms **glitch flashes**, each on a different random ~65% of dots, then a pulse 20%→60% whose period varies ±15% per dot, until the hold ends |
| Cable test | all; mask at 4× | a band **sweeps** the panel left to right and back (700 ms each way) over an 8% floor. Green (hue 130° ±14°), a crossover: then a ring **pulses** out of the test button and the panel fades by 2.4 s. Red (hue 352–12°), a straight, broken or missing cable: then the six dots around the test button **blink twice** (200 ms on, 200 off, 200 on), the rest dark. **No cable tested** (TashTalk didn't answer, W-1063): no sweep; after 200 ms dark the five dots within 2.8 rows of the LocalTalk socket blink red twice (200 on, 200 off, 200 on), the rest dark, 1 s in all. Red says no; where it blinks says LocalTalk |
| Asleep | the pilot, plus a rare twinkle | the FR4 dot right of the test button (row 3, col 4) breathes 3%→12% of the cap, driven linearly (gamma would round these levels to zero), on a 6 s period: neutral white, or a slowly cycling hue in gamer mode. Every 30 s one other FR4 dot twinkles once (1.5 s, peak 8% linear). Fade into sleep over 10 s; wake instantly. |

Below one count of drive a dot's brightest channel is lifted to one count and the others scale
with it, so dim dots keep their hue through the dither; levels under 1/256 of a count stay dark.
Frames are rendered at 8.8 fixed point and temporally dithered to the LEDs' 8 bits at a steady
100 fps (tuned on board 1: at a few counts of drive an 8-bit Ready breathe had ~6 levels and
stuttered).

Levels in this table are perceptual: every pattern except the pilot light goes through **gamma 2.2**
before the cap, on FR4 and mask dots alike, and the numbers get a final tune on the board.
No per-hue colour correction for the mask: calibration was tried and dropped (the cast is
hue-dependent and not worth a lookup table).

**Power.** Worst case is the boot spin: mask dots at 4× (192/255) with about 1.1 channels lit
on average. Steady states drive FR4 dots only, so the steady draw falls well below today's
37-dot breathe.

### Actions, moments and acknowledgements (W-1049)

`W-1049 Panel feedback for every action: OTA, reset, cables, TashTalk flash` puts everything the
board does on the panel. Wells approved the patterns on 2026-10-16 from previews rendered by the
renderer itself (the W-1049 panel-patterns page), with four held back (below).

**Colours, shared with FeatherFrame's status LED.**

| Colour | Means | On FeatherFrame |
|---|---|---|
| blue | setting up, joining | the setup portal, joining |
| green | connected, or it worked | on Wi-Fi, paired |
| amber, orange | needs a person, or not now | waiting to be added |
| purple | updating firmware | writing an update |
| red | something went wrong | the join failed, no Wi-Fi |
| white | ready, done, taken | (off in normal running) |
| the six stripes | LocalTalk | — |

The boot stays AirWriter's RGB Spin (FeatherFrame breathes white). The six stripes are the
classic Apple logo's, top to bottom over the eight rows: hue 104°, 41°, 28°, 358°, 299°, 197°
(stripe `⌊(row + 0.5)·6/8⌋`), saturation 85%, ±4° per dot.

**The sockets are on the bottom edge**: the printer (J2) under column 0.9, LocalTalk (J4) under
column 3.2, USB-C under 5.5. A pattern about a cable starts at its socket, drawn from row 8.6,
just below the grid (the sockets themselves are 4.6 rows lower), so the wave enters at once.

**Priority**, highest first:

1. **Tasks** own the panel while they run: the factory reset, then a firmware update, then a
   TashTalk flash. Each has its own slot; the highest-priority running one shows, so letting go
   of a reset hold uncovers an update still in progress. Tasks show over every state, asleep
   included.
2. **The display state**, with the **moments** that open some states. A moment is drawn from
   the state before (`ps_view_t.prev`) and `since_ms`: wherever its wave has not reached yet
   still shows the old state.
3. **Acknowledgements**: at most 1.6 s, drawn over the state (screen blend), on any dots they
   need. Never during a task, and not in Needs attention or Job failed, so an alert is never
   hidden. Not over an all-dot moment (Page out, Cancelled, Could not join, Printer back into
   Printing) or a cable test's answer either: together they would draw more than the Spin. One raised during Booting waits for the Spin to end, then plays from its start (the
   LocalTalk node comes online about 2 s into a mode-2 boot). A newer one replaces the one
   showing. They show over sleep without waking the panel; the ones a person causes also post
   `ACTIVITY` from their call site.

No frame of any of these draws more than the boot spin's heaviest (host test). FR4 dots use the
normal cap except in red phases (alert cap); mask dots use the mask cap.

**Moments.**

| Moment | When | Pattern | Length |
|---|---|---|---|
| Printer gone | Ready → Printer offline | the white drains toward the printer socket: a front runs from the far corner to the socket in 1.1 s with a white glint (85%, ±130 ms), each dot going dark as it passes, the embers fading in 250–900 ms behind it. FR4 only. | 2 s |
| Printer back | Printer offline → Ready, or Needs attention → Printing | a green wave (hue 130° ±14°, 85%) rolls out of the printer socket, crossing the panel in 1.1 s; ahead of it the old state, behind it green cross-fading to the new state over 150–900 ms after it passes. FR4 only from offline; all dots from attention (Printing's dots). | 2 s |
| Page out | Printing → Ready, the job finished | the inked sheet rises off the top: its bottom edge climbs from row 8.5 to −1.5 in 0.9 s, the ink in bands (10–30%, period 3 rows) scrolling up with it; the job's ink (warm, or the stripes for LocalTalk); Ready below the edge; mask dots carry the ink and go dark below it. | 1 s |
| Cancelled | Printing → Ready, the job cancelled | the print freezes where it stood (Printing's frame at the moment of the cancel) and fades out over 0.6 s; Ready fades in 0.3–0.9 s. No page out. | 0.9 s |
| Could not join | Setup after the saved network was tried and failed (from Joining, or straight from the Spin; `ps_view_t.join_failed`) | two red pulses (hue 2° ±8°, 500 ms each, peak 85%) on all dots, FR4 at the alert cap, then Setup fades in 1.0–1.6 s. FeatherFrame's failed-join signal. | 1.6 s |

Any other change of state opens with no moment: the new state shows as it is (a job cancelled
while it waits for paper goes from Needs attention straight to Printer offline's embers).

"Cancelled" covers a second tap of the test button during its page and an IPP Cancel-Job. A
LocalTalk job always ends as finished: a Mac that cancels closes the connection like one that is
done, and the two can't be told apart.

**LocalTalk printing.** Printing keeps its print head for every job, and the ink says whose job
it is: AirPrint and the test page ink warm (as above); a LocalTalk (PAP) job inks each row in
its stripe, the head's row at 35%, the two rows behind at 35% and 22%, the rest at 12% (so the
stripes show across the panel, not only near the head).

**Tasks** (besides the reset, above).

| Task | When | Pattern |
|---|---|---|
| Firmware downloading / installing | an update is arriving or being written; arg is progress 0..1000 | purple (hue 280° ±10°, saturation 90%) fills the FR4 dots from the bottom: the level's line sits at row `8 − 8.6p`, soft over one row; below it 45%, plus a shimmer band (+30%) running up through the filled part every 0.8 s; above it 6%. Mask dark. |
| … length unknown | arg is `LG_PROGRESS_NONE` | a purple band (60% over a 8% floor) climbs from below the grid to above it every 1.2 s |
| Verifying | the image is in; checking it | the FR4 dots pulse purple, 30% to 85%, every 0.8 s, each dot offset up to 250 ms |
| Verified | installed, set to boot | green rises through the purple from the bottom in 0.5 s, holds, fades out 1.2–2.0 s; dark until the restart |
| TashTalk flashing | the PIC is being written (about 2 s) | the six stripes at 35% on all dots, with a white bar sweeping left to right across the columns every 0.5 s |
| TashTalk OK | the flash verified | a green ripple out of the LocalTalk socket, crossing the panel in 0.9 s and leaving a 35% wash behind, fading out 0.8–1.6 s; then the state |
| TashTalk failed | the flash failed | Job failed's red glitch and pulse for 3 s, then the state |

A firmware task holds its slot until it is replaced or cleared (`LG_ACT_FW_NONE`); Verified
holds until the restart. The update's own flow decides when; W-1048 drives it.

**Acknowledgements.**

| Acknowledgement | When | Pattern | Length |
|---|---|---|---|
| LocalTalk on | the node has its node ID (at boot in DIP mode 2, or when mode 2 is switched on) | the stripes rise out of the LocalTalk socket: a front climbs from row 8.5 to −1.5 in 0.7 s, rows below it at 60% with a 35% crest; holds; fades out 1.0–1.6 s. All dots. | 1.6 s |
| A Mac heard | an NBP LkUp answered (a Chooser looking), or a frame from a node not heard since boot; at most once every 6 s | an arc of stripes out of the LocalTalk socket, crossing the panel in 0.9 s (90% at the crest, ±0.7 rows), fading out 0.8–1.1 s. All dots. | 1.1 s |
| Busy | the test button tapped during an IPP or LocalTalk job | two orange pulses (hue 30° ±4°, 300 ms each, peak 85%) on the dots around the test button (distance 0.5–1.6) | 0.6 s |
| Saved | settings saved on the web page; for Wi-Fi, just before its restart | a white ring closes into the test button in 0.6 s (80% at the ring), then the dots around the button glow 60% and fade out by 0.9 s | 0.9 s |

**The test button.** A tap while the test page prints cancels it: the page stops (the write is
aborted and the queue purged, as a Cancel-Job does), it ends as cancelled, and W-1041's resync
runs before the next job. A cancel that comes too late to cut anything is a page that printed.
A tap during an IPP or LocalTalk job, or a cable test, shows Busy and does nothing else.
Otherwise a tap tests the cable when the printer is not ready (W-1055) and prints the test page
when it is. The tap and the page's end decide under one lock, so a tap as the page finishes
never lands on the next job. A hold is the factory reset (unchanged).

**Held back** (Wells, 2026-10-16), not built: update kept (the trial passed), update refused,
update rolled back, and DIP switch flipped. Their `lg_act_t` values stay, because W-1048 calls
them, and they draw nothing: the state shows.

**Stays dark on purpose**: Get-Printer-Attributes polls (Home Assistant's every 30 s), W-1039's
CTRL-X unstick, W-1041's resync, the bench AEP echo. **Can't be seen on rev A**: a LocalTalk
cable on its own. Nothing is on the bus until a Mac speaks, and "A Mac heard" covers that.

#### The factory reset (W-1045)

The reset uses all dots, mask at 4×, FR4 at the alert cap in its red phases and at the normal
cap for white.

| Action | When | Pattern |
|---|---|---|
| Reset countdown | test button held past a tap (1 s) | a hand sweeps clockwise from 12 o'clock round the test button over 7 s, lighting each dot it passes (soft edge, the newest dots glowing, a 5% floor on the rest of the track); orange (hue 38°) through the first half, running to red by the end (`38·(1 − p^1.5)`, ±3° per dot); the lit dots beat 1 Hz rising to 3 Hz |
| Reset cancelled | let go during the countdown | the sweep winds back to 12 o'clock and fades in 0.4 s; the state returns |
| Point of no return, then done | 8 s held: settings erased | FR4 dots flash white and mask dots red (180 ms); the red drains inward into the test button (1 s); two white beats on the FR4 dots (180 ms each); dark until the button is let go and the board restarts (Spin, then Setup) |
| Reset confirmed at boot | after the bootloader's factory reset (button held through power-up) | the same flash, drain and beats once the Spin ends (2 s), then the state |
| Reset failed | the erase failed | Job failed's red glitch and pulse for 3 s, then the board restarts |

## Architecture

Units and their interfaces. Pure cores stay host-testable in `tools/rip`, as now.

**`printer_state_core`** (rewritten, pure C)
- Inputs are events: `WIFI_SETUP_AP`, `WIFI_JOINING`, `WIFI_UP`, `WIFI_DOWN`, `LINE_READY`,
  `LINE_NOT_READY`, `JOB_START`, `JOB_DONE`, `JOB_FAILED`, `ACTIVITY`, plus
  `ps_set_ap_stations(n)`.
- `ps_update(now_ms)` derives the view and runs every timer lazily: boot length, offline
  debounce, stall attention, failed hold, sleep. A state change counts as activity. The core
  owns time-since-line-change, so `serial_out` no longer decides "blocked".
- Output: `ps_view_t { state; prev; cancelled; join_failed; spin_out; spin_out_ms;
  phone_joined; greeted; asleep; since_ms; asleep_ms }`, the one view the LEDs, IPP and the web
  status all read. `greeted` marks a stretch of Ready entered from Joining, or from Booting after
  `PS_EV_WIFI_FRESH`, which the panel opens with the green Wi-Fi-connected moment. `spin_out` and
  `spin_out_ms` say the Spin is going out and for how long; `join_failed` that Setup follows a
  failed join of the saved network. `prev` is the display state before this one (Booting at first),
  which the panel's moments key off (W-1049).
- `PS_EV_JOB_CANCELLED` ends a job as `JOB_DONE` does and sets `cancelled`; `JOB_START` clears
  it. Only the panel reads it (Cancelled instead of Page out).
- Booting ends on the core's own clock (W-1049's quiet power-on): the Spin starts going out at
  the latest of `boot_ms − boot_out_ms` and the moment the board settled, or at
  `boot_cap_ms − boot_out_ms`, whichever is earlier, and Booting ends `boot_out_ms` later.
  `ps_timing_t` gains `boot_cap_ms` (30 000), `boot_out_ms` (1 300), `settle_ready_ms` (1 000) and
  `settle_offline_ms` (10 000), appended to `PS_TIMING_DEFAULT`.
- `PS_EV_WIFI_JOINING` is posted only when there are credentials to try, so Setup after it is a
  failed join (`join_failed`). `PS_EV_WIFI_FRESH` (main, at power-on, when `settings` says Wi-Fi
  was saved before this restart) lets the green open the first Ready.
- `STALL_ABORT` is not the core's to enforce: the serial writer fails a write after that long
  without FIFO progress.

**`serial_out`**
- Reports HSKo edges (`on_line(ready)`, sampled every 100 ms) instead of `on_state(BLOCKED)`.
- `serial_writer` rides out a stall and fails only after `STALL_ABORT` with no FIFO progress
  (new Kconfig `AIRWRITER_STALL_ABORT`, default 600). `AIRWRITER_STALL_TIMEOUT` (30) becomes
  the attention threshold and keeps its name.

**`job_pipe`**: `PUSH_TIMEOUT` and the end-of-job wait both follow `STALL_ABORT` + margin.

**`main`**
- Wi-Fi: backoff reconnect after `WIFI_UP` has been seen; AP station events; posts the
  matching events. The boot-time 60 s join → setup-AP path is unchanged.
- The boot animation runs while Wi-Fi starts, so the two overlap.
- A 100 ms task calls `ps_update`, logs state changes and maps them to IPP.
- `test_page`, DIP changes and IPP job operations post `ACTIVITY`.

**`ipp_server`**
- A callback `on_job_op` for Print-Job, Validate-Job and Create-Job (→ `ACTIVITY`).
- `ipp_server_set_stopped(reason)` stays; `main` maps the attention and offline states to it.

**`led_grid_core`**
- `lg_render(const ps_view_t *, const lg_opts_t *, t_ms, frame)`: one renderer per state,
  materials as above, the Spin as a one-shot keyed off `since_ms`. `lg_opts_t { gamer }`.
- Gamer mode is DIP7 (`dip_config_t.gamer`; DIP8 stays spare), read live like the other
  switches and handed to the LED task with `led_grid_set_gamer()`.
- **Test patterns do not ship.** The W-1034 rainbow and the boot-animation spike stay on their
  branches; patterns are previewed rendered in chat, not on the device.
- W-1049: `lg_opts_t { gamer; src; task[LG_FAMS]; blip; blip_ms }`. `task` holds one
  `lg_slot_t { act; arg; ms }` per family (reset, firmware, TashTalk, in priority order) and
  `lg_family(act)` says which. `lg_act_over()` and `lg_blip_over()` say when a one-shot is done.
  `src` (`LG_SRC_AIRPRINT`, `LG_SRC_LOCALTALK`) picks Printing's ink. The renderer draws the
  first running task if there is one, else the state with its moment and the acknowledgement.

**`led_grid`** (the ESP glue), all callable from any task:
- `led_grid_action(lg_act_t act, uint16_t arg)` sets that act's family slot. The same act again
  updates `arg` and keeps its clock (firmware progress); a different act restarts it.
- `led_grid_blip(lg_blip_t b)` shows an acknowledgement from now.
- `led_grid_set_source(lg_src_t s)`: whose job the next Printing is; set before `JOB_START`.

**`localtalk`**: `localtalk_cfg_t.on_event(lt_event_t)`, called on the LocalTalk task:
`LT_EV_ONLINE` (the node ID is ours), `LT_EV_LOOKUP` (an NBP LkUp answered: `st.replies` went
up), `LT_EV_NEW_NODE` (a node's first non-ENQ frame since boot: its `heard` bit was set). Raised
in `localtalk.c` by comparing the node's counters after each frame and tick; `lt_node` itself
does not change. `main` turns LOOKUP and NEW_NODE into one "A Mac heard" at most every 6 s (and
`ACTIVITY`), and ONLINE into "LocalTalk on".

**`webui`** calls `led_grid` directly (it `REQUIRES led_grid`): saving settings or Wi-Fi shows
Saved. `POST /update` is W-1048's: it moves into `fw_update.c` and drives the firmware task
there, so W-1049 leaves it alone and its firmware patterns are checked on the bench once both
branches are in.

**`main`** (W-1049): sets the source at each job's begin (`doc_begin`, `lt_print_begin`, the
test page); posts `JOB_CANCELLED` for a cancelled IPP job or test page; runs the test button as
above (tap: print, cancel or Busy); wraps the TashTalk flash in `PIC_FLASHING` then `PIC_OK` or
`PIC_FAILED`; maps the LocalTalk events.

**Web status JSON**: `"ps"` takes the new names (`booting`, `setup`, `joining`, `attention`,
`failed`, `printing`, `offline`, `ready`) plus `"asleep": bool`; the human `"state"` strings
follow ("Needs attention", "Printer offline", …). The web page's labels update to match.

## Testing

Host (`tools/rip`, `make test`):
- `test_printer_state`: every transition in the table, each timer edge (2.9 s vs 3.1 s offline
  debounce, 29 s vs 31 s attention, the 60 s failed hold, the 300 s sleep), priority between
  overlapping states, activity resetting sleep, and a job surviving a stall and resuming.
- `test_led_grid`: per state — the cap holds; FR4-only states leave mask dots dark; the Spin
  ends dark at 3.0 s; asleep lights exactly one dot.
- `test_led_grid`, W-1049: for every moment, task and acknowledgement — caps hold (alert cap
  only in red phases), no frame draws more than the Spin's heaviest, FR4-only patterns leave
  mask dots dark, and each one ends on time with the state's own frame after it. Task priority
  (a reset hold covers an update, which shows again after the cancel; firmware covers
  TashTalk). Acknowledgements are dropped during a task and in Booting, Needs attention and
  Job failed. LocalTalk ink differs from AirPrint ink. Firmware progress lights more as it rises.
  The held-back acts draw the state.
- `test_printer_state`, W-1049: `prev` across transitions; `cancelled` set by `JOB_CANCELLED`
  and cleared by `JOB_START`. The power-on paths: a quiet boot (Wi-Fi and a ready printer) goes
  Booting → Ready with nothing between and no green; no printer goes Booting → Printer offline;
  no credentials goes to Setup at 3.0 s; a failed join goes to Setup with `join_failed`; no Wi-Fi
  by the cap goes to Joining at 30 s; the green opens the first Ready only after
  `PS_EV_WIFI_FRESH`; a fast boot still ends at 3.0 s exactly.
- `test_led_grid`, W-1049: the Spin waiting past 3 s stays lit at no more than its heaviest
  frame, and goes dark `boot_out_ms` after it starts going out.
- `test_serial_writer`: a write rides out a stall and succeeds when the line returns; fails only
  past the stall limit.
- `make regress`: both `stream.bin` regressions unchanged (the raster path is untouched).

On board 1:
- Power-on shows the Spin, then Joining, then Ready (swell). Pull the printer cable → Printer
  offline within ~3 s; reconnect → Ready.
- Deselect the printer mid-job → Needs attention after 30 s; reselect → the job resumes and the
  page prints complete. Note what iOS shows during the pause, and whether its upload survives.
- Router Wi-Fi off for 1 minute → Joining; back on → Ready, with no setup AP.
- Leave idle 5 minutes → pilot light; a print or the test button wakes it.
- W-1049, each by eye: printer cable out and in (gone, back); a job to the end (page out); the
  test button tapped twice (cancelled) and during a phone job (Busy); a LocalTalk job (stripes);
  DIP mode 2 on (LocalTalk on) and the Chooser open on the Mac (A Mac heard); a TashTalk flash;
  an OTA once W-1048 is in (purple fill, verified, restart); settings saved; Wi-Fi credentials
  for a network that isn't there (could not join, then Setup).

## Out of scope

- Per-hue colour correction for mask dots (tried in the spike, dropped).
- A rev B HSKo voltage sense to tell off from deselected.
- Spooling the printer stream in PSRAM (the fallback if iOS will not hold a paused upload).
- The case and light-guide lattice, which will change the panel's look; the patterns get a
  re-tune when it exists.
