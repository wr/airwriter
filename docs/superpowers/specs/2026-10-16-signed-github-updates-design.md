# Signed firmware updates from GitHub — design

Linear: `W-1048 OTA updates from GitHub releases: signed, verified, optional auto-update`.
Written 2026-10-16. Builds on SPEC §3.11 (factory slot, trial and rollback, W-1046) and
amends it with a new §3.12.

## Intent

An AirWriter owner gets new firmware without a computer or a USB cable: the board finds
releases on github.com/wr/airwriter and installs them on request, or by itself if the owner
switched that on. Only firmware signed with the AirWriter release key installs, whether it
comes from GitHub or from a browser on the LAN through `/update`.

What the signature defends against: anyone on the LAN posting firmware to the web UI, which
has no login; someone who takes over the GitHub account or swaps a release asset; a corrupted
download. What it does not: anyone with the board on a USB cable. There is no secure boot and
no flash encryption, and no eFuse is burned, so a board stays reflashable by its owner.

## Goals, non-goals, anti-goals

Goals
- Over the air, only images signed with the release key install, on both paths.
- The board finds the newest release, shows it in the web UI, and installs it on request.
- Auto-update, off by default.
- An update never cuts a print. W-1046's trial and rollback apply to every update, and the
  factory slot is never written over the air.

Non-goals: secure boot, flash encryption, anti-rollback eFuses, a login on the web UI, delta
updates, a beta channel, release builds for the C6 devkit, a wall clock (no SNTP), building
releases in CI, a USB-flashable "full" image in the release (follow-up).

Anti-goals, each a failure even if updates work:
- anything that can brick a board (eFuses, a new bootloader over the air);
- an update that cuts a job, or starts while one waits;
- an auto-update loop on a release that rolls back;
- a board that quietly stops updating: every failure shows in the web UI;
- Wells's Wi-Fi credentials, or anything on the purge list, in a published binary.

## Signing

**Scheme (decision 1, pending; this design assumes the recommendation): ESP-IDF's signed apps
without secure boot.**

```
CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT=y
CONFIG_SECURE_SIGNED_APPS_RSA_SCHEME=y        # the S3 does SBv2 with RSA only; the C6 matches
CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT=y
CONFIG_SECURE_BOOT_BUILD_SIGNED_BINARIES=y
CONFIG_SECURE_BOOT_SIGNING_KEY="..."          # see key custody
```

How it checks (read in IDF 5.5.1, `secure_boot_signatures_app.c`): `esp_ota_end` runs
`esp_image_verify`, which hashes the new image and checks the RSA-PSS signature in its first
signature block, and requires that block's public key to have the same SHA-256 digest as the
key in the *running* image's first block. Every write through `esp_ota_*` goes through it,
so `/update` and `esp_https_ota` share one check with no code of ours in it.

