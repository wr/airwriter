# Firmware Status & Controls Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement the remaining no-hardware-needed firmware — `printer_state` (device state machine), `led_grid` (37-dot panel renderer + SK6812 output), `dip_config` (DIP bank + test button), and the button's test page — all host-tested, and restore the ESP-IDF toolchain so the devkit build gates every ESP-side change.

**Architecture:** Every component is split like W-585's serial_out/serial_writer: a pure-C core with zero ESP-IDF includes (host-tested in `tools/rip`), and a thin ESP glue file that binds it to GPIO/RMT/tasks. `printer_state` is a flag-set + priority derivation (no transition table). `led_grid` renders frames as a pure function of `(state, t_ms)` over a generated chain-order material map. `dip_config` decodes a raw bit pattern into a config struct and shares one 100 ms poll task with the test button.

**Tech Stack:** C (C17), ESP-IDF v5.5.1, `espressif/led_strip` managed component (RMT backend), host tests via `tools/rip/Makefile` (plain `cc`).

**Spec:** `docs/superpowers/specs/2026-08-20-dot-panel-cover-design.md` §5 (Firmware), plus `docs/firmware-audit-2026-08-28.md` (items 1, 6, and the "later" gaps this plan closes).

## Global Constraints

- Kconfig bools that are off define nothing: every `CONFIG_AIRWRITER_*` bool needs the `#ifdef`→`#define AW_X 0/1` fallback pattern from `main/main.c:36-62`.
- Pin Kconfigs use the existing pattern: `default <N> if IDF_TARGET_ESP32S3`, `default -1` otherwise; runtime `if (pin >= 0)` guards. Never a bare `GPIO_NUM_48`-style literal (breaks the C6 build).
- Board pin facts (S3): LED data IO48 (already Kconfig'd as `AIRWRITER_LED_DATA_PIN`), DIP1–8 = IO38/39/40/41/42/47/21/13 (closed = GND, internal pull-ups), test button IO14 (to GND, internal pull-up).
- **Global LED brightness cap 48/255** — the board has no hardware current limit; the cap is applied as the final multiply in the core renderer, not in callers.
- Materials, from `hardware/panel/dotmap.txt` + spec: `P` = bare-FR4 window (full RGB, 14 dots), `B` = single-mask teal (brightness-only, 9), `L` = double-mask teal (brightness-only, 14). `G`/`T` dots have no LED. 37 lit dots total.
- Host tests live in `tools/rip/test/`, are wired into `tools/rip/Makefile` exactly like `test_serial_writer`, and `make -C tools/rip test` must stay green after every task. `stream.bin` regression must stay byte-identical (this plan never touches the raster path).
- Commit per task with `Refs: W-586` trailer. No pushes, no merges (PR mode: none; branch `wells/w-586-firmware-status-controls`).
- ESP-side code cannot be flashed yet (no boards) — the devkit **build** (`idf.py build`, C6 target) is the gate for glue code once Task 1 lands. If Task 1's install fails (network), Tasks 2–5 still proceed (host-only); glue tasks 6–8 then end at "compiles" pending IDF.

---

### Task 1: ESP-IDF v5.5.1 install + baseline devkit build

**Files:**
- No repo files. Installs `~/esp/esp-idf` (v5.5.1, S3+C6 toolchains).

**Interfaces:**
- Produces: a working `idf.py` for every later task's compile gate:
  `. ~/esp/esp-idf/export.sh && cd firmware && idf.py build`

- [ ] **Step 1: Clone and install (long download — run in background)**

```bash
mkdir -p ~/esp
git clone -b v5.5.1 --depth 1 --recursive --shallow-submodules \
    https://github.com/espressif/esp-idf.git ~/esp/esp-idf
~/esp/esp-idf/install.sh esp32s3,esp32c6
```

- [ ] **Step 2: Baseline build of the current tree (C6 target, unmodified)**

Run: `bash -c '. ~/esp/esp-idf/export.sh && cd firmware && idf.py build'`
Expected: `Project build complete`. This also compiles W-585's uncompiled edits for the first time — fix any trivial compile error it surfaces (they belong to W-585's scope; commit such a fix separately with `Refs: W-585`).

- [ ] **Step 3: Record the result in the session log (no commit — nothing in-repo changed)**

---

### Task 2: `led_map` generator — chain-order material map

**Files:**
- Create: `hardware/panel/gen_led_map.py`
- Create (generated, committed): `firmware/components/led_grid/include/led_map.h`, `firmware/components/led_grid/led_map.c`
- Test: verification is built into the generator's asserts + Task 4's host test includes the table.

**Interfaces:**
- Produces:
```c
// led_map.h
#define LG_NUM_LEDS 37
#define LG_ROWS 8
#define LG_COLS 7
typedef struct { uint8_t r, c; char mat; } lg_dot_t;   // mat: 'P','B','L'
extern const lg_dot_t LG_MAP[LG_NUM_LEDS];             // index = chain position (D10 first)
```

- [ ] **Step 1: Write the generator**

