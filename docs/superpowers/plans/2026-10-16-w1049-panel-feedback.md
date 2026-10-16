# W-1049 Panel Feedback Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Everything the board does shows on the 37-dot panel: tasks (firmware, TashTalk) beside
the factory reset, moments that open states, short acknowledgements, LocalTalk's own ink, and a
second tap of the test button that cancels the test page.

**Architecture:** The pure renderer (`led_grid_core`) gains per-family task slots, a blip slot
and a job source in `lg_opts_t`, and draws moments from `ps_view_t.prev`/`cancelled`, which
`printer_state_core` now records. The ESP glue (`led_grid.c`) owns the slots' clocks;
`main.c`, `localtalk.c` and `webui.c` raise the events. The approved look is the throwaway spike
in the session scratchpad (`spike/part_new.c`), which this plan ports.

**Tech Stack:** C11, ESP-IDF v5.5.1 (S3 and C6), host tests in `tools/rip` (`make test`).

**Spec:** `docs/superpowers/specs/2026-10-14-status-language-design.md`, section "Actions,
moments and acknowledgements (W-1049)" and the W-1049 bullets under Architecture and Testing.

## Global Constraints

- Caps unchanged: `LG_BRIGHTNESS_CAP` 48 (FR4), `LG_ALERT_CAP` 96 (FR4, red phases only),
  `LG_MASK_CAP` 192. No frame draws more than the boot spin's heaviest.
- Held back, draw nothing (the state shows): `LG_ACT_FW_REFUSED`, `LG_ACT_FW_ROLLED_BACK`,
  update kept, DIP flipped. Their enum values stay; W-1048 calls REFUSED.
- `/update` belongs to W-1048 (`fw_update.c`): do not touch `update_post`.
- Test patterns never ship; no debug endpoints for the panel.
- New source files carry `// SPDX-License-Identifier: GPL-3.0-or-later`.
- Commits: no attribution lines; Linear IDs written with titles.

## Review Focus

1. A second tap landing as the test page ends on its own: the job lock stays balanced, and the
   page ends once, as done or cancelled, never both. (Bench: tap at the page's last line.)
2. A tap while an IPP job waits in `doc_begin` behind the test page: the tap cancels the test
   page; the IPP job then prints whole, with W-1041's resync first. (Bench.)
3. A Mac's Chooser polling LkUp every second: one "A Mac heard" per 6 s, no flicker. (Glue
   throttle; bench with the Chooser open.)
4. The job source sticks across jobs: an IPP job or test page after a LocalTalk job inks warm.
   (Set at every begin; host test of `src` in Task 2, bench in Task 5.)
5. A task raised while the panel is asleep shows at once at full level. (Host test, Task 2.)

---

### Task 1: printer_state records the previous state and a cancelled job

**Files:**
- Modify: `firmware/components/printer_state/include/printer_state_core.h`
- Modify: `firmware/components/printer_state/printer_state_core.c`
- Test: `tools/rip/test/test_printer_state.c`

**Interfaces:**
- Produces: `ps_view_t.prev` (`ps_state_t`, the display state before `state`; `PS_BOOTING`
  until the first change), `ps_view_t.cancelled` (`uint8_t`), event `PS_EV_JOB_CANCELLED`
  (appended to `ps_event_t`).

- [ ] **Step 1: Failing tests** in `test_printer_state.c`: after `up_and_ready`, Ready's view
  has `prev == PS_JOINING`; pull the line, at the debounce the view is Offline with
  `prev == PS_READY`; line back, Ready with `prev == PS_OFFLINE`. `JOB_START` then
  `JOB_CANCELLED` → Ready with `prev == PS_PRINTING && cancelled`; the next `JOB_START` → Printing
  with `cancelled == 0`; `JOB_DONE` → Ready with `!cancelled`.
- [ ] **Step 2:** `make -C tools/rip test` → `test_printer_state` fails to compile (no `prev`).
- [ ] **Step 3:** Add `prev` and `cancelled` to `ps_t`; `ps_update` sets `ps->prev = ps->last`
  on a change; `JOB_CANCELLED` is `JOB_DONE` plus `cancelled = 1`; `JOB_START` clears it.
- [ ] **Step 4:** `make -C tools/rip test` → all ok.
- [ ] **Step 5:** Commit `printer_state: the previous state and a cancelled job (W-1049)`.

### Task 2: led_grid_core draws tasks, moments, acknowledgements and LocalTalk ink