What follows from that:
- **The trusted key travels with the running image.** A board running an unsigned image (board 1
  today, every image before W-1048, board 1's factory slot) accepts any valid image. That is how
  the first signed image gets on. From then on it accepts only images signed with the same key.
- **An unsigned image built with these options would refuse every update** ("no signatures
  were found for the running app"). `BUILD_SIGNED_BINARIES` signs every build, and main.c
  refuses to build (`#error`, the W-1046 pattern) without the four options above, on both targets.
- **Changing the key takes a USB flash.** Without secure boot IDF checks only block 0, so an
  image cannot carry an old and a new signature.
- **The bootloader does not change.** `SECURE_SIGNED_ON_BOOT_NO_SECURE_BOOT` exists only for
  the ESP32's V1 scheme. Board 1 keeps its W-1046 bootloader and table; no USB flash.
- The app partitions must be multiples of 64 KB (2 MB and 1.5 MB are). The image grows by its
  padding and a 4 KB signature sector.
- Someone building from source makes a key of their own (the IDF build stops and prints the
  `espsecure.py generate_signing_key` command). Their board then takes only their builds and
  refuses AirWriter releases; the web UI says why (below). A USB flash of a release takes it back.

**Key custody (decision 2, pending; this design assumes the recommendation): local.**
- One RSA-3072 key, made once with `espsecure.py generate_signing_key --version 2 --scheme rsa3072`,
  at `~/.config/airwriter/firmware-signing-key.pem` (mode 600) on Wells's Mac only. It does not
  exist yet: the builder makes it on Wells's Mac as the first step, and Wells copies it into
  1Password. Losing it means every board in the field needs a USB flash. Everything signed
  from then on, and the committed public key and digest, follow from that one file, so no
  throwaway key is used for anything that reaches board 1.
- Never in the repo. `.gitignore` covers `firmware/*.pem` as a backstop; gitleaks flags PEM
  private keys.
- The public key is committed at `firmware/keys/airwriter-release.pub.pem` with its SHA-256
  digest in the wiki, so anyone can check a release with
  `espsecure.py verify_signature --version 2 --keyfile firmware/keys/airwriter-release.pub.pem`.
- Wells's own `firmware/sdkconfig` (gitignored) points `CONFIG_SECURE_BOOT_SIGNING_KEY` at the
  absolute path, so every bench build is signed and bench OTAs work as now.
  `sdkconfig.defaults` names `firmware_signing_key.pem` in `firmware/` (gitignored), which is
  where a stranger's key goes.

**The official key's digest is compiled in** as `CONFIG_AIRWRITER_UPDATE_KEY_DIGEST` (hex
string; a fork changes it with `CONFIG_AIRWRITER_UPDATE_REPO`). At boot the firmware reads the
running image's key digest. If it differs, the GitHub path is off and the web UI says "This
firmware is signed with a different key, so it can't install AirWriter releases." A W-1048 image
running without a signature (flashed unsigned by hand) refuses every update, so the UI says so
and points to a USB flash. This only avoids doomed downloads; the check that matters is still
`esp_ota_end`'s.

## Releases

- A release is a tag `vMAJOR.MINOR.PATCH` on `main`, pushed to origin, with two assets:
  - `airwriter-esp32s3.bin`, the signed app image (what `/update` and the board download);
  - `airwriter.json`, a flat manifest: `{"version":"v0.1.1","esp32s3":"airwriter-esp32s3.bin"}`.
- `tools/release/release.sh vX.Y.Z [notes-file]`, run on Wells's Mac:
  1. refuses unless on a clean `main` that matches origin;
  2. tags (annotated) and pushes the tag;
  3. builds the S3 image in a temporary `git worktree` at the tag, from a fresh sdkconfig made
     from `sdkconfig.defaults*` plus the key path. **Never from the local `sdkconfig`**, which
     holds Wells's Wi-Fi credentials;
  4. checks the binary: the app version is exactly the tag; no string from the local
     sdkconfig's Wi-Fi SSID or password and none from `~/.config/airwriter/purge-scrub.txt`
     occurs in it; dev options (`DEV_MODE`, `LT_BENCH`, `UART_LOOPBACK`) are off; the
     signature verifies against the committed public key;
  5. writes the manifest and runs `gh release create` with both assets;
  6. removes the worktree.
- The firmware's version is IDF's `git describe`: a release build reads `v0.1.1`, a dev build
  something like `v0.1.0-3-gabc1234-dirty`, or a bare hash before the first tag.
- **The first two releases are cut before the merge (decision 4, pending).** The bench test
  needs real releases containing this code, and the merge waits for "merge it". So v0.1.0 is
  tagged on the branch tip once the code is reviewed, and v0.1.1 on the next commit (the
  bench results in AGENTS.md). Both are pushed with the branch. The merge is `--no-ff`, as
  this repo's merges are, so both tags stay in `main`'s history. `release.sh` takes
  `--branch` for this; without it, it requires `main`. The branch is never rebased after a tag.

## How the board finds and installs a release

- **Check:** GET `https://github.com/<repo>/releases/latest/download/airwriter.json`. GitHub
  redirects that to the newest release's asset, skipping drafts and prereleases. There is no
  API call and no rate limit to share. `<repo>` is `CONFIG_AIRWRITER_UPDATE_REPO`, default
  `wr/airwriter`.
- **Install:** `esp_https_ota` from
  `https://github.com/<repo>/releases/download/<version>/<file>`, with `version` matching
  `^v\d+\.\d+\.\d+$` and `file` matching `[A-Za-z0-9._-]+` before either goes into a URL.
  The board's target name (`esp32s3`, `esp32c6`) picks the manifest key; a missing key reads
  "no release for this board". TLS uses the certificate bundle already in the sdkconfig.
  Asset downloads redirect to a signed `*.githubusercontent.com` URL several hundred
  characters long, so the HTTP client's receive and transmit buffers are 4 KB and 2 KB.
- **The manifest is a hint, never trusted.** Right after `esp_https_ota_begin`, the image's own
  app description is read (`esp_https_ota_get_img_desc`). The download aborts if its project
  name isn't `airwriter`, or, on the automatic path, if its version isn't newer than the running
  one. Those bytes are covered by the signature that `esp_https_ota_finish` then checks, so a
  version that passed here is the version that installs.
- **A specific release:** install takes an optional tag, so an owner can reinstall an older
  release (downgrades are allowed by hand, as `/update` already allows them). This is also how
  a prerelease is tested.

**Versions.** Parse the leading `vX.Y.Z`; no such prefix reads 0.0.0. A release is newer when
its triple is greater than the running triple, so a dev build past v0.1.0 is not offered
v0.1.0. A *release build* is one whose version is exactly `vX.Y.Z`.

## When an update may run (both paths)

- **The job lock is the gate.** An update takes `s_job_lock` without waiting and holds it
  from `esp_ota_begin` to the reboot, so no IPP job, PAP job, test page or CTRL-X kick can
  start meanwhile. If a job holds it, the update is refused ("printing"); the automatic path
  tries again later.
- **A job that arrives mid-download wins.** Each job path raises a "job waiting" flag when it
  finds the lock taken (IPP `doc_begin` before its blocking take; LocalTalk and the test page
  when their non-blocking take fails). The update checks the flag between chunks, aborts,
  releases the lock, and reports "stopped for a print job". `/update` does the same and
  answers 409. Once `esp_ota_end` has accepted the image, it commits: boot partition, reply,
  reboot within about a second. A job arriving in that second is cut by the reboot; that is
  accepted.
- **Not while the last update is on trial.** Checked up front with the existing message;
  `esp_ota_begin` would refuse anyway.
- **Not in setup mode.** The setup AP has no internet, so the GitHub path is off; `/update` stays.
- **Never into the factory slot.** `esp_ota_get_next_update_partition` already alternates
  ota_0 and ota_1.

## Auto-update (decision 3, pending; this design assumes the recommendation)

- A setting, NVS namespace `airwriter`, key `autoupd`, default off. Erased by both factory resets.
- **Off means the board never contacts GitHub by itself.** It checks only when someone presses
  check, or opens the web UI and the last check is over an hour old.
- **On:** the first check comes 10 minutes after the first Wi-Fi join, then every 24 hours of
  uptime. Without a clock, "daily" restarts at each boot; that is fine. A newer release installs
  once the board has been idle 10 minutes: lock free, no job begun or ended in that time, and
  not on trial. Until then it looks again every minute.
- **What it never does by itself:**
  - replace a dev build (it shows the update, and a person can install it);
  - install the version the bootloader last rolled back (`ota_health_rolled_back()`); a person
    still can;
  - retry a failed download or a refused signature before the next daily check.
- Times are Kconfig ints (`AIRWRITER_UPDATE_FIRST_MIN` 10, `_INTERVAL_H` 24, `_IDLE_MIN` 10).

## Web UI

A new "firmware" section, after "printer", in the existing key style:
- a `say` line, for example "v0.1.0. Up to date, checked 2 hours ago.", "v0.1.1 is available.",
  "Downloading v0.1.1: 40%.", or "Refused v0.1.1: not signed with the AirWriter key.";
- keys: **check** (momentary), **install** (momentary, quiet unless a newer release is known),
  **auto** (latching).

The footer's "update firmware" upload stays. Its rejection says "not signed with this board's
key, or damaged" and, for a job, "a print job arrived; try again after it".

Endpoints (the webui's handler table grows from 12 to 16 slots):
- `GET /api/update`: `{"version","release":bool,"latest","newer":bool,"state":
  "idle|checking|downloading|installing|failed","progress":0-100,"error","checked":seconds-ago
  or -1,"auto":bool,"key":"airwriter|other|unsigned","repo"}`;
- `POST /api/update/check`, `POST /api/update/install` (optional body `{"tag":"v0.1.0"}`),
  `POST /api/update/auto` (`{"auto":true}`). Install answers 409 while printing or on trial, or
  when nothing newer is known and no tag was given.

`/api/status` is unchanged; its 1 KB buffer has no room to spare.

## The panel (W-1049's hook)

`W-1049 Panel feedback for every action: OTA, reset, cables, TashTalk flash` owns the patterns
and the led_grid API: `led_grid_action(act, arg)` with `LG_ACT_FW_DOWNLOADING` (arg 0..1000, or
`LG_PROGRESS_NONE` when the length is unknown), `LG_ACT_FW_VERIFYING` (image in, `esp_ota_end`
running), `LG_ACT_FW_VERIFIED` (set to boot; holds until the restart), `LG_ACT_FW_REFUSED`
(signature, wrong image or write error; about 3 s) and `LG_ACT_FW_NONE` (clear, as on an abort
for a print job). Both paths stream into the slot, so `LG_ACT_FW_INSTALLING` is unused.

Both paths call one hook in fw_update that maps its phases onto those. The enum arrives by
cherry-picking W-1049's commit of `led_grid_core.h`; this branch writes nothing in led_grid.
W-1049 also adds the same calls to webui.c's `update_post`; this branch moves that body into
fw_update and keeps the calls there, and the merge takes W-1049's lines out of webui.c.

## Code layout

- `components/fw_update/fw_update_core.{c,h}`: pure C, host-tested in `tools/rip` (`make test`):
  version parse and compare, release-build test, manifest parse, tag and file-name validation,
  URL building, and the auto-update policy as a function of (setting, running version, latest,
  rolled back, on trial, idle time, uptime, last check) → wait / check / install.
- `components/fw_update/fw_update.{c,h}`: the IDF glue. One task, the check and install with
  `esp_http_client` and `esp_https_ota`, the NVS setting, the key-digest read, the JSON for
  `/api/update`, and a `fw_update_stream_*` API that `/update` uses so both paths share the
  lock, the job-waiting abort, the hook and the error text.
- main.c lends the job lock, the job-waiting flag and the last-job time through callbacks in
  the component's config, the way it already does for webui and localtalk.

## Board 1 and the bench

- Board 1 runs an unsigned W-1055 dev build now, so the first signed image goes on by `/update`.
- Once board 1 runs W-1048 it refuses unsigned builds, including those from branches without
  W-1048 (W-1049, W-1055). Signing is sdkconfig only, so such a branch needs no rebase: the
  four options and the key path in its own (gitignored) sdkconfig sign its build. A trap for
  `docs/traps.md`, like W-1046's.
- Every bench build is signed by Wells's sdkconfig, so `curl --data-binary @build/airwriter.bin`
  keeps working.
- *(Superseded by `W-1059 Known-good slot: factory reset boots the last version that worked`:
  there is no factory slot, and this USB write was never made.)*
  **The factory slot must hold a signed release build**, or a factory reset brings back
  firmware that takes any image. Shipped boards get a release in the factory slot at their
  first USB flash. Board 1's factory slot holds an unsigned W-1046 build. After the bench test
  it gets the v0.1.0 asset over USB (`esptool.py write_flash 0x20000 airwriter-esp32s3.bin`,
  which leaves otadata alone, so the board keeps booting its OTA slot). That needs the main
  session's go and the USB cable. A release has no Wi-Fi credentials, so from then on a full
  factory reset brings board 1 up in setup mode, as a shipped board would.

## Verification

- Host: `make -C tools/rip test` covers the core: versions, including dev strings, the 32-char
  limit and junk; manifests, including missing keys and hostile file names; every policy branch.
- Builds: S3 and C6 clean. Record the image sizes against the 2 MB and 1.5 MB slots and free
  heap after startup on both.
- Board 1, after the main session's go:
  1. `/update` with the published v0.1.0 asset; the health gate keeps it.
  2. Publish v0.1.1, switch auto on, leave the board idle: it installs v0.1.1 by itself
     (about 10 minutes after boot) and keeps it.
  3. Refused by `/update`: v0.1.1 with one byte flipped; an image signed with a throwaway key;
     an unsigned image. Each leaves the running version and slot unchanged.
  4. Refused from GitHub: a prerelease whose asset is v0.1.1 with one byte flipped, installed
     by tag. The prerelease is deleted straight after.
  5. A job sent during a GitHub download wins: the download aborts and the job prints.

## Left to the builder

Task stack sizes; the exact `say` copy within the voice above; chunk sizes; whether the core's
policy is one function or two; how the release script asks for notes.

Settled by the writer (check these): `key: "unsigned"` stays, as one branch on a zero digest
count. Only a hand-flashed `airwriter-unsigned.bin` reaches it; it is kept so the UI never claims
a key that isn't there. The automatic path stays quiet with auto off; a page load can still check.
Both paths abort for a job. A manual install is allowed from a dev build and to an older release.