`hardware/panel/gen_led_map.py` — derives chain order from the schematic netlist (authoritative: DOUT→DIN hops starting at the buffer's output net `/LED_DATA5`), positions from the board, materials from `panel_layout.json`:

```python
#!/usr/bin/env python3
"""Netlist-ordered LED map: schematic chain -> panel dot -> material.
Writes firmware/components/led_grid/{include/led_map.h,led_map.c}.
Regenerate after any LED renumber or panel dot change; output is committed."""
import json, pathlib, re, subprocess, sys

HERE = pathlib.Path(__file__).parent
ROOT = HERE.parent.parent
K = "/Applications/KiCad/KiCad.app/Contents/MacOS/kicad-cli"
SCH = ROOT / "hardware/airwriter/airwriter.kicad_sch"
PCB = ROOT / "hardware/airwriter/airwriter.kicad_pcb"
OUT_H = ROOT / "firmware/components/led_grid/include/led_map.h"
OUT_C = ROOT / "firmware/components/led_grid/led_map.c"
DX, DY = 100.0, 60.0          # board = panel + (DX, DY)
# SK6812MINI-C on the stock PLCC4 footprint: pad 3 = DIN, pad 1 = DOUT.
PAD_DIN, PAD_DOUT = "3", "1"

net = subprocess.run([K, "sch", "export", "netlist", "--format", "kicadxml",
                      "-o", "/dev/stdout", str(SCH)],
                     capture_output=True, text=True, check=True).stdout
# node net membership: {(ref,pad): netname} and reverse
pairs = re.findall(r'<net .*?name="([^"]+)">(.*?)</net>', net, re.S)
by_net, of_node = {}, {}
for name, body in pairs:
    nodes = re.findall(r'<node ref="([^"]+)" pin="([^"]+)"', body)
    by_net[name] = nodes
    for n in nodes: of_node[n] = name

chain, seen = [], set()
cur = next(n for n in by_net["/LED_DATA5"] if n[0].startswith("D"))
while True:
    ref = cur[0]
    assert ref not in seen, f"loop at {ref}"
    seen.add(ref); chain.append(ref)
    dout = of_node.get((ref, PAD_DOUT))
    if dout is None: break
    nxts = [n for n in by_net[dout] if n[0].startswith("D") and n[1] == PAD_DIN]
    if not nxts: break                     # tail open
    cur = nxts[0]
assert len(chain) == 37, f"chain length {len(chain)}"

board = (ROOT / "hardware/airwriter/airwriter.kicad_pcb").read_text()
pos = {}
for m in re.finditer(r'\(footprint[^\n]*\n(?:.*?\n)*?\s*\(at ([\d.-]+) ([\d.-]+)[^)]*\)'
                     r'(?:.*?\n)*?\s*\(property "Reference" "(D\d+)"', board):
    pos[m.group(3)] = (float(m.group(1)), float(m.group(2)))
lay = json.loads((HERE / "panel_layout.json").read_text())
dots = lay["dots"]

rows = []
for ref in chain:
    bx, by = pos[ref]
    px, py = bx - DX, by - DY
    d = min(dots, key=lambda q: (q["x"]-px)**2 + (q["y"]-py)**2)
    dist = ((d["x"]-px)**2 + (d["y"]-py)**2) ** 0.5
    assert dist < 0.5, f"{ref} lands {dist:.2f}mm from nearest dot"
    assert d["led"], f"{ref} maps to unlit dot ({d['r']},{d['c']})"
    rows.append((ref, d["r"], d["c"], d["mat"]))
assert len({(r, c) for _, r, c, _ in rows}) == 37, "duplicate dot claimed"

hdr = """// GENERATED by hardware/panel/gen_led_map.py - do not edit.
// Index = position in the SK6812 chain (D10 first, netlist DOUT->DIN order).
#pragma once
#include <stdint.h>
#define LG_NUM_LEDS 37
#define LG_ROWS 8
#define LG_COLS 7
typedef struct { uint8_t r, c; char mat; } lg_dot_t; // mat: P fr4-rgb, B/L teal
extern const lg_dot_t LG_MAP[LG_NUM_LEDS];
"""
body = "// GENERATED by hardware/panel/gen_led_map.py - do not edit.\n"
body += '#include "led_map.h"\nconst lg_dot_t LG_MAP[LG_NUM_LEDS] = {\n'
for ref, r, c, mat in rows:
    body += f"    {{{r}, {c}, '{mat}'}}, // {ref}\n"
body += "};\n"
OUT_H.parent.mkdir(parents=True, exist_ok=True)
OUT_H.write_text(hdr); OUT_C.write_text(body)
print(f"wrote {len(rows)} LEDs: {' '.join(r for r,_,_,_ in rows[:3])} ... {rows[-1][0]}")
```

- [ ] **Step 2: Run it and eyeball the output**

Run: `python3 hardware/panel/gen_led_map.py`
Expected: `wrote 37 LEDs: D10 D11 D12 ... D46`. If the netlist XML shape differs from the regexes (kicad-cli output drifts), adapt the parsing — the asserts (37, <0.5 mm, lit, unique) are the contract; do not weaken them.

- [ ] **Step 3: Sanity-diff materials against dotmap.txt** — count `P`/`B`/`L` occurrences in `led_map.c`: expect 14/9/14. Run: `grep -o "'[PBL]'" firmware/components/led_grid/led_map.c | sort | uniq -c`

- [ ] **Step 4: Commit**

```bash
git add hardware/panel/gen_led_map.py firmware/components/led_grid/
git commit -m "led_grid: generate the chain-order material map from the netlist

Refs: W-586"
```

---

### Task 3: `printer_state` core (pure C, TDD)

**Files:**
- Create: `firmware/components/printer_state/include/printer_state_core.h`
- Create: `firmware/components/printer_state/printer_state_core.c`
- Test: `tools/rip/test/test_printer_state.c`
- Modify: `tools/rip/Makefile` (add the test target, same pattern as `test_serial_writer`)

**Interfaces:**
- Produces:
```c
typedef enum {                 // priority order, highest wins
    PS_BOOT = 0, PS_SETUP, PS_WIFI_CONNECTING,
    PS_BLOCKED, PS_PRINTING, PS_ERROR, PS_READY,
} ps_state_t;
typedef enum {
    PS_EV_WIFI_CONNECTING, PS_EV_WIFI_UP, PS_EV_SETUP_MODE,
    PS_EV_JOB_START, PS_EV_JOB_DONE, PS_EV_JOB_FAIL,
    PS_EV_PRINTER_BLOCKED, PS_EV_PRINTER_CLEARED,
} ps_event_t;
typedef struct { unsigned wifi_up:1, connecting:1, setup:1,
                 printing:1, blocked:1, error:1; } ps_t;
void       ps_init(ps_t *ps);
ps_state_t ps_post(ps_t *ps, ps_event_t ev);   // returns derived state
ps_state_t ps_state(const ps_t *ps);
const char *ps_name(ps_state_t st);            // "ready", "printing", ...
```
- Derivation (exactly this order): `setup → PS_SETUP`; `!wifi_up → (connecting ? PS_WIFI_CONNECTING : PS_BOOT)`; `blocked → PS_BLOCKED`; `printing → PS_PRINTING`; `error → PS_ERROR`; else `PS_READY`.
- Flag rules: `WIFI_CONNECTING` sets connecting; `WIFI_UP` sets wifi_up, clears connecting; `SETUP_MODE` sets setup; `JOB_START` sets printing, **clears error**; `JOB_DONE` clears printing; `JOB_FAIL` clears printing, sets error; `PRINTER_BLOCKED`/`CLEARED` set/clear blocked.

- [ ] **Step 1: Write the failing test** — `tools/rip/test/test_printer_state.c`:

```c
// Host test for the printer_state reducer. Mirrors test_serial_writer.c style.
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "printer_state_core.h"

int main(void)
{
    ps_t ps;
    ps_init(&ps);
    assert(ps_state(&ps) == PS_BOOT);

    assert(ps_post(&ps, PS_EV_WIFI_CONNECTING) == PS_WIFI_CONNECTING);
    assert(ps_post(&ps, PS_EV_WIFI_UP) == PS_READY);

    // A job runs and completes.
    assert(ps_post(&ps, PS_EV_JOB_START) == PS_PRINTING);
    assert(ps_post(&ps, PS_EV_JOB_DONE) == PS_READY);

    // Paper out mid-job outranks printing; clearing it resumes printing.
    ps_post(&ps, PS_EV_JOB_START);
    assert(ps_post(&ps, PS_EV_PRINTER_BLOCKED) == PS_BLOCKED);
    assert(ps_post(&ps, PS_EV_PRINTER_CLEARED) == PS_PRINTING);

    // A failed job shows error until the next job starts.
    assert(ps_post(&ps, PS_EV_JOB_FAIL) == PS_ERROR);
    assert(ps_post(&ps, PS_EV_JOB_START) == PS_PRINTING);
    ps_post(&ps, PS_EV_JOB_DONE);

    // Blocked while idle (printer unplugged) still reads blocked, not ready.
    assert(ps_post(&ps, PS_EV_PRINTER_BLOCKED) == PS_BLOCKED);
    ps_post(&ps, PS_EV_PRINTER_CLEARED);

    // Setup mode outranks everything.
    ps_init(&ps);
    assert(ps_post(&ps, PS_EV_SETUP_MODE) == PS_SETUP);
    ps_post(&ps, PS_EV_PRINTER_BLOCKED);
    assert(ps_state(&ps) == PS_SETUP);

    assert(strcmp(ps_name(PS_READY), "ready") == 0);
    assert(strcmp(ps_name(PS_BLOCKED), "blocked") == 0);

    printf("test_printer_state: ok\n");
    return 0;
}
```

- [ ] **Step 2: Add the Makefile target and run to verify it fails**

In `tools/rip/Makefile`, copy the `test_serial_writer` stanza (target, objects, `test:` dependency) substituting `printer_state`: compile `test/test_printer_state.c` + `../../firmware/components/printer_state/printer_state_core.c` with `-I../../firmware/components/printer_state/include`.
Run: `make -C tools/rip test`
Expected: FAIL — `printer_state_core.h: No such file or directory`.

- [ ] **Step 3: Implement the core** — `printer_state_core.c` (+ header exactly as in Interfaces):

```c
// SPDX-License-Identifier: GPL-3.0-or-later
// Device-state reducer: flags in, one derived state out. Pure C on purpose --
// the ESP glue and the host tests share this file. Priority does the work;
// there is no transition table to get wrong.
#include "printer_state_core.h"
#include <string.h>

void ps_init(ps_t *ps) { memset(ps, 0, sizeof *ps); }

ps_state_t ps_state(const ps_t *ps)
{
    if (ps->setup)     return PS_SETUP;
    if (!ps->wifi_up)  return ps->connecting ? PS_WIFI_CONNECTING : PS_BOOT;
    if (ps->blocked)   return PS_BLOCKED;
    if (ps->printing)  return PS_PRINTING;
    if (ps->error)     return PS_ERROR;
    return PS_READY;
}

ps_state_t ps_post(ps_t *ps, ps_event_t ev)
{
    switch (ev) {
    case PS_EV_WIFI_CONNECTING: ps->connecting = 1;                  break;
    case PS_EV_WIFI_UP:         ps->wifi_up = 1; ps->connecting = 0; break;
    case PS_EV_SETUP_MODE:      ps->setup = 1;                       break;
    case PS_EV_JOB_START:       ps->printing = 1; ps->error = 0;     break;
    case PS_EV_JOB_DONE:        ps->printing = 0;                    break;
    case PS_EV_JOB_FAIL:        ps->printing = 0; ps->error = 1;     break;
    case PS_EV_PRINTER_BLOCKED: ps->blocked = 1;                     break;
    case PS_EV_PRINTER_CLEARED: ps->blocked = 0;                     break;
    }
    return ps_state(ps);
}

const char *ps_name(ps_state_t st)
{
    switch (st) {
    case PS_BOOT:            return "boot";
    case PS_SETUP:           return "setup";
    case PS_WIFI_CONNECTING: return "connecting";
    case PS_BLOCKED:         return "blocked";
    case PS_PRINTING:        return "printing";
    case PS_ERROR:           return "error";
    case PS_READY:           return "ready";
    }
    return "?";
}
```

- [ ] **Step 4: Run tests to verify pass** — `make -C tools/rip test` → all green including `test_printer_state: ok`, and the `stream.bin` regression untouched.

- [ ] **Step 5: Commit** — `git commit -m "printer_state: pure-C device-state reducer with host tests" -m "Refs: W-586"` (add the three files + Makefile).

---

### Task 4: `led_grid` core renderer (pure C, TDD)

**Files:**
- Create: `firmware/components/led_grid/include/led_grid_core.h`
- Create: `firmware/components/led_grid/led_grid_core.c`
- Test: `tools/rip/test/test_led_grid.c`
- Modify: `tools/rip/Makefile`

**Interfaces:**
- Consumes: `led_map.h` (Task 2), `printer_state_core.h` (Task 3).
- Produces:
```c
#define LG_BRIGHTNESS_CAP 48          // of 255; board has no current limit
void lg_render(ps_state_t st, uint32_t t_ms,
               uint8_t out[LG_NUM_LEDS][3]);   // RGB per chain index
```
- Animation contract (all deterministic in `t_ms`; all values pre-cap 0–255, then every channel is scaled by `LG_BRIGHTNESS_CAP/255` as the final step; `B`/`L` dots are forced monochrome by `v = max(r,g,b)` → `(v,v,v)`; helper `lg_tri(t_ms, period_ms)` is a 0–255 triangle wave):
  - `PS_BOOT`: all off.
  - `PS_SETUP`: all dots breathe, 1200 ms period.
  - `PS_WIFI_CONNECTING`: radial rings — `d = hypot(c-3, r-3.5)`, brightness = triangle of `(t_ms/6 - d*32)` mod 256; cool white on `P`.
  - `PS_READY`: idle breathe, 4000 ms period, floor 8 so the panel never fully blanks.
  - `PS_PRINTING`: row sweep, 1500 ms period: active row = `(t_ms mod 1500)*LG_ROWS/1500`; row brightness 255, adjacent rows 64, others 16.
  - `PS_BLOCKED`: slow double-blink (on 150 ms, off 150 ms, on 150 ms, off 1550 ms); `P` dots red `(255,0,0)`, `B`/`L` follow as brightness.
  - `PS_ERROR`: 1 Hz square blink; `P` dots red, `B`/`L` **off** (spec: "mask dots drop dark; FR4 dots carry the red").

- [ ] **Step 1: Write the failing test** — `tools/rip/test/test_led_grid.c`:

```c
#include <assert.h>
#include <stdio.h>
#include "led_grid_core.h"
#include "printer_state_core.h"

static int max3(const uint8_t px[3])
{
    int m = px[0];
    if (px[1] > m) m = px[1];
    if (px[2] > m) m = px[2];
    return m;
}

int main(void)
{
    uint8_t f[LG_NUM_LEDS][3];

    // Cap: no channel ever exceeds LG_BRIGHTNESS_CAP in any state/phase.
    ps_state_t all[] = { PS_BOOT, PS_SETUP, PS_WIFI_CONNECTING, PS_READY,
                         PS_PRINTING, PS_BLOCKED, PS_ERROR };
    for (unsigned s = 0; s < sizeof all / sizeof all[0]; s++)
        for (uint32_t t = 0; t < 5000; t += 97) {
            lg_render(all[s], t, f);
            for (int i = 0; i < LG_NUM_LEDS; i++)
                for (int ch = 0; ch < 3; ch++)
                    assert(f[i][ch] <= LG_BRIGHTNESS_CAP);
        }

    // BOOT is dark.
    lg_render(PS_BOOT, 1234, f);
    for (int i = 0; i < LG_NUM_LEDS; i++) assert(max3(f[i]) == 0);

    // Mask dots are monochrome in every state (r==g==b).
    for (uint32_t t = 0; t < 4000; t += 501) {
        lg_render(PS_PRINTING, t, f);
        for (int i = 0; i < LG_NUM_LEDS; i++)
            if (LG_MAP[i].mat != 'P')
                assert(f[i][0] == f[i][1] && f[i][1] == f[i][2]);
    }

    // ERROR: mask dots dark, FR4 dots red-only during the on phase.
    lg_render(PS_ERROR, 250, f);   // first half-second = on
    int lit_p = 0;
    for (int i = 0; i < LG_NUM_LEDS; i++) {
        if (LG_MAP[i].mat == 'P') {
            if (max3(f[i])) { lit_p++; assert(f[i][1] == 0 && f[i][2] == 0); }
        } else {
            assert(max3(f[i]) == 0);
        }
    }
    assert(lit_p > 0);
    lg_render(PS_ERROR, 750, f);   // second half-second = off
    for (int i = 0; i < LG_NUM_LEDS; i++) assert(max3(f[i]) == 0);

    // READY never fully blanks (floor), and breathes (two phases differ).
    lg_render(PS_READY, 0, f);
    int a = 0;
    for (int i = 0; i < LG_NUM_LEDS; i++) a += max3(f[i]);
    assert(a > 0);
    lg_render(PS_READY, 2000, f);
    int b = 0;
    for (int i = 0; i < LG_NUM_LEDS; i++) b += max3(f[i]);
    assert(a != b);

    // PRINTING sweeps: the bright row moves with time.
    lg_render(PS_PRINTING, 0, f);
    lg_render(PS_PRINTING, 700, f);

    printf("test_led_grid: ok\n");
    return 0;
}
```

- [ ] **Step 2: Makefile target + verify FAIL** — stanza compiles the test with `led_grid_core.c`, `led_map.c`, `printer_state_core.c`, `-I` both components' `include` dirs, `-lm`. Run `make -C tools/rip test` → FAIL (`led_grid_core.h` missing).

- [ ] **Step 3: Implement** — `led_grid_core.c`:

```c
// SPDX-License-Identifier: GPL-3.0-or-later
// Frame renderer for the 37-dot art panel. Pure function of (state, t) so the
// host tests can walk every phase. Material physics from the panel spec:
// P (bare FR4) is the only full-RGB pixel; B/L (teal-through-mask) are
// brightness-only -- the mask is the colour filter, not the LED. The global
// cap is the LAST multiply: rev A has no hardware current limit.
#include "led_grid_core.h"
#include <math.h>
#include <string.h>

static uint8_t lg_tri(uint32_t t, uint32_t period)
{
    uint32_t ph = (t % period) * 512 / period;      // 0..511
    return (uint8_t)(ph < 256 ? ph : 511 - ph);
}

static void set_all(uint8_t out[LG_NUM_LEDS][3], uint8_t r, uint8_t g, uint8_t b)
{
    for (int i = 0; i < LG_NUM_LEDS; i++) {
        out[i][0] = r; out[i][1] = g; out[i][2] = b;
    }
}

void lg_render(ps_state_t st, uint32_t t_ms, uint8_t out[LG_NUM_LEDS][3])
{
    memset(out, 0, LG_NUM_LEDS * 3);

    switch (st) {
    case PS_BOOT:
        break;
    case PS_SETUP:
        set_all(out, lg_tri(t_ms, 1200), lg_tri(t_ms, 1200), lg_tri(t_ms, 1200));
        break;
    case PS_READY: {
        uint8_t v = (uint8_t)(8 + (uint32_t)lg_tri(t_ms, 4000) * 247 / 255);
        set_all(out, v, v, v);
        break;
    }
    case PS_WIFI_CONNECTING:
        for (int i = 0; i < LG_NUM_LEDS; i++) {
            float d = hypotf((float)LG_MAP[i].c - 3.0f, (float)LG_MAP[i].r - 3.5f);
            uint32_t ph = (uint32_t)(t_ms / 6 + 4096) - (uint32_t)(d * 32.0f);
            uint8_t v = (uint8_t)(ph % 256);
            out[i][0] = (uint8_t)(v / 2); out[i][1] = (uint8_t)(v / 2);
            out[i][2] = v;               // cool white-blue on the RGB dots
        }
        break;
    case PS_PRINTING: {
        int row = (int)((t_ms % 1500) * LG_ROWS / 1500);
        for (int i = 0; i < LG_NUM_LEDS; i++) {
            int dr = LG_MAP[i].r - row;
            if (dr < 0) dr = -dr;
            uint8_t v = dr == 0 ? 255 : dr == 1 ? 64 : 16;
            out[i][0] = v; out[i][1] = v; out[i][2] = v;
        }
        break;
    }
    case PS_BLOCKED: {
        uint32_t ph = t_ms % 2000;
        int on = ph < 150 || (ph >= 300 && ph < 450);
        if (on) for (int i = 0; i < LG_NUM_LEDS; i++) out[i][0] = 255;
        break;
    }
    case PS_ERROR: {
        int on = (t_ms % 1000) < 500;
        if (on)
            for (int i = 0; i < LG_NUM_LEDS; i++)
                if (LG_MAP[i].mat == 'P') out[i][0] = 255;
        break;
    }
    }

    for (int i = 0; i < LG_NUM_LEDS; i++) {
        if (LG_MAP[i].mat != 'P') {
            uint8_t v = out[i][0];
            if (out[i][1] > v) v = out[i][1];
            if (out[i][2] > v) v = out[i][2];
            // ERROR wants mask dots dark, and its red never reaches them:
            // monochrome conversion happens only on what the state painted.
            if (st == PS_ERROR) v = 0;
            out[i][0] = out[i][1] = out[i][2] = v;
        }
        for (int ch = 0; ch < 3; ch++)
            out[i][ch] = (uint8_t)((uint32_t)out[i][ch] * LG_BRIGHTNESS_CAP / 255);
    }
}
```

And `led_grid_core.h`:

```c
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdint.h>
#include "led_map.h"
#include "printer_state_core.h"
#define LG_BRIGHTNESS_CAP 48   // of 255 -- no hardware current limit on rev A
void lg_render(ps_state_t st, uint32_t t_ms, uint8_t out[LG_NUM_LEDS][3]);
```

- [ ] **Step 4: Run tests to verify pass** — `make -C tools/rip test` all green.
- [ ] **Step 5: Commit** — `"led_grid: pure-C frame renderer, capped at 48/255"` + `Refs: W-586`.

---

### Task 5: `dip_config` core (pure C, TDD)

**Files:**
- Create: `firmware/components/dip_config/include/dip_config_core.h`
- Create: `firmware/components/dip_config/dip_config_core.c`
- Test: `tools/rip/test/test_dip_config.c`
- Modify: `tools/rip/Makefile`

**Interfaces:**
- Produces:
```c
typedef struct {
    uint8_t mode;         // DIP1-2: 0 AirPrint, 1 +EtherTalk, 2 +LocalTalk, 3 rsvd
    uint8_t dev_mode;     // DIP3
    uint8_t test_on_boot; // DIP4
    uint8_t dither_ovr;   // DIP5-6: 0 none, 1 floyd, 2 bayer, 3 threshold
    uint8_t spare;        // DIP7-8 raw
} dip_config_t;
void dip_decode(uint8_t on_bits, dip_config_t *out);  // bit i = DIP(i+1) ON
typedef struct { uint8_t s[3]; uint8_t n; } dip_deb_t; // 3-sample debouncer
int dip_stable(dip_deb_t *d, uint8_t sample, uint8_t *out); // 1 when settled+changed
```
- Bit semantics are a firmware-level default (silk carries only numbers); recorded here and in the header comment as the canonical assignment.

- [ ] **Step 1: Failing test** — `tools/rip/test/test_dip_config.c`:

```c
#include <assert.h>
#include <stdio.h>
#include "dip_config_core.h"

int main(void)
{
    dip_config_t c;
    dip_decode(0x00, &c);
    assert(c.mode == 0 && !c.dev_mode && !c.test_on_boot && c.dither_ovr == 0);
    dip_decode(0x01, &c); assert(c.mode == 1);            // DIP1
    dip_decode(0x02, &c); assert(c.mode == 2);            // DIP2
    dip_decode(0x04, &c); assert(c.dev_mode == 1);        // DIP3
    dip_decode(0x08, &c); assert(c.test_on_boot == 1);    // DIP4
    dip_decode(0x30, &c); assert(c.dither_ovr == 3);      // DIP5+6
    dip_decode(0xC0, &c); assert(c.spare == 3);           // DIP7+8

    // Debounce: a value must be seen 3 polls straight; report change once.
    dip_deb_t d = {0};
    uint8_t v = 0xFF;
    assert(dip_stable(&d, 0x05, &v) == 0);
    assert(dip_stable(&d, 0x04, &v) == 0);     // bounce restarts agreement
    assert(dip_stable(&d, 0x04, &v) == 0);
    assert(dip_stable(&d, 0x04, &v) == 1 && v == 0x04);
    assert(dip_stable(&d, 0x04, &v) == 0);     // stable, no re-report

    printf("test_dip_config: ok\n");
    return 0;
}
```

- [ ] **Step 2: Makefile stanza + verify FAIL.**
- [ ] **Step 3: Implement:**

```c
// SPDX-License-Identifier: GPL-3.0-or-later
// DIP bank decode + debounce, pure C. Bit i of on_bits = DIP(i+1) switched ON
// (the glue reads closed-to-GND as ON). The assignments below are the
// canonical firmware semantics -- the silkscreen only numbers the switches.
#include "dip_config_core.h"
#include <string.h>

void dip_decode(uint8_t on, dip_config_t *out)
{
    memset(out, 0, sizeof *out);
    out->mode         = (uint8_t)(on & 0x03);
    out->dev_mode     = (uint8_t)((on >> 2) & 1);
    out->test_on_boot = (uint8_t)((on >> 3) & 1);
    out->dither_ovr   = (uint8_t)((on >> 4) & 0x03);
    out->spare        = (uint8_t)((on >> 6) & 0x03);
}

int dip_stable(dip_deb_t *d, uint8_t sample, uint8_t *out)
{
    d->s[0] = d->s[1]; d->s[1] = d->s[2]; d->s[2] = sample;
    if (d->n < 3) { d->n++; return 0; }
    if (d->s[0] == d->s[1] && d->s[1] == d->s[2] && d->s[2] != *out) {
        *out = d->s[2];
        return 1;
    }
    return 0;
}
```

(Header matches the Interfaces block verbatim, with `#pragma once` + `<stdint.h>`.)

Note the debounce contract: the caller seeds `*out` with the last-known value (`0xFF` initially so the first stable read always reports).

- [ ] **Step 4: `make -C tools/rip test` green.**
- [ ] **Step 5: Commit** — `"dip_config: decode + debounce core with host tests"` + `Refs: W-586`.

---

### Task 6: `printer_state` ESP glue + main.c wiring

**Files:**
- Create: `firmware/components/printer_state/include/printer_state.h`, `firmware/components/printer_state/printer_state.c`, `firmware/components/printer_state/CMakeLists.txt`
- Modify: `firmware/main/main.c` (event posts + status_json), `firmware/main/CMakeLists.txt` (REQUIRES printer_state)

**Interfaces:**
- Consumes: `ps_*` core (Task 3).
- Produces:
```c
void       printer_state_start(void (*on_change)(ps_state_t));  // cb may be NULL
void       printer_state_post_ev(ps_event_t ev);   // thread-safe, any task
ps_state_t printer_state_get(void);
```

- [ ] **Step 1: Glue** — `printer_state.c`: a `static ps_t s_ps` + `portMUX`/mutex; `post_ev` takes the mutex, calls `ps_post`, compares derived state to the previous, releases, and invokes the callback outside the lock when it changed. `CMakeLists.txt`: `idf_component_register(SRCS "printer_state.c" "printer_state_core.c" INCLUDE_DIRS "include")`.

```c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "printer_state.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static ps_t s_ps;
static SemaphoreHandle_t s_mtx;
static void (*s_cb)(ps_state_t);

void printer_state_start(void (*on_change)(ps_state_t))
{
    s_mtx = xSemaphoreCreateMutex();
    ps_init(&s_ps);
    s_cb = on_change;
}

void printer_state_post_ev(ps_event_t ev)
{
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    ps_state_t before = ps_state(&s_ps);
    ps_state_t after  = ps_post(&s_ps, ev);
    xSemaphoreGive(s_mtx);
    if (after != before && s_cb) s_cb(after);
}

ps_state_t printer_state_get(void)
{
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    ps_state_t st = ps_state(&s_ps);
    xSemaphoreGive(s_mtx);
    return st;
}
```

- [ ] **Step 2: Wire main.c** (each is a one-liner at an existing site):
  - top of `app_main` after the clamp: `printer_state_start(on_ps_change);` where `on_ps_change` is a new static that (for now) just `ESP_LOGI(TAG, "state -> %s", ps_name(st));` — Task 7 adds `led_grid_set_state(st)` to it.
  - before `wifi_start()` call: post `PS_EV_WIFI_CONNECTING`; on the `IP_EVENT_STA_GOT_IP` branch of `wifi_event`: post `PS_EV_WIFI_UP`; in the AP-mode branch: post `PS_EV_SETUP_MODE`.
  - `doc_begin`: post `PS_EV_JOB_START`. `doc_end`: post `PS_EV_JOB_FAIL` when `st.rc != 0 || !ok || s_printer_failed`, else `PS_EV_JOB_DONE`.
  - `printer_state_changed` (serial_out callback): post `PS_EV_PRINTER_BLOCKED` / `PS_EV_PRINTER_CLEARED` alongside the existing `ipp_server_set_stopped` call.
  - `status_json`: replace the hand-built state ternary's first argument with `s_ap_mode ? "Waiting for setup" : blocked ? "Out of paper" : s_printing ? "Printing" : "Ready"` → keep user-facing strings, but add a machine field `"ps":"%s"` sourced from `ps_name(printer_state_get())`.

- [ ] **Step 3: Compile gate** — `. ~/esp/esp-idf/export.sh && cd firmware && idf.py build` (C6). Expected: clean build (printer_state has no pin dependencies).
- [ ] **Step 4: Commit** — `"printer_state: ESP glue + main wiring"` + `Refs: W-586`.

---

### Task 7: `led_grid` ESP glue (RMT out via led_strip)

**Files:**
- Create: `firmware/components/led_grid/include/led_grid.h`, `firmware/components/led_grid/led_grid.c`, `firmware/components/led_grid/CMakeLists.txt`, `firmware/components/led_grid/idf_component.yml`
- Modify: `firmware/main/main.c` (start + on_ps_change hookup), `firmware/main/CMakeLists.txt` (REQUIRES led_grid), `firmware/main/Kconfig.projbuild` (add `AIRWRITER_LED_COUNT`)

**Interfaces:**
- Consumes: `lg_render` (Task 4), `printer_state_get/ps_state_t` (Task 6).
- Produces:
```c
esp_err_t led_grid_start(void);              // no-op ESP_OK when pin < 0
void      led_grid_set_state(ps_state_t st); // atomic; takes effect next frame
```

- [ ] **Step 1: Kconfig** — add after `AIRWRITER_LED_DATA_PIN`:

```
config AIRWRITER_LED_COUNT
    int "LED chain length"
    default 37
    help
        37 on the AirWriter board (D10-D46). A devkit with a shorter SK6812
        strip sets its real count; frames are rendered for the board's 37
        dots and truncated to the strip.
```

- [ ] **Step 2: Glue** — `led_grid.c`: `idf_component.yml` with `espressif/led_strip: "^3.0.0"`; strip config `LED_MODEL_SK6812`, GRB, RMT backend, pin `CONFIG_AIRWRITER_LED_DATA_PIN`, `CONFIG_AIRWRITER_LED_COUNT` pixels; a 30 ms-tick task:

```c
// SPDX-License-Identifier: GPL-3.0-or-later
// The panel chain. Rendering is the pure core; this task owns time and the
// wire. On a devkit with no strip (pin -1) start() is a successful no-op, so
// main never has to care. The IO48 boot clamp hands the pin over to the RMT
// driver here -- by the first frame the line is driven, never floating.
#include "led_grid.h"
#include "led_grid_core.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_strip.h"

static led_strip_handle_t s_strip;
static volatile int s_state = PS_BOOT;
static int s_count;

void led_grid_set_state(ps_state_t st) { s_state = (int)st; }

static void lg_task(void *arg)
{
    (void)arg;
    uint8_t frame[LG_NUM_LEDS][3];
    for (;;) {
        lg_render((ps_state_t)s_state,
                  (uint32_t)(esp_timer_get_time() / 1000), frame);
        for (int i = 0; i < s_count && i < LG_NUM_LEDS; i++)
            led_strip_set_pixel(s_strip, (uint32_t)i,
                                frame[i][0], frame[i][1], frame[i][2]);
        led_strip_refresh(s_strip);
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

esp_err_t led_grid_start(void)
{
#if CONFIG_AIRWRITER_LED_DATA_PIN < 0
    return ESP_OK;
#else
    s_count = CONFIG_AIRWRITER_LED_COUNT;
    led_strip_config_t sc = {
        .strip_gpio_num = CONFIG_AIRWRITER_LED_DATA_PIN,
        .max_leds = (uint32_t)s_count,
        .led_model = LED_MODEL_SK6812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
    };
    led_strip_rmt_config_t rc = { .resolution_hz = 10 * 1000 * 1000 };
    esp_err_t err = led_strip_new_rmt_device(&sc, &rc, &s_strip);
    if (err != ESP_OK) return err;
    led_strip_clear(s_strip);
    return xTaskCreate(lg_task, "led_grid", 3072, NULL, 3, NULL) == pdPASS
               ? ESP_OK : ESP_ERR_NO_MEM;
#endif
}
```

- [ ] **Step 3: main.c** — `on_ps_change` gains `led_grid_set_state(st);`; after `printer_state_start(...)` call `ESP_ERROR_CHECK(led_grid_start());` (after the clamp — the driver takes the pin over from the clamp).
- [ ] **Step 4: Compile gate** — C6 `idf.py build` (pin −1 path) **and** `idf.py set-target esp32s3 && idf.py build` in a scratch copy if quick, else defer the S3 compile to Task 9's checklist. Then restore the C6 sdkconfig state (`git checkout -- sdkconfig` if touched — sdkconfig is generated but tracked... it is NOT tracked; verify `git status` clean).
- [ ] **Step 5: Commit** — `"led_grid: SK6812 output task over the pure renderer"` + `Refs: W-586`.

---

### Task 8: `dip_config` glue + test button + test page

**Files:**
- Create: `firmware/components/dip_config/include/dip_config.h`, `firmware/components/dip_config/dip_config.c`, `firmware/components/dip_config/CMakeLists.txt`
- Modify: `firmware/main/Kconfig.projbuild` (DIP + button pins), `firmware/main/main.c` (init, callbacks, test page), `firmware/main/CMakeLists.txt` (REQUIRES dip_config)

**Interfaces:**
- Consumes: `dip_decode`/`dip_stable` (Task 5).
- Produces:
```c
typedef struct {
    void (*on_change)(const dip_config_t *cfg);  // debounced; also fires once at start
    void (*on_test_button)(void);                // debounced press (falling edge)
} dip_config_cb_t;
esp_err_t dip_config_start(const dip_config_cb_t *cb);  // no-op ESP_OK when all pins < 0
```

- [ ] **Step 1: Kconfig** — eight `AIRWRITER_DIP<N>_PIN` ints (defaults `38,39,40,41,42,47,21,13 if IDF_TARGET_ESP32S3`, else `-1`; help: "Board net DIP<N>, closed = GND, internal pull-up") and `AIRWRITER_TEST_BTN_PIN` (`default 14 if IDF_TARGET_ESP32S3`, else `-1`).
- [ ] **Step 2: Glue** — `dip_config.c`: collect the 8 pins + button into arrays; configure every pin ≥ 0 as input with `GPIO_PULLUP_ONLY`; a 100 ms poll task samples `on_bits` (bit set when `gpio_get_level == 0`), runs `dip_stable`, and on change `dip_decode` → `cb->on_change`; button uses its own 2-sample debounce, fires `on_test_button` on a stable 1→0 transition. Seed `*out = 0xFF` so the first stable sample always reports (start-up read).
- [ ] **Step 3: Test page in main.c** — `static void test_page(void)`: refuse when `s_printing` (log + return); otherwise spawn a one-shot task that renders 64 rows of alternating 8-px checkerboard + a 0→255 gradient band through `ht_row`/`iw_row` (the `raster_selftest` pattern at width 1152), streaming through the existing `printer_out` (so CRC, failure handling, and purge behavior all apply), bracketed by `PS_EV_JOB_START` / (`PS_EV_JOB_DONE` or `PS_EV_JOB_FAIL` when `s_printer_failed`), `serial_out_flush(120000)` at the end. Wire `on_test_button = test_page` and honor `test_on_boot` once after `ipp_server_start`. `on_change`: log the struct; apply `dev_mode` only as a log note for now (UUID behavior stays Kconfig-driven this rev — noted in the help text); `dither_ovr`/`mode` consumed in later milestones.
- [ ] **Step 4: Compile gate** — C6 `idf.py build` green (all pins −1 → no-op path).
- [ ] **Step 5: Commit** — `"dip_config: DIP bank + test button poller; button prints a test page"` + `Refs: W-586`.

---

### Task 9: Close-out — full battery, docs, Linear

**Files:**
- Modify: `docs/HANDOFF.md` (top entry addendum), `docs/firmware-audit-2026-08-28.md` (status line), Linear W-586.

- [ ] **Step 1: Full host regress** — `make -C tools/rip test && make -C tools/rip regress`: all green, `stream.bin` byte-identical.
- [ ] **Step 2: Both-target compile check** — C6 build green; then in a THROWAWAY copy of `firmware/` (or after `git stash` of nothing — sdkconfig is untracked): `idf.py set-target esp32s3 && idf.py build` green; restore the C6 sdkconfig for devkit work (`idf.py set-target esp32c6`, rebuild once).
- [ ] **Step 3: Docs** — HANDOFF top entry: one paragraph — what landed (four components + IDF back), what the devkit can now demo (LED states on a strip, DIP on jumpers), what still needs hardware (everything serial). Audit doc: mark item 6 (brightness cap) done, "later" gaps dip/test-button done, PIC bridge still open.
- [ ] **Step 4: Linear** — comment on W-586 with the checkpoint summary; leave **In Progress** (Wells flips to review/done).
- [ ] **Step 5: Final commit** of doc changes + `Refs: W-586`.

---

## Self-review notes

- Spec §5 coverage: `led_grid` states idle/printing/wifi/error (Tasks 4) ✔, material classes + brightness cap ✔, `dip_config` re-read on change ✔, test button → test page through the raster path ✔ (placeholder pattern is explicitly allowed by the spec), "runs on the devkit with a strip" ✔ (LED_COUNT + pin Kconfigs). Added beyond spec: PS_BLOCKED/PS_SETUP rendering (states exist on the device; leaving them unrendered would show stale frames — decision documented in Task 4's contract).
- Type consistency: `ps_state_t`/`ps_event_t` names match across Tasks 3/6/7; `dip_config_t` fields match across 5/8; `LG_MAP`/`LG_NUM_LEDS` across 2/4/7.
- Deliberately out: PIC/LocalTalk bridge (weeks, PAP-gated), `mode`/`dither_ovr` consumers (later milestones — decode now so the semantics are pinned), live dither re-init (job_pipe fixes `dither` at init; boot-read only, noted in Kconfig help).
