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
| 1 | **Booting** | power-on | the boot animation finishes (3.0 s) | idle |
| 2 | **Setup** | no credentials, or the boot-time join failed; sub-state *phone joined* while ≥1 station is on the AP | credentials saved (the board reboots) | — |
| 3 | **Joining Wi-Fi** | at boot, and after any disconnect once joined | got an IP | — |
| 4 | **Needs attention** | a job is active and HSKo has been false for `STALL_ATTENTION` (30 s) -- or for `OFFLINE_DEBOUNCE` (3 s) if it was already false when the job started: no printer to wait for | HSKo true (the job resumes), or `STALL_ABORT` (600 s) passes → job fails | stopped, `media-empty` |
| 5 | **Job failed** | a job ends in failure (transfer cut, rip error, stall abort) | `FAILED_HOLD` (60 s) passes, or the next job starts | idle; the job is aborted |
| 6 | **Printing** | a job starts | the job ends | processing |
| 7 | **Printer offline** | no job, HSKo false for `OFFLINE_DEBOUNCE` (3 s), counted from the end of the boot animation (the ImageWriter reports busy for ~3.5 s after the AirWriter powers up, W-1039) | HSKo true | stopped, `other`¹ |
| 8 | **Ready** | Wi-Fi up, HSKo true, nothing above applies | — | idle |

**Asleep** is a modifier, not a state: after `SLEEP_AFTER` (300 s) with no activity while in
*Ready* or *Printer offline*, the panel fades to a pilot light. States 1–6 never sleep.

**Cable result** (W-1055) is a modifier too: after a cable test, its result shows over Setup,
Joining, Ready or Printer offline for 3 s (pass) or 6 s (fail); a job hides it. The test runs
from the test button while the printer is not ready (a cable from J2 to J4 holds HSKo false), or
from the web page.

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
| Booting | all; mask at 4× | **Spin**: a full-saturation hue wheel around the test button, spinning up from dark (dots fade in by distance), then collapsing inward to the test button and ending dark at 3.0 s. |
| Setup | FR4 only | blue (hue 220° ±16° per dot, drifting ±6°) **radar sweep**: an arm turns around the test button once per 2.6 s; each dot flares to 88% as it passes and decays behind it (e-folding over 55°), floor 8% |
| Setup, phone joined | FR4 only | the same blues, steady at 58% with a per-dot shimmer of ±8% on its own 3–5.5 s period |
| Joining Wi-Fi | FR4 only | blue (hue 215° ±18°) **rings pulsing outward** from the test button every 0.9 s, 8% floor, 83% at the ring |
| Wi-Fi connected | FR4 only | the first moments of a Ready entered straight from Joining or Booting (`greeted`): a green field (hue 130° ±14°) with a ripple outward over 1.4 s, held, then cross-fading to Ready's white between 2.2 s and 3.2 s. Green means "Wi-Fi just connected" and nothing else. |
| Ready | FR4 only | **neutral white**, each dot slightly warm or cool, breathing on its own 5–9 s period between 30% and 70% (first tuned at 16–38%, which peaked under 6 counts and was invisible in daylight on board 1; 30–70% is 4× the drive). (Colour tweaks made on 10-14/15 were judged while the board was actually in Printer offline, so they were reverted to this.) |
| Ready, gamer mode | FR4 only | DIP7 on: hue from position plus time, a slow RGB drift (one wheel per ~14 s), saturation 75%, the same breathing |
| Printer offline | FR4 only | **warm embers**: hue 25–43°, saturation 50–62%, each dot on its own 7–13 s cycle, mostly at 9% and swelling to 60% (4× the drive of the first 5–32% tune, for daylight) |
| Printing | all; mask at 4× | **print head**: a bright white dot crosses the current row in 1.5 s, inking it warm (hue 28–48°) behind; rows advance top to bottom and alternate direction like the ImageWriter's bidirectional head; the two rows above hold fading ink (25%, 12%), the rest 3% |
| Needs attention | all; FR4 at the alert cap (96/255), mask at 4× | deep amber (hue 18–30°: at 36° the bright green die washed it toward white) **double ripple** from the test button: two wide rings 350 ms apart every 2 s over a 12% amber floor, so the panel is never dark while it needs a person |
| Job failed | all; mask at 4× | red (hue 352–12°): three 200 ms **glitch flashes**, each on a different random ~65% of dots, then a pulse 20%→60% whose period varies ±15% per dot, until the hold ends |
| Cable test passed | FR4 only | two **white rings** out of the test button, 400 ms apart, over a 45% white field (each dot's own warm or cool white) that fades out between 2 s and 3 s |
| Cable test failed | all; mask at 4× | exactly **Job failed**, timed from the result: red means it did not work, whatever "it" was |
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

### Actions

Things a person starts show over every display state, asleep included, until they end
(`lg_opts_t.act`). The factory reset is the first (`W-1045 Long-press factory reset with live
feedback on the LED panel`, SPEC.md §3.11); `W-1049 Panel feedback for every action: OTA,
reset, cables, TashTalk flash` adds the rest and their priorities. The reset uses all dots,
mask at 4×, FR4 at the alert cap in its red phases and at the normal cap for white; no frame
draws more than the boot spin's heaviest (host test).

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
- Output: `ps_view_t { state; phone_joined; greeted; asleep; since_ms; asleep_ms }`, the one
  view the LEDs, IPP and the web status all read. `greeted` marks a stretch of Ready entered
  straight from Joining or Booting, which the panel opens with the green Wi-Fi-connected moment.
- Booting ends on the core's own timer (`boot_ms` = the Spin's 3.0 s); no event is needed.
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

## Out of scope

- Per-hue colour correction for mask dots (tried in the spike, dropped).
- A rev B HSKo voltage sense to tell off from deselected.
- Spooling the printer stream in PSRAM (the fallback if iOS will not hold a paused upload).
- The case and light-guide lattice, which will change the panel's look; the patterns get a
  re-tune when it exists.