**Files:**
- Modify: `firmware/components/led_grid/include/led_grid_core.h`
- Modify: `firmware/components/led_grid/led_grid_core.c`
- Test: `tools/rip/test/test_led_grid.c`

**Interfaces:**
- Consumes: Task 1's `prev`, `cancelled`.
- Produces (header):
  - `lg_act_t` appends `LG_ACT_PIC_NONE, LG_ACT_PIC_FLASHING, LG_ACT_PIC_OK, LG_ACT_PIC_FAILED`
    after the firmware values (order is the family boundary).
  - `typedef enum { LG_FAM_RESET, LG_FAM_FW, LG_FAM_PIC, LG_FAMS } lg_fam_t;`
    `lg_fam_t lg_family(uint8_t act);`
  - `typedef enum { LG_BLIP_NONE, LG_BLIP_MAC, LG_BLIP_LT_ON, LG_BLIP_BUSY, LG_BLIP_SAVED } lg_blip_t;`
    `int lg_blip_over(uint8_t blip, uint32_t ms);`
  - `typedef enum { LG_SRC_AIRPRINT, LG_SRC_LOCALTALK } lg_src_t;`
  - `typedef struct { uint8_t act; uint16_t arg; uint32_t ms; } lg_slot_t;`
  - `lg_opts_t { uint8_t gamer; uint8_t src; lg_slot_t task[LG_FAMS]; uint8_t blip; uint32_t blip_ms; }`
    (replaces `act`, `act_from`, `act_ms`).
  - Lengths: `LG_BACK_MS 2000`, `LG_GONE_MS 2000`, `LG_PAGE_MS 1000`, `LG_CANCEL_MS 900`,
    `LG_JOINFAIL_MS 1600`, `LG_FW_VERIFIED_MS 2000`, `LG_PIC_OK_MS 1600`,
    `LG_PIC_FAILED_MS 3000`, `LG_BLIP_MAC_MS 1100`, `LG_BLIP_LT_ON_MS 1600`,
    `LG_BLIP_BUSY_MS 600`, `LG_BLIP_SAVED_MS 900`.
- Values: every number in the spec's three tables; the spike's `part_new.c` is the reference
  implementation of each (port it, then tidy). `lg_act_over`: FW_DOWNLOADING, INSTALLING,
  VERIFYING, VERIFIED and PIC_FLASHING last until replaced; FW_REFUSED, FW_ROLLED_BACK and the
  `*_NONE` values are over at once (held back / clear).

- [ ] **Step 1: Port the reset tests** to `o.task[LG_FAM_RESET]` (`.act`, `.arg`, `.ms`); they
  must still pass unchanged otherwise.
- [ ] **Step 2: Failing tests**, one block each:
  - caps and power for every moment, task and blip over 0..length in 37 ms steps: FR4 ≤ 48
    (≤ 96 only in red phases: RESET_*, PIC_FAILED, join-failed pulses), mask ≤ 192,
    `frame_sum ≤ spin_max`;
  - FR4-only: Printer gone, Printer back from Offline, firmware DOWNLOADING/VERIFYING/VERIFIED
    leave every mask dot dark;
  - each moment ends: at its length the frame equals the plain state's frame (same view with
    `prev = PS_BOOTING`), e.g. Ready after Offline at `since = LG_BACK_MS`;
  - unlisted transitions draw the plain state (Attention→Offline with `cancelled`, Failed→Ready);
  - Cancelled differs from Page out at `since = 300` and both show ink on a mask dot;
  - LocalTalk ink: Printing with `src = LG_SRC_LOCALTALK` lights a row-0 dot greener than red
    and a row-7 dot bluer than red; AirPrint's red ≥ green ≥ blue on the same dots;
  - priority: reset HOLD + FW_DOWNLOADING draws the reset frame; reset CANCEL past
    `LG_RESET_CANCEL_MS` draws firmware; FW + PIC_FLASHING draws firmware;
  - firmware progress: lit FR4 count at arg 900 > at arg 100; `LG_PROGRESS_NONE` lights some FR4;
  - held back: FW_REFUSED and FW_ROLLED_BACK draw the plain state's frame;
  - blips: dropped (frame equals plain state) in Booting, Attention, Failed and under any task;
    each `lg_blip_over` at its length; Busy lights only dots within 1.6 of (3,3), red > green > blue;
  - a task while asleep (`asleep = 1`, `asleep_ms = 20000`) lights FR4 dots above the pilot's level.
