# Signed GitHub updates Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** AirWriter installs firmware from GitHub releases or `/update` only when it is signed
with the release key, with an optional auto-update that never cuts a print.

**Architecture:** IDF's signed apps without secure boot do the verification inside
`esp_ota_end`. A new `fw_update` component holds a pure-C core (versions, manifest, policy;
host-tested) and the IDF glue (one task, `esp_https_ota`, the shared stream path for
`/update`). main.c lends it the job lock.

**Tech Stack:** ESP-IDF 5.5.1 (esp_https_ota, esp_http_client, esp_crt_bundle, bootloader_support),
C11, host tests under `tools/rip`, bash + gh for releases.

**Spec:** `docs/superpowers/specs/2026-10-16-signed-github-updates-design.md`

## Global Constraints

- Key: `~/.config/airwriter/firmware-signing-key.pem`; public key `firmware/keys/airwriter-release.pub.pem`;
  digest `6f11af9cd8eec9923985957400aa753ba034e7e057e18ebf61033cb4d327ad89`.
- Repo `wr/airwriter`; assets `airwriter-esp32s3.bin`, `airwriter.json` = `{"version":"vX.Y.Z","esp32s3":"airwriter-esp32s3.bin"}`.
- Check URL `https://github.com/<repo>/releases/latest/download/airwriter.json`; asset URL
  `https://github.com/<repo>/releases/download/<tag>/<file>`. HTTP rx buffer 4096, tx 2048.
- Release version `^v\d+\.\d+\.\d+$`; a tag install also takes `-[0-9A-Za-z.]+` after it. File `[A-Za-z0-9._-]+`, ≤ 64, no leading dot.
- Timing defaults: first check 10 min after the first Wi-Fi join, then every 24 h; a failed check retries after 1 h; install after 10 min idle.
- NVS `airwriter`/`autoupd` (u8), default 0.
- Panel: `led_grid_action(LG_ACT_FW_DOWNLOADING, 0..1000)`, `LG_ACT_FW_VERIFYING`, `LG_ACT_FW_VERIFIED`, `LG_ACT_FW_REFUSED`, `LG_ACT_FW_NONE`. Never edit led_grid.
- Every new source file has an SPDX header (GPL-3.0-or-later). Comments in the house voice.
- Never commit the private key, Wi-Fi credentials, or purge-list strings.

## Review Focus

1. A job arriving mid-download must win: the update aborts and the job prints (Task 5 aborts on the job-waiting flag between chunks; bench step 5).
2. A manifest naming a hostile file or version (`../x`, `v1.2`, 40-char strings) must not build a URL (Task 1 tests).
3. Dev versions (`v0.1.0-3-gabc1234-dirty`, `ca0fc40-dirty`, empty, 32-char) must compare sanely and never auto-install (Task 1 tests).
4. The version the bootloader rolled back must not auto-install again (Task 1 policy test).
5. Uptime past 49 days must not break the schedule: the core works in int64 seconds (Task 1 test at 60 days).

---

### Task 1: fw_update core and host tests

