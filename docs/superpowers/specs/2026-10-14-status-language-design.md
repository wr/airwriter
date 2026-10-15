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
| 4 | **Needs attention** | a job is active and HSKo has been false for `STALL_ATTENTION` (30 s) | HSKo true (the job resumes), or `STALL_ABORT` (600 s) passes → job fails | stopped, `media-empty` |
| 5 | **Job failed** | a job ends in failure (transfer cut, rip error, stall abort) | `FAILED_HOLD` (60 s) passes, or the next job starts | idle; the job is aborted |
| 6 | **Printing** | a job starts | the job ends | processing |
| 7 | **Printer offline** | no job, HSKo false for `OFFLINE_DEBOUNCE` (3 s) | HSKo true | stopped, `offline-report`¹ |
| 8 | **Ready** | Wi-Fi up, HSKo true, nothing above applies | — | idle |

**Asleep** is a modifier, not a state: after `SLEEP_AFTER` (300 s) with no activity while in
*Ready* or *Printer offline*, the panel fades to a pilot light. States 1–6 never sleep.

**Activity** (wakes the panel and resets the sleep timer): any display-state change, a job
operation arriving (Print-Job, Validate-Job, Create-Job), a test-button press, a DIP change.
Get-Printer-Attributes does **not** count: Home Assistant's IPP integration polls it every
30 s and would keep the panel awake forever. (This trades away "lights up when the print sheet
opens"; Validate-Job still lights it the moment you tap Print.)

¹ Verify `offline-report` against the PWG IPP registry at implementation; fall back to `other`
if it is not registered. Check on an iPhone what the print sheet shows in this state.

### Behaviour changes that come with it

- **A stalled job pauses instead of aborting.** Today the 30 s stall fails the job and purges the
  rest of the page. Now the job waits (attention state, phone shows out-of-paper) and resumes
  when HSKo returns, for up to `STALL_ABORT`. This also means raising `job_pipe`'s
  `PUSH_TIMEOUT` (20 s today) to cover the pause, since the upload backs up behind the stalled
  printer. **Risk:** the phone's upload connection is held open for the whole pause; whether iOS
  tolerates a multi-minute stall is unverified. If it does not, the follow-up is to spool the
  ripped printer stream (~115 KB per page) in PSRAM so the upload can finish independently.
- **Wi-Fi reconnects forever after a drop**, with backoff 1, 2, 4 … 30 s. Today the station
  gives up after 8 retries and the panel keeps showing Ready. A drop never flips the board into
  setup mode; only a failed boot-time join does (unchanged).
- **The setup AP reports stations joining and leaving** (`WIFI_EVENT_AP_STACONNECTED` /
  `_STADISCONNECTED`), driving the *phone joined* sub-state.

## Panel language

Colours are drive targets before the global cap (`LG_BRIGHTNESS_CAP` 48/255 on FR4 dots).
"FR4 only" means mask dots stay dark.

| State | Dots | Pattern |
|---|---|---|
| Booting | all; mask at 4× | **Spin**: a full-saturation hue wheel around the test button, spinning up from dark (dots fade in by distance), then collapsing inward to the test button and ending dark. Maths as in the spike, `wells/spike-boot-anims` `led_demo.c` case 3, period-free and run once. |
| Setup | FR4 only | blue `(0.10, 0.35, 1.0)`, slow pulse 10%→80%, 2.5 s period |
| Setup, phone joined | FR4 only | the same blue, steady at 60% |
| Joining Wi-Fi | FR4 only | the same blue, quick pulse 10%→80%, 0.8 s period |
| Ready | FR4 only | green `(0.15, 1.0, 0.25)` steady at 35%; on entering from Joining, a swell to 100% over 0.5 s and back over 1.0 s |
| Printer offline | FR4 only | warm white `(1.0, 0.75, 0.45)`, slow breathe 12%→30%, 6 s period |
| Printing | all; mask at 4× | white row sweeping top to bottom every 1.5 s, neighbours at 25%/6%, like paper feeding |
| Needs attention | all; mask at 4× | amber `(1.0, 0.55, 0.0)` double-blink at 100%: 150 ms on, 150 off, 150 on, rest dark, every 2 s |
| Job failed | all; mask at 4× | red `(1.0, 0.05, 0.02)`: three flashes (200 ms on, 200 off), then a pulse 20%→60% on a 2 s period until the hold ends |
| Asleep | one FR4 dot | the FR4 dot right of the test button (row 3, col 4) breathes 3%→12% on a 6 s period in the underlying state's colour; fade into sleep over 10 s, wake in 300 ms |

Mask dots render with **gamma 2.2** like the FR4 dots, so animation curves look the same on both.
No per-hue colour correction for the mask: calibration was tried and dropped (the cast is
hue-dependent and not worth a lookup table).

**Power.** Worst case is the boot spin: mask dots at 4× (192/255) with about 1.1 channels lit
on average. Steady states drive FR4 dots only, so the steady draw falls well below today's
37-dot breathe.

## Architecture

Units and their interfaces. Pure cores stay host-testable in `tools/rip`, as now.

**`printer_state_core`** (rewritten, pure C)
- Inputs are events: `WIFI_SETUP_AP`, `WIFI_AP_STATIONS(n)`, `WIFI_JOINING`, `WIFI_UP`,
  `WIFI_DOWN`, `PRINTER_LINE(ready)`, `JOB_START`, `JOB_DONE`, `JOB_FAILED`, `ACTIVITY`,
  `BOOT_ANIM_DONE`.
- `ps_tick(now_ms)` drives every timer: offline debounce, stall attention, failed hold, sleep.
  The core owns time-since-line-change, so `serial_out` no longer decides "blocked".
- Output: `ps_view_t { state; setup_phone_joined; asleep; sleep_fade; since_ms }`. One
  function, `ps_view(now)`, used by the LEDs, IPP and the web status alike.
- `STALL_ABORT` is not the core's to enforce; it exposes `ps_stalled_for_ms()` and the
  writer decides when to give up.

**`serial_out`**
- Reports HSKo edges (`on_line(ready)`, debounced to 50 ms) instead of `on_state(BLOCKED)`.
- `serial_writer` blocks while HSKo is false and fails only after `STALL_ABORT`
  (new Kconfig `AIRWRITER_STALL_ABORT`, default 600). `AIRWRITER_STALL_TIMEOUT` (30) becomes
  the attention threshold and keeps its name.

**`job_pipe`**: `PUSH_TIMEOUT` follows `STALL_ABORT` + margin.

**`main`**
- Wi-Fi: backoff reconnect after `WIFI_UP` has been seen; AP station events; posts the
  matching events. The boot-time 60 s join → setup-AP path is unchanged.
- Starts the boot animation before Wi-Fi so the two overlap; posts `BOOT_ANIM_DONE`.
- A 100 ms timer calls `ps_tick`.
- `test_page`, DIP changes and IPP job operations post `ACTIVITY`.

**`ipp_server`**
- A callback `on_job_op` for Print-Job, Validate-Job and Create-Job (→ `ACTIVITY`).
- `ipp_server_set_stopped(reason)` stays; `main` maps the attention and offline states to it.

**`led_grid_core`**
- `lg_render(const ps_view_t *, t_ms, frame)`: one renderer per state, materials as above,
  the Spin as a one-shot keyed off `since_ms`.
- The rainbow bring-up test (W-1034) stays as the override it is today.

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
- `test_serial_writer`: a write blocks across a stall and succeeds when the line returns; fails
  only past `STALL_ABORT`.
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