- [ ] **Step 3:** `make -C tools/rip test` → `test_led_grid` fails to compile.
- [ ] **Step 4:** Implement in `led_grid_core.c`, porting the spike: `stripe()`, `view_dot()`
  (moments), `task_frame()` (reset + firmware + PIC), `blip_dot()`, `screen()`, and
  `lg_render16()`'s order (first running task by family; else state + moment + blip; then the
  pilot). Update the file's header comment.
- [ ] **Step 5:** `make -C tools/rip test` → all ok; `make -C tools/rip regress` unchanged.
- [ ] **Step 6:** Commit `led_grid: W-1049 tasks, moments, acknowledgements and LocalTalk ink`.

### Task 3: led_grid glue: slots, blips and the job source

**Files:**
- Modify: `firmware/components/led_grid/include/led_grid.h`, `firmware/components/led_grid/led_grid.c`

**Interfaces:**
- Consumes: Task 2's types.
- Produces: `void led_grid_action(lg_act_t act, uint16_t arg);` (per-family slot; the same act
  again updates `arg` and keeps its clock), `void led_grid_blip(lg_blip_t b);`,
  `void led_grid_set_source(lg_src_t s);`. A blip raised while the view is `PS_BOOTING` keeps
  restarting its clock each frame until Booting ends, so it plays whole after the Spin.

- [ ] **Step 1:** Implement; the render task copies all slots under `s_act_mux` each frame.
- [ ] **Step 2:** `idf.py build` (S3) clean.
- [ ] **Step 3:** Commit `led_grid: per-family action slots, acknowledgements, job source`.

### Task 4: the events reach the panel

**Files:**
- Modify: `firmware/components/localtalk/include/localtalk.h`, `firmware/components/localtalk/localtalk.c`
- Modify: `firmware/components/webui/webui.c`, `firmware/components/webui/CMakeLists.txt`
- Modify: `firmware/main/main.c`

**Interfaces:**
- Produces: `typedef enum { LT_EV_ONLINE, LT_EV_LOOKUP, LT_EV_NEW_NODE } lt_event_t;`
  `localtalk_cfg_t.on_event` (`void (*)(lt_event_t)`, may be NULL), called on the LocalTalk
  task after the node lock is released.

- [ ] **Step 1: localtalk.c** — after each `tt_rx_feed`/`lt_node_tick`, compare a snapshot of
  `s_node.phase`, `s_node.st.replies` and `s_node.heard[]` with the last one and raise the
  events (ONLINE once per transition to `LT_ONLINE`; NEW_NODE once per newly set bit).
- [ ] **Step 2: webui.c** — `wifi_post` and `general_post` call `led_grid_blip(LG_BLIP_SAVED)`
  after the save; add `led_grid` to `REQUIRES`.
- [ ] **Step 3: main.c**
  - `led_grid_set_source()` in `doc_begin` (AIRPRINT), `lt_print_begin` (LOCALTALK),
    `test_page_task` (AIRPRINT), before each `JOB_START`.
  - `job_finished` posts `PS_EV_JOB_CANCELLED` where it posts DONE for `s_job_cancelled`.
  - Test tap (`test_page`): lock free → print (unchanged). Lock held by the test page
    (a new `s_test_running`) → `s_job_cancelled = 1; serial_out_set_abort(1);` and the task
    ends as CANCELLED (no FAILED, link marked cut for the resync). Lock held by an IPP or
    LocalTalk job → `led_grid_blip(LG_BLIP_BUSY)`.
  - `localtalk_flash`: `PIC_FLASHING` before `pic_prog_flash_tashtalk`, then `PIC_OK` or
    `PIC_FAILED`; post `ACTIVITY`.
  - `lt_start` sets `.on_event`: ONLINE → `LG_BLIP_LT_ON`; LOOKUP / NEW_NODE → `LG_BLIP_MAC`
    at most every 6000 ms, and `ACTIVITY`.
- [ ] **Step 4:** `idf.py build` (S3) and `idf.py -B build_c6 -D SDKCONFIG=sdkconfig.c6 build`
  clean; `make -C tools/rip test` ok.
- [ ] **Step 5:** Commit `W-1049: the board's actions reach the panel; a second tap cancels the test page`.

### Task 5: bench and docs

- [ ] **Step 1:** Rebase on main once W-1048 has merged; rebuild (signed); host tests.
- [ ] **Step 2:** Ask the main session for a flash slot; OTA board 1 per the bench rules; walk
  the spec's W-1049 bench list with Wells's eyes, via the main session.
- [ ] **Step 3:** AGENTS.md (State: what the panel shows; Open work) and the wiki's
  Status-Lights page (plain first person). Commit; report ready for "merge it".