**Files:**
- Create: `firmware/components/fw_update/include/fw_update_core.h`, `firmware/components/fw_update/fw_update_core.c`
- Create: `tools/rip/test/test_fw_update.c`
- Modify: `tools/rip/Makefile` (a `build/test_fw_update` rule like `test_ota_health`'s)

**Interfaces (Produces):**
```c
typedef struct { int maj, min, pat; } fwu_ver_t;
int  fwu_parse_version(const char *s, fwu_ver_t *out);   // 1 if s starts with vX.Y.Z; else 0 and 0.0.0
int  fwu_is_release(const char *s);                      // exactly vX.Y.Z
int  fwu_valid_tag(const char *s);                       // vX.Y.Z or vX.Y.Z-[0-9A-Za-z.]+, < 32 chars
int  fwu_valid_file(const char *s);
int  fwu_newer(const char *latest, const char *running); // release triple of latest > running's
typedef enum { FWU_M_OK, FWU_M_BAD, FWU_M_NO_TARGET } fwu_manifest_t;
fwu_manifest_t fwu_parse_manifest(const char *json, const char *target,
                                  char *ver, size_t vcap, char *file, size_t fcap);
int  fwu_manifest_url(char *out, size_t cap, const char *repo);                 // 0 if it didn't fit
int  fwu_asset_url(char *out, size_t cap, const char *repo, const char *tag, const char *file);
typedef struct { int64_t first_s, interval_s, retry_s, idle_s; } fwu_timing_t;
typedef struct {
    int auto_on, setup_mode, key_ok, on_trial, busy;
    int running_release, known_newer, newer_rolled_back, install_tried, check_failed;
    int64_t now_s, joined_s, last_check_s, last_job_s;   // -1 = never
} fwu_inputs_t;
typedef enum { FWU_WAIT, FWU_CHECK, FWU_INSTALL } fwu_action_t;
fwu_action_t fwu_policy(const fwu_inputs_t *in, const fwu_timing_t *tm);
```

- [ ] **Step 1: Write `test_fw_update.c`** asserting:
  - `fwu_parse_version("v1.2.3")` → 1, {1,2,3}; `"v0.1.0-3-gabc1234-dirty"` → 1, {0,1,0}; `"ca0fc40-dirty"`, `""`, `"v1.2"`, `"1.2.3"` → 0, {0,0,0}.
  - `fwu_is_release`: true only for `v0.1.0`, `v10.20.30`; false for the dev strings above and `v0.1.0-rc.1`.
  - `fwu_valid_tag`: `v0.1.1-tamper` true; `v0.1.1-`, `v0.1.1/x`, a 40-char tag false.
  - `fwu_valid_file`: `airwriter-esp32s3.bin` true; `../x`, `.hidden`, `a/b`, `a b`, 65 chars false.
  - `fwu_newer("v0.1.1","v0.1.0")` 1; `("v0.1.0","v0.1.0-3-gabc")` 0; `("v0.1.0","ca0fc40")` 1; `("v0.2.0","v0.10.0")` 0; `("garbage","v0.1.0")` 0.
  - Manifest: the exact constraint JSON with target `esp32s3` → OK, `v0.1.1`, `airwriter-esp32s3.bin`; target `esp32c6` → NO_TARGET; `{"version":"v1.2","esp32s3":"a.bin"}` → BAD; `{"version":"v1.2.3","esp32s3":"../a"}` → BAD; whitespace after colons accepted; truncated JSON → BAD.
  - URLs: `fwu_asset_url(.., "wr/airwriter", "v0.1.1", "airwriter-esp32s3.bin")` gives the exact constraint URL; a 16-byte buffer returns 0.
  - Policy (timing 600/86400/3600/600 s): auto off → WAIT always; never joined → WAIT; joined at 20 s, now 619 → WAIT, 620 → CHECK; checked at 620 with known newer, running release, last job −1, now 621 → INSTALL; same with `newer_rolled_back`, `install_tried`, `on_trial`, `busy`, `!running_release`, `!key_ok` or `setup_mode` → WAIT; last job 300 s ago → WAIT, 600 s ago → INSTALL; `check_failed` at 620 → CHECK at 4220 not 4219; next CHECK at `last_check + 86400`; all times shifted by 60 days still behave.
- [ ] **Step 2: Run** `make -C tools/rip build/test_fw_update` → fails (no header).
- [ ] **Step 3: Implement** `fw_update_core.c`. Manifest: a flat-object key lookup like webui's `json_str`, then the validators. Policy order: gates → CHECK due → INSTALL conditions.
- [ ] **Step 4: Run** `make -C tools/rip test` → all pass, including the existing tests.
- [ ] **Step 5: Commit** `fw_update: version, manifest and auto-update policy, host-tested`.

### Task 2: Signing on by default, and builds that refuse without it

**Files:**
- Modify: `firmware/sdkconfig.defaults` (the four options + `CONFIG_SECURE_BOOT_SIGNING_KEY="firmware_signing_key.pem"`, comment block)
- Modify: `.gitignore` (`firmware/*.pem`)
- Create: `firmware/keys/airwriter-release.pub.pem` (already extracted)
- Modify: `firmware/main/main.c` (`#error` beside W-1046's when any of the four is off)
- Modify: `firmware/main/Kconfig.projbuild`: `AIRWRITER_UPDATE_REPO` ("wr/airwriter"), `AIRWRITER_UPDATE_KEY_DIGEST` (the digest), `AIRWRITER_UPDATE_FIRST_MIN` (10), `AIRWRITER_UPDATE_INTERVAL_H` (24), `AIRWRITER_UPDATE_IDLE_MIN` (10)
- Local only: `firmware/sdkconfig`, `firmware/sdkconfig.c6` get the five lines with the absolute key path

- [ ] **Step 1:** Make the edits. **Step 2:** `idf.py build` and the C6 build → clean; `espsecure.py signature_info_v2 build/airwriter.bin` shows digest `6f11af9c…`. **Step 3:** with the four options removed from a scratch copy of the sdkconfig, the build stops at the `#error`. **Step 4: Commit** `Signed app images: build them, require them on update`.

### Task 3: The auto-update setting

**Files:** Modify `firmware/components/settings/include/settings.h`, `settings.c`.

**Produces:** `uint8_t auto_update` in `settings_t` (loaded from `autoupd`, default 0); `esp_err_t settings_save_auto_update(int on);`.

- [ ] **Step 1:** Implement; `settings_erase` already clears the namespace. **Step 2:** build. **Step 3:** commit with Task 4.

### Task 4: fw_update glue

**Files:**
- Create: `firmware/components/fw_update/fw_update.c`, `include/fw_update.h`, `CMakeLists.txt`
  (REQUIRES app_update esp_https_ota esp_http_client esp-tls mbedtls bootloader_support esp_timer
  nvs_flash settings led_grid ota_health)

**Consumes:** Task 1's core; `settings_*`; `ota_health_on_trial()`, `ota_health_rolled_back()`; `led_grid_action`.

**Produces:**
```c
typedef struct {
    int     (*try_lock)(void);        // the job lock, no wait; 1 if taken
    void    (*unlock)(void);
    int     (*job_waiting)(void);     // a job found the lock taken since the last clear
    void    (*job_waiting_clear)(void);
    int64_t (*last_job_s)(void);      // uptime seconds of the last job begin or end, -1 never
    int       setup_mode;
    int       auto_on;                // from settings
} fw_update_cfg_t;
esp_err_t fw_update_start(const fw_update_cfg_t *cfg);
void      fw_update_net_up(void);                     // each Wi-Fi join; the first starts the schedule
void      fw_update_json(char *buf, size_t cap);      // GET /api/update body
esp_err_t fw_update_check(void);                      // async; ESP_ERR_INVALID_STATE if busy or off
esp_err_t fw_update_install(const char *tag, const char **why);  // async; tag NULL = latest known
esp_err_t fw_update_set_auto(int on);
// /update. One at a time; begin takes the job lock and holds it to the reboot or abort.
esp_err_t fw_update_stream_begin(size_t len, const char **why);
esp_err_t fw_update_stream_write(const void *buf, size_t n, const char **why);
esp_err_t fw_update_stream_end(const char **why);     // verified and set to boot
void      fw_update_stream_abort(void);
void      fw_update_restart(void);                    // reboot 800 ms from now
```
Errors: `ESP_ERR_INVALID_STATE` = busy, a job, on trial, setup mode (HTTP 409);
`ESP_ERR_OTA_VALIDATE_FAILED` = refused (400); anything else 500. `why` is the UI's sentence:
"printing", "the last update is still being checked; try again in two minutes",
"a print job arrived; try again after it", "not signed with this board's key, or damaged".

- [ ] **Step 1:** At start: read the running key digest with
  `esp_secure_boot_get_signature_blocks_for_running_app(true, &d)` → `key` = airwriter / other /
  unsigned against the Kconfig hex. The task wakes every 30 s (and on a request), fills
  `fwu_inputs_t` and acts on `fwu_policy`, or on the pending manual request.
- [ ] **Step 2: Check** = `esp_http_client_perform` on the manifest URL, body ≤ 512 bytes
  collected in the event handler, then `fwu_parse_manifest` with `CONFIG_IDF_TARGET`.
- [ ] **Step 3: Install** = try_lock (else busy) → clear waiting → not on trial →
  `esp_https_ota_begin` → `esp_https_ota_get_img_desc`: abort unless `project_name` is
  `airwriter` and (latest path) its version equals the manifest's → loop `esp_https_ota_perform`,
  between chunks abort on `job_waiting()` and post DOWNLOADING with permille from
  `get_image_len_read / get_image_size` (`LG_PROGRESS_NONE` if size ≤ 0) →
  `is_complete_data_received` → VERIFYING → `esp_https_ota_finish` → VERIFIED and restart, or
  REFUSED (validate failed) / NONE (job, network). Unlock on every way out but success.
- [ ] **Step 4: Stream** functions share the lock, abort and panel code with Step 3, over
  `esp_ota_begin/write/end/set_boot_partition`.
- [ ] **Step 5: JSON** per the spec's field list; `checked` is seconds since the last check or −1.
- [ ] **Step 6:** `idf.py build` clean. **Step 7: Commit** `fw_update: check, install and stream signed images` (with Task 3).

### Task 5: Web UI and main.c wiring

**Files:**
- Modify: `firmware/components/webui/webui.c` (`update_post` over the stream API; routes
  `GET /api/update`, `POST /api/update/check`, `/api/update/install` (`{"tag":...}` optional),
  `/api/update/auto` (`{"auto":true|false}`); `max_uri_handlers` 14 → 18), its CMakeLists (REQUIRES fw_update)
- Modify: `tools/webui/body.html`, `tools/webui/app.js`; regenerate `webui_page.h` with `mkheader.py`
- Modify: `firmware/main/main.c`, `firmware/main/CMakeLists.txt`

- [ ] **Step 1: main.c.** `s_job_waiting` set when doc_begin's non-blocking take fails (then it
  blocks as now), and when lt_print_begin, test_page and the cable test fail theirs; never
  from kick_task, the TashTalk flash or cable_web. `s_last_job_s` set at every job begin and
  end. `fw_update_start` after `settings_load` with the callbacks; `fw_update_net_up()` on
  `IP_EVENT_STA_GOT_IP`.
- [ ] **Step 2: UI.** A "firmware" section after "printer": a `say` line from `/api/update`,
  keys check (momentary), install (momentary, `is-quiet` unless `newer`), auto (latching).
  On page load, POST check when `checked` is −1 or > 3600. Poll `/api/update` every 2 s while
  `state` is checking or downloading. Install shows the existing overlay. The footer
  upload's errors show the server's text.
- [ ] **Step 3:** `python3 tools/webui/mkheader.py`; S3 and C6 builds clean.
- [ ] **Step 4: Commit** `Web UI: firmware section; /update through fw_update`.

### Task 6: Release script

**Files:** Create `tools/release/release.sh`.

- [ ] **Step 1:** `release.sh [--branch] vX.Y.Z [notes-file]` per spec §Releases, steps 1–6.
  App version read from the image at offset 48 (32 bytes), project name at 80. Credentials
  from `firmware/sdkconfig`'s `CONFIG_AIRWRITER_WIFI_SSID` / `_PASSWORD` in the checkout
  running the script; scrub strings are each line's left side of `==>`, not printed on a hit
  (say "a purge-list string"). `gh release create TAG airwriter-esp32s3.bin airwriter.json --verify-tag`.
- [ ] **Step 2:** `bash -n`; a dry run with `--dry-run` (builds and checks, skips tag push and
  gh) on the branch → passes. **Step 3: Commit** `tools/release: build, check, sign and publish a release`.

### Task 7: Builds, review, then the bench (gated)

- [ ] S3 and C6 clean; record image size and free heap after startup (C6 boot log not needed; size only).
- [ ] Whole-branch review (one fresh Opus reviewer); apply fixes.
- [ ] Ask the main session for the go. Then the spec's bench steps 1–5, then v0.1.0's factory-slot
  write over USB on a second go.

### Task 8: Docs

- [ ] SPEC §3.12 (new) and a line in §3.11; `docs/traps.md` (unsigned builds refused; key path
  in sdkconfig; release builds never from the local sdkconfig); AGENTS.md (State, Open work,
  Toolchain: key + release script, Verification); wiki `Settings-and-Updates` (plain first person).
- [ ] Commit, push branch, report to the main session.
