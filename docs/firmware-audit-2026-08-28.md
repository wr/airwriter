# Firmware pre-board audit — 2026-08-28

**Status (same day, W-585): must-dos 1–3 and should-do 4 are implemented** on branch
`wells/w-585-firmware-preboard-hardening`. Item 3 landed with a different mechanism
than the minimal fix sketched below — adversarial review proved both that sketch and a
first ring-bounded implementation unsound (`uart_get_tx_buffer_free_size` only tracks
the ring item in service, so `uart_write_bytes` could still park forever mid-stall).
The shipped design writes FIFO-direct via `uart_tx_chars` with no TX ring at all; see
the HANDOFF's 08-28 late entry and `serial_writer.h`. Items 5–6 and the "later" gaps
remain as written.

Three parallel auditors (pin map, serial path, boot safety) over `firmware/` while the
boards are at PCBWay. Nothing was edited; this is the work list. The good news first:
**no pin in the firmware is wrong.** Every configured pin matches the final board
(TX 17, RX 18, CTS 8, console on native USB), `CONFIG_AIRWRITER_INVERT_CTS` correctly
flips the UART peripheral, the watchdog and the status report from one knob, and the
Kconfig is already target-aware. Every problem found is an omission, not a miswire.

## Must do before boards arrive

1. **Reinstall ESP-IDF and switch the target.** IDF is not on this Mac (`~/esp/esp-idf`
   is gone — the HANDOFF stratum claiming v5.5.1 is stale; the 08-25 note is right).
   Everything below is uncompilable until this lands, and the 08-25 firmware edits have
   never been compiled. Then, for the real board:
   `idf.py set-target esp32s3` (never hand-edit `CONFIG_IDF_TARGET` in sdkconfig — it
   skips the defaults merge), `idf.py fullclean`, then confirm the regenerated sdkconfig
   shows TX=17 / RX=18 / CTS=8 and `UART_LOOPBACK` unset before first flash. The current
   `sdkconfig` is the C6 devkit build (TX=4, RX=5, CTS disabled, loopback on).

2. **IO48 clamp (the U9 LED-buffer float).** No GPIO output code exists anywhere in the
   firmware — the only GPIO call in the tree is the CTS read. Add, as the literal first
   statements of `app_main()` (before `nvs_flash_init()`):
   drive IO48 output-low, guarded at compile time for the S3 (the C6's headers have no
   `GPIO_NUM_48`, so an unguarded reference breaks the devkit build). Either follow the
   project's existing Kconfig pattern (`AIRWRITER_LED_OE_PIN`, `default 48 if
   IDF_TARGET_ESP32S3`, `default -1`, runtime `if (pin >= 0)`) or a literal
   `#if CONFIG_IDF_TARGET_ESP32S3` block. Also add `esp_driver_gpio` to
   `main/CMakeLists.txt` REQUIRES — main doesn't link it today.
   Placement matters: `wifi_start()`'s join loop can block up to 60 s, so a clamp placed
   after it means the LED chain floats for the whole join, not "briefly".

3. **`serial_out_write()` deadline.** Confirmed: `serial_out.c:151-160` loops on
   `uart_write_bytes()` (no timeout exists) with `UART_HW_FLOWCTRL_CTS` and a 2 KB TX
   ring; a stalled CTS fills the ring and the write blocks forever. The 30 s watchdog
   only reports — it never unblocks the writer. Traced consequence: the stuck `rip` task
   never exits, `job_pipe_end()` gives up after its 600 s cap with the task still alive,
   and `job_pipe_begin()` then refuses every future job (`s_task` non-NULL) — **one job
   to an unplugged printer bricks the print service ~10 minutes later until power
   cycle.** Minimal fix, no new config and no signature change: inside the write loop,
   fast-fail `ESP_FAIL` when `s_state == SERIAL_OUT_BLOCKED`, and when `!clear_to_send()`
   poll at the watchdog's cadence until CTS returns or the watchdog flips state. Then
   `main.c`'s `printer_out()` must check the return (today it discards it) and call
   `job_pipe_abort()` on failure; note `job_pipe_cfg_t.out` returns void, so the failure
   flag goes in a static next to `s_stream_crc`, and `doc_end()`'s existing
   `st.rc != 0 || !ok` branch should say "printer timeout" rather than "short transfer".

## Should do (not blocking)

4. **Host-test seam for serial_out/job_pipe.** Neither compiles on the host (hard
   ESP-IDF/FreeRTOS includes) and neither has tests; `make test` only covers the five
   pure-C raster components. To test the deadline logic without hardware, put the
   wait/poll primitive behind an injectable function pointer the way the raster
   components stayed pure. Otherwise the fix is verified on hardware only.
5. **PIC MCLR (IO2) — resolved, no code needed.** The boot auditor flagged it as
   unverified; the 08-22 hardware review settles it: TashTalk's config sets `_MCLRE_ON`
   and the datasheet pass verified MCLR rides the PIC's **internal pull-up**. The ESP32
   leaving IO2 floating at boot is safe — the PIC runs from power-on. (LT_DE while the
   PIC boots is known item 4 from 08-25, unchanged.)
6. **Brightness-cap constant.** There is no LED driver at all yet (no RMT/led_strip
   code anywhere), so nothing to cap today — but the cap (~48/255 unless the supply is
   proven ≥1.5 A) must be in `led_grid`'s first commit. Recorded so it survives until
   that component exists.

## Later (feature gaps, on record so they aren't mistaken for done)

- DIP1-8 (IO38/39/40/41/42/47/21/13), test button (IO14): zero firmware — no pull-up
  config, no readers. `dip_config` is unwritten.
- PIC/LocalTalk bridge (IO4/IO5/IO2/IO6, 1 Mbps UART): zero firmware.
- IO35 (HSKI_DRV): moot this rev — R9 is DNP so the path to the SP3232 is open. A
  one-line TODO belongs wherever R9 ever gets populated.

## Bring-up checklist additions (hardware day)

- If serial RX/TX polarity is wrong on the real printer: `uart_set_line_inverse` switches
  exist. If HSKo sense is wrong: `CONFIG_AIRWRITER_INVERT_CTS` flips hardware CTS,
  watchdog and status together — one knob, by design.
- Expect a brief random-pixel LED flash between power-on and app_main's clamp on this
  rev; a 100 k pulldown on U9.2 is the rev-B fix.
